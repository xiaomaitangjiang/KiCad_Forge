"""Diagnose the exact v1.1.1 arm64 release using native macOS signing tools."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "test-results/macos-signature-audit"
MAGIC = {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}


def main():
    RESULTS.mkdir(parents=True, exist_ok=True)
    archive = RESULTS / "KiCad_Forge-macosx-arm64-1.1.1.zip"
    urllib.request.urlretrieve(
        "https://github.com/xiaomaitangjiang/KiCad_Forge/releases/download/v1.1.1/"
        "KiCad_Forge-macosx-arm64-1.1.1.zip", archive)
    with archive.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == (
            "f5c11c371e030bd8d6774f13571bdb6a8bd1ee74365b410d7bcc978751085987")
    report = []

    def run(command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        entry = {"command": command, "returncode": result.returncode,
                 "stdout": result.stdout, "stderr": result.stderr}
        report.append(entry)
        print(json.dumps(entry, ensure_ascii=False))
        return result

    with tempfile.TemporaryDirectory(prefix="KiForge signature audit ") as temporary:
        root = Path(temporary)
        subprocess.run(["/usr/bin/ditto", "-x", "-k", str(archive), str(root)], check=True)
        app = root / "KiCad Forge.app"
        run(["/usr/bin/codesign", "--display", "--verbose=4", str(app)])
        run(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=4", str(app)])
        run(["/usr/sbin/spctl", "--assess", "--type", "execute", "--verbose=4", str(app)])
        native = []
        for file in sorted(app.rglob("*")):
            if file.is_file() and not file.is_symlink():
                with file.open("rb") as stream:
                    if stream.read(4) in MAGIC:
                        native.append(file)
        print("Native binaries:", len(native))
        for file in native:
            result = run(["/usr/bin/codesign", "--force", "--sign", "-", str(file)])
            if result.returncode:
                raise RuntimeError("Nested code signing failed")
        run(["/usr/bin/codesign", "--force", "--sign", "-", str(app)])
        run(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=4", str(app)])
        run(["/usr/sbin/spctl", "--assess", "--type", "execute", "--verbose=4", str(app)])
        (RESULTS / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    archive.unlink()


if __name__ == "__main__":
    main()
