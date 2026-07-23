#!/usr/bin/env python3
"""
LCSC Import Plugin — fetches from easyeda2kicad (bundled, MIT), merges symbol blocks.
Usage: python plugin.py import '{"source":"C37593","options":{"target_library":"/path/to/lib.kicad_sym"}}'
"""

import json, os, re, subprocess, sys

PLUGIN_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PLUGIN_DIR)

POWER_NAMES = {'VCC','VDD','V+','VCCA','VCCD','VBAT','VIN','AVDD','DVDD',
               'VSS','GND','V-','VEE','AGND','DGND','VS+','VP','VPP','VREF'}


def extract_symbol_blocks(text: str) -> list:
    """Extract top-level (symbol ...) blocks by counting parentheses."""
    blocks, pos = [], 0
    while (pos := text.find("(symbol ", pos)) != -1:
        start, depth, in_str = pos, 0, False
        while pos < len(text):
            c = text[pos]
            if c == '"' and (pos == 0 or text[pos-1] != '\\'): in_str = not in_str
            elif not in_str:
                if c == '(': depth += 1
                elif c == ')':
                    depth -= 1
                    if depth == 0: pos += 1; break
            pos += 1
        block = text[start:pos]
        m = re.search(r'\(symbol\s+"([^"]+)"', block)
        blocks.append((m.group(1) if m else "Unknown", block))
    return blocks


def fix_pin_types(block: str) -> str:
    """Replace 'unspecified line' with correct type. Finds each pin by paren depth."""
    result = []
    i, n = 0, len(block)
    while i < n:
        if block[i:i+4] == '(pin':
            # Find end of this pin by counting parentheses
            d, j, in_s = 0, i, False
            while j < n:
                c = block[j]
                if c == '"' and (j == 0 or block[j-1] != '\\'): in_s = not in_s
                elif not in_s:
                    if c == '(': d += 1
                    elif c == ')': d -= 1;
                j += 1
                if d == 0 and not in_s: break
            pin = block[i:j]
            # Extract pin name
            nm = re.search(r'\(name\s+"?([^"\s)]+)"?\s*\(effects', pin)
            pname = nm.group(1) if nm else ""
            etype = 'power_in' if pname in POWER_NAMES else 'passive'
            pin = pin.replace('unspecified line', f'{etype} line', 1)
            result.append(pin)
            i = j
        else:
            result.append(block[i])
            i += 1
    return ''.join(result)


def merge_into_file(blocks: list, target: str):
    if os.path.exists(target):
        text = open(target, encoding='utf-8').read()
    else:
        text = '(kicad_symbol_lib\n\t(version 20231120)\n\t(generator "KiCad_Forge")\n)\n'

    # Find depth-0 closer
    d, pos = 0, len(text)
    for i in range(len(text)-1, -1, -1):
        if text[i] == ')':
            d += 1
            if d == 1: pos = i; break
        elif text[i] == '(': d -= 1
    if pos < 0: raise ValueError("no closer")

    text = text[:pos] + "\n" + "\n".join(blocks) + "\n" + text[pos:]
    open(target, 'w', encoding='utf-8').write(text)


def fetch_and_merge(lcsc_id: str, target_file: str, output_dir: str) -> dict:
    os.makedirs(output_dir, exist_ok=True)

    # Run easyeda2kicad
    env = os.environ.copy()
    env["PYTHONPATH"] = PLUGIN_DIR
    r = subprocess.run(
        [sys.executable, "-m", "easyeda2kicad",
         "--lcsc_id", lcsc_id, "--output", output_dir, "--symbol", "--footprint"],
        capture_output=True, text=True, timeout=120, env=env)

    syms = [f for f in os.listdir(output_dir) if f.endswith(".kicad_sym")]
    if not syms:
        return {"ok": False, "error": f"No .kicad_sym. {r.stderr[:200]}"}

    blocks = extract_symbol_blocks(open(os.path.join(output_dir, syms[0]), encoding='utf-8').read())
    if not blocks:
        return {"ok": False, "error": "No (symbol ...) blocks"}

    # Fix pin types
    fixed = [fix_pin_types(b[1]) for b in blocks]
    # Fix footprint library prefix: easyeda2kicad:NAME → target_lib:NAME
    target_lib = os.path.splitext(os.path.basename(target_file))[0]
    for i in range(len(fixed)):
        fixed[i] = fixed[i].replace('easyeda2kicad:', target_lib + ':')
    merge_into_file(fixed, target_file)

    # Copy footprints to the corresponding .pretty folder
    fp_copied = 0
    # e.g. Symbols/EXT_LDO.kicad_sym → Footprints/EXT_LDO.pretty/
    sym_dir = os.path.dirname(target_file)
    fp_base = os.path.join(os.path.dirname(sym_dir), "Footprints") if "Symbols" in sym_dir else sym_dir
    target_lib = os.path.splitext(os.path.basename(target_file))[0]
    target_pretty = os.path.join(fp_base, target_lib + ".pretty")
    os.makedirs(target_pretty, exist_ok=True)

    # Copy from .pretty subdirectories
    for d in os.listdir(output_dir):
        src_pretty = os.path.join(output_dir, d)
        if not d.endswith(".pretty") or not os.path.isdir(src_pretty):
            continue
        for f in os.listdir(src_pretty):
            if f.endswith(".kicad_mod"):
                src = os.path.join(src_pretty, f)
                dst = os.path.join(target_pretty, f)
                with open(src, 'rb') as fr: data = fr.read()
                with open(dst, 'wb') as fw: fw.write(data)
                fp_copied += 1

    # Also copy standalone .kicad_mod files directly in output dir
    for f in os.listdir(output_dir):
        if f.endswith(".kicad_mod") and os.path.isfile(os.path.join(output_dir, f)):
            src = os.path.join(output_dir, f)
            dst = os.path.join(target_pretty, f)
            with open(src, 'rb') as fr: data = fr.read()
            with open(dst, 'wb') as fw: fw.write(data)
            fp_copied += 1

    return {"ok": True, "lcsc_id": lcsc_id, "merged_into": target_file,
            "symbol_names": [b[0] for b in blocks], "block_count": len(blocks),
            "footprints_copied": fp_copied, "output_dir": output_dir}


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(json.dumps({"ok": False, "error": "Usage: plugin.py <action> [args]"})); sys.exit(1)
    action, args = sys.argv[1], json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    if action == "import":
        id = args.get("source") or args.get("lcsc_id", "")
        tgt = args.get("options", {}).get("target_library", "")
        if not tgt: print(json.dumps({"ok": False, "error": "Missing target_library"})); sys.exit(1)
        print(json.dumps(fetch_and_merge(id, tgt, os.path.join(PLUGIN_DIR, "fetched", id))))
    elif action == "info":
        print(open(os.path.join(PLUGIN_DIR, "manifest.json")).read())
    else:
        print(json.dumps({"ok": False, "error": f"Unknown: {action}"}))
