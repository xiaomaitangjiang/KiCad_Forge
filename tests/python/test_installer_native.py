"""Execute the real NSIS script using disposable files and unique HKCU keys."""

import ctypes
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

PROJECT = Path(__file__).resolve().parents[2]
NSIS = PROJECT / "build/packaging-deps/mingw/x86_64/nsis/makensis.exe"


@unittest.skipUnless(sys.platform == "win32" and NSIS.is_file(), "Prepared native NSIS compiler required")
class NativeUninstallTests(unittest.TestCase):
    def setUp(self):
        import winreg
        self.winreg = winreg
        self.temporary = tempfile.TemporaryDirectory(prefix="KiForge native uninstall ")
        self.root = Path(self.temporary.name).resolve()
        self.identity = "KiCad_Forge_Test_" + uuid.uuid4().hex
        self.settings = "Software\\" + self.identity
        self.registration = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + self.identity
        self.install = self.root / "installed app with spaces"
        self.addCleanup(self.cleanup)
        source = self.root / "payload"
        for name in ("KiCad_Forge.exe", "runtime.dll", "kforge-distribution.json",
                     "runtime/python/python.exe", "webui/index.html", "config/seed.json",
                     "plugins/test/plugin.py", "licenses/test.txt"):
            file = source / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(b"disposable installer fixture")
        # Compile the production script with isolated product identities. Never
        # run an installer that could overwrite the user's actual registration.
        script = (PROJECT / "packaging/windows/kiforge.nsi").read_text(encoding="utf-8")
        script = script.replace('Software\\KiCad_Forge"', self.settings + '"')
        script = script.replace('Uninstall\\KiCad_Forge"', 'Uninstall\\' + self.identity + '"')
        script = script.replace("KiCad Forge", self.identity)
        script = script.replace('InstallDir "$LOCALAPPDATA\\Programs\\KiCad_Forge"',
                                'InstallDir "' + str(self.install) + '"')
        self.assertNotIn('$LOCALAPPDATA\\Programs\\KiCad_Forge', script)
        self.assertNotIn('Software\\KiCad_Forge"', script)
        self.assertNotIn('Uninstall\\KiCad_Forge"', script)
        nsi = self.root / "test.nsi"
        nsi.write_text(script, encoding="utf-8")
        setup = self.root / "setup.exe"
        subprocess.run([str(NSIS), "/V2", "/DKFORGE_SOURCE=" + str(source),
                        "/DKFORGE_OUTPUT=" + str(setup), "/DKFORGE_VERSION=0.0.0",
                        "/DKFORGE_LICENSE=" + str(PROJECT / "LICENSE"),
                        "/DKFORGE_ICON=" + str(PROJECT / "resources/icons/app.ico"), str(nsi)],
                       check=True, capture_output=True, timeout=60)
        subprocess.run([str(setup), "/S"], cwd=self.root,
                       check=True, capture_output=True, timeout=30)
        self.assertTrue((self.install / "Uninstall.exe").is_file())
        self.assertTrue(self.registered())

    def cleanup(self):
        # Only the UUID-specific test keys and shortcuts are eligible for cleanup.
        reg = self.winreg
        for view in (reg.KEY_WOW64_32KEY, reg.KEY_WOW64_64KEY):
            for key in (self.registration, self.settings):
                try:
                    reg.DeleteKeyEx(reg.HKEY_CURRENT_USER, key, view)
                except FileNotFoundError:
                    pass
        for directory in (Path.home() / "Desktop",
                          Path.home() / "AppData/Roaming/Microsoft/Windows/Start Menu/Programs" / self.identity):
            for name in (self.identity + ".lnk", "Uninstall.lnk"):
                (directory / name).unlink(missing_ok=True)
            if directory.name == self.identity and directory.exists():
                directory.rmdir()
        self.temporary.cleanup()

    def registered(self):
        reg = self.winreg
        for view in (reg.KEY_WOW64_32KEY, reg.KEY_WOW64_64KEY):
            try:
                with reg.OpenKey(reg.HKEY_CURRENT_USER, self.registration, 0, reg.KEY_READ | view):
                    return True
            except FileNotFoundError:
                pass
        return False

    def assert_settings_removed(self):
        reg = self.winreg
        for view in (reg.KEY_WOW64_32KEY, reg.KEY_WOW64_64KEY):
            with self.assertRaises(FileNotFoundError):
                reg.OpenKey(reg.HKEY_CURRENT_USER, self.settings, 0, reg.KEY_READ | view)

    def uninstall(self):
        # _?= disables NSIS's asynchronous self-copy. Running a copy outside the
        # installation also permits checking its real Uninstall.exe synchronously.
        runner = self.root / "uninstall-runner.exe"
        shutil.copy2(self.install / "Uninstall.exe", runner)
        # NSIS requires _?= as an unquoted final tail, including embedded spaces.
        command = subprocess.list2cmdline([str(runner), "/S"]) + " _?=" + str(self.install)
        return subprocess.run(command,
                              cwd=self.install, capture_output=True, timeout=30)

    def test_uninstall_removes_empty_installation_and_registration(self):
        result = self.uninstall()
        self.assertEqual(result.returncode, 0)
        self.assertFalse(self.install.exists(), "An empty installation directory must be removed")
        self.assertFalse(self.registered())
        self.assert_settings_removed()

    def test_locked_file_reports_failure_and_keeps_uninstaller_for_retry(self):
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.CreateFileW(str(self.install / "KiCad_Forge.exe"), 0x80000000, 3, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            result = self.uninstall()
            self.assertNotEqual(result.returncode, 0, "A locked executable must not report successful removal")
            self.assertTrue(self.registered(), "Keep registration so failed removal can be retried")
            self.assertTrue((self.install / "Uninstall.exe").is_file())
        finally:
            kernel.CloseHandle(handle)
        self.assertEqual(self.uninstall().returncode, 0)
        self.assertFalse(self.install.exists())
        self.assertFalse(self.registered())
        self.assert_settings_removed()

    def test_default_uninstall_preserves_portable_data_and_unrelated_files(self):
        data = self.install / "data/meta.db"
        data.parent.mkdir()
        data.write_bytes(b"user database")
        unrelated = self.install / "personal.txt"
        unrelated.write_bytes(b"user file")
        self.assertEqual(self.uninstall().returncode, 0)
        self.assertEqual(data.read_bytes(), b"user database")
        self.assertEqual(unrelated.read_bytes(), b"user file")
        self.assertFalse((self.install / "KiCad_Forge.exe").exists())
        self.assertFalse(self.registered())
        self.assert_settings_removed()


if __name__ == "__main__":
    unittest.main()
