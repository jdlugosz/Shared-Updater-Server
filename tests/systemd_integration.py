#!/usr/bin/env python3
"""Root-only, isolated service acceptance on VPS Local; never modifies the live Caddy service."""
import argparse
import concurrent.futures
import http.client
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time

from server_integration import Fixture, ID, request, wait_for


def command(*args, check=True):
    return subprocess.run(args, capture_output=True, text=True, timeout=15, check=check)


def properties(unit):
    result = command("systemctl", "show", unit, "-p", "MainPID", "-p", "NRestarts", "-p", "ActiveState")
    return dict(line.split("=", 1) for line in result.stdout.splitlines())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--unit-template", required=True)
    args = parser.parse_args()
    assert os.geteuid() == 0, "Run as root"
    assert socket.gethostname().split(".")[0] == "vps-stage", "This test is confined to VPS Local"
    root = Path(tempfile.mkdtemp(prefix="shared-updater-systemd-", dir="/run"))
    root.chmod(0o755)  # The dynamic service user must read only the synthetic catalog/config.
    fixture = Fixture(root, args.server,
                      times="worker_ms = 200\nsqlite_busy_ms = 2000\nshutdown_ms = 500")
    with socket.socket() as reserved:
        reserved.bind(("127.0.0.1", 0))
        fixture.port = reserved.getsockname()[1]
    name = "shared-updater-test-" + str(os.getpid())
    unit = name + ".service"
    unit_file = Path("/run/systemd/system") / unit
    fixture.state = Path("/var/lib") / name
    fixture.logs = Path("/var/log") / name
    configuration = fixture.config.read_text().replace(
        'listen = "127.0.0.1:0"', f'listen = "127.0.0.1:{fixture.port}"').replace(
        'state_dir = "state"', f'state_dir = "{fixture.state}"').replace(
        'log_dir = "logs"', f'log_dir = "{fixture.logs}"')
    fixture.config.write_text(configuration)
    template = Path(args.unit_template).read_text()
    template = template.replace(
        "ExecStart=/usr/local/libexec/shared-updater-server --config /etc/shared-updater-server/server.toml",
        f'ExecStart={args.server} --config {fixture.config}').replace(
        "StateDirectory=shared-updater-server", f"StateDirectory={name}").replace(
        "LogsDirectory=shared-updater-server", f"LogsDirectory={name}").replace(
        "WatchdogSec=30s", "WatchdogSec=1s").replace(
        "RestartSec=2s", "RestartSec=200ms").replace(
        "TimeoutStartSec=45s", "TimeoutStartSec=10s").replace(
        "TimeoutStopSec=25s", "TimeoutStopSec=5s")
    failed = True
    try:
        unit_file.write_text(template)
        unit_file.chmod(0o644)
        command("systemd-analyze", "verify", str(unit_file))
        command("systemctl", "daemon-reload")
        command("systemctl", "start", unit)
        wait_for(lambda: fixture.health()["ready"], "systemd readiness")
        first_pid = int(properties(unit)["MainPID"])
        assert first_pid > 0
        assert request(fixture.port)[0] == 200
        # An idle worker must remain healthy over several watchdog periods.
        time.sleep(2.3)
        assert int(properties(unit)["MainPID"]) == first_pid
        assert properties(unit)["NRestarts"] == "0"
        fixture.write_catalog("accepted")
        command("systemctl", "reload", unit)
        assert fixture.health()["revision"] == "accepted"
        assert fixture.health()["reload"] == "accepted"
        fixture.write_catalog("invalid", invalid=True)
        command("systemctl", "reload", unit)
        assert fixture.health()["revision"] == "accepted"
        assert fixture.health()["reload"] == "rejected"
        assert request(fixture.port)[0] == 200
        fixture.write_catalog("accepted")  # The next restart must see the last accepted input.
        assert (fixture.state.stat().st_mode & 0o077) == 0
        assert ((fixture.state / "census.sqlite").stat().st_mode & 0o077) == 0
        database = fixture.database()
        database.execute("BEGIN IMMEDIATE")
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            blocked = pool.submit(request, fixture.port)
            wait_for(lambda: fixture.health()["phase"] == "persisting", "watchdog test entered persistence")
            wait_for(lambda: int(properties(unit)["MainPID"]) not in (0, first_pid), "watchdog restarted service", timeout=8)
            database.rollback()
            try:
                blocked.result(timeout=4)
                raise AssertionError("The watchdog test unexpectedly returned a completed response")
            except (OSError, http.client.HTTPException):
                pass
        wait_for(lambda: fixture.health()["ready"], "readiness after watchdog restart")
        assert int(properties(unit)["NRestarts"]) >= 1
        assert request(fixture.port)[0] == 200
        journal = command("journalctl", "-u", unit, "--no-pager", "-o", "cat").stdout
        version_lines = command(args.server, "--version").stdout.splitlines()
        assert set(version_lines) <= set(journal.splitlines()), journal
        assert "watchdog" in journal.lower()
        assert ID not in journal
        (root / "service-journal.log").write_text(journal)
        # A consistent backup survives an offline restore and another service start.
        backup_path = root / "backup.sqlite"
        with sqlite3.connect(backup_path) as backup:
            database.backup(backup)
            assert backup.execute("PRAGMA quick_check").fetchone()[0] == "ok"
            assert backup.execute("PRAGMA foreign_key_check").fetchall() == []
        backup_path.chmod(0o600)
        database.close()
        command("systemctl", "stop", unit)
        original = fixture.state / "census.sqlite"
        owner = original.stat()
        original.rename(fixture.state / "census.before-restore.sqlite")
        shutil.copyfile(backup_path, original)
        original.chmod(0o600)
        os.chown(original, owner.st_uid, owner.st_gid)
        command("systemctl", "start", unit)
        wait_for(lambda: fixture.health()["ready"], "readiness after backup restore")
        assert request(fixture.port)[0] == 200
        with fixture.database() as restored:
            assert restored.execute("PRAGMA quick_check").fetchone()[0] == "ok"
            assert restored.execute("SELECT count(*) FROM installation").fetchone()[0] == 1
        command("systemctl", "stop", unit)
        assert properties(unit)["ActiveState"] == "inactive"
        failed = False
        print("systemd readiness, idle watchdog, reload, watchdog restart, shutdown, and backup restore passed")
    finally:
        command("systemctl", "stop", unit, check=False)
        if failed:
            journal = command("journalctl", "-u", unit, "--no-pager", "-o", "cat", check=False).stdout
            (root / "service-journal.log").write_text(journal)
            print(f"Failure artifacts retained: {root}\n{journal[-8000:]}", flush=True)
        # The test owns this uniquely named unit and its generated directories.
        command("systemctl", "clean", "--what=state,logs", unit, check=False)
        unit_file.unlink(missing_ok=True)
        command("systemctl", "daemon-reload", check=False)
        command("systemctl", "reset-failed", unit, check=False)
        if not failed:
            shutil.rmtree(root)


if __name__ == "__main__":
    main()
