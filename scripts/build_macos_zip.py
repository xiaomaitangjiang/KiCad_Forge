"""Archive a sealed Mac app with ditto and validate the extracted signature."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

if __package__:
    from .sign_macos_bundle import verify_bundle
    from .stage_distribution import APP_NAME, validate_output
else:
    from sign_macos_bundle import verify_bundle
    from stage_distribution import APP_NAME, validate_output


def build_zip(source, output, name):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name) or name.endswith(".zip"):
        raise ValueError("Archive name must be one filename without the .zip extension")
    source = Path(source).resolve(strict=True)
    app = source / APP_NAME
    verify_bundle(app)
    output = validate_output(output, source)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".maczip-", dir=output) as temporary:
        work = Path(temporary)
        archive = work / "distribution.zip"
        options = dict(check=True, capture_output=True, text=True, timeout=120)
        # Unlike zip's default mode, ditto preserves the relative links that
        # are recorded by the app's resource signature.
        subprocess.run(["/usr/bin/ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(app), str(archive)],
                       **options)
        if not archive.is_file() or archive.stat().st_size == 0:
            raise RuntimeError("ditto did not produce a ZIP archive")
        extracted = work / "roundtrip"
        subprocess.run(["/usr/bin/ditto", "-x", "-k", str(archive), str(extracted)], **options)
        verify_bundle(extracted / APP_NAME)
        artifact = output / (name + ".zip")
        os.replace(archive, artifact)
    return artifact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--name", required=True)
    args = parser.parse_args()
    try:
        if sys.platform != "darwin":
            raise RuntimeError("Build a macOS ZIP on a native Mac")
        print(build_zip(args.source, args.output, args.name))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("macOS ZIP build failed: " + str(error), file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
