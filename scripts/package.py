"""Stage a portable application bundle and create its ZIP archive."""

import argparse
from pathlib import Path
import shutil


def stage_bundle(binary, name, output):
    binary = binary.resolve(strict=True)
    output = output.resolve()
    # The name comes from workflow inputs/local CLI use. Validate before the
    # existing bundle directory is removed, including on Windows.
    if not name or name in (".", "..") or any(c in name for c in ('/', '\\', ':')):
        raise ValueError("--name must be one directory name")
    bundle = output / name
    if bundle.is_symlink() or bundle.resolve().parent != output:
        raise ValueError("Bundle destination must be a direct directory inside --output")
    if binary == bundle or bundle in binary.parents:
        raise ValueError("The input binary cannot be inside the bundle destination")
    required = ("webui", "plugins", "config", "licenses")
    for directory in required:
        if not (binary.parent / directory).is_dir():
            raise FileNotFoundError("Build output is incomplete: " + directory)
    for license_name in ("pcre2-LICENCE.md", "sljit-LICENSE.txt"):
        if not (binary.parent / "licenses" / license_name).is_file():
            raise FileNotFoundError("Required dependency license missing: " + license_name)
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir(parents=True)
    shutil.copy2(binary, bundle / binary.name)
    for directory in required:
        shutil.copytree(binary.parent / directory, bundle / directory,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".git", "fetched", "staging"))
    for dll in binary.parent.glob("*.dll"):
        shutil.copy2(dll, bundle / dll.name)
    (bundle / "data").mkdir()
    project = Path(__file__).resolve().parents[1]
    for filename in ("README.md", "LICENSE"):
        shutil.copy2(project / filename, bundle / filename)
    archive = shutil.make_archive(str(output / name), "zip", output, name)
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--output", type=Path, default=Path("dist"))
    args = parser.parse_args()
    archive = stage_bundle(args.binary, args.name, args.output)
    print(archive)


if __name__ == "__main__":
    main()
