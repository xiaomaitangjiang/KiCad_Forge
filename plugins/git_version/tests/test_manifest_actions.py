import json
import os
import subprocess
import sys
import unittest

PLUGIN_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if PLUGIN_DIR not in sys.path:
    sys.path.insert(0, PLUGIN_DIR)

import plugin
from git_fixture import GitFixture

MANIFEST_PATH = os.path.join(PLUGIN_DIR, "manifest.json")


class ManifestActionsTest(unittest.TestCase):
    def test_declared_actions_and_versions(self):
        with open(MANIFEST_PATH, encoding="utf-8") as handle:
            manifest = json.load(handle)
        self.assertEqual(manifest["manifestVersion"], 2)
        self.assertEqual(manifest["version"], "1.3.0")
        actions = {action["id"]: action
                   for action in manifest["python"]["actions"]}
        self.assertEqual(set(actions), {
            "commit", "status", "init", "history", "commit_detail",
            "commit_file", "diff"})
        for action_id in ("commit_detail", "commit_file"):
            action = actions[action_id]
            self.assertEqual(action["trigger"], "inline")
            self.assertEqual(action["auto_args"], ["target_library"])
            self.assertTrue(action["cancelable"])
            self.assertIs(action["button"]["show"], False)
            self.assertTrue(action["description"])
        self.assertIn("graph_v1", actions["history"]["description"])
        self.assertIs(actions["history"]["button"]["show"], False)
        self.assertEqual(actions["commit"]["trigger"], "dialog")

    def test_manifest_actions_have_dispatch_entries(self):
        with open(MANIFEST_PATH, encoding="utf-8") as handle:
            manifest = json.load(handle)
        declared = {action["id"] for action in manifest["python"]["actions"]}
        self.assertLessEqual(declared, set(plugin.ACTIONS))
        self.assertIs(plugin.ACTIONS["history"], plugin.action_history)
        self.assertIs(plugin.ACTIONS["commit_detail"],
                      plugin.action_commit_detail)
        self.assertIs(plugin.ACTIONS["commit_file"], plugin.action_commit_file)

    def test_dispatch_keeps_legacy_history_and_adds_graph(self):
        repo = GitFixture()
        self.addCleanup(repo.close)
        root = repo.commit("root", {"a.txt": "a\n"})
        second = repo.commit("second", {"a.txt": "a\nb\n"})

        legacy = plugin.ACTIONS["history"]({"target_library": repo.path})
        self.assertEqual(legacy, plugin.do_history(repo.path))
        self.assertIn("output", legacy)

        graph = plugin.ACTIONS["history"]({
            "target_library": repo.path, "format": "graph_v1"})
        self.assertTrue(graph["ok"], graph)
        self.assertEqual(graph["schema_version"], "graph_v1")
        self.assertEqual(graph["head_oid"], second)

        detail = plugin.ACTIONS["commit_detail"]({
            "target_library": repo.path, "oid": root})
        self.assertTrue(detail["ok"], detail)
        self.assertEqual(detail["commit"]["oid"], root)

        patch = plugin.ACTIONS["commit_file"]({
            "target_library": repo.path, "oid": second, "path": "a.txt"})
        self.assertTrue(patch["ok"], patch)
        self.assertIn("+b", patch["patch"])

    def test_action_history_input_validation(self):
        for bad in (None, [], "json"):
            result = plugin.action_history(bad)
            self.assertFalse(result["ok"])
            self.assertEqual(result["code"], "invalid_request")
        result = plugin.action_history({"format": "bogus"})
        self.assertEqual(result["code"], "invalid_format")
        result = plugin.action_history({"format": 1})
        self.assertEqual(result["code"], "invalid_format")

    def test_cli_dispatches_graph_and_reports_unknown_action(self):
        repo = GitFixture()
        self.addCleanup(repo.close)
        root = repo.commit("root", {"a.txt": "a\n"})
        repo.commit("second", {"a.txt": "a\nb\n"})

        def run_cli(action, args):
            environment = os.environ.copy()
            environment["PYTHONIOENCODING"] = "utf-8"
            completed = subprocess.run(
                [sys.executable, os.path.join(PLUGIN_DIR, "plugin.py"),
                 action, json.dumps(args)],
                cwd=PLUGIN_DIR, env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, encoding="utf-8",
                errors="replace")
            return completed

        history = run_cli("history", {
            "target_library": repo.path, "format": "graph_v1", "limit": 1})
        self.assertEqual(history.returncode, 0, history.stderr)
        parsed = json.loads(history.stdout)
        self.assertTrue(parsed["ok"], parsed)
        self.assertEqual(parsed["schema_version"], "graph_v1")
        self.assertEqual(len(parsed["commits"]), 1)

        detail = run_cli("commit_detail", {
            "target_library": repo.path, "oid": root})
        self.assertEqual(detail.returncode, 0, detail.stderr)
        self.assertTrue(json.loads(detail.stdout)["ok"])

        file_patch = run_cli("commit_file", {
            "target_library": repo.path, "oid": root, "path": "a.txt"})
        self.assertEqual(file_patch.returncode, 0, file_patch.stderr)
        self.assertTrue(json.loads(file_patch.stdout)["ok"])

        unknown = run_cli("does_not_exist", {})
        self.assertEqual(unknown.returncode, 1)
        self.assertFalse(json.loads(unknown.stdout)["ok"])


if __name__ == "__main__":
    unittest.main()
