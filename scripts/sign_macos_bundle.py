"""Seal a macOS app ad hoc; this establishes integrity, not Developer ID trust."""

import argparse
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys

if __package__:
    from .prepare_packaging import write_manifest
else:
    from prepare_packaging import write_manifest

MACHO_MAGICS = {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce",
                b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"}


def is_macho(path):
    if not path.is_file() or path.is_symlink():
        return False
    with path.open("rb") as stream:
        return stream.read(4) in MACHO_MAGICS


def native_files(app):
    return sorted(path for path in Path(app).rglob("*") if is_macho(path))


def check_app(app):
    app = Path(app).absolute()
    if app.is_symlink():
        raise ValueError("The application bundle itself must not be a symlink")
    # macOS exposes the system temporary directory through /var -> /private/var.
    # Canonicalize that parent before applying the bundle's internal link check.
    app = app.resolve(strict=True)
    # Defer the import: stage_distribution uses this module after its own
    # preflight, while the standalone command must check the same boundaries.
    if __package__:
        from .stage_distribution import check_internal_links
    else:
        from stage_distribution import check_internal_links
    check_internal_links(app)
    with (app / "Contents/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    if (app.suffix != ".app" or info.get("CFBundleIdentifier") != "com.kicadforge.desktop" or
            info.get("CFBundleExecutable") != "KiCad_Forge"):
        raise ValueError("Expected the KiCad Forge application bundle")
    return app


def run(command):
    return subprocess.run(command, check=True, capture_output=True, text=True, timeout=60)


def relocate_python(app):
    """Keep scripts in Resources, real native code in MacOS/Frameworks.

    Relative symlinks retain Python's installed layout. Three Tk extensions
    use an old loader-relative rpath; update it after moving them to Frameworks.
    """
    contents = app / "Contents"
    runtime = contents / "Resources/python"
    legacy = contents / "Helpers/python"
    if legacy.exists():
        if runtime.exists():
            raise ValueError("Both legacy and resource Python runtimes exist")
        os.replace(legacy, runtime)
        legacy.parent.rmdir()
    if not (runtime / "bin/python3").is_file():
        raise FileNotFoundError("Private Python interpreter is missing")
    prefix_lib = contents / "lib"
    if os.path.lexists(prefix_lib):
        raise ValueError("Unexpected pre-existing Python library prefix")
    libraries = contents / "Frameworks"
    libraries.mkdir(exist_ok=True)
    targets = []
    destinations = set()
    for source in native_files(runtime):
        relative = source.relative_to(runtime)
        parent = contents / "MacOS" if relative.parts[0] == "bin" else libraries
        destination = parent / source.name
        if destination in destinations or os.path.lexists(destination):
            raise ValueError("Native runtime filename collision: " + str(destination))
        destinations.add(destination)
        targets.append((source, destination))
    for source, destination in targets:
        os.replace(source, destination)
        source.symlink_to(os.path.relpath(destination, source.parent))
        output = run(["/usr/bin/otool", "-l", str(destination)]).stdout
        for old, new in (("@executable_path/../lib", "@executable_path/../Frameworks"),
                         ("@loader_path/../..", "@loader_path")):
            if "path " + old + " (offset" in output:
                run(["/usr/bin/install_name_tool", "-rpath", old, new, str(destination)])
    # CPython resolves its executable before finding the standard library.
    # Preserve that landmark beside MacOS without putting script files there.
    prefix_lib.symlink_to("Resources/python/lib", target_is_directory=True)
    return runtime


def verify_bundle(app):
    app = check_app(app)
    for binary in native_files(app):
        if binary != app / "Contents/MacOS/KiCad_Forge":
            run(["/usr/bin/codesign", "--verify", "--strict", "--verbose=2", str(binary)])
    run(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=2", str(app)])


def seal_bundle(app, arch):
    if sys.platform != "darwin":
        raise RuntimeError("macOS app signing requires a native Mac")
    app = check_app(app)
    runtime = relocate_python(app)
    main = app / "Contents/MacOS/KiCad_Forge"
    for binary in native_files(app):
        if binary != main:
            run(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(binary)])
    # Signing changes the binaries, so refresh their staged file digests before
    # the final resource envelope is sealed. Prepared input remains untouched.
    write_manifest(runtime, arch, platform="macosx")
    run(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(app)])
    verify_bundle(app)


def assessment(app):
    result = subprocess.run(["/usr/sbin/spctl", "--assess", "--type", "execute", "--verbose=4", str(app)],
                            capture_output=True, text=True, timeout=60)
    return {"accepted": result.returncode == 0, "exit_code": result.returncode,
            "details": result.stdout + result.stderr}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--arch", required=True, choices=("arm64", "x86_64"))
    args = parser.parse_args()
    try:
        seal_bundle(args.app, args.arch)
        print(json.dumps({"integrity": "valid", "signature": "adhoc", "notarized": False,
                          "gatekeeper": assessment(args.app)}, indent=2))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("macOS signing failed: " + str(error), file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
