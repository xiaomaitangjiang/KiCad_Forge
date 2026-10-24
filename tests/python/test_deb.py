"""Debian binary packaging behavior without a native Linux SDK."""

import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

from scripts import build_deb as deb
from scripts import smoke_distribution
from scripts.prepare_packaging import write_manifest


class DebianInstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="KiForge deb ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "bundle with spaces"
        for name in ("webui/index.html", "config/component_types.json", "config/package_types.json",
                     "config/component_type_matching.json", "plugins/git_version/manifest.json",
                     "plugins/git_version/plugin.py", "plugins/lcsc_import/manifest.json",
                     "plugins/lcsc_import/plugin.py", "licenses/pcre2-LICENCE.md", "licenses/sljit-LICENSE.txt",
                     "LICENSE", "README.md", "runtime/python/lib/python3.12/site-packages/certifi/cacert.pem",
                     "data/meta.db", "fetched/private-download"):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")
        elf = b"\x7fELF\x02\x01" + bytes(12) + struct.pack("<H", 62) + bytes(44)
        for name in ("KiCad_Forge", "runtime/python/bin/python3", "runtime/python/lib/libpython3.12.so.1.0",
                     "runtime/python/lib/libpython3.so"):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(elf)
        write_manifest(self.source / "runtime/python", "x86_64", platform="linux")
        self.marker = self.source / "kforge-distribution.json"
        self.marker.write_text(json.dumps({"platform": "linux", "arch": "x86_64", "version": "0.2.0"}))
        self.output = self.root / "installers"
        self.name = "KiCad_Forge-linux-x86_64-0.2.0"

    def build(self):
        return deb.build_deb(self.source, self.output, self.name, "0.2.0", "x86_64")

    def fake_tools(self, command, **kwargs):
        self.assertFalse(kwargs.get("shell", False))
        self.assertTrue(kwargs["check"])
        if command[0] == "objdump":
            return subprocess.CompletedProcess(command, 0,
                " NEEDED $ORIGIN/../lib/libpython3.12.so.1.0\n NEEDED libc.so.6\n NEEDED libpthread.so.0\n", "")
        if command[0] == "dpkg-shlibdeps":
            self.assertNotIn("--ignore-missing-info", command)
            self.assertEqual(sum(argument.startswith("-e") for argument in command), 3)
            self.assertFalse(any(argument.endswith("libpython3.so") for argument in command))
            return subprocess.CompletedProcess(command, 0, "shlibs:Depends=libc6 (>= 2.38), libstdc++6 (>= 14)\n", "")
        self.assertEqual(command[0], "dpkg-deb")
        self.assertIn("--root-owner-group", command)
        root = Path(command[-2])
        app = root / "opt/kicad-forge"
        self.assertEqual((app / "KiCad_Forge").read_bytes(), (self.source / "KiCad_Forge").read_bytes())
        self.assertTrue((app / "runtime/python/bin/python3").is_file())
        self.assertFalse((app / "data").exists())
        self.assertFalse((app / "fetched").exists())
        control = (root / "DEBIAN/control").read_text()
        self.assertIn("Package: kicad-forge\n", control)
        self.assertIn("Version: 0.2.0\n", control)
        self.assertIn("Architecture: amd64\n", control)
        self.assertIn("Depends: libc6 (>= 2.38), libstdc++6 (>= 14), xdg-utils\n", control)
        self.assertNotIn("python3", control.lower())
        self.assertIn("/opt/kicad-forge/KiCad_Forge", (root / "usr/bin/kicad-forge").read_text())
        self.assertTrue((root / "usr/share/pixmaps/kicad-forge.png").is_file())
        self.assertTrue((root / "usr/share/applications/kicad-forge.desktop").is_file())
        Path(command[-1]).write_bytes(b"!<arch>\nfixture")
        return subprocess.CompletedProcess(command, 0)

    def test_package_has_private_python_desktop_entry_and_real_library_dependencies(self):
        with mock.patch.object(deb.shutil, "which", side_effect=lambda name: name), \
             mock.patch.object(deb.subprocess, "run", side_effect=self.fake_tools):
            artifact = self.build()
        self.assertEqual(artifact, self.output / (self.name + ".deb"))
        self.assertEqual(artifact.read_bytes(), b"!<arch>\nfixture")
        self.assertEqual((self.source / "data/meta.db").read_bytes(), b"fixture")

    def test_missing_tools_fail_without_publishing_a_package(self):
        with mock.patch.object(deb.shutil, "which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "dpkg"):
                self.build()
        self.assertFalse(self.output.exists())

    def test_wrong_platform_or_version_is_rejected_before_packaging(self):
        for metadata in ({"platform": "mingw", "arch": "x86_64", "version": "0.2.0"},
                         {"platform": "linux", "arch": "x86_64", "version": "9.9.9"}):
            self.marker.write_text(json.dumps(metadata))
            with self.assertRaisesRegex(ValueError, "metadata"):
                self.build()

    def test_non_linux_binary_and_missing_runtime_are_rejected(self):
        binary = self.source / "KiCad_Forge"
        binary.write_bytes(b"MZ" + bytes(64))
        with self.assertRaisesRegex(ValueError, "ELF"):
            self.build()
        (self.source / "runtime/python/bin/python3").unlink()
        with self.assertRaises((FileNotFoundError, ValueError)):
            self.build()

    def test_dependency_failure_preserves_the_previous_installer(self):
        self.output.mkdir()
        artifact = self.output / (self.name + ".deb")
        artifact.write_bytes(b"old installer")
        with mock.patch.object(deb.shutil, "which", side_effect=lambda name: name), \
             mock.patch.object(deb.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "dpkg-shlibdeps")):
            with self.assertRaises(subprocess.CalledProcessError):
                self.build()
        self.assertEqual(artifact.read_bytes(), b"old installer")

    def test_unexpected_shim_dependency_is_not_silently_omitted(self):
        with mock.patch.object(deb.shutil, "which", side_effect=lambda name: name), \
             mock.patch.object(deb.subprocess, "run", return_value=subprocess.CompletedProcess(
                 "objdump", 0, " NEEDED $ORIGIN/../lib/libpython3.12.so.1.0\n NEEDED libunbundled.so.7\n", "")):
            with self.assertRaisesRegex(RuntimeError, "stable-ABI shim"):
                self.build()
        self.assertFalse((self.output / (self.name + ".deb")).exists())

    def test_system_python_dependency_is_rejected(self):
        def scan(command, **kwargs):
            if command[0] == "dpkg-shlibdeps":
                return subprocess.CompletedProcess(command, 0, "shlibs:Depends=libc6, python3\n", "")
            return self.fake_tools(command, **kwargs)
        with mock.patch.object(deb.shutil, "which", side_effect=lambda name: name), \
             mock.patch.object(deb.subprocess, "run", side_effect=scan):
            with self.assertRaisesRegex(RuntimeError, "system Python"):
                self.build()

    def test_compiler_failure_or_missing_output_preserves_the_previous_installer(self):
        self.output.mkdir()
        artifact = self.output / (self.name + ".deb")
        artifact.write_bytes(b"previous installer")
        for failure in (True, False):
            def compile_failed(command, **kwargs):
                if command[0] != "dpkg-deb":
                    return self.fake_tools(command, **kwargs)
                if failure:
                    raise subprocess.CalledProcessError(2, command)
                return subprocess.CompletedProcess(command, 0)
            with self.subTest(failure=failure), \
                 mock.patch.object(deb.shutil, "which", side_effect=lambda name: name), \
                 mock.patch.object(deb.subprocess, "run", side_effect=compile_failed):
                with self.assertRaises((subprocess.CalledProcessError, RuntimeError)):
                    self.build()
            self.assertEqual(artifact.read_bytes(), b"previous installer")

    def test_unsafe_output_name_version_and_source_overlap_are_rejected(self):
        for name, version, output in (("../escape", "0.2.0", self.output),
                                      (self.name, "0.2.0\nInjected: yes", self.output),
                                      (self.name, "0.2.0", self.source / "output")):
            with self.subTest(name=name, version=version), self.assertRaises(ValueError):
                deb.build_deb(self.source, output, name, version, "x86_64")


class DebianSmokeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="KiForge deb smoke ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.archive = self.root / "installer.deb"
        self.archive.write_bytes(b"installer")
        self.extracted = self.root / "extracted"

    def test_wrong_architecture_fails_before_extracting_or_starting_the_app(self):
        with mock.patch.object(smoke_distribution.subprocess, "run", return_value=subprocess.CompletedProcess(
            "dpkg-deb", 0, "Package: kicad-forge\nArchitecture: arm64\n", "")) as run:
            with self.assertRaisesRegex(RuntimeError, "architecture"):
                smoke_distribution.extract_deb(self.archive, self.extracted)
        self.assertEqual(run.call_count, 1)

    def test_native_deb_extracts_without_running_the_installer(self):
        def extract(command, **kwargs):
            if "--field" in command:
                return subprocess.CompletedProcess(command, 0, "Package: kicad-forge\nArchitecture: amd64\n", "")
            self.assertEqual(command[1], "--extract")
            for name in ("opt/kicad-forge/KiCad_Forge", "usr/share/applications/kicad-forge.desktop", "usr/bin/kicad-forge"):
                path = self.extracted / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"installed file")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(smoke_distribution.subprocess, "run", side_effect=extract):
            self.assertEqual(smoke_distribution.extract_deb(self.archive, self.extracted),
                             self.extracted / "opt/kicad-forge")

    def test_missing_desktop_registration_is_a_smoke_failure(self):
        with mock.patch.object(smoke_distribution.subprocess, "run", return_value=subprocess.CompletedProcess(
            "dpkg-deb", 0, "Package: kicad-forge\nArchitecture: amd64\n", "")):
            with self.assertRaisesRegex(RuntimeError, "desktop entry"):
                smoke_distribution.extract_deb(self.archive, self.extracted)


if __name__ == "__main__":
    unittest.main()
