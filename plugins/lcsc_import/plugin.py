#!/usr/bin/env python3
"""
LCSC Import Plugin — fetches from easyeda2kicad (bundled, MIT), merges symbol blocks.

Two-step flow (host UI template "two_step"):
  analyze → download/convert (no library write) + match existing footprints via host API
  import  → merge symbol + either reuse a chosen existing footprint or copy a new one

Usage:
  python plugin.py analyze '{"lcsc_id":"C37593"}'
  python plugin.py import  '{"lcsc_id":"C37593","target_library":"/path/lib.kicad_sym",
                             "footprint_choice":{"mode":"reuse","library":"Package_SO","name":"MSOP-10..."}}'
"""

import json, os, re, shutil, subprocess, sys, urllib.parse, urllib.request

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
        with open(target, encoding='utf-8') as source:
            text = source.read()
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
    with open(target, 'w', encoding='utf-8') as output:
        output.write(text)


# ============================================================
# Prepare — download + convert into the fetched cache (no library write)
# ============================================================

def prepare(lcsc_id: str, output_dir: str) -> dict:
    """Run easyeda2kicad into output_dir. Returns symbol blocks + parsed metadata.

    转换退出码必须为 0：失败时目录里可能还留着上一次的产物
    """
    os.makedirs(output_dir, exist_ok=True)

    env = os.environ.copy()
    env["PYTHONPATH"] = PLUGIN_DIR
    r = subprocess.run(
        [sys.executable, "-m", "easyeda2kicad",
         "--lcsc_id", lcsc_id, "--output", output_dir, "--symbol", "--footprint"],
        capture_output=True, text=True, timeout=120, env=env)

    if r.returncode != 0:
        detail = (r.stderr or r.stdout or "").strip()[-300:]
        return {"ok": False,
                "error": f"easyeda2kicad failed (exit {r.returncode}): {detail}"}

    syms = [f for f in os.listdir(output_dir) if f.endswith(".kicad_sym")]
    if not syms:
        return {"ok": False, "error": f"No .kicad_sym. {(r.stderr or '')[:200]}"}

    with open(os.path.join(output_dir, syms[0]), encoding='utf-8') as source:
        blocks = extract_symbol_blocks(source.read())
    if not blocks:
        return {"ok": False, "error": "No (symbol ...) blocks"}

    # "Symbol file footprint model" — (property "Footprint" "easyeda2kicad:NAME")
    footprint_name = ""
    m = re.search(r'\(property\s*\n?\s*"Footprint"\s*\n?\s*"([^"]+)"', blocks[0][1])
    if m:
        footprint_name = m.group(1).split(":", 1)[-1]

    # 3D model stem from the generated .kicad_mod (if any)
    model_name = ""
    for root, _dirs, files in os.walk(output_dir):
        for f in files:
            if f.endswith(".kicad_mod"):
                mm = re.search(r'\(model\s+"([^"]+)"',
                               open(os.path.join(root, f), encoding='utf-8', errors='ignore').read())
                if mm:
                    model_name = os.path.splitext(os.path.basename(mm.group(1)))[0]
                break
        if model_name:
            break

    return {"ok": True, "output_dir": output_dir,
            "symbol_names": [b[0] for b in blocks],
            "footprint_name": footprint_name, "model_name": model_name,
            "blocks": blocks}


# ============================================================
# Host API — match existing footprints (KFORGE_API_URL injected by host)
# ============================================================

def search_host_footprints(near: str, limit: int = 50) -> list:
    """Query the host's footprint search API. Empty list when unavailable."""
    base = os.environ.get("KFORGE_API_URL", "")
    if not base or not near:
        return []
    url = f"{base}/api/footprints/search?near={urllib.parse.quote(near)}&limit={limit}"
    try:
        with urllib.request.urlopen(url, timeout=5) as resp:
            return json.loads(resp.read().decode('utf-8'))
    except Exception:
        return []


# ============================================================
# Import — merge symbol + reuse-or-create footprint
# ============================================================

# ============================================================
# Staged import (protocol staged-import-v1)
# 只往宿主给的暂存目录里产出文件；目标库、封装落点、数据库全由宿主决定
# ============================================================

