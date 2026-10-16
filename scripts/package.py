"""Stage a portable application bundle and create its ZIP archive."""

import argparse
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--output", type=Path, default=Path("dist"))
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    bundle = output / args.name
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir(parents=True)
    shutil.copy2(binary, bundle / binary.name)
    for name in ("webui", "plugins", "config"):
        shutil.copytree(binary.parent / name, bundle / name,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    for dll in binary.parent.glob("*.dll"):
        shutil.copy2(dll, bundle / dll.name)
    (bundle / "data").mkdir()
    project = Path(__file__).resolve().parents[1]
    for name in ("README.md", "LICENSE"):
        shutil.copy2(project / name, bundle / name)
    archive = shutil.make_archive(str(output / args.name), "zip", output, args.name)
    print(archive)


if __name__ == "__main__":
    main()
