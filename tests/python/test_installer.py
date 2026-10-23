"""Offline regression tests for installer inputs and compiler invocation."""

import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from scripts import build_installer as installer


PROJECT = Path(__file__).resolve().parents[2]


class InstallerScriptTests(unittest.TestCase):
    def test_installer_defaults_are_relative_to_the_script(self):
        script = (PROJECT / "packaging/windows/kiforge.iss").read_text(encoding="utf-8")
        self.assertNotIn("D:\\VScode", script)
        for name in ("AppSourceDir", "AppLicenseFile", "AppIconFile", "AppOutputDir", "AppOutputName"):
            self.assertIn("#ifndef " + name, script)

    def test_installer_keeps_user_data_and_excludes_plugin_runtime_files(self):
        script = (PROJECT / "packaging/windows/kiforge.iss").read_text(encoding="utf-8")
        self.assertIn("AppId={{5479F8E3-FE61-4F6F-9299-B34DEBA78041}", script)
        self.assertIn('Excludes: "__pycache__,*.pyc,.git,fetched,staging"', script)
        uninstall = script.split("[UninstallDelete]", 1)[1].split("[Run]", 1)[0]
        self.assertNotIn('Name: "{userappdata}', uninstall)


class InstallerBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="KiCad Forge installer ")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name).resolve()
        self.source = self.directory / "完成的 Windows bundle"
        for relative in ("KiCad_Forge.exe", "runtime.dll", "webui/index.html",
                         "config/component_types.json", "config/package_types.json",
                         "config/component_type_matching.json", "plugins/git_version/manifest.json",
                         "plugins/git_version/plugin.py", "plugins/lcsc_import/manifest.json",
                         "plugins/lcsc_import/plugin.py", "licenses/pcre2-LICENCE.md",
                         "licenses/sljit-LICENSE.txt"):
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"test build asset")
        self.license = self.directory / "custom license.txt"
        self.icon = self.directory / "custom icon.ico"
        self.compiler = self.directory / "Inno Setup 7/ISCC.exe"
        for path in (self.license, self.icon, self.compiler):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"test input")
        self.output = self.directory / "installer output"

    def arguments(self, *extra):
        return installer.parser().parse_args([
            "--source", str(self.source), "--license", str(self.license),
            "--icon", str(self.icon), "--iscc", str(self.compiler),
            "--output", str(self.output), *extra])

    def test_overrides_preserve_spaces_unicode_and_each_compiler_argument(self):
        plan = installer.make_plan(self.arguments("--name", "KiCad Forge review"))
        self.assertEqual(plan.command, [
            str(self.compiler.resolve()), "/Qp", "/DAppSourceDir=" + str(self.source.resolve()),
            "/DAppLicenseFile=" + str(self.license.resolve()), "/DAppIconFile=" + str(self.icon.resolve()),
            "/DAppOutputDir=" + str(self.output.resolve()), "/DAppOutputName=KiCad Forge review",
            str(installer.SCRIPT)])
        self.assertEqual(plan.artifact, self.output / "KiCad Forge review.exe")
        self.assertFalse(self.output.exists())

    def test_defaults_do_not_depend_on_the_callers_directory(self):
        args = installer.parser().parse_args([])
        self.assertEqual(args.source, PROJECT / "build/mingw/x86_64/release")
        self.assertEqual(args.output, PROJECT / "dist/installers")
        self.assertEqual(args.license, PROJECT / "LICENSE")
        self.assertEqual(args.icon, PROJECT / "resources/icons/app.ico")

    def test_normal_build_frontend_under_webui_dist_is_accepted(self):
        frontend = self.source / "webui/index.html"
        (self.source / "webui/dist").mkdir()
        frontend.rename(self.source / "webui/dist/index.html")
        plan = installer.make_plan(self.arguments())
        self.assertEqual(plan.artifact, self.output / "Kicad_Forge_Setup.exe")

    def test_incomplete_builds_are_rejected_without_invoking_compiler(self):
        for relative in ("KiCad_Forge.exe", "runtime.dll", "webui/index.html",
                         "config/component_type_matching.json", "plugins/lcsc_import/plugin.py",
                         "licenses/pcre2-LICENCE.md"):
            path = self.source / relative
            original = path.read_bytes()
            path.unlink()
            try:
                with self.subTest(relative=relative), mock.patch.object(installer.subprocess, "run") as run:
                    with self.assertRaises((FileNotFoundError, ValueError)):
                        installer.make_plan(self.arguments())
                    run.assert_not_called()
                    self.assertFalse(self.output.exists())
            finally:
                path.write_bytes(original)

    def test_missing_external_inputs_are_rejected(self):
        for option in ("--license", "--icon", "--iscc"):
            with self.subTest(option=option), self.assertRaises(FileNotFoundError):
                installer.make_plan(self.arguments(option, str(self.directory / "missing input")))
        self.assertFalse(self.output.exists())

    def test_a_directory_named_dll_does_not_count_as_a_runtime_library(self):
        dll = self.source / "runtime.dll"
        dll.unlink()
        dll.mkdir()
        with self.assertRaises(FileNotFoundError):
            installer.make_plan(self.arguments())

    def test_output_cannot_be_written_into_the_input_bundle(self):
        for output in (self.source, self.source / "installers"):
            with self.subTest(output=output), self.assertRaises(ValueError):
                installer.make_plan(self.arguments("--output", str(output)))
        self.assertFalse((self.source / "installers").exists())

    def test_installer_name_cannot_escape_output_or_be_a_windows_device(self):
        for name in ("", ".", "..", "../outside", "a/b", "a\\b", "C:outside", "CON", "aux.txt",
                     "LPT1", "COM9.foo", "trailing.", "trailing ", "bad\nname", "bad?name", "setup.exe"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                installer.make_plan(self.arguments("--name", name))
        self.assertFalse(self.output.exists())

    def test_redirected_assets_are_rejected(self):
        leak = self.source / "outside.dll"
        try:
            leak.symlink_to(self.license)
        except OSError as error:
            self.skipTest("File symlinks unavailable: " + str(error))
        with self.assertRaisesRegex(ValueError, "redirected asset"):
            installer.make_plan(self.arguments())
        self.assertEqual(self.license.read_bytes(), b"test input")

    def test_compiler_uses_an_argument_list_and_does_not_run_the_installer(self):
        plan = installer.make_plan(self.arguments())

        def compile_output(*_args, **_kwargs):
            plan.artifact.parent.mkdir()
            plan.artifact.write_bytes(b"compiled installer")

        with mock.patch.object(installer.subprocess, "run", side_effect=compile_output) as run:
            self.assertEqual(installer.compile_installer(plan), plan.artifact)
        run.assert_called_once_with(plan.command, check=True, cwd=PROJECT)

    def test_failed_compiler_is_reported_without_removing_existing_outputs(self):
        self.output.mkdir()
        marker = self.output / "existing.txt"
        marker.write_bytes(b"keep")
        with mock.patch.object(installer.subprocess, "run", side_effect=subprocess.CalledProcessError(2, "ISCC")):
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                result = installer.main([
                    "--source", str(self.source), "--output", str(self.output), "--iscc", str(self.compiler)])
        self.assertEqual(result, 1)
        self.assertIn("Installer build failed", errors.getvalue())
        self.assertEqual(marker.read_bytes(), b"keep")

    def test_compiler_success_without_expected_artifact_is_rejected(self):
        with mock.patch.object(installer.subprocess, "run"):
            with self.assertRaisesRegex(RuntimeError, "did not produce"):
                installer.compile_installer(installer.make_plan(self.arguments()))

    def test_cli_dry_run_from_another_directory_writes_nothing(self):
        result = subprocess.run([
            sys.executable, "-X", "utf8", str(PROJECT / "scripts/build_installer.py"),
            "--source", str(self.source), "--output", str(self.output),
            "--iscc", str(self.compiler), "--dry-run"], cwd=self.directory,
            capture_output=True, encoding="utf-8", timeout=15, check=True)
        plan = json.loads(result.stdout)
        self.assertEqual(plan["artifact"], str(self.output / "Kicad_Forge_Setup.exe"))
        self.assertIn("/DAppSourceDir=" + str(self.source), plan["command"])
        self.assertFalse(self.output.exists())

    def test_missing_compiler_has_a_clear_error(self):
        with mock.patch.object(installer.shutil, "which", return_value=None), mock.patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(FileNotFoundError, "--iscc"):
                installer.compiler_path(None)

    def test_standard_compiler_locations_are_detected_without_execution(self):
        with mock.patch.object(installer.shutil, "which", return_value=None):
            with mock.patch.dict(os.environ, {"ProgramFiles": str(self.directory)}, clear=True):
                self.assertEqual(installer.compiler_path(None), self.compiler)


if __name__ == "__main__":
    unittest.main()
