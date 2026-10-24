"""Regress the exact v1.1.1 package failure, then verify its repaired layout."""
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import urllib.request

from sign_macos_bundle import assessment, seal_bundle, verify_bundle

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "test-results/macos-signature-audit"
DIGESTS = {"arm64": "f5c11c371e030bd8d6774f13571bdb6a8bd1ee74365b410d7bcc978751085987",
           "x86_64": "55695923d7c94e197ba7dd5da39370b73ea412d6dc0b8a9a29d151768a4fcf09"}


def main():
    arch = platform.machine()
    RESULTS.mkdir(parents=True, exist_ok=True)
    archive = RESULTS / ("KiCad_Forge-macosx-" + arch + "-1.1.1.zip")
    urllib.request.urlretrieve("https://github.com/xiaomaitangjiang/KiCad_Forge/releases/download/v1.1.1/" +
                               archive.name, archive)
    with archive.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == DIGESTS[arch]
    report = {"arch": arch, "source_version": "1.1.1", "source_sha256": DIGESTS[arch]}
    try:
        with tempfile.TemporaryDirectory(prefix="KiForge signature audit ") as temporary:
            root = Path(temporary).resolve()
            subprocess.run(["/usr/bin/ditto", "-x", "-k", str(archive), str(root)], check=True)
            app = root / "KiCad Forge.app"
            original = subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=4", str(app)],
                                      capture_output=True, text=True, timeout=60)
            report["original_integrity"] = {"exit_code": original.returncode, "details": original.stderr}
            print("Original v1.1.1:", original.returncode, original.stderr, flush=True)
            assert original.returncode != 0, "The regression input must reproduce v1.1.1's invalid bundle signature"
            report["original_gatekeeper"] = assessment(app)
            seal_bundle(app, arch)
            report["repaired_integrity"] = "valid"
            python = app / "Contents/Resources/python/bin/python3"
            result = subprocess.run([str(python), "-E", "-s", "-B", "-c",
                "import ssl,certifi,sys,_crypt,_dbm,_tkinter; "
                "ssl.create_default_context(cafile=certifi.where()); print(sys.prefix)"],
                env=dict(os.environ, PYTHONHOME="/invalid", PYTHONPATH="/invalid"),
                check=True, capture_output=True, text=True, timeout=30)
            assert str(app) in result.stdout
            report["relocated_python_prefix"] = result.stdout.strip()
            verify_bundle(app)
            report["repaired_gatekeeper"] = assessment(app)
            assert not report["repaired_gatekeeper"]["accepted"], "Ad-hoc signatures must not be described as Developer ID trust"
            print(json.dumps(report, indent=2), flush=True)
    except subprocess.CalledProcessError as error:
        report["failure"] = {"command": error.cmd, "stderr": error.stderr, "stdout": error.stdout}
        print(error.stderr, flush=True)
        raise
    finally:
        (RESULTS / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        archive.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
