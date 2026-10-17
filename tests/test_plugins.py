"""Offline tests for the bundled Python plugin entry points and library operations."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


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

    def test_lcsc_rejects_missing_target_without_network(self):
        result = self.invoke("lcsc_import", "import", {"lcsc_id": "C37593"}, expected_exit=1)
        self.assertFalse(result["ok"])
        self.assertIn("target_library", result["error"])

    def test_lcsc_merges_symbols_and_copies_footprints(self):
        spec = importlib.util.spec_from_file_location("lcsc_plugin_test", PLUGINS / "lcsc_import/plugin.py")
        plugin = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(plugin)
        source = PLUGINS / "lcsc_import/fetched/C37593"
        fetched = self.directory / "fetched"
        shutil.copytree(source, fetched)
        target = self.directory / "target library.kicad_sym"
        target.write_text('(kicad_symbol_lib (version 20231120) (symbol "Existing"))\n', encoding="utf-8")
        # Reuse the checked-in exporter output; the online fetch is intentionally mocked.
        with mock.patch.object(plugin.subprocess, "run", return_value=SimpleNamespace(returncode=0, stderr="")):
            result = plugin.fetch_and_merge("C37593", str(target), str(fetched))
        self.assertTrue(result["ok"])
        self.assertGreater(result["block_count"], 0)
        self.assertGreater(result["footprints_copied"], 0)
        merged = target.read_text(encoding="utf-8")
        self.assertIn('(symbol "Existing")', merged)
        self.assertNotIn("unspecified line", merged)
        self.assertTrue(list((self.directory / "target library.pretty").glob("*.kicad_mod")))


if __name__ == "__main__":
    unittest.main()
