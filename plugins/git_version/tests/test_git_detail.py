import os
import sys
import unittest
from unittest import mock

PLUGIN_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if PLUGIN_DIR not in sys.path:
    sys.path.insert(0, PLUGIN_DIR)

import plugin
from git_fixture import GitFixture


def same_path(left, right):
    return os.path.normcase(os.path.realpath(left)) == os.path.normcase(os.path.realpath(right))


class CommitDetailTest(unittest.TestCase):
    def setUp(self):
        self.repo = GitFixture()
        self.addCleanup(self.repo.close)
        self.root = self.repo.commit(
            "root: 初始版本\n\n第二行 & 符号",
            {
                "a.txt": "one\n",
                "[bracket].txt": "bracket\n",
                "old.txt": "alpha\nbeta\ngamma\n",
                "bin.dat": b"\x00\x01\x02\xff",
                "符号/引脚.kicad_sym": "sym\n",
            })
        self.repo.remove("old.txt")
        self.renamed = self.repo.commit(
            "rename and edit\n\nrenamed body",
            {
                "new name ü.txt": "alpha\nbeta\ngamma\ndelta\n",
                "[bracket].txt": "bracket changed\n",
                "a.txt": "one\ntwo\n",
            })

    def detail(self, oid, **extra):
        args = {"target_library": self.repo.path, "oid": oid}
        args.update(extra)
        return plugin.action_commit_detail(args)

    def file_patch(self, oid, path, **extra):
        args = {"target_library": self.repo.path, "oid": oid, "path": path}
        args.update(extra)
        return plugin.action_commit_file(args)

    def test_root_commit_metadata_and_files(self):
        result = self.detail(self.root)
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["schema_version"], "graph_v1")
        self.assertTrue(same_path(result["repository_identity"], self.repo.path))
        self.assertEqual(result["diff_base"], None)
        self.assertFalse(result["files_truncated"])
        self.assertFalse(result["body_truncated"])

        commit = result["commit"]
        self.assertEqual(commit["oid"], self.root)
        self.assertEqual(commit["parents"], [])
        self.assertEqual(commit["subject"], "root: 初始版本")
        self.assertEqual(commit["body"], "第二行 & 符号")
        self.assertEqual(commit["author_name"], "Test Author")
        self.assertEqual(commit["author_email"], "author@example.com")
        self.assertEqual(commit["committer_name"], "Test Committer")
        self.assertEqual(commit["committer_email"], "committer@example.com")
        self.assertTrue(commit["author_time"].startswith("2020-"))
        self.assertTrue(commit["committer_time"].startswith("2020-"))
        self.assertEqual(set(commit), {
            "oid", "parents", "subject", "body", "author_name",
            "author_email", "author_time", "committer_name",
            "committer_email", "committer_time"})

        files = {entry["path"]: entry for entry in result["files"]}
        self.assertEqual(set(files), {
            "a.txt", "[bracket].txt", "old.txt", "bin.dat",
            "符号/引脚.kicad_sym"})
        self.assertEqual(files["a.txt"]["status"], "A")
        self.assertEqual(files["a.txt"]["additions"], 1)
        self.assertEqual(files["a.txt"]["deletions"], 0)
        self.assertFalse(files["a.txt"]["binary"])
        self.assertIsNone(files["a.txt"]["old_path"])
        self.assertTrue(files["bin.dat"]["binary"])
        self.assertIsNone(files["bin.dat"]["additions"])
        self.assertIsNone(files["bin.dat"]["deletions"])
        for entry in result["files"]:
            self.assertEqual(set(entry), {
                "path", "old_path", "status", "additions", "deletions",
                "binary"})

    def test_rename_detail_and_patch_both_paths(self):
        result = self.detail(self.renamed)
        self.assertTrue(result["ok"], result)
        files = {entry["path"]: entry for entry in result["files"]}
        renamed = files["new name ü.txt"]
        self.assertEqual(renamed["status"], "R")
        self.assertEqual(renamed["old_path"], "old.txt")
        self.assertEqual(renamed["additions"], 1)
        self.assertEqual(renamed["deletions"], 0)
        self.assertFalse(renamed["binary"])

        patch = self.file_patch(self.renamed, "new name ü.txt")
        self.assertTrue(patch["ok"], patch)
        self.assertEqual(patch["path"], "new name ü.txt")
        self.assertEqual(patch["old_path"], "old.txt")
        self.assertEqual(patch["status"], "R")
        self.assertTrue(patch["diff_base"])
        self.assertFalse(patch["binary"])
        self.assertFalse(patch["truncated"])
        self.assertIn("rename from old.txt", patch["patch"])
        self.assertIn("rename to new name ü.txt", patch["patch"])
        self.assertIn("+delta", patch["patch"])
        self.assertNotIn("new file mode", patch["patch"])

        old_request = self.file_patch(self.renamed, "old.txt")
        self.assertTrue(old_request["ok"], old_request)
        self.assertEqual(old_request["path"], "new name ü.txt")
        self.assertEqual(old_request["old_path"], "old.txt")
        self.assertIn("rename from old.txt", old_request["patch"])

    def test_binary_and_unicode_patches(self):
        binary = self.file_patch(self.root, "bin.dat")
        self.assertTrue(binary["ok"], binary)
        self.assertTrue(binary["binary"])
        self.assertIn("bin.dat", binary["patch"])

        unicode_file = self.file_patch(self.root, "符号/引脚.kicad_sym")
        self.assertTrue(unicode_file["ok"], unicode_file)
        self.assertFalse(unicode_file["binary"])
        self.assertIn("符号/引脚.kicad_sym", unicode_file["patch"])
        self.assertIn("+sym", unicode_file["patch"])

    def test_literal_pathspec_for_bracket_file(self):
        result = self.file_patch(self.renamed, "[bracket].txt")
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["patch"])
        self.assertIn("--- a/[bracket].txt", result["patch"])
        self.assertIn("+bracket changed", result["patch"])

    def test_merge_first_parent_and_explicit_parent(self):
        merge_repo = GitFixture()
        self.addCleanup(merge_repo.close)
        base = merge_repo.commit("base", {"base.txt": "base\n"})
        merge_repo.run("checkout", "-q", "-b", "feature")
        feature_commit = merge_repo.commit("feature", {"feature.txt": "feature\n"})
        merge_repo.run("checkout", "-q", "main")
        main_commit = merge_repo.commit("main", {"main.txt": "main\n"})
        merge_repo.run("merge", "-q", "--no-ff", "-m",
                       "Merge branch 'feature'", "feature")
        merge_oid = merge_repo.head()

        first = plugin.action_commit_detail({
            "target_library": merge_repo.path, "oid": merge_oid})
        self.assertTrue(first["ok"], first)
        self.assertEqual(first["commit"]["parents"], [main_commit, feature_commit])
        self.assertEqual(first["diff_base"], main_commit)
        first_paths = {entry["path"] for entry in first["files"]}
        self.assertIn("feature.txt", first_paths)
        self.assertNotIn("main.txt", first_paths)

        second = plugin.action_commit_detail({
            "target_library": merge_repo.path, "oid": merge_oid,
            "parent_index": 1})
        self.assertTrue(second["ok"], second)
        self.assertEqual(second["diff_base"], feature_commit)
        second_paths = {entry["path"] for entry in second["files"]}
        self.assertIn("main.txt", second_paths)
        self.assertNotIn("feature.txt", second_paths)

        explicit = plugin.action_commit_detail({
            "target_library": merge_repo.path, "oid": merge_oid,
            "parent": feature_commit.upper()})
        self.assertTrue(explicit["ok"], explicit)
        self.assertEqual(explicit["diff_base"], feature_commit)
        self.assertEqual(explicit["files"], second["files"])

        file_second = plugin.action_commit_file({
            "target_library": merge_repo.path, "oid": merge_oid,
            "path": "main.txt", "parent_index": 1})
        self.assertTrue(file_second["ok"], file_second)
        self.assertEqual(file_second["diff_base"], feature_commit)
        self.assertIn("+main", file_second["patch"])

        missing = plugin.action_commit_file({
            "target_library": merge_repo.path, "oid": merge_oid,
            "path": "feature.txt", "parent_index": 1})
        self.assertFalse(missing["ok"])
        self.assertEqual(missing["code"], "unknown_path")

        for bad in (2, -1, True, "1", 1.0):
            invalid = plugin.action_commit_detail({
                "target_library": merge_repo.path, "oid": merge_oid,
                "parent_index": bad})
            self.assertFalse(invalid["ok"], invalid)
            self.assertEqual(invalid["code"], "invalid_parent", invalid)
        unknown = plugin.action_commit_detail({
            "target_library": merge_repo.path, "oid": merge_oid,
            "parent": "deadbeef"})
        self.assertEqual(unknown["code"], "unknown_parent")
        bad_oid = plugin.action_commit_detail({
            "target_library": merge_repo.path, "oid": merge_oid,
            "parent": "not-an-oid"})
        self.assertEqual(bad_oid["code"], "invalid_parent")

        root_detail = plugin.action_commit_detail({
            "target_library": merge_repo.path, "oid": base})
        self.assertIsNone(root_detail["diff_base"])
        self.assertEqual(root_detail["commit"]["parents"], [])

    def test_unknown_oid_and_path_errors(self):
        self.assertEqual(self.detail(None)["code"], "invalid_oid")
        self.assertEqual(self.detail("zzzz")["code"], "invalid_oid")
        self.assertEqual(self.detail("dead")["code"], "unknown_commit")
        self.assertEqual(self.detail(oid="a" * 64)["code"],
                         "unknown_commit")
        self.assertEqual(self.file_patch(self.root, "missing.txt")["code"],
                         "unknown_path")
        self.assertEqual(self.file_patch("zzzz", "a.txt")["code"],
                         "invalid_oid")
        self.assertEqual(self.file_patch(None, "a.txt")["code"],
                         "invalid_oid")

    def test_invalid_request_shapes(self):
        self.assertEqual(plugin.action_commit_detail(None)["code"],
                         "invalid_request")
        self.assertEqual(plugin.action_commit_file(None)["code"],
                         "invalid_request")
        for bad in ("", None, "a\x00b", "x" * 4097):
            result = self.file_patch(self.root, bad)
            self.assertFalse(result["ok"], result)
            self.assertEqual(result["code"], "invalid_request", result)

    def test_body_and_files_and_patch_truncation(self):
        body_commit = self.repo.commit(
            "subject line\n\n" + "x" * 50, {"body.txt": "body\n"})
        with mock.patch.object(plugin, "MAX_MESSAGE_CHARS", 10):
            result = self.detail(body_commit)
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["body_truncated"])
        self.assertEqual(result["commit"]["body"], "x" * 10)

        many = self.repo.commit("many files", {
            "f1.txt": "1\n", "f2.txt": "2\n", "f3.txt": "3\n"})
        with mock.patch.object(plugin, "MAX_DETAIL_FILES", 1):
            result = self.detail(many)
        self.assertTrue(result["ok"], result)
        self.assertEqual(len(result["files"]), 1)
        self.assertTrue(result["files_truncated"])

        self.repo.write("big.txt", "line\n" * 2000)
        big = self.repo.commit("big", {"big.txt": "line\n" * 2000})
        with mock.patch.object(plugin, "MAX_PATCH_BYTES", 128):
            result = self.file_patch(big, "big.txt")
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["truncated"])
        self.assertIn("patch 已截断", result["patch"])
        self.assertIn("128", result["patch"])

    def test_commit_file_patch_is_plain_text_and_parent_metadata(self):
        result = self.file_patch(self.renamed, "a.txt")
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["oid"], self.renamed)
        self.assertEqual(result["diff_base"], self.root)
        self.assertIn("--- a/a.txt", result["patch"])
        self.assertIn("+++ b/a.txt", result["patch"])
        self.assertIn("+two", result["patch"])
        self.assertFalse(result["binary"])


if __name__ == "__main__":
    unittest.main()
