#!/usr/bin/env python3
"""
LCSC -> KiCad component fetcher using easyeda2kicad (MIT license).
https://github.com/uPesy/easyeda2kicad.py

Setup:
  pip install easyeda2kicad

Usage:
  python scripts/lcsc_fetch.py C83091 C8734 -o ./fetched/
  python scripts/lcsc_fetch.py --batch parts.txt -o ./libs/
"""

import argparse
import os
import subprocess
import sys


def fetch_one(lcsc_id: str, output_dir: str) -> bool:
    """Fetch a single LCSC part using easyeda2kicad."""
    out = os.path.join(output_dir, lcsc_id)
    os.makedirs(out, exist_ok=True)

    print(f"\n[{lcsc_id}] ", end="", flush=True)

    try:
        # easyeda2kicad --lcsc_id=C83091 --output ./out/C83091 --full
        result = subprocess.run(
            [sys.executable, "-m", "easyeda2kicad",
             "--lcsc_id", lcsc_id,
             "--output", out,
             "--full"],
            capture_output=True, text=True, timeout=120
        )
        if result.returncode != 0:
            # Fallback: try without --full
            result = subprocess.run(
                [sys.executable, "-m", "easyeda2kicad",
                 "--lcsc_id", lcsc_id,
                 "--output", out],
                capture_output=True, text=True, timeout=120
            )

        # Check what was generated
        files = os.listdir(out) if os.path.isdir(out) else []
        sym = any(f.endswith(".kicad_sym") for f in files)
        mod = any(f.endswith(".kicad_mod") for f in files)
        stp = any(f.endswith(".step") or f.endswith(".stp") for f in files)

        status = []
        if sym: status.append("sym")
        if mod: status.append("mod")
        if stp: status.append("3D")

        if status:
            print(f"OK ({', '.join(status)}) -> {out}")
            return True
        else:
            print(f"FAIL (no output files)\n  stderr: {result.stderr[:200]}")
            return False

    except FileNotFoundError:
        print("FAIL (easyeda2kicad not installed)")
        print("  Install: pip install easyeda2kicad")
        return False
    except subprocess.TimeoutExpired:
        print("FAIL (timeout)")
        return False
    except Exception as e:
        print(f"FAIL ({e})")
        return False


def main():
    parser = argparse.ArgumentParser(description="LCSC -> KiCad via easyeda2kicad")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("parts", nargs="*", help="LCSC part numbers (e.g. C83091)")
    group.add_argument("--batch", "-b", help="File with one LCSC ID per line")
    parser.add_argument("--output", "-o", default="./fetched", help="Output directory")
    args = parser.parse_args()

    parts = args.parts
    if args.batch:
        with open(args.batch) as f:
            parts = [line.strip() for line in f if line.strip()]

    if not parts:
        print("No parts specified.")
        return 1

    print(f"Fetching {len(parts)} part(s) to {args.output}/ ...")
    results = [fetch_one(p, args.output) for p in parts]

    ok = sum(results)
    print(f"\nDone: {ok}/{len(results)} fetched")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
