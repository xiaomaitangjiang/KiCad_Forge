"""Signing command boundaries and signature failure propagation, without a Mac SDK."""

from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
from unittest import mock

from scripts import sign_macos_bundle as signing


class MacSigningTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="KiForge signing ")
        self.addCleanup(self.temporary.cleanup)
        self.app = Path(self.temporary.name).resolve() / "KiCad Forge.app"
        contents = self.app / "Contents"
        (contents / "MacOS").mkdir(parents=True)
        (contents / "Frameworks").mkdir()
        self.main = contents / "MacOS/KiCad_Forge"
        self.main.write_bytes(b"\xcf\xfa\xed\xfe" + bytes(64))
        self.library = contents / "Frameworks/private.dylib"
        self.library.write_bytes(b"\xca\xfe\xba\xbe" + bytes(64))
        with (contents / "Info.plist").open("wb") as stream:
            plistlib.dump({"CFBundleIdentifier": "com.kicadforge.desktop",
                          "CFBundleExecutable": "KiCad_Forge"}, stream)

    def test_native_inventory_recognizes_thin_and_universal_code_but_not_scripts(self):
        (self.app / "Contents/MacOS/helper.py").write_text("print('a script')")
        self.assertEqual(set(signing.native_files(self.app)), {self.main, self.library})

    def test_nested_and_outer_signatures_are_checked_with_strict_validation(self):
        with mock.patch.object(signing, "run") as run:
            signing.verify_bundle(self.app)
        self.assertEqual(run.call_args_list, [
            mock.call(["/usr/bin/codesign", "--verify", "--strict", "--verbose=2", str(self.library)]),
            mock.call(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=2", str(self.app)])])

    def test_a_bad_nested_signature_blocks_bundle_validation(self):
        with mock.patch.object(signing, "run", side_effect=subprocess.CalledProcessError(1, "codesign")) as run:
            with self.assertRaises(subprocess.CalledProcessError):
                signing.verify_bundle(self.app)
        self.assertEqual(run.call_count, 1)

    def test_gatekeeper_rejection_is_reported_and_never_labelled_as_trusted(self):
        rejected = subprocess.CompletedProcess([], 3, "", "rejected\nsource=no usable signature\n")
        with mock.patch.object(signing.subprocess, "run", return_value=rejected):
            result = signing.assessment(self.app)
        self.assertFalse(result["accepted"])
        self.assertIn("rejected", result["details"])

    def test_unrelated_bundles_are_rejected_before_signing(self):
        with (self.app / "Contents/Info.plist").open("wb") as stream:
            plistlib.dump({"CFBundleIdentifier": "another.app", "CFBundleExecutable": "Other"}, stream)
        with mock.patch.object(signing, "run") as run:
            with self.assertRaises(ValueError):
                signing.verify_bundle(self.app)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
