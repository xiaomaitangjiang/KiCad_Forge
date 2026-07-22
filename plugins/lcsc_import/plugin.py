#!/usr/bin/env python3
"""
LCSC Import Plugin for KiCad Forge (.kfplug)
Self-contained — bundles easyeda2kicad (MIT) inside the plugin directory.
No external dependencies needed beyond Python 3.10+.

Usage: python plugin.py import '{"source":"C83091"}'
"""

import json
import os
import subprocess
import sys

PLUGIN_DIR = os.path.dirname(os.path.abspath(__file__))

# Use bundled easyeda2kicad — no pip install needed
sys.path.insert(0, PLUGIN_DIR)
sys.path.insert(0, os.path.join(PLUGIN_DIR, "easyeda2kicad"))


def fetch_from_lcsc(lcsc_id: str, output_dir: str) -> dict:
    """Download component from LCSC using bundled easyeda2kicad."""
    os.makedirs(output_dir, exist_ok=True)

    # Run bundled easyeda2kicad via "python -m easyeda2kicad"
    # PYTHONPATH ensures the bundled copy is used, not any system install
    env = os.environ.copy()
    env["PYTHONPATH"] = PLUGIN_DIR
    cmd = [sys.executable, "-m", "easyeda2kicad",
           "--lcsc_id", lcsc_id, "--output", output_dir,
           "--symbol", "--footprint"]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120, env=env)

        files = os.listdir(output_dir) if os.path.isdir(output_dir) else []
        sym_files = [os.path.join(output_dir, f) for f in files if f.endswith(".kicad_sym")]
        mod_files = [os.path.join(output_dir, f) for f in files if f.endswith(".kicad_mod")]
        stp_files = [os.path.join(output_dir, f) for f in files if f.endswith((".step", ".stp"))]

        return {
            "ok": bool(sym_files or mod_files),
            "lcsc_id": lcsc_id,
            "output_dir": output_dir,
            "symbols": sym_files,
            "footprints": mod_files,
            "models_3d": stp_files,
            "stdout": result.stdout[-500:],
            "stderr": result.stderr[-500:] if result.returncode != 0 else "",
        }
    except Exception as e:
        return {"ok": False, "lcsc_id": lcsc_id, "error": str(e)}


def import_component(args: dict) -> dict:
    """Main entry point for import action."""
    lcsc_id = args.get("source", "") or args.get("lcsc_id", "")
    if not lcsc_id:
        return {"ok": False, "error": "Missing lcsc_id in args.source or args.lcsc_id"}

    output_base = args.get("output_dir", os.path.join(PLUGIN_DIR, "fetched"))
    output_dir = os.path.join(output_base, lcsc_id)

    result = fetch_from_lcsc(lcsc_id, output_dir)
    return result


# ============================================================
# CLI interface: python plugin.py <action> '<json>'
# ============================================================
if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(json.dumps({"ok": False, "error": "Usage: plugin.py <action> [json_args]"}))
        sys.exit(1)

    action = sys.argv[1]
    args = {}
    if len(sys.argv) > 2:
        try:
            args = json.loads(sys.argv[2])
        except json.JSONDecodeError as e:
            print(json.dumps({"ok": False, "error": f"Invalid JSON: {e}"}))
            sys.exit(1)

    if action == "info":
        # Return plugin metadata
        manifest_path = os.path.join(PLUGIN_DIR, "manifest.json")
        with open(manifest_path) as f:
            manifest = json.load(f)
        manifest["ok"] = True
        print(json.dumps(manifest))
    elif action == "import":
        result = import_component(args)
        print(json.dumps(result))
    else:
        print(json.dumps({"ok": False, "error": f"Unknown action: {action}"}))
        sys.exit(1)