def staged_import(lcsc_id: str, staging_dir: str, footprint_choice: dict = None) -> dict:
    os.makedirs(staging_dir, exist_ok=True)
    prep = prepare(lcsc_id, staging_dir)
    if not prep.get("ok"):
        return prep

    blocks = [(name, fix_pin_types(text)) for name, text in prep["blocks"]]
    symbols = []
    for name, text in blocks:
        file_name = name + ".kicad_sym"
        with open(os.path.join(staging_dir, file_name), 'w', encoding='utf-8') as f:
            f.write("(kicad_symbol_lib\n  (version 20231120)\n  (generator KiCad_Forge)\n"
                    + text + "\n)\n")
        symbols.append({"name": name, "file": file_name})

    choice = footprint_choice or {}
    mode = choice.get("mode", "new")
    footprint_entry = None
    if mode != "reuse":
        # 找 easyeda2kicad 生成的 .kicad_mod，原样放进暂存目录
        source = None
        for root, _dirs, files in os.walk(staging_dir):
            for f in files:
                if f.endswith(".kicad_mod"):
                    source = os.path.join(root, f)
                    break
            if source:
                break
        if not source:
            return {"ok": False, "error": "No .kicad_mod produced"}
        fp_name = os.path.splitext(os.path.basename(source))[0] or prep["footprint_name"]
        file_name = fp_name + ".kicad_mod"
        # 转换器输出的是 KiCad 5 的 (module 库名:封装名 ...)：把根节点规范成
        # (footprint "封装名" ...)，其余内容原样保留
        with open(source, encoding='utf-8', errors='ignore') as f:
            text = f.read()
        text = re.sub(r'^\(module\s+(?:[^:\s()]+:)?([^\s()]+)',
                      lambda m: '(footprint "%s"' % m.group(1), text, count=1)
        with open(os.path.join(staging_dir, file_name), 'w', encoding='utf-8') as f:
            f.write(text)
        footprint_entry = {"name": fp_name, "file": file_name}

    manifest = {
        "protocol": "staged-import-v1",
        "lcsc_id": lcsc_id,
        "symbols": symbols,
        "footprint": footprint_entry,
        "footprint_choice": mode,
    }
    with open(os.path.join(staging_dir, "manifest.json"), 'w', encoding='utf-8') as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    return {"ok": True, "staged": True, "symbols": [s["name"] for s in symbols],
            "footprint": footprint_entry["name"] if footprint_entry else "",
            "mode": mode}


def fetch_and_merge(lcsc_id: str, target_file: str, output_dir: str,
                    footprint_choice: dict = None) -> dict:
    os.makedirs(output_dir, exist_ok=True)
    prep = prepare(lcsc_id, output_dir)
    if not prep.get("ok"):
        return prep

    blocks = prep["blocks"]
    fixed = [fix_pin_types(b[1]) for b in blocks]

    choice = footprint_choice or {}
    mode = choice.get("mode", "new")
    target_lib = os.path.splitext(os.path.basename(target_file))[0]

    if mode == "reuse" and choice.get("name"):
        # Reuse an existing footprint: point the symbol at "Lib:Name" and copy nothing
        reference = f"{choice.get('library', '')}:{choice['name']}"
        fp_copied = 0
    else:
        reference = f"{target_lib}:{prep['footprint_name']}" if prep["footprint_name"] else ""
        fp_copied = None  # copy below

    if reference:
        for i in range(len(fixed)):
            fixed[i] = re.sub(r'(\(property\s*\n?\s*"Footprint"\s*\n?\s*")[^"]*(")',
                              lambda m: m.group(1) + reference + m.group(2), fixed[i])

    merge_into_file(fixed, target_file)

    # Copy footprints when creating a new one (reuse mode skips this entirely)
    if fp_copied is None:
        fp_copied = 0
        sym_dir = os.path.dirname(target_file)
        fp_base = os.path.join(os.path.dirname(sym_dir), "Footprints") if "Symbols" in sym_dir else sym_dir
        target_pretty = os.path.join(fp_base, target_lib + ".pretty")
        os.makedirs(target_pretty, exist_ok=True)

        for d in os.listdir(output_dir):
            src_pretty = os.path.join(output_dir, d)
            if not d.endswith(".pretty") or not os.path.isdir(src_pretty):
                continue
            for f in os.listdir(src_pretty):
                if f.endswith(".kicad_mod"):
                    with open(os.path.join(src_pretty, f), 'rb') as fr: data = fr.read()
                    with open(os.path.join(target_pretty, f), 'wb') as fw: fw.write(data)
                    fp_copied += 1

        for f in os.listdir(output_dir):
            if f.endswith(".kicad_mod") and os.path.isfile(os.path.join(output_dir, f)):
                with open(os.path.join(output_dir, f), 'rb') as fr: data = fr.read()
                with open(os.path.join(target_pretty, f), 'wb') as fw: fw.write(data)
                fp_copied += 1

    return {"ok": True, "lcsc_id": lcsc_id, "merged_into": target_file,
            "symbol_names": prep["symbol_names"], "block_count": len(blocks),
            "mode": mode, "footprint_reference": reference,
            "reused_from": choice.get("name", "") if mode == "reuse" else "",
            "footprints_copied": fp_copied, "output_dir": output_dir}


