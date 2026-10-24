"""Prepare SHA256-locked packaging tools and private Python before build."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import shutil
import stat
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

if __package__:
    from .sync_runtime_assets import check_directory_chain, check_tree
else:
    from sync_runtime_assets import check_directory_chain, check_tree

PROJECT = Path(__file__).resolve().parents[1]
LOCK = PROJECT / "dependencies/packaging.lock.json"
DEFAULT_CACHE = PROJECT / "build/packaging-cache"
PREPARED = PROJECT / "build/packaging-deps"


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def load_lock():
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    for entry in lock["licenses"]:
        if sha256(PROJECT / entry["path"]) != entry["sha256"]:
            raise ValueError("License SHA256 mismatch: " + entry["path"])
    return lock


def cached_artifact(artifact, cache, *, download):
    cache = Path(cache).absolute()
    check_directory_chain(cache)
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / artifact["filename"]
    if not target.exists():
        if not download:
            raise FileNotFoundError("Dependency missing; prepare with --download first: " + str(target))
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as stream:
            temporary = Path(stream.name)
            try:
                with urllib.request.urlopen(artifact["url"], timeout=60) as response:
                    shutil.copyfileobj(response, stream)
                stream.close()
                if sha256(temporary) != artifact["sha256"]:
                    raise ValueError("Downloaded dependency SHA256 mismatch: " + target.name)
                os.replace(temporary, target)
            finally:
                stream.close()
                temporary.unlink(missing_ok=True)
    if target.is_symlink() or sha256(target) != artifact["sha256"]:
        raise ValueError("Cached dependency SHA256 mismatch: " + target.name)
    return target


def archive_path(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or ".." in path.parts or ":" in name or "\\" in name:
        raise ValueError("Unsafe archive path: " + name)
    return path


def extract_zip(archive, output):
    output = Path(output)
    check_directory_chain(output)
    with zipfile.ZipFile(archive) as stream:
        for info in stream.infolist():
            relative = archive_path(info.filename)
            if stat.S_ISLNK(info.external_attr >> 16):
                raise ValueError("ZIP symlinks are not supported")
            path = output / Path(relative)
            if info.is_dir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                with stream.open(info) as source, path.open("wb") as destination:
                    shutil.copyfileobj(source, destination)


def extract_python(archive, output):
    """Materialize internal aliases, so preparation also works on Windows."""
    output = Path(output)
    check_directory_chain(output)
    with tarfile.open(archive, "r:gz") as stream:
        for info in stream.getmembers():
            relative = archive_path(info.name)
            if relative.parts[0] != "python":
                raise ValueError("Unexpected Python archive root")
            # The application runs plugins; it does not ship a package manager.
            name = relative.as_posix().lower()
            if "__pycache__" in relative.parts or relative.suffix.lower() in (".pyc", ".pyo") or any(name.startswith(prefix) for prefix in (
                    "python/lib/python3.12/site-packages/pip", "python/lib/python3.12/ensurepip",
                    "python/lib/site-packages/pip", "python/lib/ensurepip", "python/scripts/pip", "python/bin/pip")):
                continue
            if info.issym() or info.islnk():
                target = info.linkname if info.islnk() else posixpath.join(str(relative.parent), info.linkname)
                normalized = archive_path(posixpath.normpath(target))
                if normalized.parts[0] != "python":
                    raise ValueError("Python archive link escapes root")
            elif not info.isdir() and not info.isfile():
                raise ValueError("Unsupported Python archive entry")
            path = output / Path(relative)
            if info.isdir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                with stream.extractfile(info) as source, path.open("wb") as destination:
                    shutil.copyfileobj(source, destination)
                # Link metadata has no executable bits; copy the target mode.
                mode = stream.getmember(str(normalized)).mode if info.issym() or info.islnk() else info.mode
                path.chmod(mode & 0o777)


def write_manifest(root, arch, *, platform=None):
    root = Path(root)
    files = {p.relative_to(root).as_posix(): sha256(p) for p in sorted(root.rglob("*"))
             if p.is_file() and p.name != "kforge-runtime.json"}
    (root / "kforge-runtime.json").write_text(json.dumps({
        "arch": arch, "platform": platform, "lock_sha256": sha256(LOCK), "files": files}, indent=2) + "\n", encoding="utf-8")


def verify_prepared(root, arch, *, platform=None):
    root = Path(root)
    check_tree(root)
    manifest = json.loads((root / "kforge-runtime.json").read_text(encoding="utf-8"))
    if manifest.get("arch") != arch:
        raise ValueError("Prepared dependency architecture mismatch")
    if platform is not None and manifest.get("platform") != platform:
        raise ValueError("Prepared dependency platform mismatch")
    if manifest.get("lock_sha256") != sha256(LOCK):
        raise ValueError("Prepared dependency lock changed; prepare again")
    files = manifest.get("files", {})
    actual = {p.relative_to(root).as_posix() for p in root.rglob("*")
              if p.is_file() and p.name != "kforge-runtime.json"}
    if not files or actual != set(files):
        raise ValueError("Prepared dependency file inventory changed")
    for name, expected in files.items():
        path = root / Path(archive_path(name))
        if sha256(path) != expected:
            raise ValueError("Prepared dependency SHA256 mismatch: " + name)


def publish_prepared(payload, output, arch, platform, *, refresh=False):
    """Replace a generated dependency only after its complete replacement passes."""
    output = Path(output).absolute()
    check_directory_chain(output)
    if not output.resolve().is_relative_to(PREPARED.resolve()) or output.resolve() == PREPARED.resolve():
        raise ValueError("Prepared output must stay inside build/packaging-deps")
    write_manifest(payload, arch, platform=platform)
    verify_prepared(payload, arch, platform=platform)
    backup = Path(payload).parent / "previous"
    if output.exists():
        if not refresh:
            verify_prepared(output, arch, platform=platform)
            return output
        check_tree(output)
        os.replace(output, backup)
    try:
        os.replace(payload, output)
    except OSError:
        if backup.exists():
            os.replace(backup, output)
        raise
    return output


def prepare_component(lock, platform, arch, kind, archives, *, refresh):
    output = PREPARED / platform / arch / kind
    check_directory_chain(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() and not refresh:
        verify_prepared(output, arch, platform=platform)
        return output
    with tempfile.TemporaryDirectory(prefix=".prepare-", dir=output.parent) as temporary:
        stage = Path(temporary)
        if kind == "nsis":
            extract_zip(archives[0], stage)
            roots = [p for p in stage.iterdir() if p.is_dir() and (p / "makensis.exe").is_file()]
            if len(roots) != 1:
                raise ValueError("NSIS archive must contain one compiler directory")
            payload = roots[0]
        else:
            extract_python(archives[0], stage)
            payload = stage / "python"
            site_packages = "Lib/site-packages" if platform == "mingw" else "lib/python3.12/site-packages"
            extract_zip(archives[1], payload / site_packages)
            licenses = payload / "licenses"
            licenses.mkdir()
            for entry in lock["licenses"]:
                shutil.copy2(PROJECT / entry["path"], licenses / Path(entry["path"]).name)
        publish_prepared(payload, output, arch, platform, refresh=refresh)
    return output


def prepare(platform, arch, *, cache=DEFAULT_CACHE, download=False, refresh=False):
    lock = load_lock()
    key = "python-" + platform + "-" + arch
    if key not in lock["artifacts"]:
        raise ValueError("Unsupported packaging platform/architecture")
    names = (key, "certifi", "nsis") if platform == "mingw" else (key, "certifi")
    archives = [cached_artifact(lock["artifacts"][name], cache, download=download) for name in names]
    if platform == "mingw":
        prepare_component(lock, platform, arch, "nsis", archives[2:], refresh=refresh)
    return prepare_component(lock, platform, arch, "python", archives[:2], refresh=refresh)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", required=True, choices=("mingw", "linux", "macosx"))
    parser.add_argument("--arch", required=True, choices=("x86_64", "arm64"))
    parser.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    parser.add_argument("--download", action="store_true", help="Allow downloads in this explicit preparation step")
    parser.add_argument("--refresh", action="store_true", help="Replace generated dependencies after validating their replacements")
    args = parser.parse_args()
    try:
        print(prepare(args.platform, args.arch, cache=args.cache, download=args.download, refresh=args.refresh))
    except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
        print("Packaging dependency preparation failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
