#!/usr/bin/env python3
# Shared Updater Server - tools/catalog_report.py
# <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

"""Read the active publication and write a standalone operator report to stdout."""

import argparse
from collections import defaultdict
from datetime import datetime, timezone
import fcntl
from html import escape
import json
import os
from pathlib import Path
import re
import stat
import sys
import time
import tomllib
from urllib.parse import urlsplit
from urllib.request import ProxyHandler, build_opener


PUBLICATION_ROOT = Path("/etc/shared-updater-server/catalogs")
HEALTH_URL = "http://127.0.0.1:8080/healthz"
MAX_CATALOG_BYTES = 8 * 1024 * 1024
MAX_FRAGMENTS = 4096


def read_regular(path, limit):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise ValueError(f"expected a regular file: {path.name}")
        data = source.read(limit + 1)
    if len(data) > limit:
        raise ValueError("published catalog exceeds the report's input limits")
    return data


def loopback_health():
    opener = build_opener(ProxyHandler({}))
    with opener.open(HEALTH_URL, timeout=2) as response:
        return json.loads(response.read(4097))


def snapshot(root, read_health=loopback_health, include_target_ids=False):
    root = root.resolve(strict=True)
    fd = os.open(root / "publish.lock", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as lock:
        if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode):
            raise ValueError("expected an existing publication lock file")
        deadline = time.monotonic() + 5
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise ValueError("publication is busy; refresh the report after it completes") from None
                time.sleep(0.05)

        before = read_health()
        current = root / "current"
        slot = current.resolve(strict=True)
        if not current.is_symlink() or slot not in (root / "slot-a", root / "slot-b") or not slot.is_dir():
            raise ValueError("current does not select a published slot")
        commit = read_regular(slot / "source-commit", 65).decode("ascii").strip()
        if not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", commit):
            raise ValueError("invalid published source commit")
        paths = sorted(slot.glob("*.toml"))
        if not 0 < len(paths) <= MAX_FRAGMENTS:
            raise ValueError("unexpected number of published catalog fragments")
        remaining = MAX_CATALOG_BYTES
        fragments = []
        for path in paths:
            data = read_regular(path, remaining)
            fragments.append(data)
            remaining -= len(data)
        after = read_health()
        captured = datetime.now(timezone.utc)

    documents = [tomllib.loads(data.decode("utf-8")) for data in fragments]
    headers = [document["catalog"] for document in documents if "catalog" in document]
    if len(headers) != 1:
        raise ValueError("expected exactly one catalog header")
    revision = headers[0]["revision"]
    if any(status.get("ready") is not True or status.get("revision") != revision for status in (before, after)):
        raise ValueError("published catalog does not match the ready service revision")
    if type(before.get("reload_generation")) is not int or before["reload_generation"] != after.get("reload_generation"):
        raise ValueError("service reloaded during the snapshot; refresh the report")

    for document in documents:
        for track in document.get("track", []):
            track["targeted"] = "installation_id" in track
            if not include_target_ids:
                track.pop("installation_id", None)
    return {"revision": revision, "slot": slot.name, "source_commit": commit, "captured_at": captured}, documents


def text(value):
    return escape(str(value))


def code(value):
    return f"<code>{text(value)}</code>"


def timestamp(value):
    utc = value.astimezone(timezone.utc)
    iso = utc.isoformat(timespec="seconds").replace("+00:00", "Z")
    return f'<time datetime="{iso}" title="{iso}">{utc:%Y-%m-%d %H:%M:%S} UTC</time>'


def link(url, label):
    try:
        parsed = urlsplit(url)
        if parsed.scheme == "https" and parsed.hostname and not parsed.username and not parsed.password:
            return f'<a href="{text(url)}" rel="noreferrer">{text(label)}</a>'
    except ValueError:
        pass
    return text(url)


def definition_list(items):
    return "<dl>" + "".join(f"<div><dt>{text(label)}</dt><dd>{value}</dd></div>" for label, value in items) + "</dl>"


def table(title, headings, rows):
    columns = "".join(f'<th scope="col">{text(heading)}</th>' for heading in headings)
    body = "".join("<tr>" + "".join(f"<td>{cell}</td>" for cell in row) + "</tr>" for row in rows)
    if not body:
        body = f'<tr><td colspan="{len(headings)}">No entries.</td></tr>'
    return (f'<section><h2>{text(title)} <small>{len(rows)}</small></h2><div class="scroll">'
            f'<table aria-label="{text(title)}"><thead><tr>{columns}</tr></thead><tbody>{body}</tbody></table></div></section>')


def artifact_details(build, artifact):
    fields = [("Media type", code(artifact["media_type"])), ("SHA-256", code(artifact["sha256"]))]
    if requirements := build.get("requires"):
        fields.append(("Requirements", ", ".join(map(code, requirements))))
    for extension in build.get("extension", []):
        label = f'{extension["namespace"]} / {extension["name"]}'
        if suite := extension.get("suite"):
            label += f" ({suite})"
        fields.append((label, text(extension["text"])))
    return f'<details><summary>Details</summary>{definition_list(fields)}</details>'


