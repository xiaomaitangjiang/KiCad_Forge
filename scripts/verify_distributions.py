"""Require all native installer/archive outputs before CI uploads a Release build."""

import argparse
import os
from pathlib import Path
import re
import sys

if __package__:
    from .sync_runtime_assets import check_directory_chain, checked_stat
else:
    from sync_runtime_assets import check_directory_chain, checked_stat


def verify_distributions(directory, platform, arch, version):
    extensions = {("linux", "x86_64"): ("deb", "tar.gz", "zip"),
                  ("mingw", "x86_64"): ("exe", "zip"),
                  ("macosx", "arm64"): ("dmg", "zip"),
                  ("macosx", "x86_64"): ("dmg", "zip")}.get((platform, arch))
    if not extensions or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Unsupported distribution platform/architecture/version")
    directory = Path(directory).absolute()
    check_directory_chain(directory, required=True)
    paths = [directory / f"KiCad_Forge-{platform}-{arch}-{version}.{extension}" for extension in extensions]
    for path in paths:
        info = checked_stat(path)
        if not path.is_file() or info.st_size == 0:
            raise ValueError("Distribution is empty or is not a file: " + str(path))
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--version", default=os.environ.get("KFORGE_PACKAGE_VERSION", "1.1.1"))
    args = parser.parse_args()
    try:
        for path in verify_distributions(args.directory, args.platform, args.arch, args.version):
            print(path.name + ": " + str(path.stat().st_size) + " bytes")
    except (OSError, ValueError) as error:
        print("Distribution verification failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
