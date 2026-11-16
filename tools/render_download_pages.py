#!/usr/bin/env python3
# Shared Updater Server - see LICENCE.

"""Render the public download information pages for a validated catalog snapshot."""

import argparse
from html import escape
import json
from pathlib import Path
from string import Template
import tomllib
from urllib.parse import urlsplit


PAGES = Path(__file__).with_name("download-pages")
TRACKS = ("release", "beta", "development")
TRACK_COPY = {
    "release": "Recommended for everyday use.",
    "beta": "Available for evaluation.",
    "development": "Try the latest development changes.",
}


def catalog_view(directory):
    fragments = [tomllib.loads(path.read_text(encoding="utf-8")) for path in sorted(directory.glob("*.toml"))]
    if sum("catalog" in fragment for fragment in fragments) != 1:
        raise ValueError("expected one catalog header")
    builds = {
        (record["package"], record["id"]): record
        for fragment in fragments
        for record in fragment.get("build", [])
        if record.get("visibility", "public") == "public"
    }
    tracks = {}
    for fragment in fragments:
        for track in fragment.get("track", []):
            if (track["suite"] != "launcher" or track["platform"] != "windows-x64"
                    or track["name"] not in TRACKS or "installation_id" in track):
                continue
            build = builds[track["package"], track["build"]]
            artifact = next(item for item in build["artifact"] if item["platform"] == track["platform"])
            tracks[track["package"], track["name"]] = (build, artifact)
    about_paths = {urlsplit(build["about_url"]).path for build in builds.values()}
    return about_paths, tracks


def download_link(artifact, label):
    return f'<a class="download" href="{escape(artifact["url"])}">{escape(label)}</a>'


def launcher_tracks(tracks):
    sections = []
    for name in TRACKS:
        selected = tracks.get(("launcher", name))
        if selected is None:
            continue
        build, artifact = selected
        description = TRACK_COPY[name]
        if name == "beta" and ("launcher", "release") not in tracks:
            description = "Recommended for evaluation."
        sections.append(
            f'<h2>{name.capitalize()} {escape(build["version"])}</h2>\n'
            f'<p>{description}</p>\n'
            f'<p>{download_link(artifact, f"Download the {name} ZIP")} · '
            f'<a href="{escape(build["about_url"])}">About this build</a></p>'
        )
    return "\n\n".join(sections) or "<p>There is no current Launcher download for Windows x64.</p>"


def updater_download(tracks):
    for name in TRACKS:
        selected = tracks.get(("shared-updater", name))
        if selected is not None:
            break
    else:
        return "<p>There is no current Shared Updater Client download for Windows x64.</p>"
    build, artifact = selected
    version = build["version"]
    prerelease = "prerelease " if name != "release" else ""
    link = f'<p>{download_link(artifact, f"Download {prerelease}Setup {version} for Windows x64")}</p>'
    directory = urlsplit(artifact["url"]).path.rsplit("/", 2)[-2]
    if not directory.endswith("-prerelease"):
        return link
    return (
        '<p><strong>Prerelease for testing.</strong> This build uses the Długosz development signing '
        'certificate. Use it on a test PC configured to trust that certificate.</p>\n'
        f'{link}\n'
        '<p>For a new test PC, <a href="/downloads/shared-updater/1.0.8-prerelease/Dlugosz-Shared-Updater-Prerelease.cer">'
        'download the public signing certificate</a>. Confirm its SHA-256 fingerprint with the release '
        'operator before trusting it in your Windows test account.</p>'
    )


def monitored_update(path, about_paths):
    page_path = f"/downloads/{path}"
    if page_path not in about_paths and page_path.removesuffix("index.html") not in about_paths:
        return ""
    version = Path(path).parent.name
    return (
        '<p>For a monitored update, install the <a href="/downloads/shared-updater/">current Shared Updater Client</a> '
        'first. In Launcher, open <strong>About → Check now</strong>, '
        f'select Launcher {escape(version)} in the catalog, and choose <strong>Install…</strong>.</p>'
    )


def render(directory, output):
    about_paths, tracks = catalog_view(directory)
    template = Template((PAGES / "index.html.in").read_text(encoding="utf-8"))
    pages = json.loads((PAGES / "pages.json").read_text(encoding="utf-8"))
    for path, page in pages.items():
        parts = Path(path).parts
        if path not in ("index.html", "previous-versions/index.html") and (len(parts) not in (2, 3)
                or parts[0] not in ("launcher", "shared-updater")
                or parts[-1] != "index.html" or ".." in parts):
            raise ValueError(f"invalid public page path: {path}")
        content = "\n".join(page["body"])
        if "<!-- launcher-tracks -->" in content:
            content = content.replace("<!-- launcher-tracks -->", launcher_tracks(tracks))
        if "<!-- updater-download -->" in content:
            content = content.replace("<!-- updater-download -->", updater_download(tracks))
        if "<!-- monitored-update -->" in content:
            content = content.replace("<!-- monitored-update -->", monitored_update(path, about_paths))
        heading = page["heading"]
        html = template.substitute(
            title=escape(page.get("title", heading)),
            heading=escape(heading),
            description=escape(page.get("description", heading)),
            content="\n".join("    " + line if line else "" for line in content.splitlines()),
        )
        destination = output / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(html, encoding="utf-8", newline="\n")
    return len(pages)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="directory for the generated download tree")
    args = parser.parse_args()
    print(f"Rendered {render(args.catalog_dir, args.output)} download pages.")


if __name__ == "__main__":
    main()