def render_report(metadata, documents):
    def records(key):
        return [row for document in documents for row in document.get(key, [])]

    header = next(document["catalog"] for document in documents if "catalog" in document)
    suites = sorted(records("suite"), key=lambda row: row["id"])
    packages = sorted(records("package"), key=lambda row: row["id"])
    titles = {package["id"]: package["title"] for package in packages}
    builds = sorted(records("build"), key=lambda row: row["published_at"], reverse=True)
    tracks = sorted(records("track"), key=lambda row: (row["suite"], row["package"], row["name"], row["platform"]))
    assignments = defaultdict(set)
    for track in tracks:
        suffix = " (targeted)" if track["targeted"] else ""
        assignments[track["package"], track["build"], track["platform"]].add(f'{track["name"]} ({track["suite"]}){suffix}')

    artifact_rows = []
    for build in builds:
        for artifact in sorted(build["artifact"], key=lambda row: row["platform"]):
            labels = assignments[build["package"], build["id"], artifact["platform"]]
            artifact_rows.append([
                text(titles[build["package"]]), f'{text(build["version"])}<br>{code(build["id"])}',
                code(artifact["platform"]), ", ".join(map(text, sorted(labels))) or "&mdash;",
                timestamp(build["published_at"]), text(build.get("visibility", "public")), f'{artifact["size"]:,}',
                f'{link(artifact["url"], "Download")} &middot; {link(build["about_url"], "Info")}',
                artifact_details(build, artifact),
            ])

    summary = definition_list([
        ("Revision", code(header["revision"])), ("Catalog published", timestamp(header["published_at"])),
        ("Snapshot captured", timestamp(metadata["captured_at"])), ("Schema", text(header["schema_version"])),
        ("Publisher", text(header["author"])), ("Source commit", code(metadata["source_commit"])),
    ])
    artifacts = table("Builds and artifacts", ["Package", "Version and build", "Platform", "Tracks", "Published",
                      "Visibility", "Bytes", "Links", "Metadata"], artifact_rows)
    track_table = table("Track assignments", ["Suite", "Package", "Track", "Platform", "Build", "Audience"], [
        [code(row[key]) for key in ("suite", "package", "name", "platform", "build")] +
        (["Public"] if not row["targeted"] else [code(row["installation_id"]) if "installation_id" in row else "Targeted (ID hidden)"])
        for row in tracks
    ])
    suite_table = table("Suites", ["ID", "Title", "Packages"], [
        [code(row["id"]), text(row["title"]), ", ".join(map(code, row["packages"]))] for row in suites
    ])
    package_table = table("Packages", ["ID", "Title", "Kind"], [
        [code(row["id"]), text(row["title"]), text(row["kind"])] for row in packages
    ])
    return f'''<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; base-uri 'none'; form-action 'none'">
<title>Live updater catalog</title>
<style>
:root {{ color-scheme: light dark; font: 15px/1.5 system-ui, sans-serif; }}
body {{ margin: 0; color: CanvasText; background: color-mix(in srgb, Canvas 96%, CanvasText); }}
main {{ max-width: 95rem; margin: auto; padding: clamp(1rem, 3vw, 3rem); }}
h1, h2, dl, p {{ margin: 0; }}
h1 {{ font-size: 1.9rem; }}
h2 {{ font-size: 1.15rem; padding: .8rem 1rem; }}
header > p {{ color: GrayText; margin: .3rem 0 1rem; }}
header dl {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(13rem, 1fr)); gap: .75rem; }}
header dl > div, section {{ background: Canvas; border: 1px solid color-mix(in srgb, CanvasText 18%, Canvas); border-radius: .6rem; }}
header dl > div {{ padding: .65rem .85rem; }}
dt, small {{ color: GrayText; }}
dd {{ margin: 0; overflow-wrap: anywhere; }}
section {{ margin-top: 1rem; min-width: 0; }}
h2 small {{ font-weight: normal; margin-left: .35rem; }}
.scroll {{ overflow: auto; }}
table {{ width: 100%; border-collapse: collapse; text-align: left; }}
th, td {{ padding: .55rem .75rem; border-top: 1px solid color-mix(in srgb, CanvasText 12%, Canvas); vertical-align: top; }}
th {{ background: color-mix(in srgb, Canvas 94%, CanvasText); white-space: nowrap; }}
time {{ white-space: nowrap; }}
summary {{ cursor: pointer; color: LinkText; }}
details dl {{ min-width: 12rem; margin-top: .4rem; }}
details dt {{ overflow-wrap: anywhere; }}
details dd {{ margin-bottom: .4rem; white-space: pre-wrap; }}
code {{ font-size: .87em; }}
.structure {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 25rem), 1fr)); gap: 1rem; }}
</style>
<main>
<header><h1>Live updater catalog</h1><p>Verified snapshot of active {code(metadata["slot"])}. Rerun the viewer to refresh.</p>{summary}</header>
{artifacts}
{track_table}
<div class="structure">{suite_table}{package_table}</div>
</main>
<script>
const dateFormat = new Intl.DateTimeFormat(undefined, {{dateStyle: 'medium', timeStyle: 'long'}});
for (const time of document.querySelectorAll('time')) time.textContent = dateFormat.format(new Date(time.dateTime));
</script>
</html>'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--include-target-ids", action="store_true", help="show UUIDs in the private report")
    args = parser.parse_args()
    try:
        metadata, documents = snapshot(PUBLICATION_ROOT, include_target_ids=args.include_target_ids)
        report = render_report(metadata, documents)
    except (OSError, ValueError, KeyError) as error:
        print(f"Could not read the live catalog: {error}", file=sys.stderr)
        return 1
    # Keep the SSH output portable across Windows terminal encodings.
    print(report.encode("ascii", "xmlcharrefreplace").decode("ascii"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
