"""Build a drag-to-Applications DMG with macOS's own disk and Finder tools."""

import argparse
import os
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

if __package__:
    from .stage_distribution import APP_NAME, check_internal_links, validate_output
else:
    from stage_distribution import APP_NAME, check_internal_links, validate_output

PROJECT = Path(__file__).resolve().parents[1]
LAYOUT_SCRIPT = PROJECT / "packaging/macos/dmg-layout.applescript"


def write_background(destination):
    """A simple copper arrow between the two Finder icons; no font dependencies."""
    width, height = 640, 400
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # PNG's unfiltered scanline marker.
        for x in range(width):
            color = (245, 245, 242)
            if y < 5:
                color = (184, 68, 53)
            # A right-pointing arrow centered between icons at (160, 190)
            # and (480, 190). Finder supplies both icons and their labels.
            if (280 <= x <= 333 and 186 <= y <= 194) or (
                    326 <= x <= 350 and abs(y - 190) <= (350 - x) * 0.8):
                color = (184, 68, 53)
            rows.extend(color)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    destination.write_bytes(b"\x89PNG\r\n\x1a\n" +
                            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def build_dmg(source, output, name, version):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name) or name.endswith(".dmg"):
        raise ValueError("DMG name must be one filename without the .dmg extension")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Version must have the form major.minor.patch")
    source = Path(source).absolute()
    check_internal_links(source)
    app = source / APP_NAME
    with (app / "Contents/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    if (info.get("CFBundleIdentifier") != "com.kicadforge.desktop" or
            info.get("CFBundlePackageType") != "APPL" or
            info.get("CFBundleExecutable") != "KiCad_Forge" or
            info.get("CFBundleShortVersionString") != version):
        raise ValueError("Application bundle identity or version does not match the DMG")
    if not (app / "Contents/MacOS/KiCad_Forge").is_file():
        raise FileNotFoundError("Application bundle executable is missing")
    output = validate_output(output, source.resolve())
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".dmg-", dir=output) as temporary:
        work = Path(temporary)
        contents = work / "contents"
        contents.mkdir()
        shutil.copytree(app, contents / APP_NAME, symlinks=True)
        # This external link belongs only to the disk image, never the .app or ZIP.
        (contents / "Applications").symlink_to("/Applications", target_is_directory=True)
        background = contents / ".background"
        background.mkdir()
        write_background(background / "background.png")
        writable = work / "writable.dmg"
        mount = work / "mounted"
        mount.mkdir()
        size = sum(p.stat().st_size for p in app.rglob("*") if p.is_file())
        run = dict(check=True, capture_output=True, text=True)
        subprocess.run(["/usr/bin/hdiutil", "create", "-volname", "KiCad Forge", "-fs", "HFS+",
                        "-size", str((size + 1048575) // 1048576 + 32) + "m",
                        "-srcfolder", str(contents), "-format", "UDRW", str(writable)], timeout=120, **run)
        subprocess.run(["/usr/bin/hdiutil", "attach", "-readwrite", "-noverify", "-noautoopen", "-nobrowse",
                        "-mountpoint", str(mount), str(writable)], timeout=40, **run)
        try:
            subprocess.run(["/usr/bin/osascript", str(LAYOUT_SCRIPT), str(mount), "style"], timeout=60, **run)
            deadline = time.monotonic() + 10
            while not (mount / ".DS_Store").is_file() and time.monotonic() < deadline:
                time.sleep(0.1)
            if not (mount / ".DS_Store").is_file():
                raise RuntimeError("Finder did not save the DMG layout")
        finally:
            subprocess.run(["/usr/bin/hdiutil", "detach", str(mount)], timeout=40, **run)
        compressed = work / "installer.dmg"
        subprocess.run(["/usr/bin/hdiutil", "convert", str(writable), "-format", "UDZO",
                        "-o", str(compressed)], timeout=120, **run)
        if not compressed.is_file() or compressed.stat().st_size == 0:
            raise RuntimeError("hdiutil did not produce a compressed DMG")
        artifact = output / (name + ".dmg")
        os.replace(compressed, artifact)
    return artifact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        if sys.platform != "darwin":
            raise RuntimeError("Build the DMG on a native Mac with Finder available")
        print(build_dmg(args.source, args.output, args.name, args.version))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("DMG build failed: " + str(error), file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
