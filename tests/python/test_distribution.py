"""Distribution contents and macOS installation behavior, without native SDKs."""

import json
from pathlib import Path
import plistlib
import struct
import tempfile
import unittest
from unittest import mock

from scripts import stage_distribution as distribution
from scripts.prepare_packaging import write_manifest


class DistributionTests(unittest.TestCase):
    def setUp(self):
        signer = mock.patch.object(distribution, "seal_bundle")
        self.mac_signer = signer.start()
        self.addCleanup(signer.stop)
        self.temp = tempfile.TemporaryDirectory(prefix="KiForge distribution ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / "编译结果"
        self.output = self.root / "payload"
        for name in ("KiCad_Forge", "KiCad_Forge.exe", "runtime.dll",
                     "webui/dist/index.html", "config/component_types.json",
                     "config/package_types.json", "config/component_type_matching.json",
                     "plugins/git_version/manifest.json", "plugins/git_version/plugin.py",
                     "plugins/lcsc_import/manifest.json", "plugins/lcsc_import/plugin.py",
                     "licenses/pcre2-LICENCE.md", "licenses/sljit-LICENSE.txt"):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("build asset", encoding="utf-8")
        self.runtime = self.root / "runtime"
        for name in ("bin/python3", "lib/python3.12/site-packages/certifi/cacert.pem",
                     "licenses/LICENSE.cpython.txt"):
            path = self.runtime / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("runtime asset", encoding="utf-8")
        macho = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x100000C, 0, 2, 1, 24, 0, 0)
        macho += struct.pack("<IIIIII", 0x32, 24, 1, 0x000B0000, 0x000F0000, 0)
        (self.source / "KiCad_Forge").write_bytes(macho)
        (self.runtime / "bin/python3").write_bytes(macho)
        write_manifest(self.runtime, "arm64", platform="macosx")

    def runtime_for(self, platform):
        if platform == "macosx":
            return self.runtime
        runtime = self.root / ("runtime-" + platform)
        for name in (("python.exe", "Lib/site-packages/certifi/cacert.pem") if platform == "mingw"
                     else ("bin/python3", "lib/python3.12/site-packages/certifi/cacert.pem")):
            path = runtime / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"private runtime")
        write_manifest(runtime, "x86_64", platform=platform)
        return runtime

    def stage(self, platform="macosx", **kwargs):
        return distribution.stage_distribution(self.source, self.output, platform,
                                               "arm64" if platform == "macosx" else "x86_64",
                                               "0.2.0", runtime=self.runtime_for(platform),
                                               **kwargs)

    def test_windows_and_linux_ship_private_python_and_distribution_marker(self):
        for platform, executable in (("mingw", "python.exe"), ("linux", "bin/python3")):
            output = self.root / ("payload-" + platform)
            distribution.stage_distribution(self.source, output, platform, "x86_64", "0.2.0",
                                            runtime=self.runtime_for(platform))
            self.assertTrue((output / "runtime/python" / executable).is_file())
            metadata = json.loads((output / "kforge-distribution.json").read_text())
            self.assertEqual(metadata["platform"], platform)
            self.assertEqual(metadata["arch"], "x86_64")
            self.assertFalse((output / "data").exists())

    def test_all_platforms_reject_a_missing_private_runtime(self):
        for platform in ("mingw", "linux", "macosx"):
            with self.subTest(platform=platform), self.assertRaisesRegex((ValueError, OSError), "Python"):
                distribution.stage_distribution(self.source, self.output, platform, "x86_64", "0.2.0")
        self.assertFalse(self.output.exists())

    def test_runtime_for_another_os_or_modified_files_is_rejected(self):
        runtime = self.runtime_for("mingw")
        write_manifest(runtime, "x86_64", platform="linux")
        with self.assertRaisesRegex(ValueError, "platform"):
            distribution.stage_distribution(self.source, self.output, "mingw", "x86_64", "0.2.0", runtime=runtime)
        write_manifest(runtime, "x86_64", platform="mingw")
        (runtime / "python.exe").write_bytes(b"modified")
        with self.assertRaisesRegex(ValueError, "SHA256"):
            distribution.stage_distribution(self.source, self.output, "mingw", "x86_64", "0.2.0", runtime=runtime)
        self.assertFalse(self.output.exists())

    def test_mac_is_a_launchable_bundle_with_separate_resources_and_runtime(self):
        self.stage()
        contents = self.output / "KiCad Forge.app/Contents"
        with (contents / "Info.plist").open("rb") as stream:
            info = plistlib.load(stream)
        self.assertEqual(info["CFBundleExecutable"], "KiCad_Forge")
        self.assertEqual(info["CFBundlePackageType"], "APPL")
        self.assertEqual(info["CFBundleShortVersionString"], "0.2.0")
        self.assertEqual(info["LSMinimumSystemVersion"], "11.0.0")
        self.assertTrue((contents / "MacOS/KiCad_Forge").is_file())
        self.assertTrue((contents / "Resources/webui/dist/index.html").is_file())
        self.assertTrue((contents / "Resources/python/bin/python3").is_file())
        self.assertTrue((contents / "Resources/python/lib/python3.12/site-packages/certifi/cacert.pem").is_file())
        self.mac_signer.assert_called_once()
        self.assertEqual(self.mac_signer.call_args.args[0].name, "KiCad Forge.app")
        self.assertEqual(self.mac_signer.call_args.args[1], "arm64")
        self.assertFalse((contents / "MacOS/data").exists())
        self.assertFalse((contents / "Resources/data").exists())

    def test_platform_packages_do_not_ship_user_data_or_plugin_downloads(self):
        for name in ("data/meta.db", "plugins/lcsc_import/fetched/private.json",
                     "plugins/lcsc_import/__pycache__/plugin.pyc"):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("private")
        self.stage("mingw")
        self.assertTrue((self.output / "KiCad_Forge.exe").is_file())
        self.assertFalse((self.output / "data").exists())
        self.assertFalse((self.output / "plugins/lcsc_import/fetched").exists())
        self.assertFalse((self.output / "plugins/lcsc_import/__pycache__").exists())
        self.assertTrue((self.source / "data/meta.db").is_file())

    def test_missing_asset_preserves_existing_output(self):
        self.output.mkdir()
        (self.output / "previous").write_text("keep")
        (self.source / "webui/dist/index.html").unlink()
        with self.assertRaises(FileNotFoundError):
            self.stage()
        self.assertEqual((self.output / "previous").read_text(), "keep")

    def test_mac_requires_matching_prepared_python(self):
        (self.runtime / "kforge-runtime.json").write_text(json.dumps({"arch": "x86_64"}))
        with self.assertRaisesRegex(ValueError, "architecture"):
            self.stage()
        self.assertFalse(self.output.exists())
        with self.assertRaisesRegex((ValueError, FileNotFoundError), "Python"):
            distribution.stage_distribution(self.source, self.output, "macosx", "arm64", "0.2.0")

    def test_bad_versions_and_overlapping_paths_are_rejected(self):
        for version in ("v1.2.3", "1.2", "1.2.3;command", "1\n2.3.4"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                distribution.stage_distribution(self.source, self.output, "mingw", "x86_64", version)
        for output in (self.source, self.source / "nested", self.source.parent):
            with self.subTest(output=output), self.assertRaises(ValueError):
                distribution.stage_distribution(self.source, output, "mingw", "x86_64", "0.2.0",
                                                runtime=self.runtime_for("mingw"))

    def test_copy_failure_preserves_previous_output(self):
        self.output.mkdir()
        (self.output / "previous").write_text("keep")
        with mock.patch.object(distribution.shutil, "copytree", side_effect=OSError("copy failed")):
            with self.assertRaises(OSError):
                self.stage()
        self.assertEqual((self.output / "previous").read_text(), "keep")

    def test_mac_signing_failure_preserves_previous_output(self):
        self.output.mkdir()
        (self.output / "previous").write_text("keep")
        self.mac_signer.side_effect = RuntimeError("signature verification failed")
        with self.assertRaisesRegex(RuntimeError, "signature verification failed"):
            self.stage()
        self.assertEqual((self.output / "previous").read_text(), "keep")

    def test_mac_rejects_a_windows_or_wrong_architecture_binary(self):
        for binary in (b"MZ" + bytes(64), struct.pack("<II", 0xFEEDFACF, 0x1000007) +
                       (self.source / "KiCad_Forge").read_bytes()[8:]):
            (self.source / "KiCad_Forge").write_bytes(binary)
            with self.assertRaises(ValueError):
                self.stage()
        self.assertFalse(self.output.exists())

    def test_redirected_input_is_rejected(self):
        path = self.source / "plugins/lcsc_import/outside"
        try:
            path.symlink_to(self.runtime, target_is_directory=True)
        except OSError as error:
            self.skipTest("Symlinks unavailable: " + str(error))
        with self.assertRaises(ValueError):
            self.stage("linux")
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
