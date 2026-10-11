#!/usr/bin/env python3
"""
Git version-control plugin — init, commit, history, diff over .kicad_sym files.
Raw git output is returned as-is for the frontend to display.
"""
import json, os, subprocess, sys

TIMEOUT = 30  # seconds — plenty for a local git operation

PLUGIN_DIR = os.path.dirname(os.path.abspath(__file__))


def git(args, cwd=None):
    """Run a git command and return (ok, stdout, stderr)."""
    r = subprocess.run(
        ["git"] + args,
        capture_output=True, text=True, timeout=TIMEOUT, cwd=cwd)
    return r.returncode == 0, r.stdout.strip(), r.stderr.strip()


def repo_root(lib_path):
    """Given a library file path, find the enclosing git repo root."""
    d = os.path.dirname(os.path.abspath(lib_path)) if lib_path else os.getcwd()
    ok, out, _ = git(["rev-parse", "--show-toplevel"], cwd=d)
    return out if ok else None


# ---- action handlers ----

def do_init(lib_path):
    d = os.path.dirname(os.path.abspath(lib_path)) if lib_path else os.getcwd()
    if not os.path.isdir(d):
        return {"ok": False, "error": f"Directory not found: {d}"}
    ok, out, err = git(["init"], cwd=d)
    if not ok:
        return {"ok": False, "error": err or "git init failed"}

    # Report init and first commit as separate outcomes — identity not
    # configured, index locked or a hook rejection must not read as success.
    ok_add, _, err_add = git(["add", "-A"], cwd=d)
    ok_commit, out_c, err_c = git(["commit", "-m", "KF: initial commit"], cwd=d)

    note = ""
    if not ok_add:
        note = f"git add failed: {err_add}"
    elif not ok_commit:
        combined = f"{out_c}\n{err_c}".lower()
        note = ("nothing to commit yet" if "nothing to commit" in combined
                else f"git commit failed: {err_c}")

    return {"ok": True, "initialized": True, "committed": ok_commit,
            "output": "\n".join(x for x in (out, out_c, note) if x).strip(),
            "note": note}


def do_commit(lib_path, message):
    root = repo_root(lib_path)
    if not root:
        return {"ok": False, "error": "No Git repository found. Run Git Init first."}
    git(["add", "-A"], cwd=root)
    ok, out, err = git(["commit", "-m", message], cwd=root)
    if not ok:
        return {"ok": False, "error": err or "git commit failed (nothing to commit?)"}
    return {"ok": True, "output": out}


def do_status(lib_path):
    """Repository overview for the dialog: initialised? branch? last commit? changes?"""
    root = repo_root(lib_path)
    if not root:
        return {"ok": True, "initialized": False, "root": "",
                "branch": "", "last_commit": "", "changes": ""}
    ok_b, branch, _ = git(["rev-parse", "--abbrev-ref", "HEAD"], cwd=root)
    ok_l, last, _ = git(["log", "-1", "--oneline", "--decorate"], cwd=root)
    ok_s, status, _ = git(["status", "--porcelain"], cwd=root)
    changed = len([ln for ln in status.splitlines() if ln.strip()]) if ok_s else 0
    _, ahead, _ = git(["rev-list", "--count", "@{upstream}..HEAD"], cwd=root)
    return {"ok": True, "initialized": True, "root": root,
            "branch": branch if ok_b else "(detached)",
            "last_commit": last if ok_l else "(no commits yet)",
            "changes": f"{changed} file(s)" if changed else "clean",
            "ahead": ahead or ""}


def do_history(lib_path):
    root = repo_root(lib_path)
    if not root:
        return {"ok": False, "error": "No Git repository found."}
    ok, out, err = git(
        ["log", "--oneline", "--graph", "--max-count=20", "--decorate"], cwd=root)
    if not ok:
        return {"ok": False, "error": err or "git log failed"}
    return {"ok": True, "output": out or "(no commits yet)"}


def do_diff(lib_path):
    root = repo_root(lib_path)
    if not root:
        return {"ok": False, "error": "No Git repository found."}
    # Show unstaged + staged changes
    ok, out, err = git(["diff", "--stat"], cwd=root)
    ok2, out2, err2 = git(["diff", "--cached", "--stat"], cwd=root)
    result = []
    if out: result.append("=== Unstaged changes ===\n" + out)
    if out2: result.append("=== Staged changes ===\n" + out2)
    if not result:
        return {"ok": True, "output": "Working tree clean — no changes."}
    return {"ok": True, "output": "\n\n".join(result)}


# ---- CLI entry ----

ACTIONS = {
    "status": lambda args: do_status(args.get("target_library", "")),
    "init": lambda args: do_init(args.get("target_library", "")),
    "commit": lambda args: do_commit(
        args.get("target_library", ""),
        args.get("message", "KF: auto commit")),
    "history": lambda args: do_history(args.get("target_library", "")),
    "diff": lambda args: do_diff(args.get("target_library", "")),
}


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(json.dumps({"ok": False, "error": "Usage: plugin.py <action> [json]"}))
        sys.exit(1)

    action = sys.argv[1]
    args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}

    if action == "info":
        with open(os.path.join(PLUGIN_DIR, "manifest.json"), encoding="utf-8") as f:
            print(f.read())
        sys.exit(0)

    fn = ACTIONS.get(action)
    if fn is None:
        print(json.dumps({"ok": False, "error": f"Unknown action: {action}"}))
        sys.exit(1)

    result = fn(args)
    print(json.dumps(result))
