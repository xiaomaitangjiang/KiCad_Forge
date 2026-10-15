"""Test a relocated application bundle: startup, assets, API, DB and shutdown."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def smoke_test(binary, log_dir):
    binary = binary.resolve(strict=True)
    log_dir.mkdir(parents=True, exist_ok=True)
    log_dir = log_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="KiCad Forge smoke ") as temp:
        bundle = Path(temp) / "relocated application"
        shutil.copytree(binary.parent, bundle)
        application = bundle / binary.name
        env = os.environ.copy()
        if os.name == "nt":
            # A missing bundled DLL must not be hidden by the build toolchain's PATH.
            system_root = env["SystemRoot"]
            env["PATH"] = os.pathsep.join((system_root, str(Path(system_root) / "System32"),
                                         str(Path(sys.executable).parent)))
        stdout_path = log_dir / "smoke-stdout.log"
        with stdout_path.open("w", encoding="utf-8") as stdout:
            process = subprocess.Popen([str(application), "--headless"], cwd=bundle,
                                       env=env, stdout=stdout, stderr=subprocess.STDOUT)
            try:
                opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                deadline = time.monotonic() + 30
                url = None
                while time.monotonic() < deadline:
                    require(process.poll() is None, f"Application exited early: {process.returncode}")
                    logs = stdout_path.read_text(encoding="utf-8", errors="replace")
                    for log in (bundle / "data").glob("*.log"):
                        logs += log.read_text(encoding="utf-8", errors="replace")
                    match = re.search(r"Server bound to (127\.0\.0\.1:\d+)", logs)
                    if match:
                        url = "http://" + match.group(1)
                        try:
                            with opener.open(url + "/api/status", timeout=1) as response:
                                if json.load(response).get("ok") is True:
                                    break
                        except (OSError, urllib.error.URLError):
                            pass
                    time.sleep(0.1)
                else:
                    raise RuntimeError("Application did not become ready within 30 seconds")

                def get_json(path):
                    with opener.open(url + path, timeout=3) as response:
                        return json.load(response)

                require(get_json("/api/status")["ok"] is True, "Health check failed")
                require(isinstance(get_json("/api/libraries"), list), "Library query failed")
                plugins = get_json("/api/plugins")
                require(isinstance(plugins, list), "Plugin query failed")
                expected_plugins = {
                    json.loads(p.read_text(encoding="utf-8"))["id"]
                    for p in (bundle / "plugins").glob("*/manifest.json")
                }
                require(expected_plugins <= {p["id"] for p in plugins}, "Bundled plugins were not discovered")
                for plugin_id in expected_plugins:
                    request = urllib.request.Request(
                        url + "/api/plugins/execute?id=" + plugin_id,
                        data=json.dumps({"action": "info"}).encode("utf-8"),
                        headers={"Content-Type": "application/json"}, method="POST")
                    with opener.open(request, timeout=5) as response:
                        result = json.load(response)
                    require(result.get("plugin_result", {}).get("id") == plugin_id,
                            "Native-to-Python plugin execution failed: " + str(result))
                with opener.open(url + "/", timeout=3) as response:
                    require("text/html" in response.headers.get("Content-Type", ""), "Frontend is not HTML")
                    html = response.read().decode("utf-8")
                assets = re.findall(r'(?:src|href)="(/assets/[^"?#]+)', html)
                require(assets, "No built frontend assets found")
                for asset in assets:
                    with opener.open(url + asset, timeout=3) as response:
                        require(bool(response.read()), "Empty frontend asset: " + asset)

                request = urllib.request.Request(url + "/api/bye", data=b"", method="POST")
                with opener.open(request, timeout=3) as response:
                    require(json.load(response).get("ok") is True, "Shutdown request failed")
                require(process.wait(timeout=15) == 0, f"Application exit code: {process.returncode}")
                database = bundle / "data/meta.db"
                require(database.is_file(), "Portable database was not created")
                with sqlite3.connect(database) as db:
                    require(db.execute("PRAGMA integrity_check").fetchone()[0] == "ok", "Database integrity check failed")
                    tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
                    require({"symbols", "footprints", "libraries"} <= tables, "Database migrations were not applied")
                print("Smoke test passed: relocated bundle, frontend assets, API, plugins, database, clean exit")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
                for log in (bundle / "data").glob("*.log"):
                    shutil.copy2(log, log_dir / log.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--log-dir", type=Path, default=Path("test-results"))
    args = parser.parse_args()
    try:
        smoke_test(args.binary, args.log_dir)
    except Exception:
        for log in args.log_dir.glob("*.log"):
            print(f"--- {log.name} ---", flush=True)
            print(log.read_text(encoding="utf-8", errors="replace")[-12000:], flush=True)
        raise


if __name__ == "__main__":
    main()
