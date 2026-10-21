#!/usr/bin/env python3
"""
Git version-control plugin — init, commit, history, diff per component library.
Raw git output is returned as-is for the frontend to display.
"""
import base64, hashlib, json, os, re, subprocess, sys, tempfile, threading
import urllib.error
import urllib.request

from manifest import normalize_manifest, semantic_diff

TIMEOUT = 30  # seconds — plenty for a local git operation

PLUGIN_DIR = os.path.dirname(os.path.abspath(__file__))
MANIFEST_PATH = os.path.join(".kforge", "manifest.json")


def git(args, cwd=None):
    """Run a git command and return (ok, stdout, stderr)."""
    r = subprocess.run(
        ["git"] + args,
        capture_output=True, text=True, timeout=TIMEOUT, cwd=cwd)
    return r.returncode == 0, r.stdout.strip(), r.stderr.strip()


def library_dir(lib_path):
    """Component libraries pass a directory; legacy symbol libraries pass a file."""
    if not lib_path:
        return os.getcwd()
    path = os.path.abspath(lib_path)
    return path if os.path.isdir(path) else os.path.dirname(path)


def repo_root(lib_path):
    """Given a library file/directory, find the enclosing git repo root."""
    d = library_dir(lib_path)
    ok, out, _ = git(["rev-parse", "--show-toplevel"], cwd=d)
    return out if ok else None


# ---- semantic manifest ----

def _api_manifest(root, target):
    """Ask the host for a semantic snapshot; never leak the API URL as an error."""
    base = os.environ.get("KFORGE_API_URL", "").rstrip("/")
    if not base:
        return None, "宿主 API 不可用，已回退到原始 git diff"
    body = json.dumps({"root": root, "target": target}, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        base + "/api/library-manifest", data=body, method="POST",
        headers={"Content-Type": "application/json"})
    # 明确绕过系统代理：这是本机回环地址，代理只会把请求送错地方。
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=60) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError) as error:
        return None, f"读取语义清单失败：{error}"
    if not payload.get("ok"):
        return None, payload.get("error") or "宿主未能生成语义清单"
    return payload, None


def _write_manifest(root, target):
    """Write .kforge/manifest.json atomically before staging the commit."""
    payload, error = _api_manifest(root, target)
    if payload is None:
        return False, error
    manifest = normalize_manifest(payload, root)
    path = os.path.join(root, MANIFEST_PATH)
    text = json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    try:
        current = None
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as handle:
                current = handle.read()
        if current == text:
            return True, ""
        os.makedirs(os.path.dirname(path), exist_ok=True)
        temporary = path + ".tmp"
        with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        os.replace(temporary, path)
        return True, ""
    except OSError as error:
        return False, f"写入语义清单失败：{error}"


def _head_manifest(root):
    ok, out, _ = git(["show", "HEAD:" + MANIFEST_PATH.replace(os.sep, "/")], cwd=root)
    if not ok:
        return None
    try:
        payload = json.loads(out)
    except json.JSONDecodeError:
        return None
    if not isinstance(payload, dict) or payload.get("manifest_version") != 1:
        return None
    return payload


def _semantic_diff(root, target):
    current, error = _api_manifest(root, target)
    if current is None:
        return None
    previous = _head_manifest(root)
    if previous is None:
        return None
    current = normalize_manifest(current, root)
    lines = semantic_diff(previous, current)
    return "=== 语义差异（HEAD 清单 ↔ 当前工作区） ===\n" + "\n".join(lines)


# ---- action handlers ----

def do_init(lib_path):
    d = library_dir(lib_path)
    if not os.path.isdir(d):
        return {"ok": False, "error": f"Directory not found: {d}"}
    ok, out, err = git(["init"], cwd=d)
    if not ok:
        return {"ok": False, "error": err or "git init failed"}

    manifest_ok, manifest_note = _write_manifest(d, lib_path)

    # Report init and first commit as separate outcomes — identity not
    # configured, index locked or a hook rejection must not read as success.
    ok_add, _, err_add = git(["add", "-A"], cwd=d)
    ok_commit, out_c, err_c = git(["commit", "-m", "KF: initial commit"], cwd=d)

    note = ""
    if not manifest_ok:
        note = manifest_note
    if not ok_add:
        note = f"git add failed: {err_add}"
    elif not ok_commit:
        combined = f"{out_c}\n{err_c}".lower()
        note = ("nothing to commit yet" if "nothing to commit" in combined
                else f"git commit failed: {err_c}")

    return {"ok": True, "initialized": True, "committed": ok_commit,
            "output": "\n".join(x for x in (out, out_c, note) if x).strip(),
            "note": note, "manifest": manifest_ok}


