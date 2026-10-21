"""Offline tests for the bundled Python plugin entry points and library operations."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

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



if __name__ == "__main__":
    unittest.main()
