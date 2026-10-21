"""Verify locked inputs and prepare PCRE2 with its official build tools.

The Windows archive and native source archive are committed to dependencies/.
This script never downloads packages and never installs into the user environment.
The separate xmake configure step prepares the remaining checksum-locked packages.
"""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import tarfile


PROJECT = Path(__file__).resolve().parents[1]


def sha256(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def verify(path, expected):
    if not path.is_file():
        raise RuntimeError(f"Locked dependency missing: {path}")
    actual = sha256(path)
    if actual != expected.lower():
        raise RuntimeError(f"SHA256 mismatch: {path}\nexpected {expected}\nactual   {actual}")


def verify_inputs():
    lock = json.loads((PROJECT / "dependencies/pcre2.lock.json").read_text(encoding="utf-8"))
    verify(PROJECT / lock["source"]["archive"], lock["source"]["sha256"])
    verify(PROJECT / lock["package"]["vendored_archive"], lock["package"]["cache_sha256"])
    for item in lock["licenses"].values():
        verify(PROJECT / item["vendored"], item["sha256"])
    recipes = json.loads((PROJECT / "dependencies/xmake-recipes.lock.json").read_text(encoding="utf-8"))
    for item in recipes["recipes"] + recipes["support_files"]:
        verify(PROJECT / item["path"], item["sha256"])
    return lock


def prepare_pcre2(args, lock):
    work = PROJECT / "build/dependencies/pcre2" / args.plat / args.arch / args.mode
    prefix = work / "install"
    prefix.mkdir(parents=True, exist_ok=True)
    if args.plat == "mingw":
        if args.arch != "x86_64":
            raise RuntimeError("The locked MSYS2 PCRE2 SDK supports x86_64 only")
        archive = PROJECT / lock["package"]["vendored_archive"]
        # bsdtar/pacman's tar understands the official zstd package; extract only
        # the three locked SDK files, with the package's mingw64 prefix removed.
        subprocess.run(["tar", "-xf", str(archive), "-C", str(prefix),
                        "--strip-components", "1",
                        *("mingw64/" + item["path"] for item in lock["sdk"]["files"])], check=True)
        for item in lock["sdk"]["files"]:
            verify(prefix / item["path"], item["sha256"])
    else:
        expected_host = "Darwin" if args.plat == "macosx" else "Linux"
        if platform.system() != expected_host:
            raise RuntimeError(f"Prepare {args.plat} PCRE2 on a native {expected_host} host")
        host_arch = {"amd64": "x86_64", "aarch64": "arm64"}.get(
            platform.machine().lower(), platform.machine().lower())
        if args.plat == "linux" and host_arch != args.arch:
            raise RuntimeError("Linux PCRE2 preparation requires the native host architecture")
        source_root = work / "source"
        source_root.mkdir(parents=True, exist_ok=True)
        with tarfile.open(PROJECT / lock["source"]["archive"], "r:bz2") as archive:
            archive.extractall(source_root, filter="data")
        source = source_root / ("pcre2-" + lock["source"]["version"])
        cmake_build = work / "cmake"
        command = ["cmake", "-S", str(source), "-B", str(cmake_build),
                   "-DCMAKE_BUILD_TYPE=" + args.mode.capitalize(),
                   "-DCMAKE_INSTALL_PREFIX=" + str(prefix)]
        command.extend("-D" + key + "=" + value for key, value in lock["source"]["configs"].items())
        if args.cc:
            command.append("-DCMAKE_C_COMPILER=" + args.cc)
        if args.plat == "macosx":
            command.append("-DCMAKE_OSX_ARCHITECTURES=" + ("arm64" if args.arch == "arm64" else "x86_64"))
        subprocess.run(command, check=True)
        subprocess.run(["cmake", "--build", str(cmake_build), "--parallel", "2"], check=True)
        subprocess.run(["cmake", "--install", str(cmake_build)], check=True)
        files = [lock["sdk"]["version_header"], lock["link"]["library"]]
        for name in files:
            if not (prefix / name).is_file():
                raise RuntimeError("Official CMake build did not produce " + name)
        manifest = {"source_sha256": lock["source"]["sha256"],
                    "plat": args.plat, "arch": args.arch, "mode": args.mode,
                    "files": [{"path": name, "sha256": sha256(prefix / name)} for name in files]}
        (prefix / "kforge-sdk.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("PCRE2_SDK_DIR=" + prefix.as_posix())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plat", choices=("mingw", "linux", "macosx"))
    parser.add_argument("--arch", choices=("x86_64", "arm64"), default="x86_64")
    parser.add_argument("--mode", choices=("debug", "release"), default="debug")
    parser.add_argument("--cc", help="Native C compiler used by PCRE2's official CMake build")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    lock = verify_inputs()
    if args.verify_only:
        print("All dependency archives, official recipes and licenses match their locks")
    elif args.plat:
        prepare_pcre2(args, lock)
    else:
        parser.error("--plat is required unless --verify-only is used")


if __name__ == "__main__":
    main()
