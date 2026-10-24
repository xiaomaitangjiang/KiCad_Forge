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
from scripts import smoke_test as smoke


PLUGINS = Path(__file__).resolve().parents[2] / "plugins"


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


class SmokeHttpTests(unittest.TestCase):
    def test_json_request_distinguishes_get_and_an_empty_post_object(self):
        opener = mock.Mock()
        opener.open.side_effect = [BytesIO(b'{"ok":true}'), BytesIO(b'{"ok":false,"error":"bad"}')]
        self.assertTrue(smoke.request_json(opener, "http://127.0.0.1:1234", "/api/test")["ok"])
        self.assertFalse(smoke.request_json(opener, "http://127.0.0.1:1234", "/api/test", {})["ok"])
        get = opener.open.call_args_list[0].args[0]
        post = opener.open.call_args_list[1].args[0]
        self.assertEqual(get.get_method(), "GET")
        self.assertIsNone(get.data)
        self.assertEqual(post.get_method(), "POST")
        self.assertEqual(json.loads(post.data), {})
        self.assertEqual(post.get_header("Content-type"), "application/json")

    def test_json_request_rejects_non_object_and_malformed_responses(self):
        opener = mock.Mock()
        for response in (b"[]", b"null", b"broken JSON"):
            opener.open.return_value = BytesIO(response)
            with self.subTest(response=response), self.assertRaises((RuntimeError, json.JSONDecodeError)):
                smoke.request_json(opener, "http://127.0.0.1:1234", "/api/test")

    def test_matching_save_payload_excludes_read_only_metadata_and_rejects_missing_fields(self):
        config = {"schema_version": 1, "revision": 4, "rules": [], "exceptions": []}
        full = {**config, "ok": True, "scope": "global", "engine": {"name": "PCRE2"},
                "load_error": "", "has_snapshot": True}
        self.assertEqual(smoke.matching_config_payload(full), config)
        with self.assertRaises(RuntimeError):
            smoke.matching_config_payload({key: value for key, value in full.items() if key != "rules"})


if __name__ == "__main__":
    unittest.main()