def cleanup_fetched(keep: int = 5):
    """Delete oldest fetched dirs, keeping the `keep` most recent ones."""
    fetched_dir = os.path.join(PLUGIN_DIR, "fetched")
    if not os.path.isdir(fetched_dir):
        return
    dirs = []
    for d in os.listdir(fetched_dir):
        full = os.path.join(fetched_dir, d)
        if os.path.isdir(full):
            dirs.append((os.path.getmtime(full), full))
    dirs.sort(key=lambda x: x[0], reverse=True)  # newest first
    for _, d in dirs[keep:]:
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(json.dumps({"ok": False, "error": "Usage: plugin.py <action> [args]"})); sys.exit(1)
    action, args = sys.argv[1], json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    lcsc_id = args.get("source") or args.get("lcsc_id", "")
    out_dir = os.path.join(PLUGIN_DIR, "fetched", lcsc_id)

    if action == "analyze":
        if not lcsc_id:
            print(json.dumps({"ok": False, "error": "Missing lcsc_id"})); sys.exit(1)
        # 每次分析都转换到独立目录，成功后才提交为缓存 ——
        # 否则网络/转换失败会把上一次的结果当成这次的结果
        work_dir = os.path.join(PLUGIN_DIR, "fetched", f".work-{lcsc_id}-{os.getpid()}")
        shutil.rmtree(work_dir, ignore_errors=True)
        prep = prepare(lcsc_id, work_dir)
        if prep.get("ok"):
            shutil.rmtree(out_dir, ignore_errors=True)
            os.replace(work_dir, out_dir)
            prep["output_dir"] = out_dir
            prep["candidates"] = search_host_footprints(prep["footprint_name"])
            prep["lcsc_id"] = lcsc_id
        else:
            shutil.rmtree(work_dir, ignore_errors=True)
        prep.pop("blocks", None)  # keep stdout small
        print(json.dumps(prep))
    elif action == "import":
        # 宿主暂存协议：只产出文件到暂存目录，不碰目标库
        staging = args.get("staging_dir", "")
        if staging and args.get("protocol") == "staged-import-v1":
            result = staged_import(lcsc_id, staging, args.get("footprint_choice"))
            print(json.dumps(result))
        else:
            # 旧协议（插件自行合并）—— 宿主未启用暂存流程时保留，但不再受文件提交保护
            tgt = args.get("target_library", "") or args.get("options", {}).get(
                "target_library", "")
            if not tgt:
                print(json.dumps({"ok": False, "error": "Missing target_library"})); sys.exit(1)
            result = fetch_and_merge(lcsc_id, tgt, out_dir, args.get("footprint_choice"))
            cleanup_fetched(keep=5)
            print(json.dumps(result))
    elif action == "info":
        with open(os.path.join(PLUGIN_DIR, "manifest.json"), encoding='utf-8') as source:
            print(source.read())
    else:
        print(json.dumps({"ok": False, "error": f"Unknown: {action}"}))
