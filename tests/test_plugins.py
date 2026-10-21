"""Offline tests for the bundled Python plugin entry points and library operations."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock
from io import BytesIO

from scripts.package import stage_bundle
from scripts.prepare_dependencies import verify


PLUGINS = Path(__file__).resolve().parents[1] / "plugins"


class PluginTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="KiCad Forge plugin ")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.env = os.environ.copy()
        self.env.update({
            "PYTHONUTF8": "1",
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_AUTHOR_NAME": "KiCad Forge CI",
            "GIT_AUTHOR_EMAIL": "ci@example.invalid",
            "GIT_COMMITTER_NAME": "KiCad Forge CI",
            "GIT_COMMITTER_EMAIL": "ci@example.invalid",
        })

    def invoke(self, plugin, action, arguments=None, expected_exit=0):
        result = subprocess.run(
            [sys.executable, str(PLUGINS / plugin / "plugin.py"), action,
             json.dumps(arguments or {})], cwd=self.directory, env=self.env,
            capture_output=True, encoding="utf-8", timeout=30)
        self.assertEqual(result.returncode, expected_exit, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def test_manifest_info_entry_points(self):
        for plugin in ("git_version", "lcsc_import"):
            with self.subTest(plugin=plugin):
                manifest = json.loads((PLUGINS / plugin / "manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(self.invoke(plugin, "info")["id"], manifest["id"])

    def test_git_plugin_round_trip(self):
        library = self.directory / "test library.kicad_sym"
        library.write_text('(kicad_symbol_lib (version 20231120))\n', encoding="utf-8")
        args = {"target_library": str(library)}
        self.assertTrue(self.invoke("git_version", "init", args)["ok"])
        history = self.invoke("git_version", "history", args)
        self.assertTrue(history["ok"])
        self.assertIn("KF: initial commit", history["output"])
        library.write_text('(kicad_symbol_lib (version 20231120) (symbol "CI"))\n', encoding="utf-8")
        diff = self.invoke("git_version", "diff", args)
        self.assertTrue(diff["ok"])
        self.assertIn(library.name, diff["output"])
        self.assertTrue(self.invoke("git_version", "commit", {**args, "message": "CI update"})["ok"])
        self.assertIn("CI update", self.invoke("git_version", "history", args)["output"])
        self.assertIn("Working tree clean", self.invoke("git_version", "diff", args)["output"])

    def test_git_plugin_reports_missing_repository(self):
        result = self.invoke("git_version", "commit", {
            "target_library": str(self.directory / "not a repository.kicad_sym")})
        self.assertFalse(result["ok"])
        self.assertIn("No Git repository", result["error"])

    def test_lcsc_requires_host_staging_protocol_without_network(self):
        result = self.invoke("lcsc_import", "import", {"lcsc_id": "C37593"}, expected_exit=1)
        self.assertFalse(result["ok"])
        self.assertIn("staged-import-v1 staging_dir", result["error"])

    def test_lcsc_legacy_target_argument_never_writes_the_library(self):
        target = self.directory / "target library.kicad_sym"
        original = '(kicad_symbol_lib (version 20231120) (symbol "Existing"))\n'
        target.write_text(original, encoding="utf-8")
        result = self.invoke("lcsc_import", "import", {
            "lcsc_id": "C37593", "target_library": str(target)}, expected_exit=1)
        self.assertFalse(result["ok"])
        self.assertEqual(target.read_text(encoding="utf-8"), original)
        self.assertFalse(target.with_suffix(".pretty").exists())


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="KiCad Forge package ")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.build = self.directory / "build"
        self.build.mkdir()
        self.binary = self.build / "KiCad_Forge"
        self.binary.write_bytes(b"test application")
        for name in ("webui", "plugins", "config", "licenses", "data"):
            (self.build / name).mkdir()
        for name in ("pcre2-LICENCE.md", "sljit-LICENSE.txt"):
            (self.build / "licenses" / name).write_text(name, encoding="utf-8")
        (self.build / "webui/index.html").write_text("built frontend", encoding="utf-8")
        (self.build / "config/component_types.json").write_text("{}", encoding="utf-8")
        (self.build / "data/private.db").write_bytes(b"user data")
        cache = self.build / "plugins/example/__pycache__"
        cache.mkdir(parents=True)
        (cache / "module.pyc").write_bytes(b"cache")
        fetched = self.build / "plugins/example/fetched"
        fetched.mkdir()
        (fetched / "private.kicad_sym").write_bytes(b"runtime cache")
        self.output = self.directory / "dist"

    def test_bundle_contains_licenses_assets_and_empty_data_directory(self):
        archive = stage_bundle(self.binary, "portable", self.output)
        with zipfile.ZipFile(archive) as bundle:
            names = set(bundle.namelist())
        self.assertIn("portable/KiCad_Forge", names)
        self.assertIn("portable/licenses/pcre2-LICENCE.md", names)
        self.assertIn("portable/licenses/sljit-LICENSE.txt", names)
        self.assertIn("portable/webui/index.html", names)
        self.assertIn("portable/config/component_types.json", names)
        self.assertIn("portable/data/", names)
        self.assertFalse(any("private" in name or "__pycache__" in name for name in names))

    def test_missing_license_fails_before_existing_bundle_is_modified(self):
        previous = self.output / "portable"
        previous.mkdir(parents=True)
        marker = previous / "keep.txt"
        marker.write_text("previous bundle", encoding="utf-8")
        (self.build / "licenses/sljit-LICENSE.txt").unlink()
        with self.assertRaises(FileNotFoundError):
            stage_bundle(self.binary, "portable", self.output)
        self.assertEqual(marker.read_text(encoding="utf-8"), "previous bundle")

    def test_bundle_name_cannot_escape_output_or_contain_the_input(self):
        for name in ("", ".", "..", "../outside", "a/b", "a\\b", "C:outside"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                stage_bundle(self.binary, name, self.output)
        with self.assertRaises(ValueError):
            stage_bundle(self.binary, self.build.name, self.build.parent)
        self.assertTrue(self.binary.is_file())

    def test_dependency_hash_mismatch_and_missing_file_are_rejected(self):
        with self.assertRaises(RuntimeError):
            verify(self.binary, "0" * 64)
        with self.assertRaises(RuntimeError):
            verify(self.directory / "missing.archive", "0" * 64)


if __name__ == "__main__":
    unittest.main()
