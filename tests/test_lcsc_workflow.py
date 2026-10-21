"""LCSC 插件「只输编号 → 找元件库包 → 选封装 → 导入」工作流回归（不联网）。

覆盖插件侧与宿主无关的部分：

  * 立创编号规范化：首尾空白、大小写、纯数字补 C；路径字符/空/混杂编号拒绝，
    并且必须在构造任何缓存路径、发起任何下载之前完成校验；
  * analyze 不需要目标库：只下载并转换到独立临时目录，返回编号、器件/封装信息
    与**完整**的多个候选（同名不同 UUID 都要保留）；
  * 查询失败 ≠ 没有匹配：宿主查询异常必须回传 warning，不能报成「确定无候选」；
  * 失败不使用上一次的结果：没有缓存，每次 analyze 都是新目录，失败即失败。

运行：python -B -m unittest tests.test_lcsc_workflow
"""

import importlib.util
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN_PATH = os.path.join(HERE, "..", "plugins", "lcsc_import", "plugin.py")


def load_plugin():
    """按路径加载插件模块（不执行 __main__ 分支，不联网）。"""
    spec = importlib.util.spec_from_file_location("lcsc_import_plugin_workflow", PLUGIN_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def symbol_block(name):
    return ('(symbol "%s"\n'
            '  (property "Reference" "U" (at 0 5.08 0))\n'
            '  (property "Value" "%s" (at 0 2.54 0)))') % (name, name)


class PrepareSpy:
    """顶掉真正的下载 + easyeda2kicad 转换，记录 (编号, 输出目录)。"""

    def __init__(self, blocks=None, footprint_name="SOT-23", model_name="", fail=None,
                 footprint_text=None):
        self.calls = []
        self.blocks = blocks if blocks is not None else [("C123", symbol_block("C123"))]
        self.footprint_name = footprint_name
        self.model_name = model_name
        self.fail = fail
        self.footprint_text = footprint_text

    def __call__(self, lcsc_id, output_dir):
        self.calls.append((lcsc_id, output_dir))
        self.output_dir = output_dir
        if self.fail:
            return {"ok": False, "error": self.fail}
        os.makedirs(output_dir, exist_ok=True)
        if self.footprint_text is not None:
            with open(os.path.join(output_dir, "converter-output.kicad_mod"), "w",
                      encoding="utf-8") as handle:
                handle.write(self.footprint_text)
        return {"ok": True, "output_dir": output_dir,
                "symbol_names": [name for name, _ in self.blocks],
                "footprint_name": self.footprint_name, "model_name": self.model_name,
                "blocks": self.blocks}


class NormalizeLcscId(unittest.TestCase):
    def test_canonical_forms(self):
        plugin = load_plugin()
        self.assertEqual(plugin.normalize_lcsc_id("C123"), "C123")
        self.assertEqual(plugin.normalize_lcsc_id(" c123 "), "C123")   # 首尾空白 + 小写
        self.assertEqual(plugin.normalize_lcsc_id("123"), "C123")      # 纯数字补 C
        self.assertEqual(plugin.normalize_lcsc_id("\tC2681684\n"), "C2681684")

    def test_rejects_empty_path_like_and_mixed_numbers(self):
        plugin = load_plugin()
        for bad in ["", "   ", "C", "CX", "C12A3", "12A3", "../C123", "C12/3",
                    "C:\\temp\\C1", "C 123", "C-123", "C123.kicad_sym", None, 123]:
            with self.assertRaises(ValueError, msg="accepted %r" % (bad,)):
                plugin.normalize_lcsc_id(bad)


class AnalyzeFlow(unittest.TestCase):
    def test_analyze_needs_no_target_library_and_normalizes_the_id(self):
        plugin = load_plugin()
        spy = PrepareSpy([("C123", symbol_block("C123"))])
        plugin.prepare = spy
        plugin.search_host_footprints = lambda near, limit=50: ([], "")

        result = plugin.analyze(" c123 ")
        self.assertTrue(result.get("ok"), result)
        self.assertEqual(result["lcsc_id"], "C123")
        self.assertEqual(len(spy.calls), 1)
        self.assertEqual(spy.calls[0][0], "C123")          # 下载用的是规范编号
        self.assertEqual(result["symbol_names"], ["C123"])
        self.assertEqual(result["footprint_name"], "SOT-23")
        self.assertEqual(result["candidates"], [])

    def test_analyze_rejects_an_invalid_id_before_touching_disk(self):
        plugin = load_plugin()
        spy = PrepareSpy(fail="must not be called")
        plugin.prepare = spy
        with tempfile.TemporaryDirectory() as tmp:
            plugin.FETCHED_DIR = tmp
            result = plugin.analyze(" ../../etc/passwd ")
            self.assertFalse(result.get("ok"), result)
            self.assertIn("invalid", result["error"].lower())
            self.assertEqual(spy.calls, [])
            self.assertEqual(os.listdir(tmp), [])  # 非法编号不参与任何路径构造

    def test_analyze_keeps_every_candidate_including_same_named_uuids(self):
        plugin = load_plugin()
        plugin.prepare = PrepareSpy()
        candidates = [
            {"id": "11111111-1111", "name": "SOT-23", "library": "Package_A",
             "score": 0.91, "pad_count": 3},
            {"id": "22222222-2222", "name": "SOT-23", "library": "Package_B",
             "score": 0.88, "pad_count": 6},
            {"id": "33333333-3333", "name": "SOT-23", "library": "Package_C",
             "score": 0.4, "pad_count": 3},
        ]
        plugin.search_host_footprints = lambda near, limit=50: (list(candidates), "")

        result = plugin.analyze("C1")
        self.assertTrue(result.get("ok"), result)
        # 同名不同 UUID 的三条候选一条都不能少、不能改
        self.assertEqual(result["candidates"], candidates)
        self.assertEqual(result.get("warning", ""), "")

    def test_analyze_reports_a_query_failure_as_warning_not_as_no_match(self):
        plugin = load_plugin()
        plugin.prepare = PrepareSpy()
        plugin.search_host_footprints = lambda near, limit=50: (
            [], "host footprint search failed: connection refused")

        result = plugin.analyze("C1")
        self.assertTrue(result.get("ok"), result)          # 新建仍可继续
        self.assertEqual(result["candidates"], [])
        self.assertIn("connection refused", result["warning"])
        # 明确说明是查询失败，不是「确定没有匹配」
        self.assertNotEqual(result.get("warning", ""), "")

    def test_analyze_never_reports_the_previous_result_after_a_failure(self):
        plugin = load_plugin()
        first = PrepareSpy([("C5", symbol_block("C5"))])
        plugin.prepare = first
        plugin.search_host_footprints = lambda near, limit=50: (
            [{"id": "u1", "name": "SOT-23", "library": "L", "score": 1.0, "pad_count": 3}], "")
        good = plugin.analyze("C5")
        self.assertTrue(good.get("ok"), good)
        self.assertEqual(len(good["candidates"]), 1)

        plugin.prepare = PrepareSpy(fail="easyeda2kicad failed (exit 1): boom")
        bad = plugin.analyze("C5")
        self.assertFalse(bad.get("ok"), bad)
        self.assertIn("boom", bad["error"])
        self.assertNotIn("candidates", bad)      # 上次的候选不能跟过来
        self.assertNotIn("symbol_names", bad)

    def test_analyze_uses_a_fresh_temporary_directory_every_time(self):
        plugin = load_plugin()
        spy = PrepareSpy()
        plugin.prepare = spy
        plugin.search_host_footprints = lambda near, limit=50: ([], "")

        self.assertTrue(plugin.analyze("C7").get("ok"))
        self.assertTrue(plugin.analyze("C7").get("ok"))
        self.assertEqual(len(spy.calls), 2)
        first_dir, second_dir = spy.calls[0][1], spy.calls[1][1]
        self.assertNotEqual(first_dir, second_dir)       # 不复用缓存目录
        plugin_root = os.path.dirname(PLUGIN_PATH)
        for work_dir in (first_dir, second_dir):
            self.assertFalse(os.path.abspath(work_dir).startswith(os.path.abspath(plugin_root)))
            self.assertFalse(os.path.exists(work_dir))   # 分析完即清理

    def test_analyze_survives_a_missing_host_api_and_says_so(self):
        plugin = load_plugin()
        plugin.prepare = PrepareSpy()
        original = os.environ.pop("KFORGE_API_URL", None)
        try:
            result = plugin.analyze("C9")
        finally:
            if original is not None:
                os.environ["KFORGE_API_URL"] = original
        self.assertTrue(result.get("ok"), result)
        self.assertEqual(result["candidates"], [])
        self.assertNotEqual(result.get("warning", ""), "")


class HostSearchContract(unittest.TestCase):
    def test_search_reports_unavailable_host_as_warning(self):
        plugin = load_plugin()
        original = os.environ.pop("KFORGE_API_URL", None)
        try:
            candidates, warning = plugin.search_host_footprints("SOT-23")
        finally:
            if original is not None:
                os.environ["KFORGE_API_URL"] = original
        self.assertEqual(candidates, [])
        self.assertNotEqual(warning, "")

    def test_search_keeps_the_full_candidate_payload(self):
        plugin = load_plugin()
        payload = [{"id": "u1", "name": "SOT-23", "library": "L", "score": 0.5,
                    "pad_count": 3}]

        class FakeResponse:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self):
                return json.dumps(payload).encode("utf-8")

        original_url = os.environ.get("KFORGE_API_URL")
        original_open = plugin.urllib.request.urlopen
        os.environ["KFORGE_API_URL"] = "http://127.0.0.1:1"
        plugin.urllib.request.urlopen = lambda url, timeout=5: FakeResponse()
        try:
            candidates, warning = plugin.search_host_footprints("SOT-23")
        finally:
            plugin.urllib.request.urlopen = original_open
            if original_url is None:
                os.environ.pop("KFORGE_API_URL", None)
            else:
                os.environ["KFORGE_API_URL"] = original_url
        self.assertEqual(candidates, payload)
        self.assertEqual(warning, "")

    def test_search_reports_a_transport_failure_as_warning(self):
        plugin = load_plugin()
        original_url = os.environ.get("KFORGE_API_URL")
        original_open = plugin.urllib.request.urlopen

        def boom(url, timeout=5):
            raise OSError("connection refused")

        os.environ["KFORGE_API_URL"] = "http://127.0.0.1:1"
        plugin.urllib.request.urlopen = boom
        try:
            candidates, warning = plugin.search_host_footprints("SOT-23")
        finally:
            plugin.urllib.request.urlopen = original_open
            if original_url is None:
                os.environ.pop("KFORGE_API_URL", None)
            else:
                os.environ["KFORGE_API_URL"] = original_url
        self.assertEqual(candidates, [])
        self.assertIn("connection refused", warning)


if __name__ == "__main__":
    sys.exit(unittest.main())
