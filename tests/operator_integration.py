#!/usr/bin/env python3
# Shared Updater Server - tests/operator_integration.py
# <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

"""Exercise operator tools against the real daemon and private synthetic state."""
import argparse
from argparse import Namespace
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile

from server_integration import Fixture, ID, NIL, request


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def operations(root, binary, tools):
    census = load(tools / "census.py", "census_tools")
    fixture = Fixture(root / "operations", binary)
    try:
        fixture.start()
        assert request(fixture.port)[0] == 200
        assert request(fixture.port)[0] == 200
        assert request(fixture.port, suite="other")[0] == 200
        assert request(fixture.port, NIL)[0] == 200
        backup_directory = fixture.root / "backups"
        backup_directory.mkdir(mode=0o700)
        backup = backup_directory / "census.sqlite"
        census.backup(fixture.state / "census.sqlite", backup)
        assert backup.stat().st_mode & 0o077 == 0
        try:
            census.backup(fixture.state / "census.sqlite", backup)
            raise AssertionError("existing backup was overwritten")
        except FileExistsError:
            pass
        with sqlite3.connect(backup) as connection:
            census.verify(connection)
            today = int(__import__("time").time()) // 86400
            reported = census.report(connection, today, today + 1)
            suites = {row["suite"]: row for row in reported["suites"]}
            assert suites["launcher"]["active_installations"] == 1
            assert suites["launcher"]["newly_observed"] == 1
            assert suites["launcher"]["anonymous_requests"] == 1
            assert suites["other"]["active_installations"] == 1
            assert census.cohort(connection, today, today + 1, today + 1, today + 2) == [
                {"suite": "launcher", "cohort_size": 1, "returned": 0},
                {"suite": "other", "cohort_size": 1, "returned": 0}]
            with sqlite3.connect(":memory:") as returned:
                connection.backup(returned)
                identity = bytes.fromhex("abcdef011234423482341234567890ab")
                returned.execute("INSERT INTO installation VALUES(?,?,?,?)",
                                 ("launcher", identity, (today - 10) * 86400, (today - 2) * 86400))
                returned.executemany("INSERT INTO installation_day VALUES(?,?,?)",
                                     [("launcher", identity, today - 10), ("launcher", identity, today - 2)])
                assert census.cohort(returned, today - 10, today - 9, today - 3, today - 1) == [
                    {"suite": "launcher", "cohort_size": 1, "returned": 1}]
        result = subprocess.run([sys.executable, str(tools / "census.py"), "report", str(backup)],
                                capture_output=True, text=True, timeout=8)
        assert result.returncode == 0, result.stderr
        assert json.loads(result.stdout)["schema_version"] == 1
        result = subprocess.run([sys.executable, str(tools / "census.py"), "check", str(backup)],
                                capture_output=True, text=True, timeout=8)
        assert result.returncode == 0, result.stderr
        fixture.stop()
        # Restore while stopped, then reconstruct the server from that backup.
        source = fixture.state / "census.sqlite"
        source.rename(fixture.state / "census.before-restore.sqlite")
        shutil.copyfile(backup, source)
        source.chmod(0o600)
        fixture.start()
        assert request(fixture.port)[0] == 200
        with fixture.database() as connection:
            assert connection.execute("SELECT count(*) FROM installation").fetchone()[0] == 2
        fixture.stop()
    finally:
        fixture.cleanup()


def publication(root, binary, tools):
    publisher = load(tools / "publish_catalog.py", "publisher")
    fixture = Fixture(root / "publication", binary)
    repository = fixture.root / "repository"
    repository.mkdir()
    (repository / "catalog").mkdir()

    def git(*arguments):
        return subprocess.run(["git", "-C", str(repository), "-c", "user.name=Updater Test",
            "-c", "user.email=updater@example.invalid", "-c", "core.hooksPath=/dev/null",
            "-c", "commit.gpgsign=false", *arguments], check=True, capture_output=True, timeout=8)

    def commit(revision, invalid=False, changed_artifact=False):
        fixture.write_catalog(revision, invalid=invalid)
        content = (fixture.catalog / "catalog.toml").read_text()
        if changed_artifact:
            content = content.replace(f"size = {len(fixture.payload)}", f"size = {len(fixture.payload) + 1}")
        (repository / "catalog" / "catalog.toml").write_text(content)
        git("add", "catalog")
        git("commit", "-qm", revision)
        return git("rev-parse", "HEAD").stdout.decode().strip()

    git("init", "-q", "--initial-branch=main")
    initial = commit("initial")
    fixture.config.write_text(fixture.config.read_text().replace('catalog_dir = "catalog"', 'catalog_dir = "catalogs/current"'))
    options = Namespace(repo=str(repository), ref="HEAD", catalog_path="catalog",
        config=str(fixture.config), server=binary, service="fixture.service",
        health_url="http://127.0.0.1:1/healthz", initial=True, skip_download_verification=True)
    try:
        first = publisher.publish(options)
        assert first["commit"] == initial and first["slot"] == "slot-a" and not first["activated"]
        fixture.start()
        options.initial = False
        options.health_url = f"http://127.0.0.1:{fixture.port}/healthz"
        reload_service = lambda service: fixture.process.process.send_signal(signal.SIGHUP)
        current = fixture.root / "catalogs" / "current"
        accepted = commit("accepted")
        result = publisher.publish(options, reload_service)
        assert result["commit"] == accepted and result["slot"] == "slot-b" and result["activated"]
        assert fixture.health()["revision"] == "accepted"
        assert (current / "source-commit").read_text().strip() == accepted
        old_target = os.readlink(current)
        (repository / "catalog" / "linked.toml").symlink_to("catalog.toml")
        git("add", "catalog")
        git("commit", "-qm", "reject symbolic fragment")
        try:
            publisher.publish(options, reload_service)
            raise AssertionError("symbolic catalog fragment was exported")
        except ValueError:
            pass
        assert os.readlink(current) == old_target
        (repository / "catalog" / "linked.toml").unlink()
        git("add", "catalog")
        git("commit", "-qm", "remove symbolic fragment")
        commit("invalid", invalid=True)
        try:
            publisher.publish(options, reload_service)
            raise AssertionError("invalid catalog was published")
        except RuntimeError:
            pass
        assert os.readlink(current) == old_target
        assert fixture.health()["revision"] == "accepted"
        commit("rewritten-artifact", changed_artifact=True)
        try:
            publisher.publish(options, reload_service)
            raise AssertionError("immutable artifact rewrite was activated")
        except RuntimeError as error:
            assert "previous selection restored" in str(error), error
        assert os.readlink(current) == old_target
        assert fixture.health()["revision"] == "accepted"
        assert fixture.health()["reload"] == "accepted"  # Restoration itself was acknowledged.
        assert (current / "source-commit").read_text().strip() == accepted
        fixture.stop()
        fixture.start()
        assert fixture.health()["revision"] == "accepted"
        assert request(fixture.port)[0] == 200
        fixture.stop()
    finally:
        fixture.cleanup()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--tools", required=True)
    parser.add_argument("--mode", choices=("operations", "publication"), required=True)
    args = parser.parse_args()
    results = Path(args.server).parent / "test-results"
    results.mkdir(exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=args.mode + "-", dir=results))
    try:
        (operations if args.mode == "operations" else publication)(root, args.server, Path(args.tools))
        print(args.mode + " integration passed")
    except BaseException:
        print(f"Failure artifacts retained: {root}", flush=True)
        for log in root.rglob("*.log"):
            print(f"--- {log}\n{log.read_text(errors='replace')[-5000:]}", flush=True)
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
