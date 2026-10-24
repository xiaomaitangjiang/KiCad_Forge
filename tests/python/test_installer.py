"""Offline input and command-line regressions for the XPack NSIS backend."""

from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from scripts import build_installer as installer
from scripts.prepare_packaging import write_manifest

PROJECT = Path(__file__).resolve().parents[2]


class InstallerBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="KiForge installer ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / "完成的 Windows bundle"
        for name in ("KiCad_Forge.exe", "runtime.dll", "webui/index.html",
                     "config/component_types.json", "config/package_types.json",
                     "config/component_type_matching.json", "plugins/git_version/manifest.json",
                     "plugins/git_version/plugin.py", "plugins/lcsc_import/manifest.json",
                     "plugins/lcsc_import/plugin.py", "licenses/pcre2-LICENCE.md", "licenses/sljit-LICENSE.txt"):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"build asset")
        for name in ("runtime/python/python.exe", "runtime/python/Lib/site-packages/certifi/cacert.pem"):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"private runtime")
        write_manifest(self.source / "runtime/python", "x86_64", platform="mingw")
        (self.source / "kforge-distribution.json").write_text('{"platform":"mingw"}')
        self.compiler = self.root / "NSIS tools/makensis.exe"
        self.compiler.parent.mkdir()
        self.compiler.write_bytes(b"compiler")
        self.output = self.root / "installer output"

    def args(self, *extra):
        return installer.parser().parse_args([
            "--source", str(self.source), "--nsis", str(self.compiler),
            "--output", str(self.output), "--version", "0.2.0", *extra])

    def test_argument_list_preserves_spaces_unicode_and_version(self):
        plan = installer.make_plan(self.args("--name", "KiForge review"))
        self.assertEqual(plan.command[0], str(self.compiler.resolve()))
        self.assertIn("/DKFORGE_SOURCE=" + str(self.source.resolve()), plan.command)
        self.assertIn("/DKFORGE_VERSION=0.2.0", plan.command)
        self.assertIn("/DKFORGE_OUTPUT=" + str(self.output / "KiForge review.exe"), plan.command)
        self.assertFalse(self.output.exists())

    def test_missing_build_assets_fail_before_compiler_runs(self):
        for name in ("KiCad_Forge.exe", "runtime.dll", "webui/index.html",
                     "config/component_type_matching.json", "plugins/lcsc_import/plugin.py",
                     "licenses/pcre2-LICENCE.md", "runtime/python/python.exe", "kforge-distribution.json"):
            path = self.source / name
            original = path.read_bytes()
            path.unlink()
            try:
                with self.subTest(name=name), self.assertRaises((OSError, ValueError)):
                    installer.make_plan(self.args())
            finally:
                path.write_bytes(original)
        self.assertFalse(self.output.exists())

    def test_names_cannot_inject_nsis_or_escape_output(self):
        for name in ("", ".", "..", "../escape", "a/b", "a\\b", "CON", "aux.txt",
                     "trailing.", "trailing ", "bad\nname", "bad?name", "setup.exe", "$TEMP"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                installer.make_plan(self.args("--name", name))

    def test_bad_version_and_overlapping_output_are_rejected(self):
        with self.assertRaises(ValueError):
            installer.make_plan(self.args("--version", "1.2.3\nExec bad"))
        with self.assertRaises(ValueError):
            installer.make_plan(self.args("--output", str(self.source / "installers")))

    def test_no_artifact_is_reported_for_compiler_failure_or_missing_output(self):
        plan = installer.make_plan(self.args())
        with mock.patch.object(installer.subprocess, "run", side_effect=subprocess.CalledProcessError(2, "makensis")):
            with self.assertRaises(subprocess.CalledProcessError):
                installer.compile_installer(plan)
        with mock.patch.object(installer.subprocess, "run"):
            with self.assertRaises(RuntimeError):
                installer.compile_installer(plan)

    def test_compiler_uses_no_shell_and_requires_the_expected_artifact(self):
        plan = installer.make_plan(self.args())
        def compile_fake(*args, **kwargs):
            self.assertEqual(args[0], plan.command)
            self.assertTrue(kwargs["check"])
            self.assertFalse(kwargs.get("shell", False))
            plan.artifact.parent.mkdir(parents=True, exist_ok=True)
            plan.artifact.write_bytes(b"installer")
        with mock.patch.object(installer.subprocess, "run", side_effect=compile_fake):
            self.assertEqual(installer.compile_installer(plan), plan.artifact)

    def test_uninstall_preserves_user_data_and_excludes_downloads(self):
        script = (PROJECT / "packaging/windows/kiforge.nsi").read_text(encoding="utf-8")
        uninstall = script.split('Section "Uninstall"', 1)[1]
        self.assertNotIn("$APPDATA", uninstall)
        self.assertNotIn('RMDir /r "$INSTDIR"', uninstall)
        self.assertNotIn('RMDir /r "$INSTDIR\\data"', uninstall)
        for name in ("fetched", "staging", "__pycache__", "node_modules"):
            self.assertIn("/x " + name, script)


if __name__ == "__main__":
    unittest.main()
