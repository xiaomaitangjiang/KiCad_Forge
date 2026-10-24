"""Reject corrupt or unsafe packaging dependencies before publishing them."""

import hashlib
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

from scripts import prepare_packaging as prepare


class PackagingDependencyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="KiForge packaging deps ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()

    def test_cached_content_is_verified_and_never_silently_redownloaded(self):
        archive = self.root / "runtime.zip"
        archive.write_bytes(b"wrong")
        artifact = {"filename": archive.name, "sha256": hashlib.sha256(b"expected").hexdigest()}
        with self.assertRaisesRegex(ValueError, "SHA256"):
            prepare.cached_artifact(artifact, self.root, download=False)
        self.assertEqual(archive.read_bytes(), b"wrong")

    def test_missing_archive_requires_explicit_dependency_preparation(self):
        with self.assertRaisesRegex(FileNotFoundError, "download"):
            prepare.cached_artifact({"filename": "missing.zip"}, self.root, download=False)

    def test_failed_download_preserves_the_network_error_and_cleans_temporary_files(self):
        with mock.patch.object(prepare.urllib.request, "urlopen", side_effect=OSError("network failed")):
            with self.assertRaisesRegex(OSError, "network failed"):
                prepare.cached_artifact({"filename": "runtime.zip", "url": "https://example.invalid/runtime"},
                                        self.root, download=True)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_zip_traversal_is_rejected(self):
        archive = self.root / "bad.zip"
        with zipfile.ZipFile(archive, "w") as stream:
            stream.writestr("../escape.txt", "bad")
        with self.assertRaises(ValueError):
            prepare.extract_zip(archive, self.root / "unpack")
        self.assertFalse((self.root / "escape.txt").exists())

    def test_tar_internal_alias_is_materialized_and_external_alias_is_rejected(self):
        for target, valid in (("python3.12", True), ("../../../escape", False)):
            archive = self.root / ("good.tar.gz" if valid else "bad.tar.gz")
            with tarfile.open(archive, "w:gz") as stream:
                info = tarfile.TarInfo("python/bin/python3.12")
                info.size = 6
                info.mode = 0o755
                stream.addfile(info, io.BytesIO(b"python"))
                alias = tarfile.TarInfo("python/bin/python3")
                alias.type = tarfile.SYMTYPE
                alias.linkname = target
                stream.addfile(alias)
            output = self.root / ("good" if valid else "bad")
            if valid:
                prepare.extract_python(archive, output)
                self.assertEqual((output / "python/bin/python3").read_bytes(), b"python")
                self.assertFalse((output / "python/bin/python3").is_symlink())
            else:
                with self.assertRaises(ValueError):
                    prepare.extract_python(archive, output)

    def test_changed_prepared_file_is_rejected(self):
        runtime = self.root / "runtime"
        runtime.mkdir()
        (runtime / "python").write_bytes(b"original")
        prepare.write_manifest(runtime, "arm64")
        prepare.verify_prepared(runtime, "arm64")
        (runtime / "python").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "SHA256"):
            prepare.verify_prepared(runtime, "arm64")

    def test_windows_archive_omits_unused_pip_and_ensurepip(self):
        archive = self.root / "windows.tar.gz"
        entries = ("python/python.exe", "python/Lib/os.py", "python/Lib/site-packages/pip/__init__.py",
                   "python/Lib/ensurepip/__init__.py", "python/Scripts/pip.exe")
        with tarfile.open(archive, "w:gz") as stream:
            for name in entries:
                info = tarfile.TarInfo(name)
                info.size = 6
                stream.addfile(info, io.BytesIO(b"python"))
        output = self.root / "unpack"
        prepare.extract_python(archive, output)
        self.assertTrue((output / "python/python.exe").is_file())
        self.assertTrue((output / "python/Lib/os.py").is_file())
        self.assertFalse((output / "python/Lib/site-packages/pip").exists())
        self.assertFalse((output / "python/Lib/ensurepip").exists())
        self.assertFalse((output / "python/Scripts/pip.exe").exists())

    def test_refresh_restores_the_previous_dependency_if_publication_fails(self):
        prepared = self.root / "prepared"
        output = prepared / "mingw/x86_64/python"
        output.mkdir(parents=True)
        (output / "original").write_bytes(b"keep")
        payload = prepared / "staging/python"
        payload.mkdir(parents=True)
        (payload / "replacement").write_bytes(b"new")
        original_replace = prepare.os.replace

        def fail_new_publication(source, destination):
            if source == payload:
                raise OSError("publication blocked")
            return original_replace(source, destination)

        with mock.patch.object(prepare, "PREPARED", prepared), \
             mock.patch.object(prepare.os, "replace", side_effect=fail_new_publication):
            with self.assertRaisesRegex(OSError, "publication blocked"):
                prepare.publish_prepared(payload, output, "x86_64", "mingw", refresh=True)
        self.assertEqual((output / "original").read_bytes(), b"keep")
        self.assertFalse((output / "replacement").exists())

    def test_refresh_publishes_a_complete_replacement_inside_the_prepared_root(self):
        prepared = self.root / "prepared"
        output = prepared / "linux/x86_64/python"
        output.mkdir(parents=True)
        (output / "original").write_bytes(b"old")
        payload = prepared / "staging/python"
        payload.mkdir(parents=True)
        (payload / "replacement").write_bytes(b"new")
        with mock.patch.object(prepare, "PREPARED", prepared):
            prepare.publish_prepared(payload, output, "x86_64", "linux", refresh=True)
        self.assertFalse((output / "original").exists())
        self.assertEqual((output / "replacement").read_bytes(), b"new")
        prepare.verify_prepared(output, "x86_64", platform="linux")


if __name__ == "__main__":
    unittest.main()