def do_commit(lib_path, message):
    root = repo_root(lib_path)
    if not root:
        return {"ok": False, "error": "No Git repository found. Run Git Init first."}
    manifest_ok, manifest_note = _write_manifest(root, lib_path)
    git(["add", "-A"], cwd=root)
    ok, out, err = git(["commit", "-m", message], cwd=root)
    if not ok:
        return {"ok": False, "error": err or "git commit failed (nothing to commit?)",
                "manifest": manifest_ok, "note": manifest_note}
    result = {"ok": True, "output": out, "manifest": manifest_ok}
    if not manifest_ok:
        result["note"] = manifest_note
    return result


def do_status(lib_path):
    """Repository overview for the dialog: initialised? branch? last commit? changes?"""
    root = repo_root(lib_path)
    if not root:
        return {"ok": True, "initialized": False, "root": "",
                "branch": "", "last_commit": "", "changes": ""}
    ok_b, branch, _ = git(["rev-parse", "--abbrev-ref", "HEAD"], cwd=root)
    ok_l, last, _ = git(["log", "-1", "--oneline", "--decorate"], cwd=root)
    ok_s, status, _ = git(["status", "--porcelain"], cwd=root)
    files = []
    if ok_s:
        for line in status.splitlines():
            if not line.strip():
                continue
            code = line[:2].strip() or "?"
            path = line[3:].strip() if len(line) > 3 else ""
            files.append({"status": code, "path": path})
    changed = len(files)
    _, ahead, _ = git(["rev-list", "--count", "@{upstream}..HEAD"], cwd=root)
    return {"ok": True, "initialized": True, "root": root,
            "branch": branch if ok_b else "(detached)",
            "last_commit": last if ok_l else "(no commits yet)",
            "changes": f"{changed} file(s)" if changed else "clean",
            "files": files,
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
    semantic = _semantic_diff(root, lib_path)
    # Show unstaged + staged changes; semantic output is the primary view.
    ok, out, err = git(["diff", "--stat"], cwd=root)
    ok2, out2, err2 = git(["diff", "--cached", "--stat"], cwd=root)
    result = []
    if semantic is not None:
        result.append(semantic)
    if out: result.append("=== 原始变更（未暂存） ===\n" + out)
    if out2: result.append("=== 原始变更（已暂存） ===\n" + out2)
    if not result:
        return {"ok": True, "output": "Working tree clean — no changes."}
    return {"ok": True, "output": "\n\n".join(result)}


# ---- structured read-only graph (graph_v1) ----
#
# history / commit_detail / commit_file are read-only: they never write the
# index, the working tree or refs, and never run reset/rebase/cherry-pick.
# Git runs with an argument array (no shell) and binary-safe output, so
# Unicode paths and commit messages survive unchanged.

GRAPH_SCHEMA = "graph_v1"
GIT_EXECUTABLE = "git"
DEFAULT_PAGE_LIMIT = 50
MAX_PAGE_LIMIT = 500
MAX_GRAPH_COMMITS = 500
MAX_REFS = 500
MAX_DETAIL_FILES = 1000
MAX_FILELIST_BYTES = 4 * 1024 * 1024
MAX_MESSAGE_CHARS = 64 * 1024
MAX_PATCH_BYTES = 512 * 1024
PATCH_TRUNCATED_MARKER = "\n…（patch 已截断，仅显示前 {limit} 字节）\n"

REF_PREFIXES = (
    ("refs/heads/", "local"),
    ("refs/remotes/", "remote"),
    ("refs/tags/", "tag"),
)
_COMMIT_SUMMARY_FORMAT = "%H%x00%P%x00%s%x00%an%x00%aI%x00%cI"
_COMMIT_DETAIL_FORMAT = (
    "%H%x00%P%x00%s%x00%b%x00%an%x00%ae%x00%aI%x00%cn%x00%ce%x00%cI")
_HEX_COMMIT = re.compile(r"^[0-9a-f]{40,64}$")
_HEX_INPUT = re.compile(r"^[0-9a-fA-F]{4,64}$")


class GitFailure(Exception):
    """A structured, user-readable failure for a read-only git query."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def _classify_git_error(message):
    lowered = (message or "").lower()
    if ("permission denied" in lowered or "access is denied" in lowered
            or "operation not permitted" in lowered):
        return "permission_denied"
    if "not a git repository" in lowered:
        return "not_initialized"
    if "timed out" in lowered:
        return "timeout"
    return "git_failed"


def _raise_process_failure(returncode, stderr):
    if returncode == -1:
        raise GitFailure("git_unavailable", "未找到 Git 可执行文件（请确认 Git 已安装）")
    if returncode == -2:
        raise GitFailure("timeout", "Git 操作超时，请稍后重试")
    raise GitFailure("git_failed", stderr or "无法启动 Git")


def _git_bytes(args, cwd=None, input_bytes=None):
    """Run git without a shell and return raw output: (rc, stdout, stderr-text)."""
    try:
        process = subprocess.run(
            [GIT_EXECUTABLE, "-c", "core.quotepath=false"] + list(args),
            cwd=cwd, input=input_bytes, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=TIMEOUT)
    except FileNotFoundError:
        return -1, b"", "git executable not found"
    except subprocess.TimeoutExpired:
        return -2, b"", "git timed out"
    except OSError as error:
        return -3, b"", str(error)
    return (process.returncode, process.stdout,
            process.stderr.decode("utf-8", "replace").strip())


def _run_git(args, cwd=None, input_bytes=None, failure_code="git_failed"):
    returncode, stdout, stderr = _git_bytes(args, cwd, input_bytes)
    if returncode != 0:
        if returncode in (-1, -2, -3):
            _raise_process_failure(returncode, stderr)
        code = _classify_git_error(stderr)
        if code == "git_failed":
            code = failure_code
        raise GitFailure(code, stderr or "git 返回 %d" % returncode)
    return stdout


def _git_bytes_limited(args, cwd=None, limit=None):
    """Run git and cap stdout at *limit* bytes.

    Returns (rc, stdout, stderr-text, truncated). The child is killed once the
    cap is reached so a huge file cannot exhaust plugin memory.
    """
    if limit is None:
        limit = MAX_PATCH_BYTES
    stderr_file = tempfile.TemporaryFile()
    try:
        process = subprocess.Popen(
            [GIT_EXECUTABLE, "-c", "core.quotepath=false"] + list(args),
            cwd=cwd, stdout=subprocess.PIPE, stderr=stderr_file)
    except FileNotFoundError:
        stderr_file.close()
        return -1, b"", "git executable not found", False
    except OSError as error:
        stderr_file.close()
        return -3, b"", str(error), False

    expired = threading.Event()

    def _expire():
        expired.set()
        try:
            process.kill()
        except OSError:
            pass

    watchdog = threading.Timer(TIMEOUT, _expire)
    watchdog.start()
    stdout = b""
    truncated = False
    try:
        stdout = process.stdout.read(limit + 1)
        if len(stdout) > limit:
            stdout = stdout[:limit]
            truncated = True
            try:
                process.kill()
            except OSError:
                pass
        try:
            process.wait(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            expired.set()
            try:
                process.kill()
            except OSError:
                pass
            process.wait(timeout=5)
    finally:
        watchdog.cancel()
        try:
            process.stdout.close()
        except OSError:
            pass
        stderr_file.seek(0)
        stderr = stderr_file.read().decode("utf-8", "replace").strip()
        stderr_file.close()
        if process.poll() is None:
            try:
                process.kill()
            except OSError:
                pass
    if expired.is_set():
        return -2, b"", "git timed out", truncated
    return process.returncode, stdout, stderr, truncated


def _run_git_limited(args, cwd=None, limit=None, failure_code="git_failed"):
    returncode, stdout, stderr, truncated = _git_bytes_limited(args, cwd, limit)
    if returncode != 0 and not truncated:
        if returncode in (-1, -2, -3):
            _raise_process_failure(returncode, stderr)
        code = _classify_git_error(stderr)
        if code == "git_failed":
            code = failure_code
        raise GitFailure(code, stderr or "git 返回 %d" % returncode)
    return stdout, truncated


def _repo_context(lib_path):
    if lib_path and not isinstance(lib_path, str):
        raise GitFailure("invalid_target", "target_library 必须是字符串")
    directory = library_dir(lib_path)
    if lib_path:
        target = os.path.abspath(lib_path)
        if not os.path.exists(target):
            raise GitFailure("invalid_target", "路径不存在：%s" % target)
    if not os.path.isdir(directory):
        raise GitFailure("invalid_target", "目录不存在：%s" % directory)
    stdout = _run_git(["rev-parse", "--show-toplevel"], cwd=directory,
                      failure_code="not_initialized")
    root = stdout.decode("utf-8", "replace").strip()
    if not root:
        raise GitFailure("not_initialized", "当前目录不在 Git 仓库中")
    return root


def _read_head(root):
    returncode, stdout, stderr = _git_bytes(["symbolic-ref", "-q", "HEAD"], cwd=root)
    if returncode in (-1, -2, -3):
        _raise_process_failure(returncode, stderr)
    head_ref = stdout.decode("utf-8", "replace").strip() if returncode == 0 else None

    returncode, stdout, stderr = _git_bytes(
        ["rev-parse", "--verify", "--quiet", "HEAD^{commit}"], cwd=root)
    if returncode in (-1, -2, -3):
        _raise_process_failure(returncode, stderr)
    head_oid = None
    if returncode == 0:
        candidate = stdout.decode("ascii", "replace").strip()
        if _HEX_COMMIT.match(candidate):
            head_oid = candidate
    elif _classify_git_error(stderr) == "permission_denied":
        raise GitFailure("permission_denied", stderr)
    detached = head_oid is not None and head_ref is None
    return head_oid, head_ref, detached


def _read_refs(root, head_ref):
    stdout = _run_git([
        "for-each-ref", "--sort=refname",
        "--format=%(refname)%00%(objecttype)%00%(objectname)%00%(*objecttype)%00%(*objectname)",
    ], cwd=root)
    refs = []
    seen = 0
    for line in stdout.split(b"\n"):
        if not line:
            continue
        fields = line.split(b"\x00")
        while len(fields) < 5:
            fields.append(b"")
        full_name = fields[0].decode("utf-8", "replace")
        if not full_name:
            continue
        kind = None
        display = ""
        for prefix, candidate in REF_PREFIXES:
            if full_name.startswith(prefix):
                kind = candidate
                display = full_name[len(prefix):]
                break
        if kind is None or not display:
            continue
        object_type = fields[1]
        object_name = fields[2].decode("ascii", "replace").strip()
        peeled_type = fields[3]
        peeled_name = fields[4].decode("ascii", "replace").strip()
        if object_type == b"tag" and peeled_name:
            target_oid = peeled_name
            target_is_commit = peeled_type == b"commit"
        else:
            target_oid = object_name
            target_is_commit = object_type == b"commit"
        seen += 1
        if len(refs) >= MAX_REFS:
            continue
        refs.append({
            "name": display,
            "full_name": full_name,
            "kind": kind,
            "target_oid": target_oid,
            "target_is_commit": target_is_commit,
            "is_head": head_ref is not None and full_name == head_ref,
        })
    return refs, seen > MAX_REFS


def _public_refs(refs):
    return [{key: ref[key] for key in
             ("name", "full_name", "kind", "target_oid", "is_head")}
            for ref in refs]


def _graph_snapshot(root, range_name, branch_ref, branch_oid,
                    head_oid, head_ref, refs, refs_truncated):
    digest = hashlib.sha256()

    def feed(value):
        digest.update(("" if value is None else str(value)).encode("utf-8"))
        digest.update(b"\x00")

    feed(GRAPH_SCHEMA)
    feed(root)
    feed(range_name)
    feed(branch_ref)
    feed(branch_oid)
    feed(head_oid)
    feed(head_ref)
    feed(1 if refs_truncated else 0)
    for ref in refs:
        feed(ref["name"])
        feed(ref["kind"])
        feed(ref["target_oid"])
    return digest.hexdigest()[:32]


def _encode_cursor(payload):
    raw = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


def _decode_cursor(value):
    if not isinstance(value, str) or not value or len(value) > 512:
        return None
    try:
        padded = value + "=" * (-len(value) % 4)
        payload = json.loads(
            base64.urlsafe_b64decode(padded.encode("ascii")).decode("utf-8"))
    except (ValueError, UnicodeError):
        return None
    if not isinstance(payload, dict) or payload.get("v") != 1:
        return None
    if not isinstance(payload.get("s"), str) or not payload["s"]:
        return None
    offset = payload.get("o")
    if isinstance(offset, bool) or not isinstance(offset, int) or offset < 0:
        return None
    if payload.get("r") not in ("all", "branch"):
        return None
    if payload.get("b") is not None and not isinstance(payload.get("b"), str):
        return None
    return payload


def _page_limit(value):
    if value is None:
        return DEFAULT_PAGE_LIMIT
    if isinstance(value, bool) or not isinstance(value, int):
        raise GitFailure("invalid_limit", "limit 必须是 1 到 %d 的整数" % MAX_PAGE_LIMIT)
    if value < 1:
        raise GitFailure("invalid_limit", "limit 必须大于 0")
    return min(value, MAX_PAGE_LIMIT)


def _resolve_branch(root, ref):
    if not isinstance(ref, str) or not ref.strip() or "\x00" in ref:
        raise GitFailure("invalid_request", "branch_ref 必须是非空字符串")
    if len(ref) > 1024:
        raise GitFailure("invalid_request", "branch_ref 过长")
    returncode, stdout, stderr = _git_bytes(
        ["rev-parse", "--verify", "--quiet", "--end-of-options",
         ref + "^{commit}"], cwd=root)
    if returncode in (-1, -2, -3):
        _raise_process_failure(returncode, stderr)
    candidate = stdout.decode("ascii", "replace").strip()
    if returncode != 0 or not _HEX_COMMIT.match(candidate):
        raise GitFailure("unknown_ref", "找不到分支或引用：%s" % ref)
    return candidate


def _resolve_commit(root, value):
    if not isinstance(value, str) or not _HEX_INPUT.match(value):
        raise GitFailure("invalid_oid", "oid 必须是 4 到 64 位十六进制提交 ID")
    returncode, stdout, stderr = _git_bytes(
        ["rev-parse", "--verify", "--quiet", "--end-of-options",
         value + "^{commit}"], cwd=root)
    if returncode in (-1, -2, -3):
        _raise_process_failure(returncode, stderr)
    candidate = stdout.decode("ascii", "replace").strip()
    if returncode != 0 or not _HEX_COMMIT.match(candidate):
        raise GitFailure("unknown_commit", "找不到提交：%s" % value)
    return candidate


def _ordered_oids(root, range_name, branch_oid, refs, head_oid):
    if range_name == "branch":
        roots = [branch_oid] if branch_oid else []
    else:
        roots = [ref["target_oid"] for ref in refs if ref["target_is_commit"]]
        if head_oid:
            roots.insert(0, head_oid)
    roots = [oid for oid in dict.fromkeys(roots) if _HEX_COMMIT.match(oid)]
    if not roots:
        return []
    payload = ("\n".join(roots) + "\n").encode("ascii")
    stdout = _run_git(
        ["rev-list", "--topo-order", "--max-count=%d" % (MAX_GRAPH_COMMITS + 1),
         "--stdin"], cwd=root, input_bytes=payload)
    oids = []
    for line in stdout.decode("ascii", "replace").splitlines():
        candidate = line.strip()
        if _HEX_COMMIT.match(candidate):
            oids.append(candidate)
    return oids


def _nul_fields(payload):
    fields = payload.split(b"\x00")
    if fields and fields[-1] == b"":
        fields.pop()
    return fields


def _drop_incomplete_tail(fields, truncated):
    if truncated and fields:
        return fields[:-1]
    return fields


def _commit_summaries(root, oids):
    if not oids:
        return []
    payload = ("\n".join(oids) + "\n").encode("ascii")
    stdout = _run_git(
        ["log", "-z", "--no-walk=unsorted", "--stdin",
         "--format=" + _COMMIT_SUMMARY_FORMAT], cwd=root, input_bytes=payload)
    fields = _nul_fields(stdout)
    if len(fields) % 6 != 0:
        raise GitFailure("git_failed", "无法解析 git log 输出")
    commits = {}
    for index in range(0, len(fields), 6):
        raw_oid, raw_parents, raw_subject, raw_author, raw_author_time, raw_committer_time = fields[index:index + 6]
        oid = raw_oid.decode("ascii", "replace").strip()
        parents = raw_parents.decode("ascii", "replace").strip()
        commits[oid] = {
            "oid": oid,
            "parents": parents.split() if parents else [],
            "subject": raw_subject.decode("utf-8", "replace"),
            "author_name": raw_author.decode("utf-8", "replace"),
            "author_time": raw_author_time.decode("ascii", "replace").strip(),
            "committer_time": raw_committer_time.decode("ascii", "replace").strip(),
        }
    return [commits[oid] for oid in oids if oid in commits]


def _read_commit_meta(root, oid):
    stdout = _run_git(
        ["show", "-s", "-z", "--format=" + _COMMIT_DETAIL_FORMAT, oid],
        cwd=root)
    fields = _nul_fields(stdout)
    if len(fields) < 10:
        raise GitFailure("git_failed", "无法解析提交详情")
    (raw_oid, raw_parents, raw_subject, raw_body, raw_author_name,
     raw_author_email, raw_author_time, raw_committer_name,
     raw_committer_email, raw_committer_time) = fields[:10]
    body = raw_body.decode("utf-8", "replace").strip()
    body_truncated = len(body) > MAX_MESSAGE_CHARS
    if body_truncated:
        body = body[:MAX_MESSAGE_CHARS]
    parents = raw_parents.decode("ascii", "replace").strip()
    return {
        "oid": raw_oid.decode("ascii", "replace").strip(),
        "parents": parents.split() if parents else [],
        "subject": raw_subject.decode("utf-8", "replace"),
        "body": body,
        "author_name": raw_author_name.decode("utf-8", "replace"),
        "author_email": raw_author_email.decode("utf-8", "replace"),
        "author_time": raw_author_time.decode("ascii", "replace").strip(),
        "committer_name": raw_committer_name.decode("utf-8", "replace"),
        "committer_email": raw_committer_email.decode("utf-8", "replace"),
        "committer_time": raw_committer_time.decode("ascii", "replace").strip(),
        "body_truncated": body_truncated,
    }


def _parse_name_status(payload, truncated):
    fields = _drop_incomplete_tail(_nul_fields(payload), truncated)
    entries = []
    index = 0
    while index < len(fields):
        status = fields[index].decode("ascii", "replace").strip()
        index += 1
        if not status:
            continue
        code = status[0].upper()
        if code in ("R", "C"):
            if index + 1 >= len(fields):
                break
            old_path = fields[index].decode("utf-8", "replace")
            new_path = fields[index + 1].decode("utf-8", "replace")
            index += 2
        else:
            if index >= len(fields):
                break
            old_path = None
            new_path = fields[index].decode("utf-8", "replace")
            index += 1
        entries.append({"path": new_path, "old_path": old_path, "status": code})
    return entries


def _parse_numstat(payload, truncated):
    fields = _drop_incomplete_tail(_nul_fields(payload), truncated)
    stats = {}
    index = 0
    while index < len(fields):
        header = fields[index]
        index += 1
        if not header:
            continue
        parts = header.split(b"\t")
        if len(parts) < 3:
            break
        additions, deletions, path = parts[0], parts[1], parts[2]
        if path == b"":
            if index + 1 >= len(fields):
                break
            new_path = fields[index + 1]
            index += 2
        else:
            new_path = path
        key = new_path.decode("utf-8", "replace")
        if additions == b"-" or deletions == b"-":
            stats[key] = (None, None, True)
        else:
            try:
                stats[key] = (int(additions), int(deletions), False)
            except ValueError:
                stats[key] = (None, None, False)
    return stats


def _select_parent(parents, args):
    """Pick the parent a commit detail/file diff compares against.

    Defaults to the first parent (root commits use the empty tree). The
    frontend may pass parent_index (0-based) or parent (a parent OID) for
    merge commits.
    """
    parent_oid = args.get("parent")
    if parent_oid is not None:
        if (not isinstance(parent_oid, str) or not _HEX_INPUT.match(parent_oid)):
            raise GitFailure("invalid_parent", "parent 必须是父提交的十六进制 ID")
        lowered = parent_oid.lower()
        for parent in parents:
            if parent.lower() == lowered:
                return parent
        raise GitFailure("unknown_parent", "该父提交不属于当前提交：%s" % parent_oid)
    parent_index = args.get("parent_index")
    if parent_index is None:
        return parents[0] if parents else None
    if isinstance(parent_index, bool) or not isinstance(parent_index, int):
        raise GitFailure("invalid_parent", "parent_index 必须是整数")
    if parent_index < 0 or parent_index >= len(parents):
        raise GitFailure("invalid_parent", "parent_index 超出父提交范围")
    return parents[parent_index]


def _read_commit_files(root, base, oid):
    if base:
        revisions = [base, oid]
        root_flag = []
    else:
        revisions = [oid]
        root_flag = ["--root"]
    status_args = (["diff-tree", "-r", "--no-commit-id", "-z", "-M",
                    "--name-status"] + root_flag + revisions)
    numstat_args = (["diff-tree", "-r", "--no-commit-id", "-z", "-M",
                     "--numstat"] + root_flag + revisions)
    status_payload, status_truncated = _run_git_limited(
        status_args, cwd=root, limit=MAX_FILELIST_BYTES)
    numstat_payload, numstat_truncated = _run_git_limited(
        numstat_args, cwd=root, limit=MAX_FILELIST_BYTES)
    entries = _parse_name_status(status_payload, status_truncated)
    stats = _parse_numstat(numstat_payload, numstat_truncated)
    files = []
    for entry in entries:
        additions, deletions, binary = stats.get(entry["path"], (0, 0, False))
        files.append({
            "path": entry["path"],
            "old_path": entry["old_path"],
            "status": entry["status"],
            "additions": additions,
            "deletions": deletions,
            "binary": binary,
        })
    truncated = (status_truncated or numstat_truncated
                 or len(files) > MAX_DETAIL_FILES)
    return files[:MAX_DETAIL_FILES], truncated


def _read_file_patch(root, base, oid, pathspecs):
    args = ["--literal-pathspecs", "diff-tree", "-r", "--no-commit-id",
            "-p", "--no-color", "--no-ext-diff", "-M"]
    if base:
        args += [base, oid]
    else:
        args += ["--root", oid]
    args += ["--"] + list(pathspecs)
    payload, truncated = _run_git_limited(
        args, cwd=root, limit=MAX_PATCH_BYTES)
    return payload.decode("utf-8", "replace"), truncated


def _do_history_graph(args):
    root = _repo_context(args.get("target_library", ""))
    head_oid, head_ref, detached = _read_head(root)
    refs, refs_truncated = _read_refs(root, head_ref)

    range_name = args.get("range")
    branch_ref = args.get("branch_ref")
    cursor_value = args.get("cursor")
    cursor = None
    if cursor_value is not None:
        if not isinstance(cursor_value, str):
            raise GitFailure("invalid_cursor", "cursor 必须是字符串")
        cursor = _decode_cursor(cursor_value)
        if cursor is None:
            raise GitFailure("invalid_cursor", "cursor 无效，请刷新历史后重试")
        if range_name is not None and range_name != cursor["r"]:
            raise GitFailure("stale_cursor", "查询参数已变化，请刷新历史")
        if branch_ref is not None and (branch_ref or None) != (cursor.get("b") or None):
            raise GitFailure("stale_cursor", "查询参数已变化，请刷新历史")
        range_name = cursor["r"]
        branch_ref = cursor.get("b")
    elif range_name is None:
        range_name = "all"

    if range_name not in ("all", "branch"):
        raise GitFailure("invalid_range", "range 只支持 'all' 或 'branch'")
    if range_name == "all":
        if branch_ref not in (None, ""):
            raise GitFailure("invalid_request", "range=all 时不能指定 branch_ref")
        branch_ref = None
    else:
        if branch_ref is not None:
            if not isinstance(branch_ref, str):
                raise GitFailure("invalid_request", "branch_ref 必须是字符串")
            branch_ref = branch_ref.strip() or None

    branch_oid = None
    if range_name == "branch":
        branch_oid = _resolve_branch(root, branch_ref) if branch_ref else head_oid

    snapshot = _graph_snapshot(root, range_name, branch_ref, branch_oid,
                               head_oid, head_ref, refs, refs_truncated)
    if cursor is not None and cursor["s"] != snapshot:
        raise GitFailure("stale_cursor", "仓库或引用已变化，请刷新历史后重新分页")

    limit = _page_limit(args.get("limit"))
    oids = _ordered_oids(root, range_name, branch_oid, refs, head_oid)
    visible = min(len(oids), MAX_GRAPH_COMMITS)
    offset = cursor["o"] if cursor is not None else 0
    start = min(offset, visible)
    end = min(start + limit, visible)
    page_oids = oids[start:end]
    has_more = end < visible
    next_cursor = None
    if has_more:
        next_cursor = _encode_cursor({
            "v": 1,
            "s": snapshot,
            "o": end,
            "r": range_name,
            "b": branch_ref,
        })
    return {
        "ok": True,
        "schema_version": GRAPH_SCHEMA,
        "repository_identity": root,
        "head_oid": head_oid,
        "head_ref": head_ref,
        "detached": detached,
        "state": "unborn" if head_oid is None else "ready",
        "refs": _public_refs(refs),
        "refs_truncated": refs_truncated,
        "commits": _commit_summaries(root, page_oids),
        "page": {
            "snapshot_id": snapshot,
            "next_cursor": next_cursor,
            "has_more": has_more,
        },
        "limit": limit,
        "total": visible,
        "truncated": len(oids) > MAX_GRAPH_COMMITS,
    }


def _do_commit_detail(args):
    root = _repo_context(args.get("target_library", ""))
    oid = _resolve_commit(root, args.get("oid"))
    meta = _read_commit_meta(root, oid)
    base = _select_parent(meta["parents"], args)
    files, files_truncated = _read_commit_files(root, base, oid)
    commit = {key: meta[key] for key in (
        "oid", "parents", "subject", "body", "author_name", "author_email",
        "author_time", "committer_name", "committer_email", "committer_time")}
    return {
        "ok": True,
        "schema_version": GRAPH_SCHEMA,
        "repository_identity": root,
        "commit": commit,
        "files": files,
        "files_truncated": files_truncated,
        "body_truncated": meta["body_truncated"],
        "diff_base": base,
    }


def _do_commit_file(args):
    root = _repo_context(args.get("target_library", ""))
    oid = _resolve_commit(root, args.get("oid"))
    path = args.get("path")
    if not isinstance(path, str) or not path or "\x00" in path or len(path) > 4096:
        raise GitFailure("invalid_request", "path 必须是非空字符串")
    meta = _read_commit_meta(root, oid)
    base = _select_parent(meta["parents"], args)
    files, files_truncated = _read_commit_files(root, base, oid)
    entry = None
    for candidate in files:
        if path == candidate["path"]:
            entry = candidate
            break
        if candidate["old_path"] and path == candidate["old_path"]:
            entry = candidate
            break
    if entry is None:
        hint = "（文件列表已截断）" if files_truncated else ""
        raise GitFailure("unknown_path", "该提交中没有文件：%s%s" % (path, hint))
    pathspecs = [entry["path"]]
    if entry["old_path"] and entry["old_path"] != entry["path"]:
        pathspecs.append(entry["old_path"])
    patch, truncated = _read_file_patch(root, base, oid, pathspecs)
    if truncated:
        patch += PATCH_TRUNCATED_MARKER.format(limit=MAX_PATCH_BYTES)
    binary = (entry["binary"] or "\nBinary files " in patch
              or patch.startswith("Binary files ")
              or "GIT binary patch" in patch)
    return {
        "ok": True,
        "schema_version": GRAPH_SCHEMA,
        "repository_identity": root,
        "oid": oid,
        "path": entry["path"],
        "old_path": entry["old_path"],
        "status": entry["status"],
        "diff_base": base,
        "patch": patch,
        "truncated": truncated,
        "binary": binary,
    }


def _graph_result(handler, args):
    try:
        if not isinstance(args, dict):
            raise GitFailure("invalid_request", "参数必须是 JSON 对象")
        return handler(args)
    except GitFailure as failure:
        return {"ok": False, "schema_version": GRAPH_SCHEMA,
                "code": failure.code, "error": failure.message}
    except Exception as error:  # keep the task result JSON even on a bug
        return {"ok": False, "schema_version": GRAPH_SCHEMA,
                "code": "internal_error",
                "error": "读取 Git 数据失败：%s" % error}


def action_history(args):
    if not isinstance(args, dict):
        return {"ok": False, "code": "invalid_request",
                "error": "参数必须是 JSON 对象"}
    fmt = args.get("format")
    if fmt in (None, "", "text", "legacy"):
        return do_history(args.get("target_library", ""))
    if fmt != GRAPH_SCHEMA:
        return {"ok": False, "code": "invalid_format",
                "error": "format 只支持 graph_v1（缺省保留旧版文本历史）"}
    return _graph_result(_do_history_graph, args)


def action_commit_detail(args):
    return _graph_result(_do_commit_detail, args)


def action_commit_file(args):
    return _graph_result(_do_commit_file, args)


# ---- CLI entry ----

ACTIONS = {
    "status": lambda args: do_status(args.get("target_library", "")),
    "init": lambda args: do_init(args.get("target_library", "")),
    "commit": lambda args: do_commit(
        args.get("target_library", ""),
        args.get("message", "KF: auto commit")),
    "history": action_history,
    "commit_detail": action_commit_detail,
    "commit_file": action_commit_file,
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

    try:
        result = fn(args)
    except KeyboardInterrupt:
        result = {"ok": False, "code": "cancelled", "error": "任务已取消"}
    print(json.dumps(result))