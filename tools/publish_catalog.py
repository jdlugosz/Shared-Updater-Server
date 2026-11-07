#!/usr/bin/env python3
# Shared Updater Server - tools/publish_catalog.py
# <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

"""Publish one committed flat TOML catalog through two reusable slots."""
import argparse
import fcntl
import hashlib
import ipaddress
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tempfile
import time
import tomllib
import urllib.request
from urllib.parse import urlsplit
import uuid


def run(command, **kwargs):
    try:
        return subprocess.run(command, check=True, timeout=45, **kwargs)
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode(errors="replace") if isinstance(error.stderr, bytes) else error.stderr
        raise RuntimeError(detail or str(error)) from error


def git(repo, *arguments):
    return run(["git", "-C", str(repo), *arguments], stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def documents(directory):
    return [tomllib.loads(path.read_text(encoding="utf-8")) for path in sorted(Path(directory).glob("*.toml"))]


def revision(directory):
    headers = [document["catalog"] for document in documents(directory) if "catalog" in document]
    if len(headers) != 1:
        raise ValueError("catalog must have exactly one header")
    return headers[0]["revision"]


def export_commit(repo, reference, subtree, destination, byte_limit, fragment_limit):
    subtree = PurePosixPath(subtree)
    if subtree.is_absolute() or ".." in subtree.parts:
        raise ValueError("catalog path must be a relative Git tree path")
    commit = git(repo, "rev-parse", "--verify", "--end-of-options", reference + "^{commit}").decode().strip()
    if not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", commit):
        raise ValueError("invalid resolved Git commit")
    tree = commit if str(subtree) == "." else commit + ":" + str(subtree)
    entries = git(repo, "ls-tree", "-z", "-l", tree).split(b"\0")
    selected = []
    total = 0
    for entry in filter(None, entries):
        metadata, filename = entry.split(b"\t", 1)
        mode, kind, object_id, size = metadata.split()
        name = os.fsdecode(filename)
        if Path(name).suffix != ".toml":
            continue
        if Path(name).name != name or name in (".", "..") or kind != b"blob" or mode not in (b"100644", b"100755"):
            raise ValueError("catalog fragments must be regular files directly in the selected tree")
        total += int(size)
        selected.append((name, object_id.decode(), int(size)))
        if total > byte_limit or len(selected) > fragment_limit:
            raise ValueError("committed catalog exceeds configured input limits")
    if not selected:
        raise ValueError("no committed TOML fragments")
    for name, object_id, expected_size in selected:
        path = destination / name
        with path.open("xb") as output:
            run(["git", "-C", str(repo), "cat-file", "blob", object_id], stdout=output, stderr=subprocess.PIPE)
            if output.tell() != expected_size:
                raise ValueError("Git object length changed")
            output.flush()
            os.fchmod(output.fileno(), 0o444)
            os.fsync(output.fileno())
    with (destination / "source-commit").open("x") as output:
        output.write(commit + "\n")
        output.flush()
        os.fchmod(output.fileno(), 0o444)
        os.fsync(output.fileno())
    sync_directory(destination)
    return commit


def artifacts(directory):
    result = set()
    for document in documents(directory):
        for build in document.get("build", []):
            for artifact in build.get("artifact", []):
                result.add((artifact["url"], artifact["size"], artifact["sha256"].lower()))
    return result


class HttpsRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        if urlsplit(newurl).scheme != "https":
            raise ValueError("artifact redirect must retain HTTPS")
        return super().redirect_request(request, fp, code, message, headers, newurl)


def verify_downloads(new, old):
    opener = urllib.request.build_opener(HttpsRedirects())
    for url, size, digest in sorted(artifacts(new) - (artifacts(old) if old else set())):
        deadline = time.monotonic() + 600
        actual = 0
        checksum = hashlib.sha256()
        with opener.open(url, timeout=15) as response:
            if urlsplit(response.geturl()).scheme != "https":
                raise ValueError("artifact is not HTTPS")
            while chunk := response.read(65536):
                actual += len(chunk)
                if actual > size or time.monotonic() >= deadline:
                    raise ValueError("artifact exceeds its length or download deadline")
                checksum.update(chunk)
        if actual != size or checksum.hexdigest() != digest:
            raise ValueError("artifact length or SHA-256 does not match the catalog")


def health(url):
    # Local supervision must not use an inherited Internet proxy.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url, timeout=2) as response:
        result = json.load(response)
    if not result.get("ready"):
        raise ValueError("service is not ready")
    return result


def reload_systemd(service):
    run(["systemctl", "reload", service], stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def await_activation(url, expected_revision, after, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = health(url)
        if status["reload_generation"] > after and status["reload"] in ("accepted", "rejected"):
            if status["reload"] != "accepted" or status["revision"] != expected_revision:
                raise ValueError("service rejected the catalog or activated an unexpected revision")
            return status
        time.sleep(0.05)
    raise TimeoutError("catalog activation deadline exceeded")


def select(current, target):
    temporary = current.parent / (".current-" + uuid.uuid4().hex)
    try:
        temporary.symlink_to(target.name)
        os.replace(temporary, current)
        sync_directory(current.parent)
    finally:
        temporary.unlink(missing_ok=True)


def publish(options, reload_service=reload_systemd):
    if os.name != "posix":
        raise ValueError("publication runs locally on the Ubuntu server")
    config_path = Path(options.config).resolve()
    with config_path.open("rb") as source:
        config = tomllib.load(source)
    if "catalog_dir" not in config or "catalog_files" in config:
        raise ValueError("slot publication requires catalog_dir")
    configured = Path(config["catalog_dir"])
    current = configured if configured.is_absolute() else config_path.parent / configured
    if current.name != "current":
        raise ValueError("catalog_dir must name the publication root's current symlink")
    current.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    root = current.parent.resolve()
    current = root / "current"
    owner = root.stat()
    if owner.st_uid != os.geteuid() or owner.st_mode & 0o022:
        raise ValueError("publication root must be owned by the publisher and not writable by others")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.@-]*", options.service):
        raise ValueError("invalid service name")
    limits = config.get("limits", {})
    byte_limit = limits.get("catalog_bytes", 8 * 1024 * 1024)
    fragment_limit = limits.get("catalog_fragments", 4096)
    if type(byte_limit) is not int or type(fragment_limit) is not int or min(byte_limit, fragment_limit) <= 0:
        raise ValueError("invalid catalog resource limits")
    url = options.health_url or "http://" + config.get("listen", "127.0.0.1:8080") + "/healthz"
    parsed = urlsplit(url)
    if parsed.scheme != "http" or parsed.path != "/healthz" or parsed.query or parsed.fragment or parsed.username:
        raise ValueError("health URL must be the plain loopback health endpoint")
    if not ipaddress.ip_address(parsed.hostname).is_loopback or not parsed.port:
        raise ValueError("health URL must use a loopback address and a nonzero port")
    lock = os.open(root / "publish.lock", os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    stage = None
    try:
        locked = os.fstat(lock)
        if not stat.S_ISREG(locked.st_mode) or locked.st_nlink != 1:
            raise ValueError("publication lock is not a regular file")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        slots = {root / "slot-a", root / "slot-b"}
        previous = None
        if current.is_symlink():
            previous = current.resolve(strict=True)
            if previous not in slots or previous.is_symlink() or not previous.is_dir():
                raise ValueError("current must select a normal slot in this publication root")
        elif current.exists():
            raise ValueError("current must be a symlink")
        if options.initial != (previous is None):
            raise ValueError("use --initial only for the first publication without an existing current selection")
        old = None if options.initial else health(url)
        if old and old["revision"] != revision(previous):
            raise ValueError("current files do not match the running catalog")
        if previous:
            run([options.server, "--config", str(config_path), "--catalog-dir", str(previous), "--check-config"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stage = Path(tempfile.mkdtemp(prefix=".candidate-", dir=root))
        stage.chmod(0o755)
        commit = export_commit(Path(options.repo).resolve(), options.ref, options.catalog_path,
                               stage, byte_limit, fragment_limit)
        run([options.server, "--config", str(config_path), "--catalog-dir", str(stage), "--check-config"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if not options.skip_download_verification:
            verify_downloads(stage, previous)
        new_revision = revision(stage)
        inactive = root / ("slot-b" if previous == root / "slot-a" else "slot-a")
        # The only recursive removal is the verified inactive slot, never current.
        if inactive.is_symlink() or inactive.resolve() != inactive or inactive not in slots:
            raise ValueError("inactive slot path is unsafe")
        if inactive.exists():
            if not inactive.is_dir():
                raise ValueError("inactive slot is not a directory")
            shutil.rmtree(inactive)
        stage.rename(inactive)
        stage = None
        sync_directory(root)
        try:
            select(current, inactive)
            if not options.initial:
                reload_service(options.service)
                timeout = config.get("timeouts", {}).get("reload_ms", 30000) / 1000 + 5
                await_activation(url, new_revision, old["reload_generation"], timeout)
        except BaseException as original:
            if previous:
                select(current, previous)
                try:
                    before = health(url)["reload_generation"]
                    reload_service(options.service)
                    await_activation(url, old["revision"], before, 35)
                except BaseException as rollback:
                    raise RuntimeError("previous disk selection restored; running revision could not be confirmed") from rollback
            elif current.is_symlink() and current.resolve() == inactive:
                current.unlink()
                sync_directory(root)
            raise RuntimeError("publication failed; previous selection restored") from original
        return {"commit": commit, "revision": new_revision, "slot": inactive.name, "activated": not options.initial}
    finally:
        if stage:
            shutil.rmtree(stage)
        os.close(lock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--ref", default="HEAD")
    parser.add_argument("--catalog-path", default="catalog")
    parser.add_argument("--config", required=True)
    parser.add_argument("--server", default="/usr/local/libexec/shared-updater-server")
    parser.add_argument("--service", default="shared-updater-server.service")
    parser.add_argument("--health-url")
    parser.add_argument("--initial", action="store_true", help="First selection only; do not reload or start the service")
    parser.add_argument("--skip-download-verification", action="store_true",
                        help="Fixture/offline publication when artifact bytes have been verified elsewhere")
    print(json.dumps(publish(parser.parse_args()), indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, TimeoutError) as error:
        raise SystemExit(f"Error: {error}")
