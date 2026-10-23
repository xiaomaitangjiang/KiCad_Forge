#!/usr/bin/env python3
"""Preview and apply a bounded cleanup of the local build directory.

The cleanup is intentionally conservative:
  * the default mode only scans and reports;
  * --apply requires --yes and an explicit category;
  * only allow-listed paths below the repository build/ directory are touched;
  * build/mingw and build/dependencies are protected;
  * package and snapshot deletion requires an exact entry in a reviewed
    manifest with a matching tree fingerprint;
  * symlinks and reparse points on the build root, a candidate root, or an
    ancestor are rejected; nested links are unlinked, never traversed;
  * unreadable paths are reported and left alone.

Run from the repository root:
    python -B scripts/clean_build.py
    python -B scripts/clean_build.py --categories all --json build-cleanup.json
    python -B scripts/clean_build.py --apply --yes --categories cache
    python -B scripts/clean_build.py --fingerprint build/ci-dist/old-package
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
DEFAULT_MANIFEST = ROOT / "scripts" / "clean_build_manifest.json"

CATEGORIES = ("cache", "logs", "packages", "snapshot")

# These paths are never candidates, and no candidate may be their ancestor.
PROTECTED_RELATIVE = ("mingw", "dependencies")

FILE_ATTRIBUTE_REPARSE_POINT = 0x400


class CleanupError(RuntimeError):
    """A cleanup target is unsafe or cannot be processed."""


def is_reparse(stat_result: os.stat_result) -> bool:
    """Return True for POSIX symlinks and Windows reparse points."""
    return stat.S_ISLNK(stat_result.st_mode) or bool(
        getattr(stat_result, "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT
    )


def _absolute(path: Path | str) -> Path:
    """Return a normalized lexical path without resolving links."""
    return Path(os.path.abspath(os.fspath(path)))


def _same(left: Path | str, right: Path | str) -> bool:
    return os.path.normcase(str(_absolute(left))) == os.path.normcase(str(_absolute(right)))


def _within(child: Path | str, parent: Path | str) -> bool:
    child_text = os.path.normcase(str(_absolute(child)))
    parent_text = os.path.normcase(str(_absolute(parent)))
    try:
        return os.path.commonpath([child_text, parent_text]) == parent_text
    except ValueError:
        return False


def _relative(path: Path | str, build_root: Path | str) -> str:
    if not _within(path, build_root):
        raise CleanupError(f"target is outside build/: {path}")
    return Path(os.path.relpath(str(_absolute(path)), str(_absolute(build_root)))).as_posix()


def _assert_no_reparse(path: Path | str, build_root: Path | str) -> None:
    root = _absolute(build_root)
    current = _absolute(path)
    while True:
        if os.path.lexists(current):
            try:
                st = os.lstat(current)
            except OSError as exc:
                raise CleanupError(f"cannot inspect path: {current}: {exc}") from exc
            if is_reparse(st):
                raise CleanupError(f"target or ancestor is a link or reparse point: {current}")
        if _same(current, root):
            break
        parent = current.parent
        if _same(parent, current):
            break
        current = parent


def validate_target(path: Path, build_root: Path) -> Path:
    """Return the lexical target, or raise if it is outside the allow-list."""
    root = _absolute(build_root)
    target = _absolute(path)
    if not _within(target, root):
        raise CleanupError(f"target is outside build/: {path}")
    if _same(target, root):
        raise CleanupError("refusing to remove build/ itself")
    if _same(target, root / "ci-dist"):
        raise CleanupError("refusing to remove build/ci-dist itself")
    for relative in PROTECTED_RELATIVE:
        protected = root / relative
        if _same(target, protected) or _within(target, protected) or _within(protected, target):
            raise CleanupError(f"target overlaps a protected path: {path}")
    _assert_no_reparse(target, root)
    return target


def scan_path(path: Path) -> dict:
    """Count files and logical bytes without following symlinks or reparse points."""
    result = {
        "path": str(path),
        "exists": os.path.lexists(path),
        "files": 0,
        "bytes": 0,
        "links": [],
        "unreadable": [],
    }
    if not result["exists"]:
        return result
    st = os.lstat(path)
    if is_reparse(st):
        result["links"].append(str(path))
        return result
    if not stat.S_ISDIR(st.st_mode):
        result["files"] += 1
        result["bytes"] += int(st.st_size)
        return result
    stack = [path]
    while stack:
        current = stack.pop()
        try:
            with os.scandir(current) as entries:
                for entry in entries:
                    try:
                        st = entry.stat(follow_symlinks=False)
                    except OSError as exc:
                        result["unreadable"].append(f"{entry.path}: {exc}")
                        continue
                    if is_reparse(st):
                        result["links"].append(str(entry.path))
                        continue
                    if entry.is_dir(follow_symlinks=False):
                        stack.append(Path(entry.path))
                    else:
                        result["files"] += 1
                        result["bytes"] += int(st.st_size)
        except OSError as exc:
            result["unreadable"].append(f"{current}: {exc}")
    return result


def tree_fingerprint(path: Path) -> str:
    """Hash the reviewed tree without following links.

    The digest covers relative paths, entry types, file sizes and file bytes,
    so replacing the target or adding data after review changes the digest.
    """
    root = _absolute(path)
    if not os.path.lexists(root):
        raise CleanupError(f"reviewed tree is missing: {path}")
    st = os.lstat(root)
    if is_reparse(st):
        raise CleanupError(f"reviewed tree root is a link or reparse point: {path}")
    if stat.S_ISREG(st.st_mode):
        digest = hashlib.sha256()
        digest.update(
            b"F\0.\0" + str(st.st_size).encode("ascii") + b"\0"
        )
        try:
            with open(root, "rb") as handle:
                for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                    digest.update(chunk)
        except OSError as exc:
            raise CleanupError(f"cannot read reviewed file: {root}: {exc}") from exc
        return digest.hexdigest()
    if not stat.S_ISDIR(st.st_mode):
        raise CleanupError(f"reviewed tree is not a file or directory: {path}")
    digest = hashlib.sha256()
    stack: list[tuple[Path, Path]] = [(root, Path("."))]
    while stack:
        current, relative = stack.pop()
        try:
            st = os.lstat(current)
        except OSError as exc:
            raise CleanupError(f"cannot inspect reviewed tree: {current}: {exc}") from exc
        relative_bytes = relative.as_posix().encode("utf-8")
        if is_reparse(st):
            digest.update(b"L\0" + relative_bytes + b"\0")
            continue
        if stat.S_ISDIR(st.st_mode):
            digest.update(b"D\0" + relative_bytes + b"\0")
            try:
                with os.scandir(current) as entries:
                    children = sorted(entries, key=lambda entry: entry.name.lower())
            except OSError as exc:
                raise CleanupError(f"cannot read reviewed tree: {current}: {exc}") from exc
            for entry in children:
                stack.append((Path(entry.path), relative / entry.name))
        elif stat.S_ISREG(st.st_mode):
            digest.update(b"F\0" + relative_bytes + b"\0" + str(st.st_size).encode("ascii") + b"\0")
            try:
                with open(current, "rb") as handle:
                    for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                        digest.update(chunk)
            except OSError as exc:
                raise CleanupError(f"cannot read reviewed file: {current}: {exc}") from exc
        else:
            digest.update(b"O\0" + relative_bytes + b"\0")
    return digest.hexdigest()


def _remove_entry(path: Path, errors: list[str]) -> None:
    try:
        st = os.lstat(path)
    except FileNotFoundError:
        return
    if is_reparse(st):
        try:
            if stat.S_ISDIR(st.st_mode):
                os.rmdir(path)
            else:
                os.unlink(path)
        except OSError as exc:
            errors.append(f"{path}: {exc}")
        return
    if stat.S_ISDIR(st.st_mode):
        try:
            with os.scandir(path) as entries:
                children = [Path(entry.path) for entry in entries]
        except OSError as exc:
            errors.append(f"{path}: {exc}")
            return
        for child in children:
            _remove_entry(child, errors)
        try:
            os.rmdir(path)
        except OSError as exc:
            errors.append(f"{path}: {exc}")
        return
    try:
        os.unlink(path)
    except OSError as exc:
        errors.append(f"{path}: {exc}")


def remove_path(path: Path, build_root: Path, expected_fingerprint: str | None = None) -> dict:
    target = validate_target(path, build_root)
    before = scan_path(target)
    errors: list[str] = []
    if before["exists"]:
        if expected_fingerprint:
            actual = tree_fingerprint(target)
            if actual != expected_fingerprint:
                raise CleanupError(
                    f"reviewed fingerprint no longer matches for {target}: "
                    f"expected {expected_fingerprint}, found {actual}"
                )
        _remove_entry(target, errors)
    return {
        "path": str(target),
        "removed": not os.path.lexists(target),
        "before": before,
        "errors": errors,
    }


def _manifest_path(manifest_path: Path | None) -> Path:
    if manifest_path is None:
        return DEFAULT_MANIFEST
    path = Path(manifest_path)
    return path if path.is_absolute() else ROOT / path


def _load_manifest(manifest: dict | None, manifest_path: Path | None) -> tuple[str, list]:
    if manifest is not None:
        data = manifest
        source = "<in-memory>"
    else:
        path = _manifest_path(manifest_path)
        source = str(path)
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError as exc:
            raise CleanupError(f"reviewed manifest is missing: {path}") from exc
        except (OSError, json.JSONDecodeError) as exc:
            raise CleanupError(f"reviewed manifest cannot be read: {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise CleanupError("reviewed manifest must be a JSON object")
    if data.get("schema") != 1:
        raise CleanupError("reviewed manifest schema must be 1")
    entries = data.get("entries")
    if not isinstance(entries, list):
        raise CleanupError("reviewed manifest entries must be a list")
    return source, entries


def _normalize_manifest(entries: list, build_root: Path) -> list[dict]:
    normalized: list[dict] = []
    seen: set[str] = set()
    for index, raw in enumerate(entries, 1):
        if not isinstance(raw, dict):
            raise CleanupError(f"manifest entry {index} must be an object")
        relative_text = raw.get("path")
        if not isinstance(relative_text, str) or not relative_text.strip():
            raise CleanupError(f"manifest entry {index} has no path")
        relative = Path(relative_text.replace("\\", "/"))
        if relative.is_absolute() or any(part in ("", ".", "..") for part in relative.parts):
            raise CleanupError(f"manifest entry {index} has an unsafe path: {relative_text}")
        rel = _relative(build_root / relative, build_root)
        category = raw.get("category")
        if category not in ("packages", "snapshot"):
            raise CleanupError(f"manifest entry {index} has an invalid category: {category}")
        parts = Path(rel).parts
        if category == "packages":
            allowed = (
                len(parts) == 2 and parts[0] == "ci-dist" and bool(parts[1])
            ) or (len(parts) == 1 and parts[0] in ("ui-check-dist", "d-regression-dist"))
        else:
            allowed = len(parts) == 1 and parts[0].startswith("commit-preparation-")
        if not allowed:
            raise CleanupError(f"manifest entry {index} is not an allowed {category} target: {rel}")
        for field in ("purpose", "data", "recovery"):
            value = raw.get(field)
            if not isinstance(value, str) or not value.strip():
                raise CleanupError(f"manifest entry {index} needs a non-empty {field}")
        data_action = raw["data"].strip().lower()
        if data_action not in ("none", "archived"):
            raise CleanupError(
                f"manifest entry {index} data must be 'none' or 'archived', got: {raw['data']}"
            )
        fingerprint = raw.get("tree_sha256")
        if not isinstance(fingerprint, str) or len(fingerprint) != 64:
            raise CleanupError(f"manifest entry {index} needs a 64-character tree_sha256")
        try:
            int(fingerprint, 16)
        except ValueError as exc:
            raise CleanupError(f"manifest entry {index} tree_sha256 is not hex") from exc
        if rel in seen:
            raise CleanupError(f"manifest contains duplicate target: {rel}")
        seen.add(rel)
        normalized.append(
            {
                "path": rel,
                "category": category,
                "purpose": raw["purpose"].strip(),
                "data": data_action,
                "recovery": raw["recovery"].strip(),
                "tree_sha256": fingerprint.lower(),
            }
        )
    return normalized


def _children_no_follow(path: Path) -> list[Path]:
    if not os.path.lexists(path):
        return []
    try:
        st = os.lstat(path)
    except OSError:
        return []
    if is_reparse(st) or not stat.S_ISDIR(st.st_mode):
        return []
    try:
        with os.scandir(path) as entries:
            return sorted((Path(entry.path) for entry in entries), key=lambda item: item.name.lower())
    except OSError:
        return []


def collect_targets(
    build_root: Path,
    categories: list[str],
    entries: list[dict] | None = None,
) -> tuple[list[Path], list[str]]:
    """Return reviewed targets and retained paths.

    Cache and log targets are fixed build artifacts. Package and snapshot
    targets are accepted only when an exact reviewed manifest entry exists.
    Unknown packages and snapshots are reported as retained, never removed.
    """
    entries = entries or []
    targets: list[Path] = []
    retained: list[str] = []
    seen: set[str] = set()
    approved = {(entry["category"], entry["path"]) for entry in entries}
    root_link = os.path.lexists(build_root) and is_reparse(os.lstat(build_root))

    def add_target(path: Path) -> None:
        key = os.path.normcase(str(_absolute(path)))
        if key not in seen:
            seen.add(key)
            targets.append(path)

    def add_retained(path: Path) -> None:
        key = os.path.normcase(str(_absolute(path)))
        if key not in seen:
            seen.add(key)
            retained.append(str(path))

    if "cache" in categories:
        for name in (".build_cache", ".objs", ".deps", ".gens"):
            add_target(build_root / name)
    if "logs" in categories:
        for name in ("build-all.log", "link-error.log", "test-all.log"):
            add_target(build_root / name)
    if "packages" in categories:
        if root_link:
            add_retained(build_root)
        else:
            ci = build_root / "ci-dist"
            if os.path.lexists(ci):
                if is_reparse(os.lstat(ci)):
                    add_retained(ci)
                else:
                    for child in _children_no_follow(ci):
                        rel = _relative(child, build_root)
                        if ("packages", rel) in approved:
                            add_target(child)
                        else:
                            add_retained(child)
            for name in ("ui-check-dist", "d-regression-dist"):
                path = build_root / name
                if ("packages", name) in approved:
                    add_target(path)
                elif os.path.lexists(path):
                    add_retained(path)
        for entry in entries:
            if entry["category"] == "packages":
                add_target(build_root / entry["path"])
    if "snapshot" in categories:
        for entry in entries:
            if entry["category"] == "snapshot":
                add_target(build_root / entry["path"])
        if root_link:
            add_retained(build_root)
        else:
            for child in _children_no_follow(build_root):
                if child.name.startswith("commit-preparation-"):
                    rel = _relative(child, build_root)
                    if ("snapshot", rel) not in approved:
                        add_retained(child)
    return targets, retained


def busy_processes() -> list[str]:
    """Best-effort Windows process check; never kills anything."""
    if os.name != "nt":
        return []
    try:
        result = subprocess.run(
            ["tasklist", "/FO", "CSV", "/NH"],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError:
        return []
    if result.returncode != 0:
        return []
    names = ("kicad_forge.exe", "xmake.exe", "iscc.exe")
    found = []
    for line in result.stdout.splitlines():
        first = line.split(",", 1)[0].strip('"').lower()
        if first in names:
            found.append(first)
    return sorted(set(found))


def git_head(root: Path) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return result.stdout.strip() if result.returncode == 0 else ""
    except OSError:
        return ""


def cleanup(
    build_root: Path,
    categories: list[str],
    apply: bool,
    manifest: dict | None = None,
    manifest_path: Path | None = None,
) -> dict:
    build_root = _absolute(build_root)
    entries: list[dict] = []
    manifest_source = ""
    manifest_error = ""
    if "packages" in categories or "snapshot" in categories:
        try:
            manifest_source, raw_entries = _load_manifest(manifest, manifest_path)
            entries = _normalize_manifest(raw_entries, build_root)
        except CleanupError as exc:
            if apply:
                raise
            manifest_error = str(exc)
            manifest_source = str(_manifest_path(manifest_path))
    targets, retained = collect_targets(build_root, categories, entries)
    before_free = shutil.disk_usage(build_root).free if build_root.exists() else 0
    report = {
        "observed_at": datetime.now(timezone.utc).isoformat(),
        "root": str(ROOT),
        "build_root": str(build_root),
        "git_head": git_head(ROOT),
        "categories": categories,
        "apply": apply,
        "busy_processes": busy_processes(),
        "manifest": {
            "path": manifest_source,
            "entries": len(entries),
            "error": manifest_error,
        },
        "retained": retained,
        "retained_packages": [Path(path).name for path in retained],
        "before_free_bytes": before_free,
        "targets": [],
    }
    if apply and report["busy_processes"]:
        raise CleanupError(
            "refusing to apply while build/application processes are running: "
            + ", ".join(report["busy_processes"])
        )
    by_path = {entry["path"]: entry for entry in entries}
    for path in targets:
        try:
            target = validate_target(path, build_root)
        except CleanupError as exc:
            report["targets"].append({"path": str(path), "error": str(exc)})
            continue
        rel = _relative(target, build_root)
        entry = by_path.get(rel)
        item = {"path": str(target), "exists": os.path.lexists(target)}
        if entry:
            item["reviewed"] = True
        if item["exists"]:
            item["before"] = scan_path(target)
            if entry and apply:
                try:
                    item.update(remove_path(target, build_root, entry["tree_sha256"]))
                except CleanupError as exc:
                    item["error"] = str(exc)
            elif apply:
                item.update(remove_path(target, build_root))
            elif entry:
                try:
                    actual = tree_fingerprint(target)
                except CleanupError as exc:
                    item["error"] = str(exc)
                else:
                    if actual != entry["tree_sha256"]:
                        item["error"] = (
                            f"reviewed fingerprint no longer matches: expected "
                            f"{entry['tree_sha256']}, found {actual}"
                        )
                    else:
                        item["fingerprint_matches"] = True
        elif entry and apply:
            item["error"] = "reviewed target is missing"
        report["targets"].append(item)
    report["errors"] = [
        error
        for item in report["targets"]
        for error in ([item["error"]] if item.get("error") else [])
        + list(item.get("errors") or [])
    ]
    if apply:
        report["after_free_bytes"] = shutil.disk_usage(build_root).free if build_root.exists() else 0
        report["logical_freed_bytes"] = sum(
            int(item.get("before", {}).get("bytes", 0))
            for item in report["targets"]
            if item.get("removed")
        )
    return report


def print_report(report: dict) -> None:
    mode = "APPLY" if report["apply"] else "PREVIEW (no files removed)"
    print(f"build cleanup: {mode}")
    print(f"  root: {report['build_root']}")
    print(f"  git: {report['git_head'] or '(unknown)'}")
    print(f"  categories: {', '.join(report['categories'])}")
    if report.get("busy_processes"):
        print("  busy: " + ", ".join(report["busy_processes"]))
    manifest = report.get("manifest") or {}
    if manifest:
        print(f"  manifest: {manifest.get('path')} ({manifest.get('entries', 0)} entries)")
        if manifest.get("error"):
            print(f"  manifest ERROR: {manifest['error']}")
    retained = report.get("retained") or []
    if retained:
        print(f"  retained (not in reviewed manifest): {len(retained)}")
        for path in retained:
            print(f"    {path}")
    total_bytes = 0
    errors = 1 if manifest.get("error") else 0
    for item in report["targets"]:
        if "error" in item:
            errors += 1
            print(f"  ERROR {item['path']}: {item['error']}")
            continue
        before = item.get("before") or {}
        total_bytes += int(before.get("bytes", 0))
        if not item.get("exists"):
            print(f"  missing  {item['path']}")
            continue
        state = "removed" if item.get("removed") else ("kept" if report["apply"] else "would remove")
        print(
            f"  {state:12} {item['path']}  "
            f"{before.get('files', 0)} files, {before.get('bytes', 0) / (1024 ** 3):.2f} GiB"
            f"{', links=' + str(len(before.get('links', []))) if before.get('links') else ''}"
            f"{', unreadable=' + str(len(before.get('unreadable', []))) if before.get('unreadable') else ''}"
            f"{', reviewed' if item.get('reviewed') else ''}"
        )
        errors += len(item.get("errors") or [])
    print(f"  candidate logical total: {total_bytes / (1024 ** 3):.2f} GiB")
    if report["apply"]:
        freed = int(report.get("logical_freed_bytes", 0)) / (1024 ** 3)
        actual = (report.get("after_free_bytes", 0) - report.get("before_free_bytes", 0)) / (1024 ** 3)
        print(f"  logical removed: {freed:.2f} GiB; volume free-space delta: {actual:+.2f} GiB")
    print(f"  errors: {errors}")
    if not report["apply"] and report.get("busy_processes"):
        print("  note: busy process found; apply would be refused until it exits")
    print("  default is preview; --apply requires --yes and an explicit --categories value")


def _parse_categories(parser: argparse.ArgumentParser, value: str | None, apply: bool) -> list[str]:
    if value is None:
        if apply:
            parser.error("--apply requires an explicit --categories value")
        return list(CATEGORIES)
    parts = [part.strip() for part in value.split(",") if part.strip()]
    if "all" in parts:
        if len(parts) != 1:
            parser.error("--categories all must be used alone")
        return list(CATEGORIES)
    unknown = [part for part in parts if part not in CATEGORIES]
    if unknown:
        parser.error(f"unknown category: {', '.join(unknown)}")
    if not parts:
        parser.error("--categories must not be empty")
    return parts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--categories",
        default=None,
        help="comma-separated: cache,logs,packages,snapshot,all "
        "(default: all for preview; required for --apply)",
    )
    parser.add_argument("--apply", action="store_true", help="delete the listed targets")
    parser.add_argument("--yes", action="store_true", help="confirm non-interactive apply")
    parser.add_argument("--json", type=Path, help="write the JSON report to this path")
    parser.add_argument(
        "--manifest",
        type=Path,
        default=DEFAULT_MANIFEST,
        help="reviewed deletion manifest (default: scripts/clean_build_manifest.json)",
    )
    parser.add_argument(
        "--fingerprint",
        type=Path,
        metavar="PATH",
        help="print the reviewed tree fingerprint for PATH and exit",
    )
    args = parser.parse_args(argv)
    if args.fingerprint is not None:
        if args.apply:
            parser.error("--fingerprint cannot be combined with --apply")
        try:
            target = validate_target(args.fingerprint, BUILD)
            print(tree_fingerprint(target))
        except CleanupError as exc:
            print(f"build cleanup: ERROR: {exc}", file=sys.stderr)
            return 2
        return 0
    categories = _parse_categories(parser, args.categories, args.apply)
    if args.apply and not args.yes:
        parser.error("--apply requires --yes; preview is the default")
    try:
        report = cleanup(
            BUILD,
            categories,
            args.apply,
            manifest_path=args.manifest,
        )
    except CleanupError as exc:
        print(f"build cleanup: ERROR: {exc}", file=sys.stderr)
        return 2
    print_report(report)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 1 if report.get("errors") or report["manifest"].get("error") else 0


if __name__ == "__main__":
    raise SystemExit(main())
