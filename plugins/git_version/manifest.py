"""Git manifest helpers: canonical content hashes and semantic diffs.

The host owns KiCad parsing; this module only compares the JSON snapshots the
host returns. Keeping it independent makes the diff unit-testable without an
app process or a real git repository.
"""
import copy
import hashlib
import json
import os

MANIFEST_VERSION = 1
MAX_DIFF_LINES = 240


def canonical_hash(value):
    payload = json.dumps(value, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def file_sha256(path):
    try:
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def normalize_manifest(manifest, root):
    """Return a deterministic copy with content hashes filled in.

    Manifest paths are relative to the repository root whenever possible so the
    committed file stays usable on another checkout. A symbol's content hash
    covers every semantic field the host exported, including its KF identity;
    the hash field itself is excluded so hashing is stable.
    """
    data = copy.deepcopy(manifest)
    data.pop("root", None)
    data["manifest_version"] = MANIFEST_VERSION
    data.setdefault("libraries", [])
    data.setdefault("symbols", [])
    data.setdefault("footprints", [])
    data.setdefault("models", [])

    for symbol in data["symbols"]:
        symbol.pop("content_sha256", None)
        symbol["content_sha256"] = canonical_hash(symbol)

    for model in data["models"]:
        path = model.get("path", "")
        absolute = path if os.path.isabs(path) else os.path.join(root, path)
        model["sha256"] = file_sha256(absolute)

    return data


def _library_key(library):
    return library.get("id") or library.get("source_key") or library.get("name", "")


def _symbol_key(symbol):
    return symbol.get("uuid") or (symbol.get("library_id", ""), symbol.get("name", ""))


def _footprint_key(footprint):
    return footprint.get("uuid") or (footprint.get("library", ""), footprint.get("name", ""))


def _label(symbol):
    library = symbol.get("library_name") or symbol.get("library_id") or "?"
    return f"{library}:{symbol.get('name', '?')}"


def _group_pins(pins):
    grouped = {}
    for pin in pins or []:
        grouped.setdefault(str(pin.get("number", "")), []).append(
            (str(pin.get("name", "")), str(pin.get("electrical_type", ""))))
    for value in grouped.values():
        value.sort()
    return grouped


def _group_pads(pads):
    grouped = {}
    for pad in pads or []:
        grouped.setdefault(str(pad.get("number", "")), []).append(
            tuple((key, pad.get(key)) for key in
                  ("type", "shape", "x", "y", "width", "height", "drill")))
    for value in grouped.values():
        value.sort()
    return grouped


def _short(value, limit=64):
    text = "" if value is None else str(value)
    return text if len(text) <= limit else text[:limit - 1] + "…"


def _diff_properties(old, new, prefix, lines):
    old = old or {}
    new = new or {}
    added = sorted(set(new) - set(old))
    removed = sorted(set(old) - set(new))
    changed = sorted(key for key in set(old) & set(new) if old[key] != new[key])
    if not (added or removed or changed):
        return
    lines.append(f"{prefix}属性: +{len(added)} / -{len(removed)} / ~{len(changed)}")
    for key in added:
        lines.append(f"{prefix}  + {key}: {_short(new[key])}")
    for key in removed:
        lines.append(f"{prefix}  - {key}: {_short(old[key])}")
    for key in changed:
        lines.append(f"{prefix}  ~ {key}: {_short(old[key])} → {_short(new[key])}")


def semantic_diff(old, new):
    """Compare two normalized manifests; return human-readable lines."""
    lines = []
    old_libraries = {_library_key(item): item for item in old.get("libraries", [])}
    new_libraries = {_library_key(item): item for item in new.get("libraries", [])}
    library_added = sorted(set(new_libraries) - set(old_libraries))
    library_removed = sorted(set(old_libraries) - set(new_libraries))

    old_symbols = {_symbol_key(item): item for item in old.get("symbols", [])}
    new_symbols = {_symbol_key(item): item for item in new.get("symbols", [])}
    symbol_added = sorted(set(new_symbols) - set(old_symbols))
    symbol_removed = sorted(set(old_symbols) - set(new_symbols))

    old_footprints = {_footprint_key(item): item for item in old.get("footprints", [])}
    new_footprints = {_footprint_key(item): item for item in new.get("footprints", [])}
    footprint_added = sorted(set(new_footprints) - set(old_footprints))
    footprint_removed = sorted(set(old_footprints) - set(new_footprints))

    old_models = {item.get("path", ""): item for item in old.get("models", [])}
    new_models = {item.get("path", ""): item for item in new.get("models", [])}
    model_added = sorted(set(new_models) - set(old_models))
    model_removed = sorted(set(old_models) - set(new_models))

    if library_added or library_removed:
        lines.append(f"元件库: +{len(library_added)} / -{len(library_removed)}")
        for key in library_added:
            lines.append(f"  + {new_libraries[key].get('name') or key}")
        for key in library_removed:
            lines.append(f"  - {old_libraries[key].get('name') or key}")

    # Symbols first: the per-symbol lines identify pin/property changes.
    changed_symbols = []
    for key in sorted(set(old_symbols) & set(new_symbols)):
        before = old_symbols[key]
        after = new_symbols[key]
        detail = []
        for field, label in (("name", "名称"), ("footprint_ref", "封装引用"),
                             ("kf_id", "KF ID"), ("component_type_id", "元件类型"),
                             ("package_type_id", "封装类型")):
            if before.get(field) != after.get(field):
                detail.append(f"{label}: {_short(before.get(field))} → {_short(after.get(field))}")
        old_pins = _group_pins(before.get("pins"))
        new_pins = _group_pins(after.get("pins"))
        pin_added = sorted(set(new_pins) - set(old_pins))
        pin_removed = sorted(set(old_pins) - set(new_pins))
        pin_changed = sorted(number for number in set(old_pins) & set(new_pins)
                             if old_pins[number] != new_pins[number])
        if pin_added or pin_removed or pin_changed:
            detail.append(f"引脚: +{len(pin_added)} / -{len(pin_removed)} / ~{len(pin_changed)}")
            for number in pin_added:
                detail.append(f"  + 引脚 {number}")
            for number in pin_removed:
                detail.append(f"  - 引脚 {number}")
            for number in pin_changed:
                detail.append(f"  ~ 引脚 {number}: {old_pins[number]} → {new_pins[number]}")
        old_uuid = before.get("footprint_uuid", "")
        new_uuid = after.get("footprint_uuid", "")
        if old_uuid != new_uuid:
            detail.append(f"封装链接: {_short(old_uuid)} → {_short(new_uuid)}")

        prop_lines = []
        _diff_properties(before.get("properties"), after.get("properties"), "  ", prop_lines)
        if prop_lines:
            detail.extend(prop_lines)

        old_footprint = old_footprints.get(old_uuid)
        new_footprint = new_footprints.get(new_uuid)
        if old_footprint and new_footprint:
            old_pads = _group_pads(old_footprint.get("pads"))
            new_pads = _group_pads(new_footprint.get("pads"))
            pad_added = sorted(set(new_pads) - set(old_pads))
            pad_removed = sorted(set(old_pads) - set(new_pads))
            pad_changed = sorted(number for number in set(old_pads) & set(new_pads)
                                 if old_pads[number] != new_pads[number])
            if pad_added or pad_removed or pad_changed:
                detail.append(f"焊盘: +{len(pad_added)} / -{len(pad_removed)} / ~{len(pad_changed)}")
                for number in pad_added:
                    detail.append(f"  + 焊盘 {number}")
                for number in pad_removed:
                    detail.append(f"  - 焊盘 {number}")
                for number in pad_changed:
                    detail.append(f"  ~ 焊盘 {number}")
            if old_footprint.get("parse_error") != new_footprint.get("parse_error"):
                detail.append("封装文件解析状态变化")

        if detail:
            changed_symbols.append((_label(after), detail))

    if symbol_added or symbol_removed or changed_symbols:
        lines.append(f"符号: +{len(symbol_added)} / -{len(symbol_removed)} / ~{len(changed_symbols)}")
        for key in symbol_added:
            lines.append(f"  + {_label(new_symbols[key])}")
        for key in symbol_removed:
            lines.append(f"  - {_label(old_symbols[key])}")
        for label, detail in changed_symbols:
            lines.append(f"  ~ {label}")
            lines.extend("    " + item if not item.startswith("  ") else "  " + item
                         for item in detail)

    changed_footprints = []
    for key in sorted(set(old_footprints) & set(new_footprints)):
        before = old_footprints[key]
        after = new_footprints[key]
        detail = []
        for field, label in (("name", "名称"), ("library", "库名"), ("pad_count", "焊盘数")):
            if before.get(field) != after.get(field):
                detail.append(f"{label}: {_short(before.get(field))} → {_short(after.get(field))}")
        old_pads = _group_pads(before.get("pads"))
        new_pads = _group_pads(after.get("pads"))
        pad_added = sorted(set(new_pads) - set(old_pads))
        pad_removed = sorted(set(old_pads) - set(new_pads))
        pad_changed = sorted(number for number in set(old_pads) & set(new_pads)
                             if old_pads[number] != new_pads[number])
        if pad_added or pad_removed or pad_changed:
            detail.append(f"焊盘: +{len(pad_added)} / -{len(pad_removed)} / ~{len(pad_changed)}")
            for number in pad_added:
                detail.append(f"  + 焊盘 {number}")
            for number in pad_removed:
                detail.append(f"  - 焊盘 {number}")
            for number in pad_changed:
                detail.append(f"  ~ 焊盘 {number}")
        if detail:
            changed_footprints.append(
                (f"{after.get('library', '?')}:{after.get('name', '?')}", detail))

    if footprint_added or footprint_removed or changed_footprints:
        lines.append(f"封装: +{len(footprint_added)} / -{len(footprint_removed)} / ~{len(changed_footprints)}")
        for key in footprint_added:
            item = new_footprints[key]
            lines.append(f"  + {item.get('library', '?')}:{item.get('name', '?')}")
        for key in footprint_removed:
            item = old_footprints[key]
            lines.append(f"  - {item.get('library', '?')}:{item.get('name', '?')}")
        for label, detail in changed_footprints:
            lines.append(f"  ~ {label}")
            lines.extend("    " + item if not item.startswith("  ") else "  " + item
                         for item in detail)

    changed_models = []
    for path in sorted(set(old_models) & set(new_models)):
        if old_models[path].get("sha256") != new_models[path].get("sha256"):
            changed_models.append(path)
    if model_added or model_removed or changed_models:
        lines.append(f"3D 模型: +{len(model_added)} / -{len(model_removed)} / ~{len(changed_models)}")
        for path in model_added:
            lines.append(f"  + {path}")
        for path in model_removed:
            lines.append(f"  - {path}")
        for path in changed_models:
            lines.append(f"  ~ {path}")

    if not lines:
        return ["工作区与 HEAD 清单一致（无语义变化）。"]
    if len(lines) > MAX_DIFF_LINES:
        omitted = len(lines) - MAX_DIFF_LINES
        lines = lines[:MAX_DIFF_LINES] + [f"… 还有 {omitted} 行差异未显示"]
    return lines
