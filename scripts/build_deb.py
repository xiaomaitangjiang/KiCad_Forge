"""Build XPack's Linux binary installer with native dpkg tools; never install it."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

if __package__:
    from .stage_distribution import ASSETS, validated_source, validate_output, validate_runtime
    from .sync_runtime_assets import CACHE_IGNORE
else:
    from stage_distribution import ASSETS, validated_source, validate_output, validate_runtime
    from sync_runtime_assets import CACHE_IGNORE

PROJECT = Path(__file__).resolve().parents[1]
TEMPLATES = PROJECT / "packaging/linux"
MAINTAINER = "WESP_Wheat <2109941907@qq.com>"


def is_elf(path):
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"


def validate_elf(path):
    with path.open("rb") as stream:
        header = stream.read(20)
    if (len(header) != 20 or header[:6] != b"\x7fELF\x02\x01"
            or struct.unpack_from("<H", header, 18)[0] != 62):
        raise ValueError("Debian package requires native x86_64 Linux ELF binaries: " + str(path))


def write_control(root, version, depends):
    size = sum(p.stat().st_size for p in root.rglob("*") if p.is_file())
    control = root / "DEBIAN/control"
    control.parent.mkdir(exist_ok=True)
    control.write_text((TEMPLATES / "control.in").read_text(encoding="utf-8").format(
        version=version, maintainer=MAINTAINER, depends=depends, installed_size=(size + 1023) // 1024),
        encoding="utf-8", newline="\n")


def shared_dependencies(root, work, tool):
    # DEBIAN/control identifies all private libraries as belonging to this
    # package. dpkg-shlibdeps then excludes them, while requiring dependency
    # information for every external system library.
    debian = work / "debian"
    debian.mkdir()
    (debian / "control").write_text(
        "Source: kicad-forge\nMaintainer: " + MAINTAINER + "\n\nPackage: kicad-forge\nArchitecture: amd64\n"
        "Description: KiCad component library manager\n", encoding="utf-8", newline="\n")
    app = root / "opt/kicad-forge"
    binaries = sorted(p for p in app.rglob("*") if p.is_file() and is_elf(p))
    shim = app / "runtime/python/lib/libpython3.so"
    if shim in binaries:
        # dpkg-shlibdeps expands ORIGIN in RPATH, but not in DT_NEEDED.
        # This stable-ABI shim only forwards to the separately scanned private
        # libpython and libc/pthread. Check that contract before excluding it.
        result = subprocess.run(["objdump", "-p", str(shim)], check=True,
                                capture_output=True, text=True)
        needed = set(re.findall(r"^\s*NEEDED\s+(\S+)", result.stdout, re.MULTILINE))
        private = "$ORIGIN/../lib/libpython3.12.so.1.0"
        if private not in needed or needed - {private, "libc.so.6", "libpthread.so.0"}:
            raise RuntimeError("Unexpected system dependencies in the Python stable-ABI shim")
        binaries.remove(shim)
    result = subprocess.run([tool, "-O", "-S" + str(root), "-l" + str(app / "runtime/python/lib"),
                             *("-e" + str(binary) for binary in binaries)],
                            cwd=work, check=True, capture_output=True, text=True)
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    dependencies = [line.removeprefix("shlibs:Depends=") for line in result.stdout.splitlines()
                    if line.startswith("shlibs:Depends=")]
    if len(dependencies) != 1 or not dependencies[0].strip():
        raise RuntimeError("dpkg-shlibdeps did not produce system library dependencies")
    if re.search(r"\b(?:lib)?python[0-9.-]*\b", dependencies[0], re.IGNORECASE):
        raise RuntimeError("Private Python unexpectedly requires a system Python package")
    return dependencies[0] + ", xdg-utils"


def build_deb(source, output, name, version, arch):
    if arch != "x86_64" or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Debian packaging requires x86_64 and a major.minor.patch version")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name) or name.endswith(".deb"):
        raise ValueError("Package name must be one filename without the .deb extension")
    source, executable = validated_source(source, "linux")
    runtime = validate_runtime(source / "runtime/python", "linux", arch)
    metadata = json.loads((source / "kforge-distribution.json").read_text(encoding="utf-8"))
    if any(metadata.get(key) != value for key, value in
           (("platform", "linux"), ("arch", arch), ("version", version))):
        raise ValueError("Distribution metadata does not match the Debian package")
    validate_elf(source / executable)
    validate_elf(runtime / "bin/python3")
    output = validate_output(output, source)
    compiler, scanner = shutil.which("dpkg-deb"), shutil.which("dpkg-shlibdeps")
    if not compiler or not scanner:
        raise RuntimeError("Native dpkg-deb and dpkg-shlibdeps (dpkg-dev) are required")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".deb-", dir=output) as temporary:
        work = Path(temporary)
        root = work / "package"
        app = root / "opt/kicad-forge"
        app.mkdir(parents=True)
        for name_in in (*ASSETS, "runtime", executable, "LICENSE", "README.md", "kforge-distribution.json"):
            src, dst = source / name_in, app / name_in
            if src.is_dir():
                shutil.copytree(src, dst, symlinks=True, ignore=CACHE_IGNORE)
            else:
                shutil.copy2(src, dst)
        for src, relative, mode in (
            (TEMPLATES / "kicad-forge", "usr/bin/kicad-forge", 0o755),
            (TEMPLATES / "kicad-forge.desktop", "usr/share/applications/kicad-forge.desktop", 0o644),
            (source / "LICENSE", "usr/share/doc/kicad-forge/copyright", 0o644),
        ):
            dst = root / relative
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(src.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
            dst.chmod(mode)
        icon = root / "usr/share/pixmaps/kicad-forge.png"
        icon.parent.mkdir(parents=True)
        shutil.copy2(PROJECT / "resources/icons/app_logo.png", icon)
        write_control(root, version, "xdg-utils")
        write_control(root, version, shared_dependencies(root, work, scanner))
        for path in (root, *root.rglob("*")):
            path.chmod(0o755 if path.is_dir() or path.stat().st_mode & 0o111 else 0o644)
        (app / executable).chmod(0o755)
        (app / "runtime/python/bin/python3").chmod(0o755)
        staged = work / "installer.deb"
        subprocess.run([compiler, "--root-owner-group", "--uniform-compression", "-Zxz",
                        "--threads-max=2", "--build", str(root), str(staged)], check=True)
        if not staged.is_file() or staged.stat().st_size <= 8:
            raise RuntimeError("dpkg-deb did not produce a Debian installer")
        with staged.open("rb") as stream:
            if stream.read(8) != b"!<arch>\n":
                raise RuntimeError("dpkg-deb did not produce a Debian installer")
        artifact = output / (name + ".deb")
        os.replace(staged, artifact)
    return artifact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--arch", required=True)
    args = parser.parse_args()
    try:
        if sys.platform != "linux":
            raise RuntimeError("Build the Debian installer on a native Linux host")
        print(build_deb(args.source, args.output, args.name, args.version, args.arch))
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("Debian installer build failed: " + str(error), file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
