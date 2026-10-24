"""Build the Windows NSIS installer used by XPack; never install or run it."""

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import subprocess
import sys

if __package__:
    from .stage_distribution import validated_source, validate_output, validate_runtime
    from .prepare_packaging import PREPARED, verify_prepared
    from .sync_runtime_assets import check_directory_chain
else:
    from stage_distribution import validated_source, validate_output, validate_runtime
    from prepare_packaging import PREPARED, verify_prepared
    from sync_runtime_assets import check_directory_chain

PROJECT = Path(__file__).resolve().parents[1]
SCRIPT = PROJECT / "packaging/windows/kiforge.nsi"


@dataclass(frozen=True)
class InstallerPlan:
    command: list[str]
    artifact: Path


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--source", type=Path, default=PROJECT / "build/mingw/x86_64/release")
    result.add_argument("--output", type=Path, default=PROJECT / "dist/installers")
    result.add_argument("--name", default="KiCad_Forge_Setup")
    result.add_argument("--version", default="1.1.0")
    result.add_argument("--nsis", type=Path, help="Optional explicit makensis.exe; otherwise use the locked prepared compiler")
    result.add_argument("--dry-run", action="store_true")
    return result


def make_plan(args):
    source, _ = validated_source(args.source, "mingw")
    validate_runtime(source / "runtime/python", "mingw", "x86_64")
    if not (source / "kforge-distribution.json").is_file():
        raise FileNotFoundError("Distribution marker missing; stage the XPack payload first")
    output = validate_output(args.output, source)
    name = args.name
    if (not name or name in (".", "..") or name.endswith((".", " "))
            or any(c in name for c in '<>:"/\\|?*$') or any(ord(c) < 32 for c in name)
            or name.lower().endswith(".exe")
            or name.split(".")[0].upper() in ("CON", "PRN", "AUX", "NUL", *(
                prefix + str(n) for prefix in ("COM", "LPT") for n in range(1, 10)))):
        raise ValueError("--name must be one Windows filename without the .exe extension")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version) or any(int(n) > 65535 for n in args.version.split(".")):
        raise ValueError("--version must be a Windows-compatible major.minor.patch")
    if args.nsis is None:
        compiler_root = PREPARED / "mingw/x86_64/nsis"
        verify_prepared(compiler_root, "x86_64")
        compiler = compiler_root / "makensis.exe"
    else:
        compiler = args.nsis.expanduser().resolve(strict=True)
    if not compiler.is_file():
        raise FileNotFoundError("NSIS compiler missing; run scripts/prepare_packaging.py first")
    artifact = output / (name + ".exe")
    definitions = {"KFORGE_SOURCE": source, "KFORGE_OUTPUT": artifact,
                   "KFORGE_VERSION": args.version, "KFORGE_LICENSE": PROJECT / "LICENSE",
                   "KFORGE_ICON": PROJECT / "resources/icons/app.ico"}
    for value in definitions.values():
        if any(c in str(value) for c in ('"', '$', '\r', '\n')):
            raise ValueError("NSIS input paths cannot contain quotes, dollar signs or line breaks")
    return InstallerPlan([str(compiler), "/V2", *(
        "/D" + key + "=" + str(value) for key, value in definitions.items()), str(SCRIPT)], artifact)


def compile_installer(plan):
    check_directory_chain(plan.artifact.parent)
    plan.artifact.parent.mkdir(parents=True, exist_ok=True)
    if plan.artifact.is_symlink():
        raise ValueError("Installer output is a redirected file")
    plan.artifact.unlink(missing_ok=True)
    subprocess.run(plan.command, check=True, cwd=PROJECT)
    if not plan.artifact.is_file():
        raise RuntimeError("NSIS did not produce the expected installer: " + str(plan.artifact))
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
