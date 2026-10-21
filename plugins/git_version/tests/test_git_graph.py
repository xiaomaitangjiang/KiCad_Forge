import json
import os
import sys
import tempfile
import unittest
from unittest import mock

PLUGIN_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if PLUGIN_DIR not in sys.path:
    sys.path.insert(0, PLUGIN_DIR)

import plugin
from git_fixture import GitFixture


def same_path(left, right):
    return os.path.normcase(os.path.realpath(left)) == os.path.normcase(os.path.realpath(right))


class GraphHistoryTest(unittest.TestCase):
    def setUp(self):
        self.repo = GitFixture()
        self.addCleanup(self.repo.close)

    def graph(self, **extra):
        args = {"target_library": self.repo.path, "format": "graph_v1"}
        args.update(extra)
        return plugin.action_history(args)

    def test_linear_history_refs_and_legacy_output(self):
        first = self.repo.commit("first", {"a.txt": "a\n"})
        second = self.repo.commit("feat: 引脚 → 焊盘", {"a.txt": "a\nb\n"})
        third = self.repo.commit("third", {"b.txt": "b\n"})

        result = self.graph()
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["schema_version"], "graph_v1")
        self.assertEqual(result["state"], "ready")
        self.assertTrue(same_path(result["repository_identity"], self.repo.path))
        self.assertEqual(result["head_oid"], third)
        self.assertEqual(result["head_ref"], "refs/heads/main")
        self.assertFalse(result["detached"])
        self.assertEqual(result["total"], 3)
        self.assertFalse(result["truncated"])
        self.assertFalse(result["page"]["has_more"])
        self.assertIsNone(result["page"]["next_cursor"])
        self.assertTrue(result["page"]["snapshot_id"])

        commits = result["commits"]
        self.assertEqual([item["oid"] for item in commits], [third, second, first])
        self.assertEqual(commits[0]["parents"], [second])
        self.assertEqual(commits[1]["parents"], [first])
        self.assertEqual(commits[2]["parents"], [])
        self.assertEqual(commits[1]["subject"], "feat: 引脚 → 焊盘")
        self.assertEqual(commits[0]["author_time"], self.repo.git_value(third, "%aI"))
        self.assertEqual(commits[0]["committer_time"], self.repo.git_value(third, "%cI"))
        self.assertTrue(commits[0]["author_time"].startswith("2020-"))
        self.assertEqual(set(commits[0]), {
            "oid", "parents", "subject", "author_name",
            "author_time", "committer_time"})

        refs = result["refs"]
        self.assertEqual(len(refs), 1)
        self.assertEqual(refs[0]["name"], "main")
        self.assertEqual(refs[0]["full_name"], "refs/heads/main")
        self.assertEqual(refs[0]["kind"], "local")
        self.assertEqual(refs[0]["target_oid"], third)
        self.assertTrue(refs[0]["is_head"])
        self.assertFalse(result["refs_truncated"])

        legacy = plugin.action_history({"target_library": self.repo.path})
        self.assertEqual(legacy, plugin.do_history(self.repo.path))
        self.assertIn("output", legacy)
        self.assertIn("first", legacy["output"])
        self.assertEqual(plugin.action_history(
            {"target_library": self.repo.path, "format": "text"}), legacy)
        self.assertEqual(plugin.action_history(
            {"target_library": self.repo.path, "format": "legacy"}), legacy)

    def test_unborn_repository_is_not_a_load_error(self):
        result = self.graph()
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["state"], "unborn")
        self.assertIsNone(result["head_oid"])
        self.assertEqual(result["head_ref"], "refs/heads/main")
        self.assertFalse(result["detached"])
        self.assertEqual(result["commits"], [])
        self.assertEqual(result["refs"], [])
        self.assertEqual(result["total"], 0)
        self.assertFalse(result["page"]["has_more"])
        self.assertIsNone(result["page"]["next_cursor"])

    def test_uninitialized_and_missing_target(self):
        with tempfile.TemporaryDirectory() as empty:
            result = plugin.action_history({
                "target_library": empty, "format": "graph_v1"})
            self.assertFalse(result["ok"])
            self.assertEqual(result["code"], "not_initialized")
            detail = plugin.action_commit_detail({
                "target_library": empty, "oid": "a" * 40})
            self.assertEqual(detail["code"], "not_initialized")

            missing = os.path.join(empty, "missing")
            result = plugin.action_history({
                "target_library": missing, "format": "graph_v1"})
            self.assertEqual(result["code"], "invalid_target")

    def test_tags_remote_refs_and_sorted_public_shape(self):
        first = self.repo.commit("first", {"a.txt": "a\n"})
        second = self.repo.commit("second", {"a.txt": "a\nb\n"})
        self.repo.run("tag", "-a", "v1", "-m", "release one", first)
        self.repo.run("tag", "light", second)
        self.repo.run("update-ref", "refs/remotes/origin/main", second)

        result = self.graph()
        self.assertTrue(result["ok"], result)
        refs = {ref["name"]: ref for ref in result["refs"]}
        self.assertEqual(refs["v1"]["kind"], "tag")
        self.assertEqual(refs["v1"]["target_oid"], first)
        self.assertEqual(refs["light"]["kind"], "tag")
        self.assertEqual(refs["light"]["target_oid"], second)
        self.assertEqual(refs["origin/main"]["kind"], "remote")
        self.assertEqual(refs["origin/main"]["target_oid"], second)
        self.assertEqual(refs["main"]["kind"], "local")
        self.assertTrue(refs["main"]["is_head"])
        for ref in result["refs"]:
            self.assertEqual(set(ref), {
                "name", "full_name", "kind", "target_oid", "is_head"})

    def test_detached_head(self):
        first = self.repo.commit("first", {"a.txt": "a\n"})
        second = self.repo.commit("second", {"a.txt": "a\nb\n"})
        self.repo.run("checkout", "-q", "--detach", first)

        result = self.graph()
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["detached"])
        self.assertIsNone(result["head_ref"])
        self.assertEqual(result["head_oid"], first)
        self.assertEqual(result["refs"][0]["target_oid"], second)
        all_oids = [item["oid"] for item in result["commits"]]
        self.assertIn(first, all_oids)
        self.assertIn(second, all_oids)
        branch = self.graph(range="branch")
        self.assertEqual(branch["commits"][0]["oid"], first)
        self.assertEqual([item["oid"] for item in branch["commits"]], [first])

    def test_branch_range_filters_other_branches(self):
        base = self.repo.commit("base", {"base.txt": "base\n"})
        self.repo.run("checkout", "-q", "-b", "feature")
        feature = self.repo.commit("feature", {"feature.txt": "feature\n"})
        self.repo.run("checkout", "-q", "main")
        main = self.repo.commit("main", {"main.txt": "main\n"})

        everyone = self.graph()
        self.assertEqual({item["oid"] for item in everyone["commits"]},
                         {base, feature, main})
        self.assertEqual(len(everyone["commits"]), 3)

        only_feature = self.graph(range="branch", branch_ref="feature")
        self.assertEqual([item["oid"] for item in only_feature["commits"]],
                         [feature, base])
        only_feature_full = self.graph(range="branch",
                                       branch_ref="refs/heads/feature")
        self.assertEqual(only_feature_full["commits"], only_feature["commits"])

    def test_merge_commit_keeps_all_parents(self):
        base = self.repo.commit("base", {"base.txt": "base\n"})
        self.repo.run("checkout", "-q", "-b", "feature")
        feature = self.repo.commit("feature", {"feature.txt": "feature\n"})
        self.repo.run("checkout", "-q", "main")
        main = self.repo.commit("main work", {"main.txt": "main\n"})
        self.repo.run("merge", "-q", "--no-ff", "-m", "Merge branch 'feature'",
                      "feature")
        merge = self.repo.head()

        result = self.graph()
        self.assertEqual(result["commits"][0]["oid"], merge)
        entry = {item["oid"]: item for item in result["commits"]}[merge]
        self.assertEqual(entry["parents"], [main, feature])
        self.assertEqual(len(entry["parents"]), 2)
        self.assertEqual(entry["subject"], "Merge branch 'feature'")
        self.assertEqual({item["oid"] for item in result["commits"]},
                         {base, feature, main, merge})

    def test_pagination_is_stable_and_deduplicated(self):
        oids = [self.repo.commit("c%d" % index, {"n.txt": "%d\n" % index})
                for index in range(7)]

        first = self.graph(limit=3)
        self.assertTrue(first["ok"], first)
        self.assertEqual(len(first["commits"]), 3)
        self.assertTrue(first["page"]["has_more"])
        self.assertIsNotNone(first["page"]["next_cursor"])
        self.assertEqual(first["total"], 7)

        second = self.graph(limit=3, cursor=first["page"]["next_cursor"])
        self.assertTrue(second["ok"], second)
        self.assertEqual(len(second["commits"]), 3)
        self.assertTrue(second["page"]["has_more"])
        self.assertEqual(second["page"]["snapshot_id"],
                         first["page"]["snapshot_id"])

        third = self.graph(limit=3, cursor=second["page"]["next_cursor"])
        self.assertEqual(len(third["commits"]), 1)
        self.assertFalse(third["page"]["has_more"])
        self.assertIsNone(third["page"]["next_cursor"])

        combined = (first["commits"] + second["commits"] + third["commits"])
        combined_oids = [item["oid"] for item in combined]
        self.assertEqual(len(combined_oids), 7)
        self.assertEqual(len(set(combined_oids)), 7)
        self.assertEqual(set(combined_oids), set(oids))

        complete = self.graph(limit=50)
        self.assertEqual(combined_oids,
                         [item["oid"] for item in complete["commits"]])
        again = self.graph(limit=3)
        self.assertEqual(again["commits"], first["commits"])
        self.assertEqual(again["page"], first["page"])

    def test_stale_and_invalid_cursors(self):
        for index in range(4):
            self.repo.commit("c%d" % index, {"n.txt": "%d\n" % index})

        cursor = self.graph(limit=2)["page"]["next_cursor"]
        self.repo.commit("new head", {"new.txt": "new\n"})
        stale = self.graph(limit=2, cursor=cursor)
        self.assertFalse(stale["ok"])
        self.assertEqual(stale["code"], "stale_cursor")

        cursor = self.graph(limit=2)["page"]["next_cursor"]
        self.repo.run("tag", "later", self.repo.head())
        stale = self.graph(limit=2, cursor=cursor)
        self.assertEqual(stale["code"], "stale_cursor")

        cursor = self.graph(range="all", limit=2)["page"]["next_cursor"]
        stale = self.graph(range="branch", branch_ref="main", limit=2,
                           cursor=cursor)
        self.assertEqual(stale["code"], "stale_cursor")

        for bad in ("not-a-cursor", "", 123, []):
            result = self.graph(cursor=bad)
            self.assertFalse(result["ok"], result)
            self.assertEqual(result["code"], "invalid_cursor", result)

    def test_limit_validation_and_cap(self):
        self.repo.commit("one", {"a.txt": "a\n"})
        for bad in (0, -1, True, False, "2", 2.5, []):
            result = self.graph(limit=bad)
            self.assertFalse(result["ok"], result)
            self.assertEqual(result["code"], "invalid_limit", result)
        capped = self.graph(limit=10000)
        self.assertTrue(capped["ok"], capped)
        self.assertEqual(capped["limit"], plugin.MAX_PAGE_LIMIT)
        default = self.graph()
        self.assertEqual(default["limit"], plugin.DEFAULT_PAGE_LIMIT)

    def test_commit_and_refs_caps(self):
        for index in range(4):
            self.repo.commit("c%d" % index, {"n.txt": "%d\n" % index})
        self.repo.run("tag", "v1")
        self.repo.run("update-ref", "refs/remotes/origin/main", self.repo.head())

        with mock.patch.object(plugin, "MAX_GRAPH_COMMITS", 2):
            result = self.graph()
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["truncated"])
        self.assertEqual(result["total"], 2)
        self.assertEqual(len(result["commits"]), 2)
        self.assertFalse(result["page"]["has_more"])

        with mock.patch.object(plugin, "MAX_REFS", 2):
            result = self.graph()
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["refs_truncated"])
        self.assertEqual(len(result["refs"]), 2)
        self.assertEqual([ref["name"] for ref in result["refs"]],
                         ["main", "origin/main"])

    def test_unicode_and_separator_subjects(self):
        subject = "feat: 引脚\t→ 焊盘 | 分隔"
        self.repo.commit(subject, {"a.txt": "a\n"})
        result = self.graph()
        self.assertEqual(result["commits"][0]["subject"], subject)
        encoded = json.dumps(result, ensure_ascii=False)
        self.assertEqual(json.loads(encoded)["commits"][0]["subject"], subject)

    def test_error_mapping_and_git_unavailable(self):
        self.repo.commit("one", {"a.txt": "a\n"})
        with mock.patch.object(plugin, "_git_bytes",
                               return_value=(128, b"", "fatal: permission denied")):
            result = self.graph()
        self.assertEqual(result["code"], "permission_denied")

        with mock.patch.object(plugin, "_git_bytes",
                               return_value=(-2, b"", "git timed out")):
            result = self.graph()
        self.assertEqual(result["code"], "timeout")

        with mock.patch.object(plugin, "GIT_EXECUTABLE",
                               "kforge-missing-git-xyz"):
            result = self.graph()
        self.assertEqual(result["code"], "git_unavailable")

    def test_range_and_branch_validation(self):
        self.repo.commit("one", {"a.txt": "a\n"})
        self.assertEqual(self.graph(range="bogus")["code"], "invalid_range")
        self.assertEqual(self.graph(range="all", branch_ref="main")["code"],
                         "invalid_request")
        self.assertEqual(self.graph(range="branch", branch_ref="nope")["code"],
                         "unknown_ref")
        self.assertEqual(self.graph(range="branch", branch_ref=123)["code"],
                         "invalid_request")
        self.assertEqual(self.graph(range=123)["code"], "invalid_range")
        self.assertEqual(self.graph(format="bogus")["code"], "invalid_format")

    def test_read_only_queries_do_not_change_the_repository(self):
        first = self.repo.commit("first", {"a.txt": "a\n"})
        self.repo.commit("second", {"a.txt": "a\nb\n"})
        status_before = self.repo.status()
        head_before = self.repo.head()
        refs_before = self.repo.refs()

        self.assertTrue(self.graph()["ok"])
        detail = plugin.action_commit_detail({
            "target_library": self.repo.path, "oid": first})
        self.assertTrue(detail["ok"], detail)
        patch = plugin.action_commit_file({
            "target_library": self.repo.path, "oid": first, "path": "a.txt"})
        self.assertTrue(patch["ok"], patch)
        plugin.action_history({"target_library": self.repo.path})

        self.assertEqual(self.repo.status(), status_before)
        self.assertEqual(self.repo.head(), head_before)
        self.assertEqual(self.repo.refs(), refs_before)


if __name__ == "__main__":
    unittest.main()
