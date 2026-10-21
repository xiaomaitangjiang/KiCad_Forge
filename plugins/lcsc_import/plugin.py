#!/usr/bin/env python3
"""
LCSC Import Plugin — fetches from easyeda2kicad (bundled, MIT), merges symbol blocks.

Flow (host UI template "lcsc_import"):
  analyze → download/convert into a private temp dir (no library write, no cache),
            return the part info + every auto-matched footprint candidate
  import  → staged-import-v1: only write symbol/footprint artifacts into the host's
            private staging dir; the host owns the target library, the footprint
            destination and the database

Usage:
  python plugin.py analyze '{"lcsc_id":"C37593"}'
  python plugin.py import  '{"source":"C37593","staging_dir":"/tmp/stage","protocol":"staged-import-v1",
                             "footprint_choice":{"mode":"reuse","footprint_id":"<uuid>"}}'
"""

import json, os, re, shutil, subprocess, sys, tempfile, urllib.parse, urllib.request

PLUGIN_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PLUGIN_DIR)

# 旧「插件自行合并到目标库」流程的落盘目录。新流程只写宿主给的暂存目录，
# 这里保留常量是为了兼容旧调用方，不再作为缓存使用。
FETCHED_DIR = os.path.join(PLUGIN_DIR, "fetched")

POWER_NAMES = {'VCC','VDD','V+','VCCA','VCCD','VBAT','VIN','AVDD','DVDD',
               'VSS','GND','V-','VEE','AGND','DGND','VS+','VP','VPP','VREF'}


def normalize_lcsc_id(value) -> str:
    """规范化立创编号：首尾空白 + 大小写，纯数字补 C。

    只接受 C+数字；空、路径字符、混杂编号一律 ValueError。
    必须在构造任何缓存路径或发起下载之前调用。
    """
    if not isinstance(value, str):
        raise ValueError("invalid LCSC part number: %r" % (value,))
    text = value.strip().upper()
    if re.fullmatch(r"[0-9]+", text):
        text = "C" + text
    if not re.fullmatch(r"C[0-9]+", text):
        raise ValueError("invalid LCSC part number: %r" % (value,))
    return text


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


# ============================================================
# Prepare — download + convert (no library write)
# ============================================================

