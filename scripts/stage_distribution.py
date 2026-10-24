"""Validate a completed build and stage the payload consumed by XPack."""

import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import struct
import sys
import tempfile

if __package__:
    from .sync_runtime_assets import CACHE_IGNORE, PROTECTED_ROOTS, check_directory_chain, check_tree, overlaps
    from .prepare_packaging import verify_prepared
    from .sign_macos_bundle import seal_bundle
else:
    from sync_runtime_assets import CACHE_IGNORE, PROTECTED_ROOTS, check_directory_chain, check_tree, overlaps
    from prepare_packaging import verify_prepared
    from sign_macos_bundle import seal_bundle

PROJECT = Path(__file__).resolve().parents[1]
ASSETS = ("webui", "config", "plugins", "licenses")
APP_NAME = "KiCad Forge.app"


def macos_binary_info(binary):
    with Path(binary).open("rb") as stream:
        header = stream.read(32)
        if len(header) != 32 or header[:4] != b"\xcf\xfa\xed\xfe":
            raise ValueError("macOS package requires a native 64-bit Mach-O executable")
        cpu, _, _, commands, size = struct.unpack_from("<IIIII", header, 4)
        architecture = {0x1000007: "x86_64", 0x100000C: "arm64"}.get(cpu)
        if not architecture or size > 1024 * 1024:
            raise ValueError("Unsupported macOS executable architecture/load commands")
        data = stream.read(size)
    offset = 0
    for _ in range(commands):
        if offset + 8 > len(data):
            raise ValueError("Truncated Mach-O load commands")
        command, length = struct.unpack_from("<II", data, offset)
        if length < 8 or offset + length > len(data):
            raise ValueError("Invalid Mach-O load command")
        if command in (0x24, 0x32):  # LC_VERSION_MIN_MACOSX / LC_BUILD_VERSION
            version_offset = 8 if command == 0x24 else 12
            if length < version_offset + 4:
                raise ValueError("Truncated macOS deployment target")
            version = struct.unpack_from("<I", data, offset + version_offset)[0]
            return architecture, f"{version >> 16}.{(version >> 8) & 255}.{version & 255}"
        offset += length
    raise ValueError("Mach-O executable has no macOS deployment target")


def validated_source(source, platform):
    source = Path(source).absolute()
    check_tree(source, source=True)
    executable = "KiCad_Forge.exe" if platform == "mingw" else "KiCad_Forge"
    for name in (executable, "config/component_types.json", "config/package_types.json",
                 "config/component_type_matching.json", "plugins/git_version/manifest.json",
                 "plugins/git_version/plugin.py", "plugins/lcsc_import/manifest.json",
                 "plugins/lcsc_import/plugin.py", "licenses/pcre2-LICENCE.md", "licenses/sljit-LICENSE.txt"):
        if not (source / name).is_file():
            raise FileNotFoundError("Build asset missing: " + str(source / name))
    if not any((source / p).is_file() for p in ("webui/dist/index.html", "webui/index.html")):
        raise FileNotFoundError("Built frontend index.html is missing")
    if platform == "mingw" and not any(p.is_file() for p in source.glob("*.dll")):
        raise FileNotFoundError("Windows runtime DLLs are missing")
    return source.resolve(), executable


def check_internal_links(root):
    """Prepared Python contains legitimate relative links; never follow escapes."""
    root = Path(root)
    check_directory_chain(root, required=True)
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = Path(directory) / name
            if getattr(path, "is_junction", lambda: False)():
                raise ValueError("Redirected directory: " + str(path))
            if not path.resolve().is_relative_to(root.resolve()):
                raise ValueError("Link escapes payload: " + str(path))
            if not path.is_file() and not path.is_dir():
                raise ValueError("Invalid payload entry: " + str(path))


def validate_output(output, source, runtime=None):
    output = Path(output).absolute()
    check_directory_chain(output)
    output = output.resolve()
    if overlaps(output, source) or (runtime and overlaps(output, runtime)):
        raise ValueError("Payload output overlaps an input directory")
    if output == PROJECT or output in PROJECT.parents or output == Path(output.anchor):
        raise ValueError("Payload output cannot replace a project or filesystem root")
    for name in PROTECTED_ROOTS:
        if overlaps(output, PROJECT / name):
            raise ValueError("Payload output overlaps project source: " + str(output))
    if output.exists():
        check_internal_links(output)
    return output


