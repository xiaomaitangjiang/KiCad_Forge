"""Test a relocated application bundle: startup, assets, API, DB and shutdown."""

import argparse
from contextlib import closing
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


def smoke_failed_startup(binary, log_dir):
    """An unopened API service must not crash after an earlier service fails."""
    binary = binary.resolve(strict=True)
    log_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="KiCad Forge failed startup ") as temp:
        bundle = Path(temp).resolve(strict=True) / "relocated application"
        shutil.copytree(binary.parent, bundle)
        # SQLite cannot open a directory as its database. Only this disposable
        # bundle is changed; the user's database and library paths are untouched.
        (bundle / "data/meta.db").mkdir()
        output = log_dir / "startup-failure-stdout.log"
        with output.open("w", encoding="utf-8") as stdout:
            result = subprocess.run(
                [str(bundle / binary.name), "--headless", "--no-auto-import"],
                cwd=bundle, stdout=stdout, stderr=subprocess.STDOUT, timeout=20)
        logs = output.read_text(encoding="utf-8", errors="replace")
        require("Database open failed:" in logs and "Launch failed:" in logs,
                "The fixture did not reach the expected database startup failure: " + logs)
        require(result.returncode == 1,
                f"Failed startup must exit with 1, got {result.returncode}: " + logs)


def request_json(opener, origin, path, payload=None):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(origin + path, data=data,
                                     headers={"Content-Type": "application/json"},
                                     method="GET" if payload is None else "POST")
    with opener.open(request, timeout=5) as response:
        result = json.load(response)
    require(isinstance(result, dict), "Expected a JSON object from " + path)
    return result


def matching_config_payload(response):
    keys = ("schema_version", "revision", "rules", "exceptions")
    require(isinstance(response, dict) and all(key in response for key in keys),
            "Matching configuration is missing a required schema field")
    return {key: response[key] for key in keys}


def check_symbol_sublibraries_http(request, bundle, get_json):
    """Enumerate empty physical symbol files in disposable package roots only."""
    fixtures = bundle / "smoke library fixtures"
    symbols = fixtures / "symbols"
    files = (symbols / "Device.kicad_sym", symbols / "nested/Device.kicad_sym")
    empty_library = b'(kicad_symbol_lib (version 20231120) (generator smoke))\n'
    for path in files:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(empty_library)
    # A directory whose suffix looks like a symbol file must never be offered.
    (symbols / "not-a-file.kicad_sym").mkdir()

    def add_package(name, symbol_path):
        added = request("/api/component-libraries", {
            "action": "add", "name": name, "symbol_path": str(symbol_path),
            "footprint_path": str(fixtures / (name + " footprints")),
        })
        require(added.get("ok") is True, "Could not register smoke package: " + str(added))
        package_id = added.get("library", {}).get("id")
        require(isinstance(package_id, str) and bool(package_id), "Smoke package has no UUID")
        return package_id

    def catalog(package_id):
        result = request(f"/api/component-libraries/{package_id}/symbol-libraries")
        require(result.get("ok") is True and isinstance(result.get("libraries"), list)
                and type(result.get("can_create")) is bool
                and type(result.get("case_sensitive")) is bool,
                "Invalid symbol sublibrary response: " + str(result))
        return result

    directory_id = add_package("SmokeDirectory", symbols)
    directory = catalog(directory_id)
    require(directory["can_create"] is True, "Directory roots must permit new child libraries")
    require({row.get("path") for row in directory["libraries"]}
            == {"Device.kicad_sym", "nested/Device.kicad_sym"},
            "Empty or nested physical symbol libraries were hidden: " + str(directory))
    require(all(row.get("name") == "Device" and row.get("id") == ""
                for row in directory["libraries"]),
            "Unregistered child libraries must remain selectable without invented UUIDs")
    require(all(path.read_bytes() == empty_library for path in files),
            "Listing symbol libraries changed their contents")

    missing = fixtures / "not created symbols"
    missing_catalog = catalog(add_package("SmokeMissingDirectory", missing))
    require(missing_catalog["libraries"] == [] and missing_catalog["can_create"] is True
            and not missing.exists(), "Listing a missing root must not create it")

    direct = fixtures / "Direct.kicad_sym"
    direct.write_bytes(empty_library)
    direct_catalog = catalog(add_package("SmokeDirectFile", direct))
    require(direct_catalog["can_create"] is False
            and [row.get("path") for row in direct_catalog["libraries"]] == ["Direct.kicad_sym"],
            "A single-file root must expose only that symbol library: " + str(direct_catalog))

    invalid = request("/api/component-libraries", {
        "action": "add", "name": "SmokeInvalidFootprintRoot",
        "symbol_path": str(symbols), "footprint_path": str(direct),
    })
    require(invalid.get("ok") is False
            and invalid.get("error_code") == "invalid_library_path"
            and invalid.get("field") == "footprint_path"
            and ".kicad_sym" in invalid.get("error", ""),
            "A symbol file in the footprint path was accepted or not explained: " + str(invalid))
    before = next(row for row in get_json("/api/component-libraries") if row["id"] == directory_id)
    invalid_update = request("/api/component-libraries", {
        "action": "update", "id": directory_id, "name": "ShouldNotBeSaved",
        "symbol_path": str(symbols), "footprint_path": str(direct),
    })
    require(invalid_update.get("ok") is False and invalid_update.get("field") == "footprint_path",
            "Editing a library accepted a symbol file as its footprint directory")
    after = next(row for row in get_json("/api/component-libraries") if row["id"] == directory_id)
    require(after == before, "Rejected path edit changed the library registration")
    model_file = fixtures / "not-a-model-directory.STEP"
    model_file.write_text("ISO-10303-21;\nEND-ISO-10303-21;\n", encoding="utf-8")
    invalid_model = request("/api/component-libraries", {
        "action": "update", "id": directory_id, "name": "ShouldNotBeSaved",
        "symbol_path": str(symbols), "footprint_path": str(fixtures / "SmokeDirectory footprints"),
        "model_3d_path": str(model_file),
    })
    require(invalid_model.get("ok") is False
            and invalid_model.get("error_code") == "invalid_library_path"
            and invalid_model.get("field") == "model_3d_path",
            "A model file was accepted as a model directory: " + str(invalid_model))
    require(next(row for row in get_json("/api/component-libraries") if row["id"] == directory_id) == before,
            "Rejected model path edit changed the library registration")


