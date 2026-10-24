"""Refresh build assets after checking every source and destination for redirects.

Only build assets are replaced. The application's data directory and executable
are preserved. All four sources are staged before any existing asset is removed.
"""

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile


ASSETS = (
    ("webui/dist", "webui", "dist"),
    ("plugins", "plugins", ""),
    ("config", "config", ""),
    ("resources/licenses", "licenses", ""),
)
PROTECTED_ROOTS = (
    "webui", "plugins", "config", "resources", "src", "scripts", "tests",
    "docs", "dependencies", ".git", ".agents", ".codex", ".github", ".vscode",
)
CACHE_IGNORE = shutil.ignore_patterns("__pycache__", "*.pyc", ".git", "fetched", "staging", "node_modules")


@dataclass(frozen=True)
class AssetPlan:
    project: Path
    output: Path
    sources: tuple[tuple[Path, str, str], ...]


def absolute_path(value, base):
    path = Path(value).expanduser()
    # Do not normalize away a possible redirected component before checking it.
    if ".." in path.parts:
        raise ValueError("Asset paths cannot contain '..': " + str(path))
    return path if path.is_absolute() else base / path


def checked_stat(path):
    result = os.lstat(path)
    attributes = getattr(result, "st_file_attributes", 0)
    if (stat.S_ISLNK(result.st_mode)
            or attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)):
        raise ValueError("Runtime asset path is redirected: " + str(path))
    return result


def check_directory_chain(path, *, required=False):
    """Check lexical ancestors with lstat before any resolve/follow operation."""
    for candidate in reversed((path, *path.parents)):
        try:
            info = checked_stat(candidate)
        except FileNotFoundError:
            if required:
                raise
            continue
        if not stat.S_ISDIR(info.st_mode):
            raise ValueError("Runtime asset directory is not a directory: " + str(candidate))


def check_tree(root, *, source=False):
    check_directory_chain(root, required=True)
    def report_walk_error(error):
        raise error

    for directory, dirs, files in os.walk(root, followlinks=False, onerror=report_walk_error):
        current = Path(directory)
        if source:
            ignored = set(CACHE_IGNORE(str(current), dirs + files))
            dirs[:] = [name for name in dirs if name not in ignored]
            files = [name for name in files if name not in ignored]
        for name in dirs + files:
            path = current / name
            info = checked_stat(path)
            if not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
                raise ValueError("Runtime asset is not a regular file or directory: " + str(path))


def overlaps(first, second):
    return first == second or first in second.parents or second in first.parents


def make_plan(project, output):
    project = absolute_path(project, Path.cwd())
    check_directory_chain(project, required=True)
    project = project.resolve(strict=True)
    output = absolute_path(output, project)
    check_directory_chain(output)
    resolved_output = output.resolve()
    if resolved_output == project or not resolved_output.is_relative_to(project):
        raise ValueError("Runtime asset output must be a build directory inside the project")
    for name in PROTECTED_ROOTS:
        protected = (project / name).resolve()
        if overlaps(resolved_output, protected):
            raise ValueError("Runtime asset output overlaps project source: " + str(project / name))

    sources = tuple((project / relative, name, subdirectory)
                    for relative, name, subdirectory in ASSETS)
    for source, name, _ in sources:
        check_tree(source, source=True)
        resolved_source = source.resolve(strict=True)
        target = output / name
        check_directory_chain(target)
        resolved_target = target.resolve()
        if overlaps(resolved_target, resolved_source):
            raise ValueError("Runtime asset target overlaps its source: " + str(target))
        if target.exists():
            check_tree(target)
    return AssetPlan(project, output, sources)


def sync_runtime_assets(project, output):
    plan = make_plan(project, output)
    plan.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".runtime-assets-", dir=plan.output) as temporary:
        staging = Path(temporary)
        for source, name, subdirectory in plan.sources:
            target = staging / name
            if subdirectory:
                target /= subdirectory
            shutil.copytree(source, target, ignore=CACHE_IGNORE, symlinks=True)
        # Preserve newly introduced symbolic links instead of following them,
        # then reject any copied links before replacing old assets. Junctions
        # are rejected during preflight; this is not a concurrent-write lock.
        for _, name, _ in plan.sources:
            check_tree(staging / name)
        # A source read/copy failure above leaves all existing assets untouched.
        # Four independent directories are not a single atomic transaction.
        for _, name, _ in plan.sources:
            target = plan.output / name
            check_directory_chain(target)
            if target.exists():
                check_tree(target)
        for _, name, _ in plan.sources:
            target = plan.output / name
            if target.exists():
                shutil.rmtree(target)
            os.replace(staging / name, target)
    data = plan.output / "data"
    if not os.path.lexists(data):
        data.mkdir()
    return plan.output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path,
                        help="Build directory inside the project; relative paths use --project")
    args = parser.parse_args(argv)
    try:
        output = sync_runtime_assets(args.project, args.output)
    except (OSError, ValueError) as error:
        print("Runtime asset refresh failed: " + str(error), file=sys.stderr)
        return 1
    print("Runtime assets refreshed: " + str(output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
