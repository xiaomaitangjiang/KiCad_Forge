"""Mount a generated DMG and exercise an isolated installed app with private Python."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

if __package__:
    from .build_dmg import LAYOUT_SCRIPT
else:
    from build_dmg import LAYOUT_SCRIPT


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def inventory(app):
    return {p.relative_to(app).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in app.rglob("*") if p.is_file()}


def check_dmg_layout(mount):
    require((mount / "KiCad Forge.app/Contents/Info.plist").is_file(), "DMG application bundle is missing")
    applications = mount / "Applications"
    require(applications.is_symlink() and os.readlink(applications) == "/Applications",
            "DMG is missing its drag-to-Applications link")
    require((mount / ".background/background.png").is_file(), "DMG background is missing")
    require((mount / ".DS_Store").is_file(), "DMG Finder layout is missing")
    subprocess.run(["/usr/bin/osascript", str(LAYOUT_SCRIPT), str(mount), "verify"], check=True, timeout=60)


def check_app(app, home, log_dir, platform="macosx"):
    if platform == "macosx":
        binary = app / "Contents/MacOS/KiCad_Forge"
        resources = app / "Contents/Resources"
        python = app / "Contents/Helpers/python/bin/python3"
        data = home / "Library/Application Support/KiCad_Forge/data"
    else:
        binary = app / ("KiCad_Forge.exe" if platform == "mingw" else "KiCad_Forge")
        resources = app
        python = app / "runtime/python" / ("python.exe" if platform == "mingw" else "bin/python3")
        data = home / ("AppData/Roaming/KiCad_Forge/data" if platform == "mingw" else ".KiCad_Forge")
    empty_path = home / "empty-path"
    empty_path.mkdir()
    env = dict(os.environ, HOME=str(home), APPDATA=str(home / "AppData/Roaming"), PATH=str(empty_path),
               PYTHONHOME=str(home / "invalid-python-home"), PYTHONPATH=str(home / "outside-python-packages"),
               PYTHONUSERBASE=str(home / "outside-python-userbase"))
    require(shutil.which("python", path=env["PATH"]) is None and shutil.which("python3", path=env["PATH"]) is None,
            "Smoke environment still exposes external Python")
    result = subprocess.run([str(python), "-E", "-s", "-B", "-X", "utf8", "-c",
        "import sys,ssl,certifi; ssl.create_default_context(cafile=certifi.where()); print(sys.executable)"],
        check=True, capture_output=True, text=True, env=env, timeout=20)
    require(str(app) in result.stdout, "Python resolved outside the installed app")
    for plugin in (resources / "plugins").iterdir():
        if not (plugin / "manifest.json").is_file():
            continue
        result = subprocess.run([str(python), "-E", "-s", "-B", "-X", "utf8", str(plugin / "plugin.py"), "info", "{}"],
            check=True, capture_output=True, text=True, env=env, timeout=20)
        require(isinstance(json.loads(result.stdout), dict), "Bundled plugin did not return JSON")
    before = inventory(app)
    log_dir.mkdir(parents=True, exist_ok=True)
    log = log_dir / (platform + "-installed-app.log")
    with log.open("w", encoding="utf-8") as output:
        process = subprocess.Popen([str(binary), "--headless", "--no-auto-import"],
                                   cwd=home, env=env, stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 30
            origin = None
            while time.monotonic() < deadline:
                require(process.poll() is None, "Installed app exited early; see " + str(log))
                logs = log.read_text(encoding="utf-8", errors="replace")
                if (data / "forge.log").exists():
                    logs += (data / "forge.log").read_text(encoding="utf-8", errors="replace")
                match = re.search(r"Server bound to (127\.0\.0\.1:\d+)", logs)
                if match:
                    origin = "http://" + match.group(1)
                    break
                time.sleep(0.1)
            require(origin, "Installed app did not start its HTTP service")
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

            def request(path, body=None):
                request = urllib.request.Request(origin + path,
                    data=None if body is None else json.dumps(body).encode(),
                    headers={"Content-Type": "application/json"})
                with opener.open(request, timeout=5) as response:
                    return json.load(response)

            require(isinstance(request("/api/status"), dict), "Installed status API failed")
            with opener.open(origin + "/", timeout=5) as response:
                require(b"<html" in response.read().lower(), "Installed frontend is missing")
            plugins = request("/api/plugins")
            require({"com.kicad_forge.lcsc_import", "com.kicad_forge.git_version"}
                    <= {p["id"] for p in plugins}, "Bundled plugins were not discovered")
            # info is intentionally not a public declared action. Use LCSC's
            # declared analyze action with invalid input: entirely offline,
            # but it must reach Python's business validation through the host.
            for plugin in [p for p in plugins if p["id"] == "com.kicad_forge.lcsc_import"]:
                task = request("/api/tasks", {"plugin_id": plugin["id"], "action": "analyze", "args": {"lcsc_id": "invalid"}})
                task_id = task.get("task_id") or task.get("id")
                require(task_id, "Plugin analyze task was not created: " + str(task))
                task_deadline = time.monotonic() + 20
                while time.monotonic() < task_deadline:
                    state = request("/api/tasks/" + task_id)
                    if state.get("state") in ("succeeded", "failed", "cancelled"):
                        break
                    time.sleep(0.1)
                require(state.get("state") == "failed" and "invalid LCSC part number" in json.dumps(state),
                        "Host did not reach bundled Python validation: " + str(state))
            require(request("/api/bye", {}).get("ok") is True, "Installed app shutdown failed")
            require(process.wait(timeout=15) == 0, "Installed app exited with an error")
            require((data / "meta.db").is_file(), "Database was not created in the isolated user data directory")
            require(inventory(app) == before, "Running the app modified its signed/immutable bundle")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
    print(platform + " installed-app smoke passed: no external Python, user data, frontend, TLS certificates and both plugins")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dmg", required=True, type=Path)
    args = parser.parse_args()
    require(sys.platform == "darwin", "This test must run on a Mac")
    with tempfile.TemporaryDirectory(prefix="KiForge install smoke ") as temporary:
        root = Path(temporary)
        mount = root / "mounted"
        mount.mkdir()
        subprocess.run(["hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", str(mount),
                        str(args.dmg.resolve(strict=True))], check=True, timeout=40)
        try:
            check_dmg_layout(mount)
            app = root / "Applications/KiCad Forge.app"
            shutil.copytree(mount / "KiCad Forge.app", app, symlinks=True)
        finally:
            subprocess.run(["hdiutil", "detach", str(mount)], check=True, timeout=40)
        home = root / "user home"
        home.mkdir()
        check_app(app, home, Path("test-results"))


if __name__ == "__main__":
    main()