def check_matching_http(request, bundle):
    """Exercise the actual matching routes; mutate only the relocated data copy."""
    base = "/api/component-type-matching"

    def healthy(response, revision=None):
        require(response.get("ok") is True and response.get("has_snapshot") is True
                and response.get("load_error") == "" and response.get("scope") == "global",
                "Matching configuration is unhealthy: " + str(response))
        if revision is not None:
            require(response.get("revision") == revision, "Unexpected matching configuration revision")

    def rejected(response, label):
        require(response.get("ok") is False and bool(response.get("error")), label + ": " + str(response))

    initial = request(base)
    healthy(initial)
    config = matching_config_payload(initial)
    revision = config["revision"]
    require(type(revision) is int and revision >= 0, "Invalid initial matching revision")
    sample = request(base + "/preview", {"sample": {"reference": "R?"}})
    require(sample.get("ok") is True and sample.get("mode") == "sample"
            and sample.get("source") == "saved" and sample.get("saved_healthy") is True
            and sample.get("revision") == revision and sample.get("status") == "matched"
            and sample.get("type_id") == "Resistor",
            "Saved R? sample did not match the resistor seed: " + str(sample))

    saved = request(base, {"config": config, "expected_revision": revision})
    require(saved.get("ok") is True and type(saved.get("revision")) is int
            and saved["revision"] == revision + 1, "Matching save failed: " + str(saved))
    current_revision = saved["revision"]
    current = request(base)
    healthy(current, current_revision)
    current_config = matching_config_payload(current)
    require(current_config == {**config, "revision": current_revision},
            "Saving changed matching rules or exceptions")
    rejected(request(base + "/apply", {"expected_revision": revision}), "Stale apply was accepted")
    applied = request(base + "/apply", {"expected_revision": current_revision})
    summary = applied.get("summary", {})
    require(applied.get("ok") is True and applied.get("source") == "saved"
            and summary.get("revision") == current_revision and summary.get("total") == 0
            and summary.get("type_updated") == 0 and applied.get("details") == [],
            "Empty database apply failed or imported local data: " + str(applied))
    rejected(request(base + "/preview", {"sample": {"reference": None}}),
             "Null sample reference was accepted")
    healthy(request(base + "/reload", {}), current_revision)

    # This path is rooted in the TemporaryDirectory bundle. The source project
    # seeds and the user's installed/portable configuration are never edited.
    config_path = bundle / "data/config/component_type_matching.json"
    require(config_path.is_file(), "Matching save did not create the portable configuration")
    valid_bytes = config_path.read_bytes()
    invalid_bytes = b'{"schema_version": 1, "rules": [ invalid smoke configuration\n'
    try:
        config_path.write_bytes(invalid_bytes)
        rejected(request(base + "/reload", {}), "Reload accepted a corrupt configuration")
        invalid = request(base)
        require(invalid.get("ok") is False and invalid.get("has_snapshot") is True
                and bool(invalid.get("load_error")) and invalid.get("scope") == "global"
                and matching_config_payload(invalid) == current_config,
                "Corrupt reload did not retain the last valid snapshot: " + str(invalid))
        rejected(request(base, {"config": current_config, "expected_revision": current_revision}),
                 "Save bypassed the configuration load error")
        require(config_path.read_bytes() == invalid_bytes, "Blocked save changed corrupt configuration bytes")
        rejected(request(base + "/apply", {"expected_revision": current_revision}),
                 "Apply bypassed the configuration load error")
        require(config_path.read_bytes() == invalid_bytes, "Blocked apply changed corrupt configuration bytes")
    finally:
        # Restore even when a regression assertion/HTTP operation above fails.
        config_path.write_bytes(valid_bytes)
        recovered = request(base + "/reload", {})
    healthy(recovered, current_revision)
    require(matching_config_payload(recovered) == current_config,
            "Reload did not recover the saved configuration")
    require(config_path.read_bytes() == valid_bytes, "Recovery changed the restored configuration bytes")


