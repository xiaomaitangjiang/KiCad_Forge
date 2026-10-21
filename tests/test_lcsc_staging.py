"""LCSC 插件暂存产物回归（不联网）。

用 stub 顶掉 prepare（下载 + easyeda2kicad 转换），只验证暂存目录里的产物：

  * symbol 用宿主约定之外的固定编号安全文件名 symbol-N.kicad_sym，
    上游元件名只出现在 manifest/内容里，不参与磁盘路径；
  * 名字里带路径分量的元件（"../../escaped"）不会把文件写到暂存目录之外；
  * manifest 的 name 仍是上游名字 —— 名字与路径彻底解耦。

运行：python -m unittest tests/test_lcsc_staging.py
"""

import importlib.util
import json
import os
import sys
import tempfile
import unittest

from tests.test_lcsc_workflow import PrepareSpy  # 同一套下载桩（不联网）

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN_PATH = os.path.join(HERE, "..", "plugins", "lcsc_import", "plugin.py")


def load_plugin():
    """按路径加载插件模块（不执行 __main__ 分支，不联网）。"""
    spec = importlib.util.spec_from_file_location("lcsc_import_plugin_staging", PLUGIN_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def symbol_block(name):
    return ('(symbol "%s"\n'
            '  (property "Reference" "U" (at 0 5.08 0))\n'
            '  (property "Value" "%s" (at 0 2.54 0)))') % (name, name)


def stub_prepare(blocks, footprint_name="", footprint_text=None):
    def prepare(lcsc_id, output_dir):
        os.makedirs(output_dir, exist_ok=True)
        if footprint_text is not None:
            with open(os.path.join(output_dir, "converter-output.kicad_mod"), "w",
                      encoding="utf-8") as handle:
                handle.write(footprint_text)
        return {"ok": True, "output_dir": output_dir,
                "symbol_names": [name for name, _ in blocks],
                "footprint_name": footprint_name, "model_name": "",
                "blocks": blocks}

    return prepare


class StagedSymbolFileNames(unittest.TestCase):
    def test_symbol_files_use_fixed_numbered_names(self):
        plugin = load_plugin()
        plugin.prepare = stub_prepare([("C123", symbol_block("C123"))])
        with tempfile.TemporaryDirectory() as tmp:
            staging = os.path.join(tmp, "staging")
            result = plugin.staged_import("C123", staging, {"mode": "reuse"})
            self.assertTrue(result.get("ok"), result)

            # 只看清单里声明的名字，产物名由插件自己编号决定
            with open(os.path.join(staging, "manifest.json"), encoding="utf-8") as handle:
                manifest = json.load(handle)
            self.assertEqual(manifest["symbols"],
                             [{"name": "C123", "file": "symbol-0.kicad_sym"}])
            self.assertEqual(sorted(os.listdir(staging)),
                             ["manifest.json", "symbol-0.kicad_sym"])
            with open(os.path.join(staging, "symbol-0.kicad_sym"), encoding="utf-8") as handle:
                self.assertIn('"C123"', handle.read())

    def test_hostile_symbol_name_never_writes_outside_staging(self):
        plugin = load_plugin()
        hostile = "../../escaped"
        plugin.prepare = stub_prepare([(hostile, symbol_block(hostile))])
        with tempfile.TemporaryDirectory() as tmp:
            staging = os.path.join(tmp, "staging")
            result = plugin.staged_import("C1", staging, {"mode": "reuse"})
            self.assertTrue(result.get("ok"), result)

            # 名字照旧写进 manifest（宿主负责校验），磁盘路径只有固定编号
            with open(os.path.join(staging, "manifest.json"), encoding="utf-8") as handle:
                manifest = json.load(handle)
            self.assertEqual(manifest["symbols"], [{"name": hostile, "file": "symbol-0.kicad_sym"}])
            self.assertEqual(sorted(os.listdir(staging)),
                             ["manifest.json", "symbol-0.kicad_sym"])

            # "staging/../../escaped.kicad_sym" 会落在临时根之上：那里不能出现文件
            self.assertFalse(os.path.exists(os.path.join(os.path.dirname(staging),
                                                         "escaped.kicad_sym")))
            self.assertFalse(os.path.exists(os.path.join(os.path.dirname(tmp),
                                                         "escaped.kicad_sym")))

    def test_new_footprint_uses_fixed_name_and_keeps_declared_name(self):
        plugin = load_plugin()
        plugin.prepare = stub_prepare(
            [("U1", symbol_block("U1"))], footprint_name="SOT-23",
            footprint_text='(footprint "converter-output" (layer "F.Cu"))')
        with tempfile.TemporaryDirectory() as tmp:
            staging = os.path.join(tmp, "staging")
            result = plugin.staged_import("C2", staging, {"mode": "new"})
            self.assertTrue(result.get("ok"), result)

            with open(os.path.join(staging, "manifest.json"), encoding="utf-8") as handle:
                manifest = json.load(handle)
            self.assertEqual(manifest["footprint"],
                             {"name": "converter-output", "file": "footprint-0.kicad_mod"})
            # 磁盘路径是固定安全名；上游封装名只留在 manifest 里交给宿主校验
            with open(os.path.join(staging, "footprint-0.kicad_mod"), encoding="utf-8") as handle:
                self.assertIn('(footprint "converter-output"', handle.read())

    def test_manifest_name_follows_the_normalized_root_node(self):
        plugin = load_plugin()
        plugin.prepare = stub_prepare(
            [("U1", symbol_block("U1"))], footprint_name="from-filename",
            footprint_text='(module easyeda2kicad:PACKAGE_BODY (layer F.Cu))')
        with tempfile.TemporaryDirectory() as tmp:
            staging = os.path.join(tmp, "staging")
            result = plugin.staged_import("C4", staging, {"mode": "new"})
            self.assertTrue(result.get("ok"), result)

            with open(os.path.join(staging, "manifest.json"), encoding="utf-8") as handle:
                manifest = json.load(handle)
            # 宿主会核对「清单名 == 文件根节点名」：两者必须都来自同一个根节点
            self.assertEqual(manifest["footprint"],
                             {"name": "PACKAGE_BODY", "file": "footprint-0.kicad_mod"})
            with open(os.path.join(staging, "footprint-0.kicad_mod"), encoding="utf-8") as handle:
                self.assertIn('(footprint "PACKAGE_BODY"', handle.read())


class StagedImportContract(unittest.TestCase):
    """staged_import 只写宿主私有暂存目录，并且严格对应规范编号。"""

    def test_staged_import_normalizes_the_part_number_before_downloading(self):
        plugin = load_plugin()
        spy = PrepareSpy([("C123", symbol_block("C123"))])
        plugin.prepare = spy
        with tempfile.TemporaryDirectory() as tmp:
            staging = os.path.join(tmp, "staging")
            result = plugin.staged_import(" c123 ", staging, {"mode": "reuse"})
            self.assertTrue(result.get("ok"), result)
            self.assertEqual(spy.calls[0][0], "C123")
            with open(os.path.join(staging, "manifest.json"), encoding="utf-8") as handle:
                manifest = json.load(handle)
            self.assertEqual(manifest["lcsc_id"], "C123")

    def test_staged_import_rejects_an_invalid_part_number_before_any_download(self):
        plugin = load_plugin()
        spy = PrepareSpy(fail="must not be called")
        plugin.prepare = spy
        with tempfile.TemporaryDirectory() as tmp:
            staging = os.path.join(tmp, "staging")
            result = plugin.staged_import("../evil", staging, {"mode": "new"})
            self.assertFalse(result.get("ok"), result)
            self.assertIn("invalid", result["error"].lower())
            self.assertEqual(spy.calls, [])
            self.assertFalse(os.path.exists(os.path.join(staging, "manifest.json")))

    def test_staged_import_rejects_an_unknown_footprint_mode(self):
        plugin = load_plugin()
        plugin.prepare = PrepareSpy([("C1", symbol_block("C1"))])
        with tempfile.TemporaryDirectory() as tmp:
            staging = os.path.join(tmp, "staging")
            result = plugin.staged_import("C1", staging, {"mode": "guess"})
            self.assertFalse(result.get("ok"), result)
            self.assertFalse(os.path.exists(os.path.join(staging, "manifest.json")))

    def test_reuse_never_writes_a_footprint_file(self):
        plugin = load_plugin()
        plugin.prepare = PrepareSpy(
            [("C1", symbol_block("C1"))], footprint_name="SOT-23",
            footprint_text='(footprint "converter-output" (layer "F.Cu"))')
        with tempfile.TemporaryDirectory() as tmp:
            staging = os.path.join(tmp, "staging")
            result = plugin.staged_import("C1", staging, {"mode": "reuse",
                                                          "footprint_id": "uuid-1"})
            self.assertTrue(result.get("ok"), result)
            with open(os.path.join(staging, "manifest.json"), encoding="utf-8") as handle:
                manifest = json.load(handle)
            self.assertIsNone(manifest["footprint"])
            self.assertEqual(manifest["footprint_choice"], "reuse")
            self.assertEqual(sorted(os.listdir(staging)),
                             ["manifest.json", "symbol-0.kicad_sym"])

    def test_new_footprint_keeps_the_converted_artifact(self):
        plugin = load_plugin()
        plugin.prepare = PrepareSpy(
            [("C1", symbol_block("C1"))], footprint_name="SOT-23",
            footprint_text='(module easyeda2kicad:SOT-23 (layer F.Cu))')
        with tempfile.TemporaryDirectory() as tmp:
            staging = os.path.join(tmp, "staging")
            result = plugin.staged_import("C1", staging, {"mode": "new"})
            self.assertTrue(result.get("ok"), result)
            with open(os.path.join(staging, "manifest.json"), encoding="utf-8") as handle:
                manifest = json.load(handle)
            self.assertEqual(manifest["footprint"],
                             {"name": "SOT-23", "file": "footprint-0.kicad_mod"})
            self.assertEqual(manifest["footprint_choice"], "new")
            with open(os.path.join(staging, "footprint-0.kicad_mod"), encoding="utf-8") as handle:
                self.assertIn('(footprint "SOT-23"', handle.read())

    def test_a_failed_conversion_leaves_no_manifest(self):
        plugin = load_plugin()
        plugin.prepare = PrepareSpy(fail="easyeda2kicad failed (exit 1): no such part")
        with tempfile.TemporaryDirectory() as tmp:
            staging = os.path.join(tmp, "staging")
            result = plugin.staged_import("C1", staging, {"mode": "new"})
            self.assertFalse(result.get("ok"), result)
            self.assertIn("no such part", result["error"])
            self.assertFalse(os.path.exists(os.path.join(staging, "manifest.json")))


if __name__ == "__main__":
    sys.exit(unittest.main())
