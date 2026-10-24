"""Mac archives must retain their signed app and preserve previous outputs on failure."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from scripts import build_macos_zip as archive


class MacZipTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="KiForge mac zip ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "payload"
        self.source.mkdir()
        self.output = self.root / "artifacts"

    def fake_ditto(self, command, **kwargs):
        self.assertFalse(kwargs.get("shell", False))
        if "-c" in command:
            self.assertIn("--keepParent", command)
            self.assertIn("--sequesterRsrc", command)
            Path(command[-1]).write_bytes(b"native ZIP")
        return subprocess.CompletedProcess(command, 0, "", "")

    def test_generated_archive_is_checked_after_extraction_before_publication(self):
        with mock.patch.object(archive, "verify_bundle") as verify, \
             mock.patch.object(archive.subprocess, "run", side_effect=self.fake_ditto):
            result = archive.build_zip(self.source, self.output, "macos-review")
        self.assertEqual(result.read_bytes(), b"native ZIP")
        self.assertEqual(verify.call_count, 2)
        self.assertEqual(verify.call_args_list[0].args[0], self.source / "KiCad Forge.app")
        self.assertEqual(verify.call_args_list[1].args[0].parent.name, "roundtrip")

    def test_bad_extracted_signature_keeps_previous_archive(self):
        self.output.mkdir()
        previous = self.output / "macos-review.zip"
        previous.write_bytes(b"previous archive")
        with mock.patch.object(archive, "verify_bundle", side_effect=[None, RuntimeError("broken signature")]), \
             mock.patch.object(archive.subprocess, "run", side_effect=self.fake_ditto):
            with self.assertRaisesRegex(RuntimeError, "broken signature"):
                archive.build_zip(self.source, self.output, "macos-review")
        self.assertEqual(previous.read_bytes(), b"previous archive")

    def test_failed_compressor_keeps_previous_archive(self):
        self.output.mkdir()
        previous = self.output / "macos-review.zip"
        previous.write_bytes(b"previous archive")
        with mock.patch.object(archive, "verify_bundle"), \
             mock.patch.object(archive.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "ditto")):
            with self.assertRaises(subprocess.CalledProcessError):
                archive.build_zip(self.source, self.output, "macos-review")
        self.assertEqual(previous.read_bytes(), b"previous archive")

    def test_names_and_overlapping_outputs_are_rejected(self):
        with mock.patch.object(archive, "verify_bundle"), mock.patch.object(archive.subprocess, "run") as run:
            for name in ("../escape", "review.zip", "bad/name"):
                with self.assertRaises(ValueError):
                    archive.build_zip(self.source, self.output, name)
            with self.assertRaises(ValueError):
                archive.build_zip(self.source, self.source / "output", "review")
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
