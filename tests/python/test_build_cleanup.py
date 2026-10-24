from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("clean_build", ROOT / "scripts" / "clean_build.py")
assert SPEC and SPEC.loader
clean_build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(clean_build)


class BuildCleanupTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="kf-clean-test-"))
        self.build = self.tmp / "build"
        self.build.mkdir()

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def make_link(self, link: Path, target: Path) -> None:
        try:
            os.symlink(target, link, target_is_directory=True)
            return
        except (OSError, NotImplementedError):
            if os.name != "nt":
                self.fail("directory symlinks are not available on POSIX")
        result = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            self.skipTest("directory symlinks and junctions are not available")

    def posix_lstat(self) -> mock._patch:
        """Model POSIX lstat semantics for links created as Windows junctions."""
        native_lstat = os.lstat

        def as_posix(path, *args, **kwargs):
            result = native_lstat(path, *args, **kwargs)
            if getattr(result, "st_file_attributes", 0) & clean_build.FILE_ATTRIBUTE_REPARSE_POINT:
                return SimpleNamespace(
                    st_mode=stat.S_IFLNK | stat.S_IMODE(result.st_mode),
                    st_size=result.st_size,
                    st_ino=result.st_ino,
                    st_dev=result.st_dev,
                )
            return result

        return mock.patch.object(clean_build.os, "lstat", side_effect=as_posix)

    def manifest(self, *entries: dict) -> dict:
        return {
            "schema": 1,
            "reviewed_at": "2026-10-23",
            "entries": list(entries),
        }

    def entry(self, relative: str, category: str = "packages") -> dict:
        return {
            "path": relative,
            "category": category,
            "purpose": "synthetic reviewed fixture",
            "data": "archived",
            "recovery": "fixture can be recreated from source",
            "tree_sha256": clean_build.tree_fingerprint(self.build / relative),
        }

    def run_main(self, *argv: str) -> tuple[int, str]:
        output = io.StringIO()
        with mock.patch.object(clean_build, "BUILD", self.build), \
                mock.patch.object(clean_build, "busy_processes", return_value=[]), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            try:
                code = clean_build.main(list(argv))
            except SystemExit as exc:
                code = int(exc.code or 0)
        return code, output.getvalue()

    def test_preview_does_not_modify_the_tree(self) -> None:
        target = self.build / ".build_cache" / "payload.bin"
        target.parent.mkdir()
        target.write_bytes(b"cache")
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(self.build, ["cache"], apply=False)
        self.assertTrue(target.exists())
        self.assertFalse(report["apply"])
        self.assertEqual(report["targets"][0]["before"]["files"], 1)
        self.assertEqual(report["targets"][0]["before"]["bytes"], 5)

    def test_posix_symlink_mode_is_recognized_without_windows_attributes(self) -> None:
        st = SimpleNamespace(st_mode=stat.S_IFLNK | 0o777)
        self.assertTrue(clean_build.is_reparse(st))

    def test_outside_build_is_rejected(self) -> None:
        outside = self.tmp / "outside"
        outside.mkdir()
        with self.assertRaises(clean_build.CleanupError):
            clean_build.validate_target(outside, self.build)

    def test_build_root_and_runtime_data_are_protected(self) -> None:
        (self.build / "mingw" / "data").mkdir(parents=True)
        with self.assertRaises(clean_build.CleanupError):
            clean_build.validate_target(self.build, self.build)
        with self.assertRaises(clean_build.CleanupError):
            clean_build.validate_target(self.build / "mingw", self.build)
        with self.assertRaises(clean_build.CleanupError):
            clean_build.validate_target(self.build / "mingw" / "data", self.build)

    def test_unknown_package_is_retained_without_a_reviewed_entry(self) -> None:
        package = self.build / "ci-dist" / "KiCad_Forge-current-2026-10-23"
        payload = package / "data" / "meta.db"
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b"unreviewed runtime data")
        manifest = self.manifest()
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(self.build, ["packages"], apply=True, manifest=manifest)
        self.assertTrue(payload.exists())
        self.assertNotIn(str(package), [item["path"] for item in report["targets"]])
        self.assertTrue(any("KiCad_Forge-current" in item for item in report["retained"]))

    def test_reviewed_package_is_removed(self) -> None:
        package = self.build / "ci-dist" / "old-package"
        payload = package / "data" / "meta.db"
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b"reviewed fixture")
        manifest = self.manifest(self.entry("ci-dist/old-package"))
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(self.build, ["packages"], apply=True, manifest=manifest)
        self.assertFalse(package.exists())
        self.assertTrue(report["targets"][0]["removed"])

    def test_reviewed_package_zip_is_removed(self) -> None:
        archive = self.build / "ci-dist" / "old-package.zip"
        archive.parent.mkdir(parents=True)
        archive.write_bytes(b"reviewed zip fixture")
        manifest = self.manifest(self.entry("ci-dist/old-package.zip"))
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(self.build, ["packages"], apply=True, manifest=manifest)
        self.assertFalse(archive.exists())
        self.assertTrue(report["targets"][0]["removed"])

    def test_package_changed_after_review_is_not_removed(self) -> None:
        package = self.build / "ci-dist" / "old-package"
        payload = package / "data" / "meta.db"
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b"reviewed fixture")
        manifest = self.manifest(self.entry("ci-dist/old-package"))
        (package / "data" / "new-user-file.db").write_bytes(b"new unreviewed data")
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(self.build, ["packages"], apply=True, manifest=manifest)
        self.assertTrue(payload.exists())
        self.assertFalse(report["targets"][0].get("removed", False))
        self.assertTrue(report["targets"][0].get("error") or report["targets"][0].get("errors"))

    def test_invalid_or_missing_manifest_does_not_delete(self) -> None:
        package = self.build / "ci-dist" / "old-package"
        payload = package / "data" / "meta.db"
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b"reviewed fixture")
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            with self.assertRaises(clean_build.CleanupError):
                clean_build.cleanup(
                    self.build,
                    ["packages"],
                    apply=True,
                    manifest={"schema": 1, "entries": [{"path": "ci-dist/old-package"}]},
                )
            with self.assertRaises(clean_build.CleanupError):
                clean_build.cleanup(
                    self.build,
                    ["packages"],
                    apply=True,
                    manifest_path=self.tmp / "missing-manifest.json",
                )
        self.assertTrue(payload.exists())

    def test_snapshot_also_requires_a_reviewed_entry(self) -> None:
        snapshot = self.build / "commit-preparation-2026-10-21"
        payload = snapshot / "state.json"
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b"{\"reviewed\": false}")
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(self.build, ["snapshot"], apply=True, manifest=self.manifest())
        self.assertTrue(payload.exists())
        self.assertTrue(any("commit-preparation" in item for item in report["retained"]))
        manifest = self.manifest(self.entry("commit-preparation-2026-10-21", category="snapshot"))
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            clean_build.cleanup(self.build, ["snapshot"], apply=True, manifest=manifest)
        self.assertFalse(snapshot.exists())

    def test_exact_apply_removes_only_the_candidate(self) -> None:
        cache = self.build / ".build_cache" / "payload.bin"
        cache.parent.mkdir()
        cache.write_bytes(b"cache")
        keep = self.build / "mingw" / "data" / "meta.db"
        keep.parent.mkdir(parents=True)
        keep.write_bytes(b"user-data")
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(self.build, ["cache"], apply=True)
        self.assertFalse(cache.exists())
        self.assertTrue(keep.exists())
        self.assertTrue(report["targets"][0]["removed"])

    def test_nested_reparse_point_is_unlinked_without_touching_its_target(self) -> None:
        outside = self.tmp / "outside"
        outside.mkdir()
        payload = outside / "keep.txt"
        payload.write_text("keep", encoding="utf-8")
        cache = self.build / ".build_cache"
        cache.mkdir()
        link = cache / "link"
        self.make_link(link, outside)
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            clean_build.cleanup(self.build, ["cache"], apply=True)
        self.assertFalse(os.path.lexists(link))
        self.assertTrue(payload.exists())

    def test_posix_build_root_symlink_is_rejected(self) -> None:
        real_build = self.tmp / "real-build"
        payload = real_build / ".build_cache" / "keep.txt"
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b"keep")
        alias = self.tmp / "build-alias"
        self.make_link(alias, real_build)
        with self.posix_lstat(), mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(alias, ["cache"], apply=True)
        self.assertTrue(os.path.lexists(alias))
        self.assertTrue(payload.exists())
        self.assertTrue(report["errors"])

    def test_posix_cache_root_symlink_is_rejected(self) -> None:
        destination = self.build / "unclassified-data"
        destination.mkdir()
        payload = destination / "keep.txt"
        payload.write_bytes(b"keep")
        alias = self.build / ".build_cache"
        self.make_link(alias, destination)
        with self.posix_lstat(), mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(self.build, ["cache"], apply=True)
        self.assertTrue(os.path.lexists(alias))
        self.assertTrue(payload.exists())
        self.assertTrue(report["errors"])

    def test_posix_ci_dist_ancestor_symlink_is_rejected(self) -> None:
        destination = self.tmp / "outside-ci"
        package = destination / "old-package"
        payload = package / "data" / "meta.db"
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b"outside package")
        ci = self.build / "ci-dist"
        self.make_link(ci, destination)
        with self.posix_lstat(), mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(
                self.build,
                ["packages"],
                apply=True,
                manifest=self.manifest(self.entry("ci-dist/old-package")),
            )
        self.assertTrue(os.path.lexists(ci))
        self.assertTrue(payload.exists())
        self.assertTrue(report["errors"])

    def test_cache_root_junction_is_rejected_and_destination_is_preserved(self) -> None:
        destination = self.build / "unclassified-data"
        destination.mkdir()
        payload = destination / "keep.txt"
        payload.write_bytes(b"review fixture only")
        alias = self.build / ".build_cache"
        self.make_link(alias, destination)
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(self.build, ["cache"], apply=True)
        self.assertTrue(os.path.lexists(alias))
        self.assertTrue(payload.exists())
        self.assertTrue(report["targets"][0].get("error"))

    def test_build_root_junction_is_rejected(self) -> None:
        real_build = self.tmp / "real-build"
        real_build.mkdir()
        payload = real_build / ".build_cache" / "keep.txt"
        payload.parent.mkdir()
        payload.write_bytes(b"keep")
        alias = self.tmp / "build-alias"
        self.make_link(alias, real_build)
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(alias, ["cache"], apply=True)
        self.assertTrue(payload.exists())
        self.assertTrue(report["targets"][0].get("error"))

    def test_ci_dist_ancestor_junction_is_rejected(self) -> None:
        destination = self.tmp / "outside-ci"
        package = destination / "old-package"
        payload = package / "data" / "meta.db"
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b"outside package")
        ci = self.build / "ci-dist"
        self.make_link(ci, destination)
        manifest = self.manifest(self.entry("ci-dist/old-package"))
        with mock.patch.object(clean_build, "busy_processes", return_value=[]):
            report = clean_build.cleanup(self.build, ["packages"], apply=True, manifest=manifest)
        self.assertTrue(payload.exists())
        self.assertTrue(report["targets"][0].get("error"))

    def test_apply_without_categories_is_rejected_without_deleting(self) -> None:
        cache = self.build / ".build_cache" / "item"
        cache.parent.mkdir()
        cache.write_bytes(b"review fixture cache")
        package = self.build / "ci-dist" / "new-package" / "data" / "meta.db"
        package.parent.mkdir(parents=True)
        package.write_bytes(b"review fixture data")
        code, output = self.run_main("--apply", "--yes")
        self.assertNotEqual(code, 0, output)
        self.assertTrue(cache.exists())
        self.assertTrue(package.exists())

    def test_all_mixed_or_unknown_categories_are_rejected(self) -> None:
        cache = self.build / ".build_cache" / "item"
        cache.parent.mkdir()
        cache.write_bytes(b"review fixture cache")
        for categories in ("all,cache", "cache,typo"):
            code, output = self.run_main("--apply", "--yes", "--categories", categories)
            self.assertNotEqual(code, 0, output)
            self.assertTrue(cache.exists())

    def test_cli_uses_explicit_manifest_and_package_category(self) -> None:
        package = self.build / "ci-dist" / "old-package"
        payload = package / "data" / "meta.db"
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b"reviewed fixture")
        manifest_path = self.tmp / "manifest.json"
        manifest_path.write_text(
            __import__("json").dumps(self.manifest(self.entry("ci-dist/old-package"))),
            encoding="utf-8",
        )
        code, output = self.run_main(
            "--apply",
            "--yes",
            "--categories",
            "packages",
            "--manifest",
            str(manifest_path),
        )
        self.assertEqual(code, 0, output)
        self.assertFalse(package.exists())

    def test_cli_fingerprint_prints_the_same_tree_digest(self) -> None:
        package = self.build / "ci-dist" / "old-package"
        payload = package / "data" / "meta.db"
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b"reviewed fixture")
        code, output = self.run_main("--fingerprint", str(package))
        self.assertEqual(code, 0, output)
        self.assertEqual(output.strip(), clean_build.tree_fingerprint(package))

    def test_explicit_cache_apply_does_not_touch_unknown_packages(self) -> None:
        cache = self.build / ".build_cache" / "item"
        cache.parent.mkdir()
        cache.write_bytes(b"review fixture cache")
        payload = self.build / "ci-dist" / "new-package" / "data" / "meta.db"
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b"review fixture data")
        code, output = self.run_main("--apply", "--yes", "--categories", "cache")
        self.assertEqual(code, 0, output)
        self.assertFalse(cache.exists())
        self.assertTrue(payload.exists())

    def test_nsis_compiler_blocks_cleanup_without_deleting_build_files(self) -> None:
        cache = self.build / ".build_cache" / "item"
        cache.parent.mkdir()
        cache.write_bytes(b"active compiler input")
        tasklist = subprocess.CompletedProcess(
            ["tasklist"], 0, '"MakeNSIS.exe","123","Console","1","1024 K"\n', "")
        with mock.patch.object(clean_build.os, "name", "nt"), \
             mock.patch.object(clean_build.subprocess, "run", return_value=tasklist):
            running = clean_build.busy_processes()
        self.assertEqual(running, ["makensis.exe"])
        with mock.patch.object(clean_build, "busy_processes", return_value=running):
            with self.assertRaisesRegex(RuntimeError, "makensis.exe"):
                clean_build.cleanup(self.build, ["cache"], apply=True)
        self.assertEqual(cache.read_bytes(), b"active compiler input")


if __name__ == "__main__":
    unittest.main()