def write_mac_icon(destination):
    """Wrap the existing ICO's PNG frame in ICNS, without changing its pixels."""
    data = (PROJECT / "resources/icons/app.ico").read_bytes()
    if len(data) < 6 or data[:4] != b"\0\0\1\0":
        return False
    frames = []
    tags = {128: b"ic07", 256: b"ic08", 512: b"ic09", 1024: b"ic10"}
    for i in range(struct.unpack_from("<H", data, 4)[0]):
        entry = 6 + i * 16
        if entry + 16 > len(data):
            break
        size, offset = struct.unpack_from("<II", data, entry + 8)
        frame = data[offset:offset + size]
        width = data[entry] or 256
        if width in tags and frame.startswith(b"\x89PNG\r\n\x1a\n"):
            frames.append(tags[width] + struct.pack(">I", len(frame) + 8) + frame)
    if not frames:
        return False
    payload = b"".join(frames)
    destination.write_bytes(b"icns" + struct.pack(">I", len(payload) + 8) + payload)
    return True


def validate_runtime(runtime, platform, arch):
    if runtime is None:
        raise ValueError("Prepared Python is required; run scripts/prepare_packaging.py first")
    runtime = Path(runtime).absolute()
    check_internal_links(runtime)
    executable, site_packages = (("python.exe", "Lib/site-packages") if platform == "mingw"
                                else ("bin/python3", "lib/python3.12/site-packages"))
    for name in (executable, "kforge-runtime.json", site_packages + "/certifi/cacert.pem"):
        if not (runtime / name).is_file():
            raise FileNotFoundError("Prepared Python asset missing: " + str(runtime / name))
    verify_prepared(runtime, arch, platform=platform)
    return runtime


def stage_distribution(source, output, platform, arch, version, *, runtime=None):
    if platform not in ("mingw", "linux", "macosx"):
        raise ValueError("Unsupported distribution platform")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Version must have the form major.minor.patch")
    source, executable = validated_source(source, platform)
    runtime = validate_runtime(runtime, platform, arch)
    if platform == "macosx":
        native_arch, minimum_os = macos_binary_info(source / executable)
        python_arch, python_minimum = macos_binary_info(runtime / "bin/python3")
        if native_arch != arch or python_arch != arch:
            raise ValueError("Mach-O executable architecture does not match the package")
        minimum_os = max((minimum_os, python_minimum), key=lambda s: tuple(map(int, s.split("."))))
    output = validate_output(output, source, runtime)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".payload-", dir=output.parent) as temporary:
        stage = Path(temporary) / "payload"
        stage.mkdir()
        resources = binary_dir = stage
        if platform == "macosx":
            contents = stage / APP_NAME / "Contents"
            binary_dir = contents / "MacOS"
            resources = contents / "Resources"
            binary_dir.mkdir(parents=True)
            resources.mkdir(parents=True)
            shutil.copytree(runtime, resources / "python", symlinks=True, ignore=CACHE_IGNORE)
            info = {"CFBundleName": "KiCad Forge", "CFBundleDisplayName": "KiCad Forge",
                    "CFBundleIdentifier": "com.kicadforge.desktop", "CFBundlePackageType": "APPL",
                    "CFBundleExecutable": executable, "CFBundleShortVersionString": version,
                    "CFBundleVersion": version, "LSMinimumSystemVersion": minimum_os,
                    "NSHighResolutionCapable": True}
            if write_mac_icon(resources / "app.icns"):
                info["CFBundleIconFile"] = "app.icns"
            with (contents / "Info.plist").open("wb") as stream:
                plistlib.dump(info, stream)
        else:
            shutil.copytree(runtime, stage / "runtime/python", symlinks=True, ignore=CACHE_IGNORE)
            (stage / "kforge-distribution.json").write_text(json.dumps({
                "schema_version": 1, "platform": platform, "arch": arch, "version": version}, indent=2) + "\n",
                encoding="utf-8")
        shutil.copy2(source / executable, binary_dir / executable)
        (binary_dir / executable).chmod((binary_dir / executable).stat().st_mode | 0o111)
        for name in ASSETS:
            shutil.copytree(source / name, resources / name, ignore=CACHE_IGNORE, symlinks=True)
        if platform == "mingw":
            for dll in source.glob("*.dll"):
                shutil.copy2(dll, stage / dll.name)
        for name in ("LICENSE", "README.md"):
            shutil.copy2(PROJECT / name, resources / name)
        # Preflight and copy failures leave any previous payload untouched.
        if platform == "macosx":
            seal_bundle(stage / APP_NAME, arch)
        check_internal_links(stage)
        if output.exists():
            check_internal_links(output)
            shutil.rmtree(output)
        os.replace(stage, output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("mingw", "linux", "macosx"))
    parser.add_argument("--arch", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--runtime", type=Path)
    args = parser.parse_args()
    try:
        print(stage_distribution(args.source, args.output, args.platform, args.arch, args.version, runtime=args.runtime))
    except (OSError, ValueError) as error:
        print("Distribution staging failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
