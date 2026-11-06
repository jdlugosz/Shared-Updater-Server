#!/usr/bin/env python3
"""Exercise the built daemon through real TCP, real SQLite, and optionally Caddy."""
import argparse
import concurrent.futures
import hashlib
import importlib.util
import http.client as http_client
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import sqlite3
import ssl
import struct
import subprocess
import tempfile
import time
import uuid
import xml.etree.ElementTree as ET
from unittest.mock import patch

NIL = "00000000-0000-0000-0000-000000000000"
ID = "12345678-1234-4234-8234-1234567890ab"
TARGET = "abcdef01-1234-4234-8234-1234567890ab"
ATOM = "{http://www.w3.org/2005/Atom}"
UPD = "{https://dlugosz.enterprises/ns/updater/1}"


def wait_for(predicate, description, timeout=8):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except (OSError, http_client.HTTPException, json.JSONDecodeError) as error:
            last = error
        time.sleep(0.01)
    raise AssertionError(f"Timed out: {description}; last error: {last}")


def fetch(port, target, method="GET", body=None, headers=None, context=None):
    kind = http_client.HTTPSConnection if context else http_client.HTTPConnection
    options = dict(timeout=4)
    if context:
        options["context"] = context
    connection = kind("127.0.0.1", port, **options)
    try:
        connection.request(method, target, body=body, headers=headers or {})
        response = connection.getresponse()
        data = response.read()
        return response.status, dict(response.getheaders()), data
    finally:
        connection.close()


def request(port, identity=ID, suite="Launcher", **kwargs):
    return fetch(port, f"/updchk/{identity}/{suite}", **kwargs)


def parsed(data):
    result = ET.fromstring(data)
    assert result.tag == ATOM + "feed"
    return result


class Process:
    def __init__(self, command, log, env=None):
        self.log = Path(log)
        self.output = self.log.open("wb")
        self.process = subprocess.Popen(command, stdout=self.output, stderr=subprocess.STDOUT, env=env)

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
                raise AssertionError(f"Process did not stop: {self.log}")
        self.output.close()
        assert self.process.returncode == 0, (self.process.returncode, self.log.read_text(errors="replace"))

    def cleanup(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=3)
        self.output.close()


class Fixture:
    def __init__(self, root, binary, *, artifact_url=None, limits="", times=""):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.binary = binary
        self.catalog = self.root / "catalog"
        self.catalog.mkdir()
        self.state = self.root / "state"
        self.logs = self.root / "logs"
        self.config = self.root / "server.toml"
        self.process = None
        self.payload = b"Shared updater integration artifact\n"
        self.url = artifact_url or "https://example.invalid/downloads/fixture.bin"
        self.config.write_text(f'''listen = "127.0.0.1:0"
catalog_dir = "catalog"
state_dir = "state"
log_dir = "logs"
publisher_id_base = "https://example.invalid/identities/"
[limits]
{limits}
[timeouts]
header_ms = 200
{times}
''')
        self.write_catalog("initial")

    def write_catalog(self, revision, invalid=False):
        digest = hashlib.sha256(self.payload).hexdigest()
        build = "missing" if invalid else "stable"
        content = f'''[catalog]
schema_version = 1
revision = "{revision}"
published_at = 2026-11-05T12:00:00Z
author = "Długosz"
[[suite]]
id = "launcher"
title = "Launcher & tools"
packages = ["app"]
[[suite]]
id = "other"
title = "Other"
packages = ["app"]
[[package]]
id = "app"
title = "Launcher"
kind = "installer"
[[build]]
package = "app"
id = "stable"
version = "1.0"
published_at = 2026-11-04T12:00:00Z
about_url = "https://example.invalid/notes?a=1&b=2"
[[build.artifact]]
platform = "any"
url = "{self.url}"
media_type = "application/octet-stream"
size = {len(self.payload)}
sha256 = "{digest}"
[[track]]
suite = "launcher"
package = "app"
name = "current"
platform = "any"
build = "{build}"
'''
        temporary = self.catalog / "candidate.tmp"
        temporary.write_text(content, encoding="utf-8")
        temporary.replace(self.catalog / "catalog.toml")

    def command(self, *extra):
        return [self.binary, "--config", str(self.config), *extra]

    def start(self):
        self.process = Process(self.command(), self.root / "daemon.log")

        def port():
            assert self.process.process.poll() is None, self.process.log.read_text(errors="replace")
            match = re.search(r"Listening on 127\.0\.0\.1:(\d+)", self.process.log.read_text(errors="replace"))
            return int(match[1]) if match else None

        self.port = wait_for(port, "listener startup")
        wait_for(lambda: self.health()["ready"], "readiness")
        return self

    def health(self):
        status, _, body = fetch(self.port, "/healthz")
        assert status in (200, 503)
        return json.loads(body)

    def stop(self):
        if self.process:
            self.process.stop()
            self.process = None

    def cleanup(self):
        if self.process:
            self.process.cleanup()

    def database(self):
        return sqlite3.connect(self.state / "census.sqlite", timeout=2)

    def contacts(self):
        result = []
        for name in ("previous.jsonl", "current.jsonl"):
            path = self.logs / name
            if path.exists():
                assert path.stat().st_size <= 1024 * 1024
                result.extend(json.loads(line) for line in path.read_text().splitlines())
        return result


