"""CI must publish every expected native installer, not only an archive."""

from pathlib import Path
import tempfile
import unittest

from scripts.verify_distributions import verify_distributions


class DistributionOutputTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()

    def write(self, platform, arch, extension, content=b"package"):
        path = self.root / f"KiCad_Forge-{platform}-{arch}-0.2.0.{extension}"
        path.write_bytes(content)
        return path

    def test_all_four_native_platforms_require_their_installers_and_archives(self):
        for platform, arch, extensions in (("linux", "x86_64", ("deb", "tar.gz", "zip")),
                                            ("mingw", "x86_64", ("exe", "zip")),
                                            ("macosx", "arm64", ("dmg", "zip")),
                                            ("macosx", "x86_64", ("dmg", "zip"))):
            expected = [self.write(platform, arch, extension) for extension in extensions]
            self.assertEqual(verify_distributions(self.root, platform, arch, "0.2.0"), expected)

    def test_linux_archives_without_a_deb_do_not_pass(self):
        self.write("linux", "x86_64", "tar.gz")
        self.write("linux", "x86_64", "zip")
        with self.assertRaisesRegex(FileNotFoundError, "deb"):
            verify_distributions(self.root, "linux", "x86_64", "0.2.0")

    def test_empty_installer_wrong_architecture_or_version_do_not_pass(self):
        self.write("macosx", "arm64", "zip")
        self.write("macosx", "arm64", "dmg", b"")
        self.write("macosx", "x86_64", "dmg")
        with self.assertRaisesRegex(ValueError, "empty"):
            verify_distributions(self.root, "macosx", "arm64", "0.2.0")
        with self.assertRaises(FileNotFoundError):
            verify_distributions(self.root, "macosx", "arm64", "0.3.0")

    def test_unknown_platform_or_injected_version_is_rejected(self):
        for platform, version in (("unknown", "0.2.0"), ("linux", "../0.2.0")):
            with self.assertRaises(ValueError):
                verify_distributions(self.root, platform, "x86_64", version)


if __name__ == "__main__":
    unittest.main()
