from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_repository_layout", ROOT / "scripts" / "check_repository_layout.py"
)
layout = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(layout)


class RepositoryLayoutCheckTests(unittest.TestCase):
    def test_published_docs_navigation_does_not_require_local_agent_docs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in layout.tracked_files(ROOT):
                if name.startswith("docs/"):
                    target = root / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.touch()
            navigation = root / "docs/README.md"
            navigation.write_bytes((ROOT / "docs/README.md").read_bytes())
            self.assertFalse((root / "docs/AgentDocs").exists())
            errors = layout.find_broken_links(root, ["docs/README.md"])
        self.assertEqual([], errors)

    def test_case_collisions_detect_case_only_duplicates(self):
        groups = layout.case_collisions(["docs/README.md", "DOCS/readme.md", "docs/other.md"])
        self.assertEqual([["docs/README.md", "DOCS/readme.md"]], groups)

    def test_case_collisions_allow_fixture_exemption(self):
        self.assertEqual([], layout.case_collisions([
            "tests/fixtures/Unicode.txt", "tests/fixtures/unicode.txt",
        ]))

    def test_forbidden_paths_are_reported_before_file_existence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            errors = layout.check_forbidden([
                "pkg/__pycache__/mod.pyc",
                "compile_commands.json",
                "build/out.bin",
            ], root)
        self.assertEqual(3, len(errors))

    def test_stale_migration_reference_is_reported(self):
        old_doc = "docs/" + "API.md"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "README.md").write_text("see " + old_doc + "\n", encoding="utf-8")
            errors = layout.stale_references(root, ["README.md"])
        self.assertTrue(any("stale old API doc" in error for error in errors), errors)

    def test_missing_and_present_links_are_distinguished(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "doc.md").write_text("# doc\n", encoding="utf-8")
            (root / "README.md").write_text(
                "[ok](doc.md) [missing](missing.md) [web](https://example.invalid/x)\n",
                encoding="utf-8",
            )
            errors = layout.find_broken_links(root, ["README.md"])
        self.assertEqual(["README.md:1: missing local link: missing.md"], errors)

    def test_relative_asset_references_are_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "app.js").write_text("export {}\n", encoding="utf-8")
            (root / "index.html").write_text(
                '<script src="./app.js"></script><link href="missing.css">\n',
                encoding="utf-8",
            )
            errors = layout.find_broken_asset_refs(root, ["index.html"])
        self.assertEqual(["index.html: missing local asset: missing.css"], errors)


if __name__ == "__main__":
    unittest.main()