def prepare(lcsc_id: str, output_dir: str) -> dict:
    """Run easyeda2kicad into output_dir. Returns symbol blocks + parsed metadata.

    转换退出码必须为 0：失败时目录里可能还留着上一次的产物，
    只看「有没有 .kicad_sym」会把旧文件当成新结果报成功。
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

    sym_path = os.path.join(output_dir, syms[0])
    with open(sym_path, encoding='utf-8') as symbol_file:
        blocks = extract_symbol_blocks(symbol_file.read())
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
                with open(os.path.join(root, f), encoding='utf-8', errors='ignore') as footprint_file:
                    mm = re.search(r'\(model\s+"([^"]+)"', footprint_file.read())
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

def search_host_footprints(near: str, limit: int = 50):
    """查询宿主封装搜索 API，返回 (候选列表, warning)。

    候选原样回传（UUID/名称/库/分数/焊盘数都在里面，同名不同 UUID 也各自保留）。
    查询不可用/失败返回 warning —— 不能把「查不到」说成「确定没有匹配」，
    新建路径仍然可以继续。
    """
    base = os.environ.get("KFORGE_API_URL", "")
    if not base:
        return [], "host footprint search unavailable: KFORGE_API_URL is not set"
    if not near:
        return [], "host footprint search skipped: the converted symbol has no footprint name"
    url = f"{base}/api/footprints/search?near={urllib.parse.quote(near)}&limit={limit}"
    try:
        with urllib.request.urlopen(url, timeout=5) as resp:
            payload = json.loads(resp.read().decode('utf-8'))
    except Exception as exc:
        return [], "host footprint search failed: %s" % (exc,)
    if not isinstance(payload, list):
        return [], "host footprint search failed: unexpected response payload"
    return payload, ""


# ============================================================
# Analyze — 只输编号：下载转换 + 完整候选，不要求目标库，不缓存
# ============================================================

def analyze(lcsc_id) -> dict:
    """分析一个立创编号：转换到独立临时目录，返回器件/封装信息与全部候选。

    失败不回传上一次的结果；临时目录用完即删（不落插件目录，不复用缓存）。
    """
    try:
        canonical = normalize_lcsc_id(lcsc_id)
    except ValueError as exc:
        return {"ok": False, "error": str(exc)}

    work_dir = tempfile.mkdtemp(prefix="kforge-lcsc-analyze-")
    try:
        prep = prepare(canonical, work_dir)
        if not prep.get("ok"):
            return {"ok": False, "error": prep.get("error", "conversion failed")}
        candidates, warning = search_host_footprints(prep.get("footprint_name", ""))
        return {"ok": True,
                "lcsc_id": canonical,
                "symbol_names": list(prep.get("symbol_names", [])),
                "footprint_name": prep.get("footprint_name", ""),
                "model_name": prep.get("model_name", ""),
                "candidates": candidates,
                "warning": warning}
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)


# ============================================================
# Staged import (protocol staged-import-v1)
# 只往宿主给的暂存目录里产出文件；目标库、封装落点、数据库全由宿主决定
# ============================================================

def _converted_footprint_artifacts(staging_dir):
    """暂存目录里转换器留下的封装产物（.kicad_mod 文件与 .pretty 目录）。"""
    found = []
    for entry in sorted(os.listdir(staging_dir)):
        full = os.path.join(staging_dir, entry)
        if entry.endswith(".kicad_mod") and os.path.isfile(full):
            found.append(full)
        elif entry.endswith(".pretty") and os.path.isdir(full):
            for name in sorted(os.listdir(full)):
                if name.endswith(".kicad_mod"):
                    found.append(os.path.join(full, name))
    return found


def _drop_converted_footprints(staging_dir, keep=None):
    """删掉转换产物，只留宿主认可的那一份（keep）。"""
    for path in _converted_footprint_artifacts(staging_dir):
        if keep is not None and os.path.normcase(os.path.abspath(path)) == \
                os.path.normcase(os.path.abspath(keep)):
            continue
        try:
            os.remove(path)
        except OSError:
            pass
    for entry in sorted(os.listdir(staging_dir)):
        full = os.path.join(staging_dir, entry)
        if entry.endswith(".pretty") and os.path.isdir(full):
            try:
                if not os.listdir(full):
                    os.rmdir(full)
            except OSError:
                pass


def staged_import(lcsc_id, staging_dir: str, footprint_choice: dict = None) -> dict:
    """把某个规范立创编号的符号/封装写进宿主暂存目录。

    复用模式不写封装文件；新建模式保留转换产物。每次严格对应同一规范编号：
    非法编号在磁盘操作之前拒绝。
    """
    try:
        canonical = normalize_lcsc_id(lcsc_id)
    except ValueError as exc:
        return {"ok": False, "error": str(exc)}

    if footprint_choice is None:
        choice = {}
    elif isinstance(footprint_choice, dict):
        choice = footprint_choice
    else:
        return {"ok": False, "error": "footprint_choice must be an object"}
    mode = choice.get("mode", "new")
    if mode not in ("new", "reuse"):
        return {"ok": False, "error": "Unknown footprint mode: %r" % (mode,)}

    os.makedirs(staging_dir, exist_ok=True)
    prep = prepare(canonical, staging_dir)
    if not prep.get("ok"):
        return {"ok": False, "error": prep.get("error", "conversion failed")}

    blocks = [(name, fix_pin_types(text)) for name, text in prep["blocks"]]
    symbols = []
    for index, (name, text) in enumerate(blocks):
        # 暂存文件名由插件自己固定编号，不让上游元件名影响磁盘路径：
        # 名字仍然写在 manifest 与文件内容里，宿主按名字校验
        file_name = "symbol-%d.kicad_sym" % index
        with open(os.path.join(staging_dir, file_name), 'w', encoding='utf-8') as f:
            f.write("(kicad_symbol_lib\n  (version 20231120)\n  (generator KiCad_Forge)\n"
                    + text + "\n)\n")
        symbols.append({"name": name, "file": file_name})

    footprint_entry = None
    if mode != "reuse":
        # 找 easyeda2kicad 生成的 .kicad_mod，规范根节点后原样放进暂存目录
        sources = _converted_footprint_artifacts(staging_dir)
        if not sources:
            return {"ok": False, "error": "No .kicad_mod produced"}
        source = sources[0]
        fp_name = os.path.splitext(os.path.basename(source))[0] or prep["footprint_name"]
        # 磁盘路径用固定安全名；封装名只作为 manifest 里的显示名交给宿主校验
        file_name = "footprint-0.kicad_mod"
        # 转换器输出的是 KiCad 5 的 (module 库名:封装名 ...)：把根节点规范成
        # (footprint "封装名" ...)，其余内容原样保留
        with open(source, encoding='utf-8', errors='ignore') as f:
            text = f.read()
        text = re.sub(r'^\(module\s+(?:[^:\s()]+:)?([^\s()]+)',
                      lambda m: '(footprint "%s"' % m.group(1), text, count=1)
        # manifest 的封装名必须等于文件根节点名（宿主会核对）：以规范化后的
        # 根节点为准，文件名只是兜底 —— 上游文件名与封装名不一致时不能两边各说各话。
        # 旧格式的「库名:封装名」与宿主解析器同一规则：取第一个冒号之后的部分。
        root_match = re.search(r'\A\s*\(\s*(?:footprint|module)\s+"(?:[^:\s()"]+:)?([^"]*)"', text)
        if root_match and root_match.group(1):
            fp_name = root_match.group(1)
        target = os.path.join(staging_dir, file_name)
        with open(target, 'w', encoding='utf-8') as f:
            f.write(text)
        # 只留这一份规范化产物：让转换器的原始文件留在暂存目录里，
        # 宿主可能按文件名/根节点挑到未经规范的那一份
        _drop_converted_footprints(staging_dir, keep=target)
        footprint_entry = {"name": fp_name, "file": file_name}
    else:
        # 复用既有封装：一个封装文件都不留（转换产物只是下载过程中的中间物）
        _drop_converted_footprints(staging_dir)

    manifest = {
        "protocol": "staged-import-v1",
        "lcsc_id": canonical,
        "symbols": symbols,
        "footprint": footprint_entry,
        "footprint_choice": mode,
    }
    with open(os.path.join(staging_dir, "manifest.json"), 'w', encoding='utf-8') as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    return {"ok": True, "staged": True, "lcsc_id": canonical,
            "symbols": [s["name"] for s in symbols],
            "footprint": footprint_entry["name"] if footprint_entry else "",
            "mode": mode}


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(json.dumps({"ok": False, "error": "Usage: plugin.py <action> [args]"})); sys.exit(1)
    action, args = sys.argv[1], json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    lcsc_id = args.get("source") or args.get("lcsc_id", "")

    if action == "analyze":
        # 分析不要求目标库：只下载转换到独立临时目录并回传候选
        print(json.dumps(analyze(lcsc_id)))
    elif action == "import":
        # 宿主暂存协议：只产出文件到暂存目录，不碰目标库。
        # 旧「插件自行合并到任意目标文件」的分支已移除：UI 操作不再由插件直接改库。
        staging = args.get("staging_dir", "")
        if args.get("protocol") != "staged-import-v1" or not staging:
            print(json.dumps({"ok": False,
                              "error": "Missing staged-import-v1 staging_dir"})); sys.exit(1)
        print(json.dumps(staged_import(lcsc_id, staging, args.get("footprint_choice"))))
    elif action == "info":
        with open(os.path.join(PLUGIN_DIR, "manifest.json"), encoding='utf-8') as manifest_file:
            print(manifest_file.read())
    else:
        print(json.dumps({"ok": False, "error": f"Unknown: {action}"}))
