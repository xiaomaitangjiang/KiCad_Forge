import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import plugin


def payload(name="R1", pins=None, value="10k"):
    return {
        "ok": True,
        "root": "",
        "manifest_version": 1,
        "libraries": [{"id": "lib1", "name": "Device", "source_key": "device"}],
        "symbols": [{
            "uuid": "s1", "library_id": "lib1", "library_name": "Device",
            "library_source_key": "device", "name": name,
            "footprint_ref": "R:0402", "footprint_uuid": "f1",
            "kf_id": "KF1", "pre_kf_id": "",
            "component_type_id": "resistor", "package_type_id": "0402",
            "pins": pins or [{"number": "1", "name": "~", "electrical_type": "passive"}],
            "properties": {"Value": value},
        }],
        "footprints": [{
            "uuid": "f1", "name": "0402", "library": "R", "pad_count": 1,
            "pads": [{"number": "1", "type": "smd", "shape": "rect", "x": 0,
                      "y": 0, "width": 1, "height": 1, "drill": 0}],
        }],
        "models": [],
    }


class PluginManifestFlowTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.lib = os.path.join(self.directory.name, "lib")
        os.makedirs(self.lib)
        subprocess.run(["git", "init"], cwd=self.lib, check=True,
                       capture_output=True, text=True)
        subprocess.run(["git", "config", "user.email", "test@example.com"],
                       cwd=self.lib, check=True)
        subprocess.run(["git", "config", "user.name", "Test"], cwd=self.lib,
                       check=True)
        self.original = plugin._api_manifest

    def tearDown(self):
        plugin._api_manifest = self.original
        self.directory.cleanup()

    def test_commit_writes_manifest_and_diff_is_semantic(self):
        plugin._api_manifest = lambda root, target: (payload(), None)
        init = plugin.do_init(self.lib)
        self.assertTrue(init["ok"], init)
        self.assertTrue(init["committed"], init)
        manifest_path = os.path.join(self.lib, ".kforge", "manifest.json")
        self.assertTrue(os.path.isfile(manifest_path))
        with open(manifest_path, encoding="utf-8") as handle:
            first = json.load(handle)
        self.assertEqual(first["symbols"][0]["name"], "R1")
        self.assertIn("content_sha256", first["symbols"][0])

        changed = payload(pins=[
            {"number": "1", "name": "~", "electrical_type": "passive"},
            {"number": "2", "name": "VCC", "electrical_type": "power_in"}],
            value="12k")
        plugin._api_manifest = lambda root, target: (changed, None)
        result = plugin.do_diff(self.lib)
        self.assertTrue(result["ok"], result)
        self.assertIn("语义差异", result["output"])
        self.assertIn("引脚: +1", result["output"])
        self.assertIn("Value: 10k → 12k", result["output"])

        commit = plugin.do_commit(self.lib, "KF: test")
        self.assertTrue(commit["ok"], commit)
        self.assertTrue(commit["manifest"], commit)
        import subprocess as sp
        shown = sp.run(["git", "show", "HEAD:.kforge/manifest.json"], cwd=self.lib,
                       check=True, capture_output=True, text=True).stdout
        committed = json.loads(shown)
        self.assertEqual(committed["symbols"][0]["properties"]["Value"], "12k")
        self.assertEqual(plugin.do_diff(self.lib)["output"].splitlines()[0],
                         "=== 语义差异（HEAD 清单 ↔ 当前工作区） ===")
        self.assertIn("一致", plugin.do_diff(self.lib)["output"])

    def test_api_failure_falls_back_to_raw_diff(self):
        plugin._api_manifest = lambda root, target: (None, "offline")
        commit = plugin.do_commit(self.lib, "KF: first")
        # No HEAD yet: commit may fail with nothing to commit, but it must not
        # crash and must report the manifest problem.
        self.assertFalse(commit["manifest"])
        self.assertIn("offline", commit.get("note", ""))

    def test_http_api_contract_writes_manifest(self):
        received = []

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                received.append((self.path, json.loads(self.rfile.read(length))))
                data = json.dumps(payload()).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        previous = os.environ.get("KFORGE_API_URL")
        os.environ["KFORGE_API_URL"] = f"http://127.0.0.1:{server.server_address[1]}"
        self.addCleanup(lambda: os.environ.__setitem__("KFORGE_API_URL", previous)
                        if previous is not None else os.environ.pop("KFORGE_API_URL", None))

        result = plugin.do_init(self.lib)
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["manifest"], result)
        self.assertEqual(received[0][0], "/api/library-manifest")
        self.assertEqual(received[0][1], {"root": self.lib, "target": self.lib})
        with open(os.path.join(self.lib, ".kforge", "manifest.json"), encoding="utf-8") as handle:
            manifest = json.load(handle)
        self.assertEqual(manifest["symbols"][0]["name"], "R1")
        self.assertIn("content_sha256", manifest["symbols"][0])


if __name__ == "__main__":
    unittest.main()
