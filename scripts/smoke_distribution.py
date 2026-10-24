"""Extract the Windows/Linux XPack archive and test private Python with no PATH Python."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

if __package__:
    from .prepare_packaging import archive_path, extract_zip
    from .smoke_macos import check_app, require
else:
    from prepare_packaging import archive_path, extract_zip
    from smoke_macos import check_app, require


def extract_deb(archive, extracted):
    archive = str(archive.resolve(strict=True))
    metadata = subprocess.run(["dpkg-deb", "--field", archive],
                              check=True, capture_output=True, text=True).stdout
    fields = dict(line.split(": ", 1) for line in metadata.splitlines() if ": " in line and not line.startswith(" "))
    require(fields.get("Package") == "kicad-forge" and fields.get("Architecture") == "amd64",
            "Unexpected Debian package identity or architecture")
    subprocess.run(["dpkg-deb", "--extract", archive, str(extracted)], check=True, timeout=60)
    require((extracted / "usr/share/applications/kicad-forge.desktop").is_file(), "Debian desktop entry is missing")
    require((extracted / "usr/bin/kicad-forge").is_file(), "Debian command launcher is missing")
    return extracted / "opt/kicad-forge"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("mingw", "linux"))
    args = parser.parse_args()
    require((sys.platform == "win32") == (args.platform == "mingw"), "Run on the package's native operating system")
    with tempfile.TemporaryDirectory(prefix="KiForge private Python smoke ") as temporary:
        root = Path(temporary)
        extracted = root / "extracted"
        extracted.mkdir()
        if args.platform == "linux" and args.archive.suffix == ".deb":
            source = extract_deb(args.archive, extracted)
        elif args.platform == "mingw":
            extract_zip(args.archive, extracted)
        else:
            with tarfile.open(args.archive, "r:gz") as archive:
                for member in archive.getmembers():
                    archive_path(member.name)
                    require(member.isfile() or member.isdir(), "Distribution contains unsupported links or special files")
                archive.extractall(extracted, filter="data")
        if args.archive.suffix != ".deb":
            folders = list(extracted.iterdir())
            require(len(folders) == 1 and folders[0].is_dir(), "Archive must have one application directory")
            source = folders[0]
        require(source.is_dir(), "Installed application payload is missing")
        app = root / "installed app with spaces"
        require(source.resolve().is_relative_to(root.resolve()) and app.resolve().is_relative_to(root.resolve()),
                "Smoke application move escapes its temporary directory")
        shutil.move(str(source), app)
        home = root / "isolated user home"
        home.mkdir()
        check_app(app, home, Path("test-results/private-python"), args.platform)


if __name__ == "__main__":
    main()