def smoke_test(binary, log_dir):
    binary = binary.resolve(strict=True)
    log_dir.mkdir(parents=True, exist_ok=True)
    log_dir = log_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="KiCad Forge smoke ") as temp:
        # macOS /var is a system alias for /private/var. Use the physical
        # fixture root so the app's intentional no-redirection checks exercise
        # the package paths rather than rejecting the test runner's temp alias.
        bundle = Path(temp).resolve(strict=True) / "relocated application"
        shutil.copytree(binary.parent, bundle)
        application = bundle / binary.name
        for name in ("pcre2-LICENCE.md", "sljit-LICENSE.txt"):
            require((bundle / "licenses" / name).is_file(), "Missing bundled dependency license: " + name)
        env = os.environ.copy()
        if os.name == "nt":
            # A missing bundled DLL must not be hidden by the build toolchain's PATH.
            system_root = env["SYSTEMROOT"]
            env["PATH"] = os.pathsep.join((system_root, str(Path(system_root) / "System32"),
                                         str(Path(sys.executable).parent)))
        stdout_path = log_dir / "smoke-stdout.log"
        with stdout_path.open("w", encoding="utf-8") as stdout:
            process = subprocess.Popen([str(application), "--headless", "--no-auto-import"], cwd=bundle,
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
                import_status = get_json("/api/import-status")
                require(import_status.get("importing") is False, "Disabled auto-import unexpectedly started")
                require(import_status.get("progress") == {"phase": "idle", "completed": 0, "total": 0},
                        "Import task snapshot is missing or invalid: " + str(import_status))
                require(not {"symbols", "footprints", "models", "busy", "counts_available", "counts_stale"}
                        .intersection(import_status), "Import progress still exposes database statistics")
                require(isinstance(get_json("/api/libraries"), list), "Library query failed")
                empty_name = "Smoke_EmptySymbols"
                empty_created = request_json(opener, url, "/api/libraries",
                                             {"action": "create", "name": empty_name})
                require(empty_created.get("ok") is True, "Could not register an empty symbol library")
                empty_libraries = [library for library in get_json("/api/libraries")
                                   if library.get("name") == empty_name]
                require(len(empty_libraries) == 1 and empty_libraries[0].get("symbol_count") == 0,
                        "An empty symbol library was hidden from /api/libraries")
                check_symbol_sublibraries_http(
                    lambda path, payload=None: request_json(opener, url, path, payload), bundle, get_json)
                check_matching_http(lambda path, payload=None: request_json(opener, url, path, payload), bundle)
                plugins = get_json("/api/plugins")
                require(isinstance(plugins, list), "Plugin query failed")
                manifests = [json.loads(p.read_text(encoding="utf-8"))
                             for p in (bundle / "plugins").glob("*/manifest.json")]
                expected_plugins = {manifest["id"] for manifest in manifests}
                require(expected_plugins, "No bundled plugin manifests found")
                require(expected_plugins <= {p["id"] for p in plugins}, "Bundled plugins were not discovered")
                for manifest in manifests:
                    # Frontend-only v2 plugins have no Python entry point.
                    if manifest.get("manifestVersion", 1) >= 2 and not manifest.get("python"):
                        continue
                    plugin_id = manifest["id"]
                    # Declared actions only: the task API must not open undeclared
                    # Python entry points.
                    strict_request = urllib.request.Request(
                        url + "/api/tasks", method="POST",
                        data=json.dumps({"plugin_id": plugin_id, "action": "ci_undeclared_action",
                                         "args": {}}).encode("utf-8"),
                        headers={"Content-Type": "application/json"})
                    try:
                        with opener.open(strict_request, timeout=5) as response:
                            raise RuntimeError("Undeclared task action was accepted: " + response.read().decode())
                    except urllib.error.HTTPError as error:
                        problem = json.load(error)
                        require(problem.get("error_code") == "action_unknown",
                                "Wrong undeclared-action error: " + str(problem))
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
                with closing(sqlite3.connect(database)) as db:
                    require(db.execute("PRAGMA integrity_check").fetchone()[0] == "ok", "Database integrity check failed")
                    tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
                    require({"symbols", "footprints", "libraries"} <= tables, "Database migrations were not applied")
                print("Smoke test passed: relocated bundle, matching HTTP routes and recovery, "
                      "frontend assets, plugins, database, clean exit")
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
        smoke_failed_startup(args.binary, args.log_dir)
        smoke_test(args.binary, args.log_dir)
    except Exception:
        for log in args.log_dir.glob("*.log"):
            print(f"--- {log.name} ---", flush=True)
            print(log.read_text(encoding="utf-8", errors="replace")[-12000:], flush=True)
        raise


if __name__ == "__main__":
    main()
