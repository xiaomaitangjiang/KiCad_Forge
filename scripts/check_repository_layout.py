#!/usr/bin/env python3
"""Check repository layout rules, generated-file reflow and local links.

This is deliberately stdlib-only so it can run inside the existing CI without
adding a directory-management dependency. It checks:
  * tracked generated files (__pycache__, .pyc, compile databases);
  * tracked build/test output that must stay out of Git;
  * case-insensitive path collisions;
  * references to paths that were moved by the layout migration;
  * relative references in Markdown, HTML and CSS.

Run locally from any directory:
    python -B scripts/check_repository_layout.py

The script works on the index plus untracked-but-not-ignored files. In CI this
is the committed tree; locally it also covers new files that are intentionally
still untracked.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_PARTS = frozenset({"__pycache__"})
FORBIDDEN_NAMES = frozenset({"compile_commands.json"})
FORBIDDEN_PREFIXES = ("build/", "dist/", "test-results/", "install_file/")
FORBIDDEN_EXACT = frozenset({"webui/-c", "webui/.exe", "webui/npm run build"})

# Each entry is (label, regex). The patterns intentionally describe only the
# old, unambiguous first-party locations. Fixtures below are exempt because
# they may deliberately contain historical path strings as test data.
STALE_PATTERNS = (
    ("old API doc", re.compile(r"docs/API\.md")),
    ("old CI doc", re.compile(r"docs/CI\.md")),
    ("old attention doc", re.compile(r"docs/Attention(?:_en)?\.md")),
    ("old Windows resource", re.compile(r"resources/(?:app\.rc|resource\.h)")),
    ("old Inno script", re.compile(r"resources/Kicad_Forge_V0\.1\.1\.iss")),
    ("old tracked icon", re.compile(r"resources/app\.ico")),
    ("root C++ test", re.compile(r"(?<![/\w])tests/test_[A-Za-z0-9_]+\.cpp")),
    ("root Python test", re.compile(r"(?<![/\w])tests/test_[A-Za-z0-9_]+\.py")),
    ("root benchmark", re.compile(r"(?<![/\w])tests/bench_import\.cpp")),
    ("root webui runner", re.compile(r"webui/tests/run-[A-Za-z0-9_-]+\.mjs")),
    ("root webui unit test", re.compile(r"webui/tests/[A-Za-z0-9_]+\.test\.(?:js|mjs)")),
    ("old installer output", re.compile(r"(?<![/\w])install_file(?:/|\b)")),
)
STALE_EXEMPT_PREFIXES = (
    "tests/fixtures/",
    "webui/tests/fixtures/",
    # The checker itself names the forbidden legacy paths.
    "scripts/check_repository_layout.py",
)
TEXT_SUFFIXES = frozenset({
    ".md", ".markdown", ".html", ".htm", ".css", ".js", ".mjs", ".jsx",
    ".ts", ".tsx", ".json", ".yml", ".yaml", ".toml", ".py", ".cpp", ".h",
    ".lua", ".rc", ".iss", ".cmd", ".ps1", ".txt",
})
LINK_DOC_SUFFIXES = frozenset({".md", ".markdown"})
LINK_EXEMPT_PREFIXES = ("docs/AgentDocs/",)
CASE_EXEMPT_PREFIXES = ("tests/fixtures/", "webui/tests/fixtures/")

LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
FENCE_RE = re.compile(r"^\s*(?:\x60{3}|~~~)")
HTML_REF_RE = re.compile(r"""(?:src|href)\s*=\s*["']([^"']+)["']""", re.IGNORECASE)
CSS_REF_RE = re.compile(r"""url\(\s*(["']?)([^"')]+)\1\s*\)""", re.IGNORECASE)


class LayoutError(RuntimeError):
    """A layout rule or a local link failed."""


def git_paths(root: Path, *args: str) -> list[str]:
    """Return NUL-separated Git paths, decoded as UTF-8."""
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", *args],
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        raise LayoutError(
            "git ls-files failed: " + result.stderr.decode("utf-8", "replace").strip()
        )
    return [item for item in result.stdout.decode("utf-8", "replace").split("\0") if item]


def tracked_files(root: Path) -> list[str]:
    return git_paths(root, "--cached")


def working_files(root: Path) -> list[str]:
    """Indexed files plus new, non-ignored files in the working tree."""
    return sorted(set(git_paths(root, "--cached", "--others", "--exclude-standard")))


def is_exempt(path: str, prefixes: tuple[str, ...]) -> bool:
    normalized = path.replace("\\", "/")
    return any(normalized == prefix.rstrip("/") or normalized.startswith(prefix)
               for prefix in prefixes)


def check_forbidden(files: list[str], root: Path) -> list[str]:
    errors: list[str] = []
    for path in files:
        parts = PurePosixPath(path).parts
        if any(part in FORBIDDEN_PARTS for part in parts):
            errors.append(f"tracked generated directory: {path}")
            continue
        if path.endswith(".pyc"):
            errors.append(f"tracked Python bytecode: {path}")
            continue
        if PurePosixPath(path).name in FORBIDDEN_NAMES:
            errors.append(f"tracked machine compile database: {path}")
            continue
        if path.replace("\\", "/") in FORBIDDEN_EXACT:
            errors.append(f"tracked command mistake directory/file: {path}")
            continue
        if path.replace("\\", "/").startswith(FORBIDDEN_PREFIXES):
            errors.append(f"tracked build output: {path}")
            continue
        if not (root / path).exists():
            # A staged deletion is expected while a migration is in progress.
            continue
    return errors


def case_collisions(files: list[str]) -> list[list[str]]:
    groups: dict[str, list[str]] = {}
    for path in files:
        if is_exempt(path, CASE_EXEMPT_PREFIXES):
            continue
        key = path.replace("\\", "/").casefold()
        groups.setdefault(key, []).append(path)
    return [group for group in groups.values() if len(group) > 1]


def stale_references(root: Path, files: list[str]) -> list[str]:
    errors: list[str] = []
    for path in files:
        if is_exempt(path, STALE_EXEMPT_PREFIXES):
            continue
        if PurePosixPath(path).suffix.lower() not in TEXT_SUFFIXES:
            continue
        full = root / path
        if not full.is_file():
            continue
        try:
            text = full.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for line_number, line in enumerate(text.splitlines(), 1):
            for label, pattern in STALE_PATTERNS:
                if pattern.search(line):
                    errors.append(f"{path}:{line_number}: stale {label}: {line.strip()}")
    return errors


def _is_external(target: str) -> bool:
    if not target or target.startswith(("#", "//", "data:", "mailto:", "tel:", "javascript:")):
        return True
    parsed = urlsplit(target)
    return bool(parsed.scheme) or bool(parsed.netloc)


def _resolve_target(root: Path, source: Path, raw: str) -> Path | None:
    target = raw.strip().strip("<>")
    if _is_external(target):
        return None
    target = unquote(target.split("#", 1)[0].split("?", 1)[0])
    if not target or any(char in target for char in "{}<>*"):
        return None
    candidate = Path(target)
    if candidate.is_absolute():
        return root / candidate.as_posix().lstrip("/")
    return source.parent / candidate


def find_broken_links(root: Path, files: list[str]) -> list[str]:
    errors: list[str] = []
    for path in files:
        if is_exempt(path, LINK_EXEMPT_PREFIXES):
            continue
        if PurePosixPath(path).suffix.lower() not in LINK_DOC_SUFFIXES:
            continue
        full = root / path
        if not full.is_file():
            continue
        in_fence = False
        try:
            lines = full.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeDecodeError):
            continue
        for line_number, line in enumerate(lines, 1):
            if FENCE_RE.match(line):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            for match in LINK_RE.finditer(line):
                resolved = _resolve_target(root, full, match.group(1))
                if resolved is not None and not resolved.exists():
                    errors.append(
                        f"{path}:{line_number}: missing local link: {match.group(1)}"
                    )
    return errors


def find_broken_asset_refs(root: Path, files: list[str]) -> list[str]:
    """Check relative src/href and CSS url() references.

    Vite root-absolute URLs (/src/main.jsx) are intentionally skipped: the
    dev/build server maps them, not the repository root. Test fixture HTML lives
    next to its JSX entry and is checked normally.
    """
    errors: list[str] = []
    for path in files:
        suffix = PurePosixPath(path).suffix.lower()
        if suffix not in {".html", ".htm", ".css"}:
            continue
        full = root / path
        if not full.is_file():
            continue
        try:
            text = full.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        refs = HTML_REF_RE.findall(text) if suffix in {".html", ".htm"} else []
        refs += [match.group(2) for match in CSS_REF_RE.finditer(text)]
        for raw in refs:
            target = raw.strip()
            if target.startswith("/") or _is_external(target):
                continue
            resolved = _resolve_target(root, full, target)
            if resolved is not None and not resolved.exists():
                errors.append(f"{path}: missing local asset: {target}")
    return errors


def run_checks(root: Path = ROOT) -> list[str]:
    tracked = tracked_files(root)
    working = working_files(root)
    errors: list[str] = []
    errors.extend(check_forbidden(tracked, root))
    errors.extend(f"case-insensitive path collision: {' | '.join(group)}"
                  for group in case_collisions(working))
    errors.extend(stale_references(root, working))
    errors.extend(find_broken_links(root, working))
    errors.extend(find_broken_asset_refs(root, working))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root")
    args = parser.parse_args(argv)
    root = args.root.expanduser().resolve()
    try:
        errors = run_checks(root)
    except LayoutError as error:
        print("layout check failed: " + str(error), file=sys.stderr)
        return 2
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        print(f"repository layout: {len(errors)} problem(s)", file=sys.stderr)
        return 1
    print("repository layout: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
