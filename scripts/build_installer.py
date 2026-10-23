"""Validate a Windows build or portable bundle, then compile its Inno installer.

This builds an installer executable; it does not install or run the application.
"""

import argparse
from dataclasses import dataclass
from itertools import chain
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


PROJECT = Path(__file__).resolve().parents[1]
SCRIPT = PROJECT / "packaging/windows/kiforge.iss"
APP_EXE = "KiCad_Forge.exe"


@dataclass(frozen=True)
class InstallerPlan:
    command: list[str]
    artifact: Path


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--source", type=Path, default=PROJECT / "build/mingw/x86_64/release",
                        help="Completed Windows build/bundle directory (default: repository Release output)")
    result.add_argument("--license", type=Path, default=PROJECT / "LICENSE")
    result.add_argument("--icon", type=Path, default=PROJECT / "resources/icons/app.ico")
    result.add_argument("--output", type=Path, default=PROJECT / "dist/installers")
    result.add_argument("--name", default="Kicad_Forge_Setup", help="Installer filename without .exe")
    result.add_argument("--iscc", type=Path, help="ISCC.exe (default: PATH, then standard Inno 7/6 locations)")
    result.add_argument("--dry-run", action="store_true", help="Validate and print the command without writing files")
    return result


def required_file(path):
    resolved = path.expanduser().resolve(strict=True)
    if not resolved.is_file():
        raise ValueError("Required file is not a regular file: " + str(resolved))
    return resolved


def compiler_path(explicit):
    if explicit is not None:
        return required_file(explicit)
    found = shutil.which("ISCC.exe")
    if found:
        return required_file(Path(found))
    for version in ("7", "6"):
        for variable in ("ProgramFiles", "ProgramFiles(x86)"):
            base = os.environ.get(variable)
            if base:
                candidate = Path(base) / ("Inno Setup " + version) / "ISCC.exe"
                if candidate.is_file():
                    return candidate.resolve()
    raise FileNotFoundError("Inno Setup compiler not found; pass --iscc PATH/ISCC.exe")


def validate_source(source):
    source = source.expanduser().resolve(strict=True)
    if not source.is_dir():
        raise ValueError("--source must be a completed build/bundle directory")
    # No runtime data directory is copied. Verify actual shipped assets, rather
    # than accepting a repository root, source tree or arbitrary executable.
    for relative in (APP_EXE, "config/component_types.json",
                     "config/package_types.json", "config/component_type_matching.json",
                     "plugins/git_version/manifest.json", "plugins/git_version/plugin.py",
                     "plugins/lcsc_import/manifest.json", "plugins/lcsc_import/plugin.py",
                     "licenses/pcre2-LICENCE.md", "licenses/sljit-LICENSE.txt"):
        required_file(source / relative)
    if not any((source / relative).is_file() for relative in ("webui/dist/index.html", "webui/index.html")):
        raise FileNotFoundError("Windows build/bundle is missing its built frontend index.html: " + str(source))
    dlls = tuple(source.glob("*.dll"))
    if not dlls or not all(path.is_file() for path in dlls):
        raise FileNotFoundError("Windows build/bundle is missing runtime DLLs: " + str(source))
    # Inno recursively reads these trees. Reject redirected assets before any
    # compiler/output operation so it cannot silently ship files outside input.
    for path in (source / APP_EXE, *dlls,
                 *(source / name for name in ("webui", "config", "plugins", "licenses"))):
        entries = chain((path,), path.rglob("*")) if path.is_dir() else (path,)
        for entry in entries:
            if (entry.is_symlink() or getattr(entry, "is_junction", lambda: False)()
                    or not entry.resolve().is_relative_to(source)):
                raise ValueError("Installer source contains a redirected asset: " + str(entry))
    return source


def make_plan(args):
    source = validate_source(args.source)
    license_file = required_file(args.license)
    icon = required_file(args.icon)
    script = required_file(SCRIPT)
    output = args.output.expanduser().resolve()
    if output == source or source in output.parents:
        raise ValueError("--output must be outside the input build/bundle directory")
    if output.exists() and not output.is_dir():
        raise ValueError("--output is not a directory: " + str(output))
    name = args.name
    if (not name or name in (".", "..") or name.endswith((".", " "))
            or any(c in name for c in '<>:"/\\|?*') or any(ord(c) < 32 for c in name)):
        raise ValueError("--name must be one Windows filename without a path")
    if name.lower().endswith(".exe"):
        raise ValueError("--name must omit the .exe extension")
    if name.split(".")[0].upper() in ("CON", "PRN", "AUX", "NUL", *(
            prefix + str(number) for prefix in ("COM", "LPT") for number in range(1, 10))):
        raise ValueError("--name is a reserved Windows filename")
    definitions = {"AppSourceDir": source, "AppLicenseFile": license_file,
                   "AppIconFile": icon, "AppOutputDir": output, "AppOutputName": name}
    if any(any(c in str(value) for c in ('"', '\r', '\n')) for value in definitions.values()):
        raise ValueError("Installer paths cannot contain quotes or line breaks")
    command = [str(compiler_path(args.iscc)), "/Qp"]
    command.extend("/D" + key + "=" + str(value) for key, value in definitions.items())
    command.append(str(script))
    return InstallerPlan(command, output / (name + ".exe"))


def compile_installer(plan):
    # List arguments preserve spaces/Unicode without a shell or hand quoting.
    subprocess.run(plan.command, check=True, cwd=PROJECT)
    if not plan.artifact.is_file():
        raise RuntimeError("Compiler did not produce the expected installer: " + str(plan.artifact))
    return plan.artifact


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        plan = make_plan(args)
        if args.dry_run:
            print(json.dumps({"command": plan.command, "artifact": str(plan.artifact)}, ensure_ascii=False, indent=2))
        else:
            print(compile_installer(plan))
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("Installer build failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