def offline_and_http(root, binary):
    fixture = Fixture(root / "http", binary, times="reload_ms = 50")
    try:
        checked = subprocess.run(fixture.command("--check-config"), capture_output=True, text=True, timeout=8)
        assert checked.returncode == 0, checked.stderr
        assert re.fullmatch(r"Success\. 1/4096 fragments, \d+/8192 kiBytes read\.\n", checked.stdout)
        assert not fixture.state.exists() and not fixture.logs.exists()
        dumped = subprocess.run(fixture.command("--feed", "Launcher"), capture_output=True, timeout=8)
        assert dumped.returncode == 0, dumped.stderr
        parsed(dumped.stdout)
        assert not fixture.state.exists() and not fixture.logs.exists()
        fixture.start()
        initial_pid = fixture.process.process.pid
        status, headers, body = request(fixture.port)
        assert status == 200 and headers["Cache-Control"] == "no-store"
        assert headers["Content-Type"] == "application/atom+xml; charset=utf-8"
        assert parsed(body).findtext(UPD + "revision") == "initial"
        assert request(fixture.port)[0] == 200
        assert request(fixture.port, suite="Other")[0] == 200
        for _ in range(2):
            assert request(fixture.port, NIL)[0] == 200
        before = len(fixture.contacts())
        for identity, suite, code in (("bad", "Launcher", 400), (NIL, "missing", 404),
                                       (NIL, "1bad", 400), (NIL, "Launcher/extra", 400)):
            assert request(fixture.port, identity, suite)[0] == code
        assert fetch(fixture.port, f"/updchk/{ID}/Launcher?x=1")[0] == 400
        assert fetch(fixture.port, "/elsewhere")[0] == 404
        assert request(fixture.port, method="POST")[0] == 405
        assert request(fixture.port, method="HEAD")[2] == b""
        assert request(fixture.port, body=b"body")[0] == 400
        assert len(fixture.contacts()) == before
        identities = [str(uuid.uuid4()) for _ in range(16)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            statuses = list(pool.map(lambda identity: request(fixture.port, identity)[0], identities))
        assert statuses == [200] * 16
        with fixture.database() as database:
            assert database.execute("SELECT count(*) FROM installation").fetchone()[0] == 18
            assert database.execute("SELECT sum(request_count) FROM anonymous_day").fetchone()[0] == 2
            assert database.execute("PRAGMA quick_check").fetchone()[0] == "ok"
            assert database.execute("PRAGMA foreign_key_check").fetchall() == []
        duplicate = subprocess.run(fixture.command(), capture_output=True, text=True, timeout=8)
        assert duplicate.returncode != 0 and "another daemon" in duplicate.stderr
        assert fixture.process.process.pid == initial_pid
        generation = fixture.health()["reload_generation"]
        fixture.write_catalog("bad", invalid=True)
        fixture.process.process.send_signal(signal.SIGHUP)
        wait_for(lambda: fixture.health()["reload_generation"] > generation and fixture.health()["reload"] == "rejected",
                 "invalid reload rejection")
        assert parsed(request(fixture.port)[2]).findtext(UPD + "revision") == "initial"
        generation = fixture.health()["reload_generation"]
        fixture.write_catalog("accepted")
        fixture.process.process.send_signal(signal.SIGHUP)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            feeds = list(pool.map(lambda _: request(fixture.port), range(24)))
        assert all(code == 200 and parsed(data).findtext(UPD + "revision") in ("initial", "accepted")
                   for code, _, data in feeds)
        wait_for(lambda: fixture.health()["reload_generation"] > generation and fixture.health()["reload"] == "accepted",
                 "valid reload activation")
        assert fixture.health()["revision"] == "accepted"
        assert fixture.process.process.pid == initial_pid
        # Read timeout closes a partially transmitted header without census effects.
        with socket.create_connection(("127.0.0.1", fixture.port), timeout=2) as client:
            client.sendall(b"GET /")
            assert client.recv(1) == b""
        with socket.create_connection(("127.0.0.1", fixture.port), timeout=2) as client:
            client.sendall(f"HEAD /updchk/{ID}/Launcher HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
            response = b""
            while chunk := client.recv(8192):
                response += chunk
            assert b"405" in response.split(b"\r\n", 1)[0]
            assert response.split(b"\r\n\r\n", 1)[1] == b""
        fixture.stop()
        assert ID not in (fixture.root / "daemon.log").read_text()
        # A restart reconstructs the accepted catalog and preserves census state.
        fixture.start()
        assert fixture.health()["revision"] == "accepted"
        with fixture.database() as database:
            assert database.execute("SELECT count(*) FROM installation").fetchone()[0] == 18
        fixture.stop()
    finally:
        fixture.cleanup()


def bounded_queue(root, binary):
    fixture = Fixture(root / "queue", binary, limits="unfinished_requests = 2",
                      times="queue_ms = 200\nsqlite_busy_ms = 2000")
    try:
        fixture.start()
        database = fixture.database()
        database.execute("BEGIN IMMEDIATE")
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            active = pool.submit(request, fixture.port)
            wait_for(lambda: fixture.health()["phase"] == "persisting", "blocked census write")
            queued = pool.submit(request, fixture.port, str(uuid.uuid4()))
            wait_for(lambda: fixture.health()["queued"] == 1, "queued second check")
            code, headers, _ = request(fixture.port, str(uuid.uuid4()))
            assert code == 503 and headers["Retry-After"] == "30"
            assert queued.result(timeout=3)[0] == 503
            database.rollback()
            assert active.result(timeout=3)[0] == 200
        wait_for(lambda: fixture.health()["unfinished"] == 0, "released admission reservations")
        assert len(fixture.contacts()) == 1
        assert database.execute("SELECT count(*) FROM installation").fetchone()[0] == 1
        # A reset during an already-started transaction must not abandon it.
        database.execute("BEGIN IMMEDIATE")
        reset_id = str(uuid.uuid4())
        client = socket.create_connection(("127.0.0.1", fixture.port), timeout=2)
        client.sendall(f"GET /updchk/{reset_id}/Launcher HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
        wait_for(lambda: fixture.health()["phase"] == "persisting", "second blocked write")
        client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        client.close()
        database.rollback()
        wait_for(lambda: fixture.health()["unfinished"] == 0, "reset completion cleanup")
        assert database.execute("SELECT count(*) FROM installation").fetchone()[0] == 2
        database.close()
        fixture.stop()
    finally:
        fixture.cleanup()


def proxy(root, binary, caddy):
    assert caddy, "Caddy executable is required"
    with socket.socket() as reserved:
        reserved.bind(("127.0.0.1", 0))
        port = reserved.getsockname()[1]
    fixture = Fixture(root / "proxy", binary,
                      artifact_url=f"https://127.0.0.1:{port}/downloads/fixture.bin")
    caddy_process = None
    try:
        fixture.start()
        site = fixture.root / "site" / "downloads"
        site.mkdir(parents=True)
        (site / "fixture.bin").write_bytes(fixture.payload)
        certificate = fixture.root / "certificate.pem"
        key = fixture.root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
                        "-days", "1", "-keyout", str(key), "-out", str(certificate),
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost"],
                       capture_output=True, check=True, timeout=15)
        key.chmod(0o600)
        caddyfile = fixture.root / "Caddyfile"
        caddyfile.write_text(f'''{{
    admin off
    auto_https off
    log default {{
        exclude http.log.error.updater
    }}
    log updater_runtime {{
        include http.log.error.updater
        output stderr
        format filter {{
            wrap json
            fields {{
                request delete
            }}
        }}
    }}
}}
https://127.0.0.1:{port} {{
    tls "{certificate}" "{key}"
    log updater
    log_skip /updchk/*
    handle /updchk/* {{
        reverse_proxy 127.0.0.1:{fixture.port} {{
            lb_retries 0
            transport http {{
                versions 1.1
                dial_timeout 2s
                response_header_timeout 25s
            }}
        }}
    }}
    handle /downloads/* {{
        root * "{site.parent}"
        file_server
    }}
    handle {{
        respond "Not found" 404
    }}
}}
''')
        env = os.environ.copy()
        env["XDG_DATA_HOME"] = str(fixture.root / "caddy-data")
        env["XDG_CONFIG_HOME"] = str(fixture.root / "caddy-config")
        validated = subprocess.run([caddy, "validate", "--config", str(caddyfile), "--adapter", "caddyfile"],
                                   capture_output=True, env=env, timeout=15)
        assert validated.returncode == 0, validated.stderr.decode()
        caddy_process = Process([caddy, "run", "--config", str(caddyfile), "--adapter", "caddyfile"],
                                fixture.root / "caddy.log", env)
        context = ssl.create_default_context(cafile=str(certificate))
        wait_for(lambda: fetch(port, "/downloads/fixture.bin", context=context)[0] == 200, "TLS Caddy startup")
        status, headers, body = request(port, context=context)
        assert status == 200 and headers["Cache-Control"] == "no-store"
        feed = parsed(body)
        links = feed.find(ATOM + "entry").findall(ATOM + "link")
        enclosure = next(link for link in links if link.attrib["rel"] == "enclosure")
        assert enclosure.attrib["href"] == fixture.url
        status, _, artifact = fetch(port, "/downloads/fixture.bin", context=context)
        assert status == 200 and artifact == fixture.payload
        assert int(enclosure.attrib["length"]) == len(artifact)
        assert hashlib.sha256(artifact).hexdigest() == feed.find(ATOM + "entry").findtext(UPD + "sha256")
        publisher_path = Path(__file__).resolve().parents[1] / "tools" / "publish_catalog.py"
        spec = importlib.util.spec_from_file_location("publisher_download_test", publisher_path)
        publisher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(publisher)
        with patch.dict(os.environ, {"SSL_CERT_FILE": str(certificate)}):
            publisher.verify_downloads(fixture.catalog, None)
            wrong = fixture.root / "wrong-checksum"
            wrong.mkdir()
            (wrong / "catalog.toml").write_text((fixture.catalog / "catalog.toml").read_text().replace(
                hashlib.sha256(fixture.payload).hexdigest(), "0" * 64))
            try:
                publisher.verify_downloads(wrong, None)
                raise AssertionError("mismatched artifact checksum was accepted")
            except ValueError:
                pass
        assert request(port, context=context)[0] == 200
        assert request(port, NIL, context=context)[0] == 200
        assert fetch(port, "/healthz", context=context)[0] == 404
        identities = [str(uuid.uuid4()) for _ in range(8)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            assert list(pool.map(lambda identity: request(port, identity, context=context)[0], identities)) == [200] * 8
        with fixture.database() as database:
            assert database.execute("SELECT count(*) FROM installation").fetchone()[0] == 9
            assert database.execute("SELECT sum(request_count) FROM anonymous_day").fetchone()[0] == 1
        fixture.stop()
        assert request(port, context=context)[0] == 502
        caddy_process.stop()
        caddy_process = None
        for log in (fixture.root / "daemon.log", fixture.root / "caddy.log"):
            text = log.read_text(errors="replace")
            for identity in [ID, NIL, *identities]:
                assert identity not in text, (identity, log)
        assert len(fixture.contacts()) == 11
    finally:
        if caddy_process:
            caddy_process.cleanup()
        fixture.cleanup()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--mode", choices=("http", "proxy"), default="http")
    parser.add_argument("--caddy")
    args = parser.parse_args()
    results = Path(args.server).parent / "test-results"
    results.mkdir(exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=f"{args.mode}-", dir=results))
    try:
        if args.mode == "http":
            offline_and_http(root, args.server)
            bounded_queue(root, args.server)
        else:
            proxy(root, args.server, args.caddy)
        print(f"{args.mode} integration passed")
    except BaseException:
        print(f"Failure artifacts retained: {root}", flush=True)
        for log in root.rglob("*.log"):
            print(f"--- {log}\n{log.read_text(errors='replace')[-6000:]}", flush=True)
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
