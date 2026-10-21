import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manifest import canonical_hash, normalize_manifest, semantic_diff


def symbol(**overrides):
    value = {
        "uuid": "s1", "library_id": "lib1", "library_name": "Device",
        "library_source_key": "device", "name": "R1", "footprint_ref": "R:0402",
        "footprint_uuid": "f1", "kf_id": "KF1", "pre_kf_id": "",
        "component_type_id": "resistor", "package_type_id": "0402",
        "pins": [{"number": "1", "name": "~", "electrical_type": "passive"}],
        "properties": {"Value": "10k"},
    }
    value.update(overrides)
    return value


def manifest(symbols=None, footprints=None, models=None, libraries=None):
    return {
        "manifest_version": 1,
        "root": "D:/repo",
        "libraries": libraries if libraries is not None else [
            {"id": "lib1", "name": "Device", "source_key": "device"}],
        "symbols": symbols if symbols is not None else [symbol()],
        "footprints": footprints if footprints is not None else [
            {"uuid": "f1", "name": "0402", "library": "R", "pad_count": 2,
             "pads": [{"number": "1", "type": "smd", "shape": "rect", "x": 0,
                       "y": 0, "width": 1, "height": 1, "drill": 0}],
             "models": []}],
        "models": models if models is not None else [],
    }


class CanonicalHashTest(unittest.TestCase):
    def test_key_order_does_not_change_hash(self):
        self.assertEqual(canonical_hash({"a": 1, "b": [2]}),
                         canonical_hash({"b": [2], "a": 1}))


class NormalizeManifestTest(unittest.TestCase):
    def test_hash_and_model_digest_are_stable_and_root_is_dropped(self):
        with tempfile.TemporaryDirectory() as directory:
            model_path = os.path.join(directory, "part.step")
            with open(model_path, "wb") as handle:
                handle.write(b"solid")
            data = manifest(models=[{"path": "part.step", "sha256": None}])
            normalized = normalize_manifest(data, directory)
            self.assertNotIn("root", normalized)
            self.assertEqual(normalized["manifest_version"], 1)
            self.assertIsNotNone(normalized["symbols"][0]["content_sha256"])
            self.assertEqual(len(normalized["models"][0]["sha256"]), 64)
            again = normalize_manifest(data, directory)
            self.assertEqual(normalized, again)

    def test_missing_model_file_hashes_to_null(self):
        with tempfile.TemporaryDirectory() as directory:
            data = manifest(models=[{"path": "missing.step"}])
            normalized = normalize_manifest(data, directory)
            self.assertIsNone(normalized["models"][0]["sha256"])


class SemanticDiffTest(unittest.TestCase):
    def test_no_changes(self):
        lines = semantic_diff(manifest(), manifest())
        self.assertEqual(lines, ["工作区与 HEAD 清单一致（无语义变化）。"])

    def test_added_and_removed_symbols(self):
        old = manifest(symbols=[symbol()])
        new = manifest(symbols=[symbol(uuid="s2", name="C1")])
        text = "\n".join(semantic_diff(old, new))
        self.assertIn("符号: +1 / -1", text)
        self.assertIn("+ Device:C1", text)
        self.assertIn("- Device:R1", text)

    def test_pin_property_and_footprint_changes(self):
        old = manifest()
        changed = symbol(
            pins=[{"number": "1", "name": "~", "electrical_type": "passive"},
                  {"number": "2", "name": "VCC", "electrical_type": "power_in"}],
            properties={"Value": "12k"})
        new = manifest(symbols=[changed])
        text = "\n".join(semantic_diff(old, new))
        self.assertIn("引脚: +1 / -0", text)
        self.assertIn("+ 引脚 2", text)
        self.assertIn("属性: +0 / -0 / ~1", text)
        self.assertIn("Value: 10k → 12k", text)

    def test_pad_and_model_hash_changes(self):
        old = manifest(models=[{"path": "part.step", "sha256": "a"}])
        new_footprint = {
            "uuid": "f1", "name": "0402", "library": "R", "pad_count": 2,
            "pads": [{"number": "1", "type": "smd", "shape": "rect", "x": 0,
                      "y": 0, "width": 2, "height": 1, "drill": 0}],
            "models": []}
        new = manifest(footprints=[new_footprint],
                       models=[{"path": "part.step", "sha256": "b"}])
        text = "\n".join(semantic_diff(old, new))
        self.assertIn("焊盘: +0 / -0 / ~1", text)
        self.assertIn("~ 焊盘 1", text)
        self.assertIn("3D 模型: +0 / -0 / ~1", text)
        self.assertIn("~ part.step", text)

    def test_library_and_footprint_additions(self):
        old = manifest(libraries=[], footprints=[])
        text = "\n".join(semantic_diff(old, manifest()))
        self.assertIn("元件库: +1 / -0", text)
        self.assertIn("封装: +1 / -0", text)


if __name__ == "__main__":
    unittest.main()
