"""Offline checks for refreshing build assets without touching source or user data."""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from scripts import sync_runtime_assets as assets


class RuntimeAssetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="KiCad Forge assets ")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.project = self.base / "源码 project"
        self.output = self.project / "custom build/Windows 输出"
        self.sources = (
            "webui/dist/index.html", "plugins/example/plugin.py", "config/types.json",
            "resources/licenses/LICENSE.txt",
        )
        for relative in self.sources:
            path = self.project / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("new " + relative, encoding="utf-8")
        for relative in ("webui/old.html", "plugins/old.py", "config/old.json",
                         "licenses/old.txt", "data/meta.db", "KiCad_Forge.exe", "runtime.dll"):
            path = self.output / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"old build or user data")
        self.before = self.source_hashes()

    def source_hashes(self):
        return {relative: hashlib.sha256((self.project / relative).read_bytes()).hexdigest()
                for relative in self.sources if (self.project / relative).is_file()}

    def old_output(self):
        return {str(path.relative_to(self.output)): path.read_bytes()
                for path in self.output.rglob("*") if path.is_file()}

    def assert_rejected_unchanged(self, output=None, project=None):
        before = self.old_output()
        with self.assertRaises((OSError, ValueError)):
            assets.sync_runtime_assets(project or self.project, output or self.output)
        self.assertEqual(self.old_output(), before)
        self.assertEqual(self.source_hashes(), self.before)

    def make_link(self, path, target, kind):
        if kind == "junction":
            if os.name != "nt":
                self.skipTest("Windows junction regression")
            escaped_path = str(path).replace("'", "''")
            escaped_target = str(target).replace("'", "''")
            result = subprocess.run([
                "powershell", "-NoProfile", "-NonInteractive", "-Command",
                "$ErrorActionPreference = 'Stop'; New-Item -ItemType Junction -Path '"
                + escaped_path + "' -Target '" + escaped_target + "' | Out-Null",
            ], capture_output=True, text=True)
            if result.returncode:
                self.skipTest("Cannot create a Windows junction: " + result.stderr.strip())
        else:
            try:
                path.symlink_to(target, target_is_directory=True)
            except OSError as error:
                self.skipTest("Cannot create a directory symbolic link: " + str(error))
        self.addCleanup(self.remove_link, path)

    @staticmethod
    def remove_link(path):
        # Remove only the alias itself. Never recursively remove its target.
        if path.is_symlink():
            path.unlink()
        elif os.path.lexists(path):
            os.rmdir(path)

    def test_refresh_preserves_source_hashes_data_and_other_files(self):
        assets.sync_runtime_assets(self.project, self.output)
        for relative, target in zip(self.sources, (
                "webui/dist/index.html", "plugins/example/plugin.py", "config/types.json",
                "licenses/LICENSE.txt")):
            self.assertEqual((self.output / target).read_bytes(), (self.project / relative).read_bytes())
        for relative in ("webui/old.html", "plugins/old.py", "config/old.json", "licenses/old.txt"):
            self.assertFalse((self.output / relative).exists())
        for relative in ("data/meta.db", "KiCad_Forge.exe", "runtime.dll"):
            self.assertEqual((self.output / relative).read_bytes(), b"old build or user data")
        self.assertEqual(self.source_hashes(), self.before)

    def test_first_build_creates_assets_and_empty_data_directory(self):
        output = self.project / "another build/nested/release"
        assets.sync_runtime_assets(self.project, output)
        self.assertTrue((output / "webui/dist/index.html").is_file())
        self.assertEqual(list((output / "data").iterdir()), [])

    def test_cache_files_are_excluded(self):
        for relative in ("plugins/example/__pycache__/plugin.cpython-311.pyc",
                         "plugins/example/stale.pyc", "webui/dist/cache.pyc",
                         "plugins/example/fetched/user-library.kicad_sym",
                         "plugins/example/staging/pending.json", "plugins/example/.git/config"):
            path = self.project / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"cache")
        assets.sync_runtime_assets(self.project, self.output)
        self.assertEqual(list(self.output.rglob("__pycache__")), [])
        self.assertEqual(list(self.output.rglob("*.pyc")), [])
        for name in ("fetched", "staging", ".git"):
            self.assertEqual(list(self.output.rglob(name)), [])

    def test_missing_any_source_preserves_all_old_outputs(self):
        for relative in ("webui/dist", "plugins", "config", "resources/licenses"):
            source = self.project / relative
            backup = source.with_name(source.name + ".saved")
            source.rename(backup)
            try:
                with self.subTest(source=relative):
                    before = self.old_output()
                    with self.assertRaises((OSError, ValueError)):
                        assets.sync_runtime_assets(self.project, self.output)
                    self.assertEqual(self.old_output(), before)
            finally:
                backup.rename(source)
            self.assertEqual(self.source_hashes(), self.before)

    def test_source_path_must_be_a_directory(self):
        path = self.project / "config"
        backup = self.project / "config.saved"
        path.rename(backup)
        path.write_bytes(b"not a directory")
        try:
            before = self.old_output()
            with self.assertRaises((OSError, ValueError)):
                assets.sync_runtime_assets(self.project, self.output)
            self.assertEqual(self.old_output(), before)
        finally:
            path.unlink()
            backup.rename(path)

    def test_project_root_and_source_outputs_are_rejected(self):
        for relative in (".", "webui", "webui/dist/output", "plugins/build", "config/build",
                         "resources/build", "src/build", "scripts/build", "tests/build", ".git/build"):
            with self.subTest(output=relative):
                self.assert_rejected_unchanged(self.project / relative)

    def test_output_outside_project_is_rejected_before_creation(self):
        output = self.base / "outside/output"
        self.assert_rejected_unchanged(output)
        self.assertFalse(output.exists())

    def test_output_leaf_file_is_rejected(self):
        output = self.project / "custom build/file"
        output.write_bytes(b"keep me")
        self.assert_rejected_unchanged(output)
        self.assertEqual(output.read_bytes(), b"keep me")

    def test_existing_target_file_rejects_before_other_targets_change(self):
        target = self.output / "licenses"
        shutil.rmtree(target)
        target.write_bytes(b"keep me")
        self.assert_rejected_unchanged()

    def test_output_ancestor_file_is_rejected(self):
        ancestor = self.project / "not a directory"
        ancestor.write_bytes(b"keep me")
        self.assert_rejected_unchanged(ancestor / "release")
        self.assertEqual(ancestor.read_bytes(), b"keep me")

    def test_copy_failure_preserves_all_old_outputs(self):
        before = self.old_output()
        original = assets.shutil.copytree
        calls = 0

        def fail_after_first(*args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("simulated source read failure")
            return original(*args, **kwargs)

        with mock.patch.object(assets.shutil, "copytree", side_effect=fail_after_first):
            with self.assertRaisesRegex(OSError, "simulated"):
                assets.sync_runtime_assets(self.project, self.output)
        self.assertEqual(self.old_output(), before)
        self.assertEqual(self.source_hashes(), self.before)

    def test_cli_uses_project_for_relative_output_from_unrelated_cwd(self):
        script = Path(assets.__file__).resolve()
        result = subprocess.run([
            sys.executable, "-B", "-X", "utf8", str(script), "--project", str(self.project),
            "--output", "CLI build/发布",
        ], cwd=self.base, capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.project / "CLI build/发布/config/types.json").is_file())
        self.assertFalse((self.base / "CLI build").exists())

    def test_output_symlink_to_source_is_rejected(self):
        link = self.project / "redirected build"
        self.make_link(link, self.project / "plugins", "symlink")
        self.assert_rejected_unchanged(link)

    def test_output_junction_to_source_is_rejected(self):
        link = self.project / "redirected build"
        self.make_link(link, self.project / "plugins", "junction")
        self.assert_rejected_unchanged(link)

    def test_output_ancestor_symlink_to_source_is_rejected(self):
        link = self.project / "redirected parent"
        self.make_link(link, self.project / "resources", "symlink")
        self.assert_rejected_unchanged(link / "nested/release")

    def test_output_ancestor_junction_to_source_is_rejected(self):
        link = self.project / "redirected parent"
        self.make_link(link, self.project / "resources", "junction")
        self.assert_rejected_unchanged(link / "nested/release")

    def test_target_junction_to_source_is_rejected(self):
        target = self.output / "config"
        shutil.rmtree(target)
        self.make_link(target, self.project / "config", "junction")
        self.assert_rejected_unchanged()

    def test_nested_target_alias_to_source_is_rejected(self):
        link = self.output / "licenses/redirected source"
        self.make_link(link, self.project / "plugins", "junction" if os.name == "nt" else "symlink")
        self.assert_rejected_unchanged()

    def test_source_directory_alias_is_rejected(self):
        source = self.project / "plugins"
        backup = self.project / "original plugins"
        source.rename(backup)
        self.make_link(source, backup, "junction" if os.name == "nt" else "symlink")
        self.assert_rejected_unchanged()

    def test_source_ancestor_alias_is_rejected(self):
        source = self.project / "resources"
        backup = self.project / "original resources"
        source.rename(backup)
        self.make_link(source, backup, "junction" if os.name == "nt" else "symlink")
        self.assert_rejected_unchanged()

    def test_nested_source_alias_is_rejected(self):
        link = self.project / "plugins/example/redirected config"
        self.make_link(link, self.project / "config", "junction" if os.name == "nt" else "symlink")
        self.assert_rejected_unchanged()


if __name__ == "__main__":
    unittest.main()
