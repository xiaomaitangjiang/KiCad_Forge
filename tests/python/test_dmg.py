"""DMG drag-install layout and failure recovery without a macOS host."""

from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from scripts import build_dmg as dmg
from scripts import smoke_macos


class DiskImageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="KiForge dmg ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "payload"
        contents = self.source / "KiCad Forge.app/Contents"
        (contents / "MacOS").mkdir(parents=True)
        (contents / "MacOS/KiCad_Forge").write_bytes(b"native executable")
        with (contents / "Info.plist").open("wb") as stream:
            plistlib.dump({"CFBundleIdentifier": "com.kicadforge.desktop", "CFBundleExecutable": "KiCad_Forge",
                          "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "1.1.0"}, stream)
        self.output = self.root / "installers"
        self.name = "KiCad_Forge-macosx-arm64-1.1.0"
        self.events = []

    def build(self):
        return dmg.build_dmg(self.source, self.output, self.name, "1.1.0")

    def fake_tools(self, command, **kwargs):
        self.assertTrue(kwargs["check"])
        self.assertFalse(kwargs.get("shell", False))
        self.events.append(command[1] if command[0].endswith("hdiutil") else "layout")
        if command[1] == "create":
            stage = Path(command[command.index("-srcfolder") + 1])
            self.assertTrue((stage / "KiCad Forge.app/Contents/Info.plist").is_file())
            self.assertTrue((stage / ".background/background.png").read_bytes().startswith(b"\x89PNG\r\n\x1a\n"))
            self.assertTrue((stage / "Applications").exists())
            Path(command[-1]).write_bytes(b"writable disk image")
        elif command[1] == "attach":
            mount = Path(command[command.index("-mountpoint") + 1])
            (mount / "KiCad Forge.app").mkdir()
        elif command[0].endswith("osascript"):
            self.assertEqual(Path(command[1]), dmg.LAYOUT_SCRIPT)
            self.assertEqual(command[-1], "style")
            (Path(command[-2]) / ".DS_Store").write_bytes(b"Finder layout")
        elif command[1] == "convert":
            Path(command[command.index("-o") + 1]).write_bytes(b"finished compressed DMG")
        return subprocess.CompletedProcess(command, 0, "", "")

    def tools(self):
        def link_fixture(path, target, **kwargs):
            self.assertEqual(str(target), "/Applications")
            self.assertEqual(path.name, "Applications")
            path.touch()
        return mock.patch.object(Path, "symlink_to", autospec=True, side_effect=link_fixture)

    def test_image_contains_applications_link_background_and_persisted_finder_layout(self):
        with self.tools(), mock.patch.object(dmg.subprocess, "run", side_effect=self.fake_tools):
            artifact = self.build()
        self.assertEqual(artifact, self.output / (self.name + ".dmg"))
        self.assertEqual(artifact.read_bytes(), b"finished compressed DMG")
        self.assertEqual(self.events, ["create", "attach", "layout", "detach", "convert"])
        self.assertEqual(list(self.source.iterdir()), [self.source / "KiCad Forge.app"])

    def test_finder_failure_detaches_image_and_preserves_previous_artifact(self):
        self.output.mkdir()
        artifact = self.output / (self.name + ".dmg")
        artifact.write_bytes(b"previous DMG")
        def fail_layout(command, **kwargs):
            if command[0].endswith("osascript"):
                raise subprocess.CalledProcessError(1, command, stderr="Finder unavailable")
            return self.fake_tools(command, **kwargs)
        with self.tools(), mock.patch.object(dmg.subprocess, "run", side_effect=fail_layout):
            with self.assertRaises(subprocess.CalledProcessError):
                self.build()
        self.assertIn("detach", self.events)
        self.assertNotIn("convert", self.events)
        self.assertEqual(artifact.read_bytes(), b"previous DMG")

    def test_unsaved_layout_or_missing_converted_artifact_is_not_published(self):
        for missing in ("layout", "convert"):
            with self.subTest(missing=missing):
                def omit_result(command, **kwargs):
                    if ((missing == "layout" and command[0].endswith("osascript")) or
                            (missing == "convert" and command[1] == "convert")):
                        return subprocess.CompletedProcess(command, 0, "", "")
                    return self.fake_tools(command, **kwargs)
                with self.tools(), mock.patch.object(dmg.subprocess, "run", side_effect=omit_result):
                    with self.assertRaises(RuntimeError):
                        self.build()
                self.assertFalse((self.output / (self.name + ".dmg")).exists())

    def test_invalid_bundle_version_names_and_overlapping_output_fail_before_tools_run(self):
        for name, version, output in (("../escape", "1.1.0", self.output),
                                      (self.name, "2.0.0", self.output),
                                      (self.name, "1.1.0", self.source / "nested")):
            with mock.patch.object(dmg.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    dmg.build_dmg(self.source, output, name, version)
                run.assert_not_called()
        shutil.rmtree(self.source / "KiCad Forge.app/Contents/MacOS")
        with mock.patch.object(dmg.subprocess, "run") as run:
            with self.assertRaises((ValueError, OSError)):
                self.build()
            run.assert_not_called()

    def test_read_only_image_smoke_requires_saved_layout_and_checks_it_in_finder(self):
        mount = self.root / "mounted image"
        shutil.copytree(self.source, mount)
        (mount / ".background").mkdir()
        dmg.write_background(mount / ".background/background.png")
        with mock.patch.object(Path, "is_symlink", return_value=True), \
             mock.patch.object(smoke_macos.os, "readlink", return_value="/Applications"), \
             mock.patch.object(smoke_macos.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "Finder layout"):
                smoke_macos.check_dmg_layout(mount)
            run.assert_not_called()
            (mount / ".DS_Store").write_bytes(b"saved layout")
            smoke_macos.check_dmg_layout(mount)
            self.assertEqual(run.call_args.args[0],
                             ["/usr/bin/osascript", str(dmg.LAYOUT_SCRIPT), str(mount), "verify"])
            self.assertTrue(run.call_args.kwargs["check"])

    @unittest.skipUnless(sys.platform == "darwin", "Native Finder and hdiutil required")
    def test_native_read_only_image_preserves_finder_layout_after_remount(self):
        artifact = self.build()
        mount = self.root / "remounted read only image"
        mount.mkdir()
        subprocess.run(["/usr/bin/hdiutil", "attach", "-readonly", "-nobrowse", "-noautoopen",
                        "-mountpoint", str(mount), str(artifact)], check=True, timeout=40)
        try:
            smoke_macos.check_dmg_layout(mount)
        except Exception:
            diagnostics = dmg.PROJECT / "test-results/dmg-layout"
            diagnostics.mkdir(parents=True, exist_ok=True)
            store = (mount / ".DS_Store").read_bytes()
            (diagnostics / "Finder.DS_Store").write_bytes(store)
            # Finder stores its icon-view dictionary as a length-prefixed
            # binary plist. Dump only diagnostic fields, never modify the store.
            for match in re.finditer(b"bplist00", store):
                if match.start() < 4:
                    continue
                size = int.from_bytes(store[match.start() - 4:match.start()], "big")
                try:
                    data = plistlib.loads(store[match.start():match.start() + size])
                except (ValueError, plistlib.InvalidFileException):
                    continue
                if isinstance(data, dict) and "backgroundType" in data:
                    print("Finder saved icon-view options:", {
                        key: value for key, value in data.items() if not isinstance(value, bytes)})
                    alias = data.get("backgroundImageAlias", b"")
                    print("Finder background alias strings:", re.findall(rb"[ -~]{5,}", alias))
            subprocess.run(["/usr/sbin/screencapture", "-x", str(diagnostics / "Finder.png")], timeout=10)
            raise
        finally:
            subprocess.run(["/usr/bin/hdiutil", "detach", str(mount)], check=True, timeout=40)


if __name__ == "__main__":
    unittest.main()
