#!/usr/bin/env python3
# Shared Updater Server - tests/catalog_report_integration.py
# <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

"""Check the catalog viewer against a private, synthetic published slot."""

from html import escape
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from catalog_report import render_report, snapshot


REVISION = "report-fixture"
COMMIT = "4" * 40
TARGET = "aabbccdd-1234-4000-8000-123456789abc"
EXTENSION = "<img src=x onerror=alert(1)>"


class CatalogReportTests(unittest.TestCase):
    def setUp(self):
        temporary = TemporaryDirectory(prefix="updater-catalog-report-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        slot = self.root / "slot-a"
        slot.mkdir()
        (self.root / "current").symlink_to(slot.name, target_is_directory=True)
        (self.root / "publish.lock").touch()
        (slot / "source-commit").write_text(COMMIT + "\n", encoding="ascii")
        self.fragment = slot / "catalog.toml"
        self.fragment.write_text(f'''[catalog]
schema_version = 1
revision = "{REVISION}"
published_at = 2026-11-05T12:00:00Z
author = "Długosz"

[[suite]]
id = "launcher"
title = "Launcher"
packages = ["launcher"]

[[package]]
id = "launcher"
title = "Launcher"
kind = "installer"

[[build]]
package = "launcher"
id = "fixture-build"
version = "1.2.3"
published_at = 2026-11-05T11:00:00Z
about_url = "https://example.invalid/launcher"
visibility = "targeted"

[[build.artifact]]
platform = "windows-x64"
url = "https://example.invalid/downloads/launcher.zip"
media_type = "application/zip"
size = 1234
sha256 = "{'0' * 64}"

[[build.extension]]
namespace = "https://example.invalid/metadata"
name = "notes"
text = "{EXTENSION}"

[[track]]
suite = "launcher"
package = "launcher"
name = "beta"
platform = "windows-x64"
build = "fixture-build"
installation_id = "{TARGET}"
''', encoding="utf-8")

    def health(self):
        return {"ready": True, "revision": REVISION, "reload_generation": 2}

    def test_snapshot_redacts_installation_id_unless_requested(self):
        metadata, documents = snapshot(self.root, self.health)
        self.assertEqual(metadata["slot"], "slot-a")
        self.assertEqual(metadata["revision"], REVISION)
        self.assertEqual(metadata["source_commit"], COMMIT)
        self.assertTrue(metadata["captured_at"])
        track, = documents[0]["track"]
        self.assertIs(track["targeted"], True)
        self.assertNotIn("installation_id", track)
        self.assertNotIn(TARGET, render_report(metadata, documents))
        self.assertIn(TARGET, self.fragment.read_text(encoding="utf-8"))

        metadata, documents = snapshot(self.root, self.health, include_target_ids=True)
        self.assertEqual(documents[0]["track"][0]["installation_id"], TARGET)
        self.assertIn(TARGET, render_report(metadata, documents))

    def test_report_escapes_untrusted_catalog_metadata(self):
        metadata, documents = snapshot(self.root, self.health)
        report = render_report(metadata, documents)
        self.assertIn(REVISION, report)
        self.assertIn("1.2.3", report)
        self.assertIn(escape(EXTENSION), report)
        self.assertNotIn(EXTENSION, report)

    def test_report_keeps_unsafe_urls_inert(self):
        metadata, documents = snapshot(self.root, self.health)
        url = "javascript:alert(1)"
        documents[0]["build"][0]["artifact"][0]["url"] = url
        report = render_report(metadata, documents)
        self.assertIn(url, report)
        self.assertNotIn(f'href="{url}"', report)
        self.assertIn('href="https://example.invalid/launcher"', report)

    def test_snapshot_rejects_inactive_revision(self):
        def health():
            return dict(self.health(), revision="another-revision")

        with self.assertRaises((RuntimeError, ValueError)):
            snapshot(self.root, health)

    def test_snapshot_rejects_a_reload_during_read(self):
        responses = iter((self.health(), dict(self.health(), reload_generation=3)))
        with self.assertRaises((RuntimeError, ValueError)):
            snapshot(self.root, lambda: next(responses))

    def test_snapshot_rejects_a_symlinked_fragment(self):
        outside = self.root / "outside.toml"
        outside.write_text("# Harmless TOML outside the published slot.\n", encoding="utf-8")
        (self.fragment.parent / "extra.toml").symlink_to(outside)
        with self.assertRaises((OSError, ValueError)):
            snapshot(self.root, self.health)


if __name__ == "__main__":
    unittest.main()
