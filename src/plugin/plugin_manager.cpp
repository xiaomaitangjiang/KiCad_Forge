#include "plugin/plugin_manager.h"
#include "util/logger.h"
#include "util/platform.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace kforge::plugin
{
using Kind = util::Error::Kind;

namespace
{
// Action identifiers come from HTTP request bodies and are interpolated into
// the plugin command line — restrict to a safe alphabet. Defense in depth:
// routes.cpp also validates before calling execute().
bool is_valid_action(const std::string& action)
{
    return !action.empty() &&
           action.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                    "0123456789_-") == std::string::npos;
}

// Narrow a filesystem path for logging/messages without ever throwing: paths
// may hold characters the active narrow encoding cannot represent (MSVC's
// path::string() throws for those). Always returns UTF-8.
std::string path_display(const std::filesystem::path& p)
{
    try
    {
        auto utf8 = p.u8string();
        std::string out;
        out.reserve(utf8.size());
        for (auto c : utf8)
        {
            out.push_back(static_cast<char>(c));
        }
        return out;
    }
    catch (...)
    {
        return std::string("<path not representable>");
    }
}

/// The error returned when cancellation wins before the plugin's script has
/// run — shared so every cancel path reports the same wording.
util::Error cancelled_error()
{
    return util::Error::make<Kind::PluginError>(
        "Plugin execution cancelled: the plugin manager was stopped");
}

/// Bounded failure context for a plugin that exited non-zero. A plugin's own
/// timeout often aborts inside its script (Python raises TimeoutExpired), so the
/// action route can only classify the failure from the captured output tail.
/// Keep the tail compact and free of control bytes; drop a leading partial UTF-8
/// sequence so an appended message never becomes invalid UTF-8.
std::string plugin_failure_detail(const std::string& output, std::size_t limit = 1200)
{
    if (output.empty())
    {
        return {};
    }
    std::size_t begin = output.size() > limit ? output.size() - limit : 0;
    while (begin < output.size() &&
           (static_cast<unsigned char>(output[begin]) & 0xC0) == 0x80)
    {
        ++begin;
    }
    std::string detail;
    bool pending_space = false;
    for (std::size_t i = begin; i < output.size(); ++i)
    {
        const auto byte = static_cast<unsigned char>(output[i]);
        if (byte < 0x20 || byte == 0x7f)
        {
            pending_space = !detail.empty();
            continue;
        }
        if (pending_space)
        {
            detail.push_back(' ');
            pending_space = false;
        }
        detail.push_back(output[i]);
    }
    while (!detail.empty() && detail.back() == ' ')
    {
        detail.pop_back();
    }
    return detail;
}

/// Manifest strings (entry point, icon, …) are UTF-8 by contract. Constructing a
/// path from a narrow std::string would reinterpret those bytes through the
/// active ANSI code page on Windows, so non-ASCII entry file names must go
/// through char8_t — the one character type filesystem::path defines as UTF-8.
std::filesystem::path path_from_utf8(const std::string& utf8)
{
    if (utf8.empty())
    {
        return {};
    }
    const auto* bytes = reinterpret_cast<const char8_t*>(utf8.data());
    return std::filesystem::path(std::u8string_view(bytes, utf8.size()));
}

// ============================================================
// Frontend asset path validation (manifest fields + asset resolver)
// ============================================================

/// A frontend path is URL-style ("dist/assets/app.js"): '/'-separated and
/// relative to its base. Absolute paths, drive letters, backslashes (Windows
/// escape form), control characters and ""/"."/".." components are refused.
/// The same rule is applied to manifest fields at parse time and to request
/// paths at resolve time, so a frontend can never point outside its root.
bool is_safe_relative_asset_path(const std::string& path)
{
    if (path.empty())
    {
        return false;
    }
    for (unsigned char c : path)
    {
        if (c < 0x20 || c == 0x7f)
        {
            return false;  // control characters (incl. NUL)
        }
        if (c == '\\' || c == ':')
        {
            return false;  // Windows separator, drive letter, alternate data stream
        }
    }
    if (path.front() == '/')
    {
        return false;  // absolute POSIX path
    }
    std::size_t start = 0;
    for (;;)
    {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string::npos ? path.size() : slash;
        const auto length = end - start;
        if (length == 0)
        {
            return false;  // empty component: "", "//", leading/trailing '/'
        }
        if ((length == 1 && path[start] == '.') ||
            (length == 2 && path.compare(start, 2, "..") == 0))
        {
            return false;  // "."/".." component traversal
        }
        if (slash == std::string::npos)
        {
            return true;
        }
        start = slash + 1;
    }
}

/// True when the path itself is a symlink or any other reparse point. Windows
/// directory junctions are reparse points but are NOT reported by
/// std::filesystem::is_symlink, hence the attribute check. symlink_status() is
/// used everywhere so a link is never silently followed.
bool is_link_like(const std::filesystem::path& path)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return true;
    }
#endif
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
}

bool same_path_component(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
{
#ifdef _WIN32
    // Windows paths are case-insensitive: canonical() may return the on-disk
    // casing, which need not match the casing used to build the root.
    return ::CompareStringOrdinal(lhs.native().c_str(), -1, rhs.native().c_str(), -1, TRUE) ==
           CSTR_EQUAL;
#else
    return lhs == rhs;
#endif
}

/// True when `candidate` is strictly below `root`, compared component-wise.
/// Both paths must be absolute and canonical.
bool is_strictly_below(const std::filesystem::path& root,
                       const std::filesystem::path& candidate)
{
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it)
    {
        if (candidate_it == candidate.end() || !same_path_component(*root_it, *candidate_it))
        {
            return false;
        }
    }
    return candidate_it != candidate.end();  // the root itself is not an asset
}

/// Resolve `base/relative` as a directory that stays inside `base`: every
/// component is checked with no-follow status (a symlink/junction is refused
/// even when it points back inside), and the canonical result must be strictly
/// below the canonical base. Missing components are NotFound; links, escapes
/// and non-directories are ValidationError. Filesystem exceptions propagate to
/// the caller, which owns the final try/catch.
util::Result<std::filesystem::path> resolve_contained_directory(
    const std::filesystem::path& base, const std::string& relative)
{
    std::error_code error;
    const auto canonical_base = std::filesystem::weakly_canonical(base);
    if (!std::filesystem::is_directory(canonical_base, error) || error)
    {
        return std::unexpected(util::Error::make<Kind::NotFound>(
            "Plugin directory not found: " + path_display(canonical_base)));
    }

    auto current = canonical_base;
    for (const auto& component : path_from_utf8(relative))
    {
        current /= component;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error || !std::filesystem::exists(status))
        {
            return std::unexpected(util::Error::make<Kind::NotFound>(
                "Frontend root not found: " + path_display(current)));
        }
        if (is_link_like(current))
        {
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                "Frontend root traverses a link: \"" + relative + "\""));
        }
        if (!std::filesystem::is_directory(status))
        {
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                "Frontend root is not a directory: \"" + relative + "\""));
        }
    }

    const auto resolved = std::filesystem::canonical(current, error);
    if (error || resolved.empty())
    {
        return std::unexpected(util::Error::make<Kind::NotFound>(
            "Frontend root not found: " + path_display(current)));
    }
    if (!is_strictly_below(canonical_base, resolved))
    {
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "Frontend root escapes the plugin directory: \"" + relative + "\""));
    }
    return resolved;
}

// Result of one non-blocking read attempt on a plugin's output pipe.
enum class ChunkResult
{
    Data,        // bytes were appended to the output buffer
    Empty,       // nothing buffered right now
    PipeClosed,  // every writer closed the pipe (EOF) — NOT the same as "process exited"
    Overflow,    // the configured output cap would be exceeded
};

#ifdef _WIN32
// ============================================================
// Windows process helpers
// ============================================================

/// CloseHandle RAII. It is never used to wrap a handle handed to another owner.
struct HandleGuard
{
    HANDLE handle{nullptr};
    HandleGuard() = default;
    explicit HandleGuard(HANDLE h) : handle(h) {}
    ~HandleGuard()
    {
        reset();
    }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    void reset(HANDLE h = nullptr)
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
        handle = h;
    }
    [[nodiscard]] bool valid() const
    {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    }
};

/// Storage for a PROC_THREAD_ATTRIBUTE_LIST plus its required teardown.
///
/// DeleteProcThreadAttributeList must only ever receive a list that
/// InitializeProcThreadAttributeList successfully created, so `list` is left
/// null until that call succeeded (resize() may reallocate `storage`, which
/// would invalidate any pointer published before it).
struct AttrListGuard
{
    std::vector<char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list{nullptr};
    AttrListGuard() = default;
    AttrListGuard(const AttrListGuard&) = delete;
    AttrListGuard& operator=(const AttrListGuard&) = delete;
    ~AttrListGuard()
    {
        if (list != nullptr)
        {
            DeleteProcThreadAttributeList(list);
        }
    }
};

/// UTF-8 → UTF-16 for process arguments. Returns false on malformed input so the
/// caller reports an error instead of mangling the argument.
bool utf8_to_wide(const std::string& utf8, std::wstring& out)
{
    out.clear();
    if (utf8.empty())
    {
        return true;
    }
    if (utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }
    const int len = static_cast<int>(utf8.size());
    const int need =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), len, nullptr, 0);
    if (need <= 0)
    {
        return false;
    }
    out.resize(static_cast<size_t>(need));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), len, out.data(),
                               need) == need;
}

// CommandLineToArgvW-compatible quoting: a run of N backslashes before a quote
// is doubled (so the child parses them back correctly), and a trailing run of
// backslashes is doubled before the closing quote.
std::wstring quote_win_arg(const std::wstring& arg)
{
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg)
    {
        if (c == L'\\')
        {
            backslashes++;
        }
        else if (c == L'"')
        {
            out.append((backslashes * 2) + 1, L'\\');
            out += L'"';
            backslashes = 0;
        }
        else
        {
            out.append(backslashes, L'\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}
#endif  // _WIN32

/// Run one plugin subprocess with hard bounds.
///
/// Guarantees, on every return path: the direct child is terminated and reaped
/// before returning, no descendant keeps running (Job Object on Windows,
/// process group on POSIX), and the output never exceeds
/// options.max_output_bytes — exceeding it is an error, never a silent
/// truncation. `cancelled()` is polled on every loop iteration together with the
/// deadline, so flooding, silence, EOF and a closed pipe are all bounded.
util::Result<std::string> run_plugin_process(const std::filesystem::path& script_path,
                                             const std::string& action,
                                             const std::string& json_args,
                                             const ExecutionOptions& options,
                                             const std::function<bool()>& cancelled)
{
    using Kind = util::Error::Kind;
    const std::size_t cap = options.max_output_bytes;
    std::string output;

#ifdef _WIN32
    // UTF-8 → UTF-16: the script path goes wide natively, the JSON arguments
    // (Chinese text, quotes, backslashes) are converted explicitly. No ANSI code
    // page is involved anywhere on this path.
    const std::wstring script_w = script_path.wstring();
    std::wstring action_w;
    std::wstring args_w;
    if (!utf8_to_wide(action, action_w) || !utf8_to_wide(json_args, args_w))
    {
        return std::unexpected(
            util::Error::make<Kind::ValidationError>("Plugin arguments are not valid UTF-8"));
    }
    std::wstring command = L"python -X utf8 " + quote_win_arg(script_w) + L" " + quote_win_arg(action_w) +
                           L" " + quote_win_arg(args_w);
    kforge::util::log_debug{}("[plugin] Execute: python {} {} {}", path_display(script_path),
                              action, json_args);
    std::vector<wchar_t> cmdline(command.begin(), command.end());
    cmdline.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_raw = nullptr;
    HANDLE write_raw = nullptr;
    if (!CreatePipe(&read_raw, &write_raw, &sa, 0))
    {
        return std::unexpected(util::Error::make<Kind::IoError>("Failed to create pipe"));
    }
    HandleGuard hRead(read_raw);
    HandleGuard hWrite(write_raw);
    SetHandleInformation(hRead.handle, HANDLE_FLAG_INHERIT, 0);

    // Only this execution's pipe write end is inheritable: with
    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST a concurrently running plugin can no
    // longer inherit — and keep alive — somebody else's pipe.
    AttrListGuard attrs;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    if (attr_size == 0)
    {
        return std::unexpected(
            util::Error::make<Kind::IoError>("Failed to build process attributes"));
    }
    attrs.storage.resize(attr_size);
    auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.storage.data());
    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size))
    {
        // `attr_list` was never initialized: publishing it would make the guard
        // Delete an uninitialized list — leak the storage instead.
        return std::unexpected(
            util::Error::make<Kind::IoError>("Failed to build process attributes"));
    }
    attrs.list = attr_list;  // only initialized lists reach DeleteProcThreadAttributeList
    HANDLE inherited[] = {hWrite.handle};
    if (!UpdateProcThreadAttribute(attrs.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), nullptr, nullptr))
    {
        return std::unexpected(
            util::Error::make<Kind::IoError>("Failed to restrict inherited handles"));
    }

    // Kill-on-close job: closing the handle terminates the plugin and everything
    // it spawned. The child is created suspended, assigned to the job, then
    // resumed — so it cannot spawn a child that escapes the job.
    // Without a working kill-on-close job there is no way to bound descendants
    // (a plugin that spawns a helper would keep running past this call), so a
    // job failure is a hard error *before* anything is launched — never a warn
    // followed by a run with a weakened guarantee.
    HandleGuard job(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid())
    {
        return std::unexpected(
            util::Error::make<Kind::IoError>("Failed to create job object for plugin process"));
    }
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.handle, JobObjectExtendedLimitInformation, &info,
                                     sizeof(info)))
        {
            return std::unexpected(util::Error::make<Kind::IoError>(
                "Failed to configure job object for plugin process"));
        }
    }

    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(siex);
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    siex.StartupInfo.hStdOutput = hWrite.handle;
    siex.StartupInfo.hStdError = hWrite.handle;
    siex.StartupInfo.wShowWindow = SW_HIDE;
    siex.lpAttributeList = attrs.list;

    const auto cancelled_now = [&cancelled]() { return cancelled && cancelled(); };

    // Cancellation observed before the child exists: nothing to start, nothing
    // to clean up — the user's script must not run.
    if (cancelled_now())
    {
        return std::unexpected(cancelled_error());
    }

    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                        &siex.StartupInfo, &pi))
    {
        return std::unexpected(util::Error::make<Kind::IoError>("Failed to launch plugin"));
    }
    HandleGuard hProcess(pi.hProcess);
    HandleGuard hThread(pi.hThread);
    hWrite.reset();  // the child owns the write end now

    if (!AssignProcessToJobObject(job.handle, hProcess.handle))
    {
        // Never leak a process the job refused: the child is still suspended, so
        // terminate + reap it before reporting.
        TerminateProcess(hProcess.handle, 1);
        WaitForSingleObject(hProcess.handle, INFINITE);
        return std::unexpected(
            util::Error::make<Kind::IoError>("Failed to assign plugin process to job object"));
    }
    // Cancellation observed after the assignment but before the first
    // instruction: the child has never run user code, so terminate + reap here
    // instead of resuming it (job kill-on-close reaps any descendant, of which
    // there cannot be any).
    if (cancelled_now())
    {
        TerminateProcess(hProcess.handle, 1);
        WaitForSingleObject(hProcess.handle, INFINITE);
        hThread.reset();
        job.reset();
        return std::unexpected(cancelled_error());
    }
    if (ResumeThread(hThread.handle) == static_cast<DWORD>(-1))
    {
        // The child never ran: terminate + reap so a suspended process can never
        // be leaked, and report instead of waiting out the 120 s deadline.
        TerminateProcess(hProcess.handle, 1);
        WaitForSingleObject(hProcess.handle, INFINITE);
        hThread.reset();
        job.reset();
        return std::unexpected(
            util::Error::make<Kind::IoError>("Failed to resume plugin process"));
    }
    hThread.reset();

    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    char buf[4096];
    bool pipe_open = true;
    bool exited = false;
    bool overflow = false;
    bool timed_out = false;
    bool cancel = false;
    DWORD exit_code = 0;

    auto read_chunk = [&]() -> ChunkResult
    {
        if (!pipe_open)
        {
            return ChunkResult::PipeClosed;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(hRead.handle, nullptr, 0, nullptr, &avail, nullptr))
        {
            pipe_open = false;  // broken pipe = EOF; the process may still be alive
            return ChunkResult::PipeClosed;
        }
        if (avail == 0)
        {
            return ChunkResult::Empty;
        }
        const DWORD want = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
        DWORD got = 0;
        if (!ReadFile(hRead.handle, buf, want, &got, nullptr) || got == 0)
        {
            pipe_open = false;
            return ChunkResult::PipeClosed;
        }
        if (output.size() + got > cap)
        {
            return ChunkResult::Overflow;
        }
        output.append(buf, got);
        return ChunkResult::Data;
    };

    for (;;)
    {
        // Cancellation and the deadline are re-checked first on every pass, so
        // no branch below (flood / silence / EOF) can run unbounded.
        if (cancelled())
        {
            cancel = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            timed_out = true;
            break;
        }
        const ChunkResult chunk = read_chunk();
        if (chunk == ChunkResult::Overflow)
        {
            overflow = true;
            break;
        }
        const bool progressed = (chunk == ChunkResult::Data);

        if (!exited && WaitForSingleObject(hProcess.handle, 0) == WAIT_OBJECT_0)
        {
            // STILL_ACTIVE is never treated as an exit.
            if (GetExitCodeProcess(hProcess.handle, &exit_code) && exit_code != STILL_ACTIVE)
            {
                exited = true;
            }
        }
        if (exited)
        {
            break;  // tail drain happens below, after descendants are converged
        }
        if (!progressed)
        {
            Sleep(20);
        }
    }

    // Single exit path: kill first (direct child, then the job so no descendant
    // survives the call), reap after — only then drain the bytes already
    // buffered in the pipe. Killing the descendants before draining is what
    // keeps the drain finite: a plugin that spawns a writer can no longer keep
    // feeding the pipe. Cancel and the deadline are still re-checked on every
    // drain pass, so even a stuck writer cannot make this loop unbounded.
    if (!exited)
    {
        TerminateProcess(hProcess.handle, 1);
        WaitForSingleObject(hProcess.handle, INFINITE);
        GetExitCodeProcess(hProcess.handle, &exit_code);
        exited = true;
    }
    TerminateJobObject(job.handle, 0);
    job.reset();
    for (;;)
    {
        if (cancelled_now())
        {
            cancel = true;
            break;
        }
        const ChunkResult tail = read_chunk();
        if (tail == ChunkResult::Overflow)
        {
            overflow = true;
            break;
        }
        if (tail != ChunkResult::Data)
        {
            break;
        }
        // Data kept arriving: the deadline is re-checked only here, so a drain
        // that completes normally is never reported as a timeout, while a writer
        // that keeps the pipe fed cannot spin past the deadline.
        if (std::chrono::steady_clock::now() >= deadline)
        {
            timed_out = true;
            break;
        }
    }
#else
    // ---- POSIX: fork/exec into its own process group ----
    kforge::util::log_debug{}("[plugin] Execute: python {} {} {}", path_display(script_path),
                              action, json_args);
    int fds[2];
    if (pipe(fds) != 0)
    {
        return std::unexpected(util::Error::make<Kind::IoError>("Failed to create pipe"));
    }
    struct FdGuard
    {
        int fd{-1};
        explicit FdGuard(int raw_fd) : fd(raw_fd) {}
        ~FdGuard()
        {
            close_now();
        }
        FdGuard(const FdGuard&) = delete;
        FdGuard& operator=(const FdGuard&) = delete;
        void close_now()
        {
            if (fd >= 0)
            {
                ::close(fd);
                fd = -1;
            }
        }
    } read_guard{fds[0]}, write_guard{fds[1]};

    const pid_t pid = fork();
    if (pid < 0)
    {
        return std::unexpected(util::Error::make<Kind::IoError>("Failed to fork"));
    }
    if (pid == 0)
    {
        // Own process group: kill(-pid) reaches grandchildren as well.
        setpgid(0, 0);
        dup2(write_guard.fd, STDOUT_FILENO);
        dup2(write_guard.fd, STDERR_FILENO);
        read_guard.close_now();
        write_guard.close_now();
        execlp("python", "python", "-X", "utf8", script_path.string().c_str(), action.c_str(),
               json_args.c_str(), static_cast<char*>(nullptr));
        _exit(127);  // exec failed
    }
    setpgid(pid, pid);  // race-free: whichever side wins, the group is the child's
    write_guard.close_now();

    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    char buf[4096];
    bool pipe_open = true;
    bool exited = false;
    bool status_known = false;  // `status` holds a real wait status
    bool overflow = false;
    bool timed_out = false;
    bool cancel = false;
    int status = 0;

    // Unwinding (e.g. output.append throwing) must not orphan the child or its
    // process group: this local guard converges them on *every* path, including
    // exceptions, and is disarmed once the explicit teardown below has run.
    struct ChildGuard
    {
        pid_t pid{-1};
        bool armed{true};
        bool reaped{false};  // the direct child already has a wait status
        ~ChildGuard()
        {
            if (!armed || pid <= 1)
            {
                return;
            }
            ::kill(-pid, SIGKILL);  // group first: descendants before the leader
            if (!reaped)
            {
                ::kill(pid, SIGKILL);
            }
            int st = 0;
            pid_t w;
            do
            {
                w = ::waitpid(pid, &st, 0);
            } while (w < 0 && errno == EINTR);
        }
    } child_guard{pid};

    auto read_chunk = [&]() -> ChunkResult
    {
        if (!pipe_open)
        {
            return ChunkResult::PipeClosed;
        }
        struct pollfd pfd
        {
            read_guard.fd, POLLIN, 0
        };
        const int pr = ::poll(&pfd, 1, 0);
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                return ChunkResult::Empty;
            }
            pipe_open = false;
            return ChunkResult::PipeClosed;
        }
        if (pr == 0 || !(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
        {
            return ChunkResult::Empty;
        }
        const ssize_t nr = ::read(read_guard.fd, buf, sizeof(buf));
        if (nr > 0)
        {
            if (output.size() + static_cast<std::size_t>(nr) > cap)
            {
                return ChunkResult::Overflow;
            }
            output.append(buf, static_cast<std::size_t>(nr));
            return ChunkResult::Data;
        }
        if (nr == 0)
        {
            pipe_open = false;  // EOF: all writers gone, process may still run
            return ChunkResult::PipeClosed;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return ChunkResult::Empty;
        }
        pipe_open = false;
        return ChunkResult::PipeClosed;
    };

    auto child_exited = [&]() -> bool
    {
        if (exited)
        {
            return true;
        }
        for (;;)
        {
            int st = 0;
            const pid_t w = ::waitpid(pid, &st, WNOHANG);
            if (w == pid)
            {
                status = st;
                status_known = true;
                exited = true;
                child_guard.reaped = true;
                return true;
            }
            if (w == 0)
            {
                return false;  // still running
            }
            if (errno == EINTR)
            {
                continue;  // interrupted query: retry — never an exit
            }
            // ECHILD: the child was reaped elsewhere, so its exit status is
            // unknown — and unknown must never default to "exited 0". Any other
            // error is a failed query and fails the call as well.
            exited = true;
            status_known = false;
            child_guard.reaped = true;  // nothing left to wait for either way
            return true;
        }
    };

    for (;;)
    {
        if (cancelled())
        {
            cancel = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            timed_out = true;
            break;
        }
        const ChunkResult chunk = read_chunk();
        if (chunk == ChunkResult::Overflow)
        {
            overflow = true;
            break;
        }
        const bool progressed = (chunk == ChunkResult::Data);

        if (child_exited())
        {
            break;  // tail drain happens below, after the group is converged
        }
        if (!progressed)
        {
            ::usleep(20000);
        }
    }

    // Kill first, reap after (that order may block on the OS, which is fine);
    // a live child never leaves this function. Success included: no descendant
    // may keep running — or keep the pipe open — after this call returns.
    if (pid > 1 && !exited)
    {
        ::kill(pid, SIGKILL);  // direct child, in case setpgid never took effect
    }
    if (pid > 1)
    {
        ::kill(-pid, SIGKILL);  // whole group, descendants included
    }
    if (!exited)
    {
        for (int i = 0; !exited && i < 500; ++i)
        {
            if (child_exited())
            {
                break;
            }
            ::usleep(10000);
        }
    }
    if (!exited)
    {
        // SIGKILL was delivered — bounded in practice. Retry EINTR so no zombie
        // survives; a failed reap leaves the status unknown, never "0".
        int st = 0;
        pid_t w;
        do
        {
            w = ::waitpid(pid, &st, 0);
        } while (w < 0 && errno == EINTR);
        if (w == pid)
        {
            status = st;
            status_known = true;
        }
        exited = true;  // failed reap: status stays unknown → explicit failure
        child_guard.reaped = true;
    }

    // Drain whatever the pipe already buffered, now that no writer is left: the
    // loop is finite because every writer is dead, and cancel/deadline are still
    // re-checked so even a surviving writer cannot make it unbounded.
    for (;;)
    {
        if (cancelled && cancelled())
        {
            cancel = true;
            break;
        }
        const ChunkResult tail = read_chunk();
        if (tail == ChunkResult::Overflow)
        {
            overflow = true;
            break;
        }
        if (tail != ChunkResult::Data)
        {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            timed_out = true;
            break;
        }
    }

    if (pid > 1)
    {
        for (int i = 0; i < 100; ++i)
        {
            if (::kill(-pid, 0) != 0 && errno == ESRCH)
            {
                break;  // group fully gone
            }
            ::usleep(5000);
        }
    }
    child_guard.armed = false;  // child + group already converged above
    const int rc = status_known && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    const bool exited_zero = status_known && WIFEXITED(status) && rc == 0;
#endif  // _WIN32

    if (cancel)
    {
        return std::unexpected(cancelled_error());
    }
    if (timed_out)
    {
        return std::unexpected(
            util::Error::make<Kind::IoError>("Plugin timed out after " +
                                             std::to_string(options.timeout.count()) +
                                             " ms (process terminated)"));
    }
    if (overflow)
    {
        return std::unexpected(util::Error::make<Kind::IoError>(
            "Plugin output exceeded the " + std::to_string(cap) +
            " byte limit (process terminated)"));
    }
#ifdef _WIN32
    if (!exited || exit_code != 0)
    {
        const std::string detail = plugin_failure_detail(output);
        return std::unexpected(util::Error::make<Kind::IoError>(
            "Plugin exited with code " + std::to_string(exit_code) +
            (detail.empty() ? std::string() : ": " + detail)));
    }
#else
    if (!status_known)
    {
        // The child is gone but its exit status was never observed (reaped
        // elsewhere / waitpid failed). Success is not inferable from that.
        return std::unexpected(util::Error::make<Kind::IoError>(
            "Plugin exit status could not be determined"));
    }
    if (!exited_zero)
    {
        const std::string detail = plugin_failure_detail(output);
        return std::unexpected(util::Error::make<Kind::IoError>(
            "Plugin exited with code " + std::to_string(rc) +
            (detail.empty() ? std::string() : ": " + detail)));
    }
#endif
    return output;
}

// Standard search paths: bundled plugins next to the exe, plus user plugins
// (portable mode: <exe_dir>/plugins; installed: %APPDATA%/KiCad_Forge/plugins).
std::vector<std::filesystem::path> default_search_paths()
{
    std::vector<std::filesystem::path> paths;
    paths.reserve(2);  // bundled + user, at most
    auto bundled = kforge::util::get_exe_dir() / "plugins";
    if (std::filesystem::exists(bundled))
    {
        paths.emplace_back(bundled);
    }
    auto user_plugins = std::filesystem::path(kforge::util::get_data_dir()) / ".." / "plugins";
    auto canonical = std::filesystem::weakly_canonical(user_plugins);
    if (std::filesystem::exists(canonical) && canonical != std::filesystem::weakly_canonical(bundled))
    {
        paths.emplace_back(canonical);
    }
    return paths;
}
}  // namespace

PluginManager::PluginManager(const std::vector<std::filesystem::path>& paths,
                             const ExecutionOptions& execution_options)
    : execution_options_(execution_options), search_paths_(paths)
{
}

PluginManager::~PluginManager()
{
    shutdown_all();
}

void PluginManager::set_api_port(int port)
{
    api_port_.store(port, std::memory_order_relaxed);
    if (port <= 0)
    {
        return;  // no host API — plugins fall back to standalone mode
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(port);
#ifdef _WIN32
    _putenv_s("KFORGE_API_URL", url.c_str());  // inherited by plugin subprocesses
#else
    setenv("KFORGE_API_URL", url.c_str(), 1);
#endif
    kforge::util::log_info{}("[plugin] Host API exposed to plugins: {}", url);
}

std::unique_ptr<PluginManager> PluginManager::create_default()
{
    auto mgr = std::make_unique<PluginManager>(default_search_paths());
    mgr->discover();
    int loaded = 0;
    for (auto& m : mgr->available_plugins())
    {
        if (!m.one_click)
        {
            continue;
        }
        auto res = mgr->load(m.id, nullptr);
        if (res)
        {
            loaded++;
        }
        else
        {
            kforge::util::log_warn{}("[plugin] Auto-load failed for {}: {}", m.id,
                                     res.error().format_message());
        }
    }
    kforge::util::log_info{}("Plugins: {} available, {} loaded", mgr->available_plugins().size(),
                             loaded);
    return mgr;
}

// ============================================================
// Discovery — scan for manifest.json in plugin directories
// ============================================================

void PluginManager::discover()
{
    std::lock_guard lock(mutex_);
    if (stopped_.load(std::memory_order_acquire))
    {
        return;  // a stopped manager never revives — not even via discovery
    }
    available_.clear();
    plugin_dirs_.clear();
    diagnostics_.clear();  // a rescan recomputes diagnostics instead of accumulating

    // Record a failure for one bundle. The manifest id is recovered from the
    // raw document when possible; if even the JSON is broken the bundle
    // directory name identifies the plugin (the id is unknowable then).
    const auto record_manifest_failure = [this](const util::Error& error,
                                                const std::filesystem::path& manifest_path,
                                                const std::filesystem::path& bundle_dir)
    {
        const std::string message = error.message();
        const bool unsupported_version =
            message.rfind("unsupported manifestVersion", 0) == 0;
        std::string plugin_id;
        try
        {
            std::ifstream manifest_file(manifest_path);
            const auto doc = nlohmann::json::parse(manifest_file);
            plugin_id = doc.value("id", "");
        }
        catch (...)
        {
            // Unparseable manifest: fall back to the directory name below.
        }
        if (plugin_id.empty())
        {
            plugin_id = path_display(bundle_dir.filename());
        }
        diagnostics_.push_back(
            {unsupported_version ? "manifest/unsupported-version" : "manifest/invalid", plugin_id,
             error.format_message()});
    };

    for (auto& base : search_paths_)
    {
        if (!std::filesystem::exists(base))
            continue;
        for (const auto& entry : std::filesystem::directory_iterator(base))
        {
            if (!entry.is_directory())
                continue;
            auto manifest_path = entry.path() / "manifest.json";
            if (!std::filesystem::exists(manifest_path))
                continue;

            // One bad manifest must never abort discovery of the others —
            // skip it with a warning/diagnostic and continue.
            try
            {
                // filesystem::path goes straight to the parser: non-ASCII
                // plugin directories survive on Windows (no ANSI narrowing).
                auto m = PluginManifest::from_file(manifest_path);
                if (m && !m->id.empty() && !m->name.empty())
                {
                    auto id = m->id;
                    // 先扫描到的路径优先（内置插件目录在前）：同名 ID 的用户插件
                    // 不能顶掉内置项 —— 声明宿主协议的 action 只认受控来源
                    if (available_.contains(id))
                    {
                        diagnostics_.push_back(
                            {"plugin/duplicate-id", id,
                             "Duplicate plugin id " + id + " at " + path_display(entry.path()) +
                                 " (already discovered at " + path_display(plugin_dirs_[id]) +
                                 ")"});
                        kforge::util::log_warn{}(
                            "[plugin] Ignoring duplicate plugin id {} at {} (already loaded from {})",
                            id, path_display(entry.path()), path_display(plugin_dirs_[id]));
                        continue;
                    }
                    kforge::util::log_info{}("[plugin] Discovered: {} v{} ({}) at {}", m->name,
                                             m->version, id, path_display(entry.path()));
                    available_[id] = std::move(*m);
                    plugin_dirs_[id] = entry.path();
                }
                else if (!m)
                {
                    record_manifest_failure(m.error(), manifest_path, entry.path());
                    kforge::util::log_warn{}("[plugin] Skipping {}: {}",
                                             path_display(entry.path()),
                                             m.error().format_message());
                }
            }
            catch (const std::exception& e)
            {
                diagnostics_.push_back({"manifest/invalid",
                                        path_display(entry.path().filename()),
                                        "Malformed manifest: " + std::string(e.what())});
                kforge::util::log_warn{}("[plugin] Skipping {}: malformed manifest ({})",
                                         path_display(entry.path()), e.what());
            }
        }
    }
}

// ============================================================
// Lifecycle
// ============================================================

std::vector<PluginManifest> PluginManager::available_plugins() const
{
    std::lock_guard lock(mutex_);
    std::vector<PluginManifest> result;
    for (auto& [id, m] : available_)
        result.push_back(m);
    return result;
}

std::vector<PluginDiagnostic> PluginManager::diagnostics() const
{
    std::lock_guard lock(mutex_);
    return diagnostics_;
}

std::vector<PluginManifest> PluginManager::loaded_plugins() const
{
    std::lock_guard lock(mutex_);
    std::vector<PluginManifest> result;
    for (auto& [id, lp] : loaded_)
        result.push_back(lp.manifest);
    return result;
}

bool PluginManager::is_loaded(const std::string& plugin_id) const
{
    std::lock_guard lock(mutex_);
    return loaded_.count(plugin_id) > 0;
}

size_t PluginManager::count() const
{
    std::lock_guard lock(mutex_);
    return loaded_.size();
}

std::filesystem::path PluginManager::plugin_path(const std::string& plugin_id) const
{
    std::lock_guard lock(mutex_);
    // Check loaded first, then discovered
    auto lit = loaded_.find(plugin_id);
    if (lit != loaded_.end())
        return lit->second.plugin_dir;
    auto dit = plugin_dirs_.find(plugin_id);
    if (dit != plugin_dirs_.end())
        return dit->second;
    return {};
}

util::Result<std::filesystem::path> PluginManager::resolve_frontend_asset(
    const std::string& plugin_id, const std::string& relative) const
{
    // Copy the metadata under the lock, then walk the filesystem without it:
    // asset requests must not stall discovery/load/execute.
    std::filesystem::path plugin_dir;
    PluginFrontend frontend;
    {
        std::lock_guard lock(mutex_);
        const auto dirs = plugin_dirs_.find(plugin_id);
        const auto manifests = available_.find(plugin_id);
        if (dirs == plugin_dirs_.end() || manifests == available_.end())
        {
            return std::unexpected(
                util::Error::make<Kind::NotFound>("Plugin not found: " + plugin_id));
        }
        if (!manifests->second.frontend)
        {
            return std::unexpected(
                util::Error::make<Kind::NotFound>("Plugin has no frontend: " + plugin_id));
        }
        plugin_dir = dirs->second;
        frontend = *manifests->second.frontend;
    }

    if (!is_safe_relative_asset_path(relative))
    {
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "Invalid frontend asset path: \"" + relative + "\""));
    }

    try
    {
        // The frontend root itself is manifest-controlled: it must be a real
        // directory inside the plugin directory (no symlink/junction escape),
        // and containment below is anchored at its canonical path.
        auto root_result = resolve_contained_directory(plugin_dir, frontend.root);
        if (!root_result)
        {
            return std::unexpected(root_result.error());
        }
        const std::filesystem::path root = std::move(*root_result);
        std::error_code error;

        // Walk one component at a time using no-follow status: no component may
        // be a symlink/junction, intermediate ones must be directories, and the
        // leaf must be a regular file.
        const std::filesystem::path relative_path = path_from_utf8(relative);
        std::vector<std::filesystem::path> components(relative_path.begin(), relative_path.end());
        if (components.empty())
        {
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                "Invalid frontend asset path: \"" + relative + "\""));
        }
        std::filesystem::path current = root;
        for (std::size_t i = 0; i < components.size(); ++i)
        {
            current /= components[i];
            const auto status = std::filesystem::symlink_status(current, error);
            if (error || !std::filesystem::exists(status))
            {
                return std::unexpected(util::Error::make<Kind::NotFound>(
                    "Frontend asset not found: " + path_display(current)));
            }
            if (is_link_like(current))
            {
                return std::unexpected(util::Error::make<Kind::ValidationError>(
                    "Frontend asset path traverses a link: \"" + relative + "\""));
            }
            const bool leaf = (i + 1 == components.size());
            if (leaf ? !std::filesystem::is_regular_file(status)
                     : !std::filesystem::is_directory(status))
            {
                return std::unexpected(util::Error::make<Kind::ValidationError>(
                    leaf ? "Frontend asset is not a regular file: \"" + relative + "\""
                         : "Frontend asset path is not a directory: \"" + relative + "\""));
            }
        }

        // Defense in depth: the canonical leaf must stay strictly below the
        // canonical root even if a future change weakens the walk above.
        const std::filesystem::path resolved = std::filesystem::canonical(current, error);
        if (error || resolved.empty())
        {
            return std::unexpected(util::Error::make<Kind::NotFound>(
                "Frontend asset not found: " + path_display(current)));
        }
        if (!is_strictly_below(root, resolved))
        {
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                "Frontend asset escapes the frontend root: \"" + relative + "\""));
        }
        return resolved;
    }
    catch (const std::exception& e)
    {
        // Invalid UTF-8 bytes in the request path surface here (path_from_utf8
        // conversion) — report an error instead of unwinding into the HTTP layer.
        return std::unexpected(util::Error::make<Kind::IoError>(
            "Cannot resolve frontend asset \"" + relative + "\": " + e.what()));
    }
}

util::Result<void> PluginManager::load(const std::string& plugin_id, IPluginContext*)
{
    std::lock_guard lock(mutex_);

    if (stopped_.load(std::memory_order_acquire))
    {
        return std::unexpected(util::Error::make<Kind::NotFound>("Plugin not found: " + plugin_id));
    }

    auto dit = plugin_dirs_.find(plugin_id);
    if (dit == plugin_dirs_.end())
    {
        return std::unexpected(util::Error::make<Kind::NotFound>("Plugin not found: " + plugin_id));
    }

    if (loaded_.count(plugin_id))
    {
        return {};  // already loaded
    }

    // Both maps are cleaned together, but never dereference an end() iterator:
    // a manifest that vanished between the two lookups is "not found".
    auto ait = available_.find(plugin_id);
    if (ait == available_.end())
    {
        return std::unexpected(util::Error::make<Kind::NotFound>("Plugin not found: " + plugin_id));
    }

    LoadedPlugin lp;
    lp.manifest = ait->second;
    lp.plugin_dir = dit->second;
    lp.initialized = true;
    loaded_[plugin_id] = std::move(lp);

    kforge::util::log_info{}("[plugin] Loaded: {}", plugin_id);
    return {};
}

void PluginManager::unload(const std::string& plugin_id)
{
    std::lock_guard lock(mutex_);
    loaded_.erase(plugin_id);
}

PluginManager::ExecToken PluginManager::register_execution()
{
    std::lock_guard lock(exec_mutex_);
    if (stopped_.load(std::memory_order_acquire))
    {
        return nullptr;  // no new execution once stopped
    }
    auto token = std::make_shared<std::atomic<bool>>(false);
    active_execs_.push_back(token);
    return token;
}

void PluginManager::unregister_execution(const ExecToken& token)
{
    std::lock_guard lock(exec_mutex_);
    std::erase(active_execs_, token);
}

void PluginManager::shutdown_all()
{
    // 1) Raise cancellation first (lock-free): an in-flight execute() sees it on
    //    its next loop iteration and terminates + reaps its own child, so this
    //    call never waits on a subprocess while holding a mutex.
    stopped_.store(true, std::memory_order_release);
    std::vector<ExecToken> active;
    {
        std::lock_guard lock(exec_mutex_);
        active = active_execs_;
    }
    for (auto& token : active)
    {
        if (token)
        {
            token->store(true, std::memory_order_release);
        }
    }

    // 2) Drop all plugin metadata. plugin_dirs_ must go as well: leaving a stale
    //    directory behind made load() dereference available_.end().
    std::lock_guard lock(mutex_);
    loaded_.clear();
    available_.clear();
    plugin_dirs_.clear();
    diagnostics_.clear();  // the stopped manager exposes no diagnostics either
}

// ============================================================
// Execute — run a plugin action via CLI
// ============================================================

util::Result<std::string> PluginManager::execute(const std::string& plugin_id,
                                                 const std::string& action,
                                                 const std::string& json_args,
                                                 const std::function<bool()>& external_cancelled)
{
    // Refuse new work once stopped — checked again after the lookup below so a
    // shutdown racing this call can never start a process.
    if (stopped_.load(std::memory_order_acquire))
    {
        return std::unexpected(util::Error::make<Kind::PluginError>(
            "Plugin manager is stopped: cannot execute " + plugin_id));
    }

    // ---- action whitelist: the value travels into the command line ----
    if (!is_valid_action(action))
    {
        return std::unexpected(
            util::Error::make<Kind::ValidationError>("Invalid action: " + action));
    }

    std::filesystem::path plugin_dir;
    std::string entry_script;
    {
        std::lock_guard lock(mutex_);
        if (stopped_.load(std::memory_order_acquire))
        {
            return std::unexpected(util::Error::make<Kind::PluginError>(
                "Plugin manager is stopped: cannot execute " + plugin_id));
        }
        // Try loaded plugins first
        auto lit = loaded_.find(plugin_id);
        if (lit != loaded_.end())
        {
            plugin_dir = lit->second.plugin_dir;
            entry_script = lit->second.manifest.entry_point;
        }
        // Try discovered (but not loaded) plugins
        if (plugin_dir.empty())
        {
            auto dit = plugin_dirs_.find(plugin_id);
            if (dit != plugin_dirs_.end())
            {
                plugin_dir = dit->second;
                auto ait = available_.find(plugin_id);  // never dereference end()
                if (ait != available_.end())
                {
                    entry_script = ait->second.entry_point;
                }
            }
        }
    }

    if (plugin_dir.empty())
    {
        return std::unexpected(util::Error::make<Kind::NotFound>("Plugin not found: " + plugin_id));
    }
    if (entry_script.empty())
    {
        entry_script = "plugin.py";
    }

    // entry_script comes from the manifest (UTF-8) — never join it as a narrow
    // path, or a non-ASCII entry file name breaks on Windows.
    auto script_path = plugin_dir / path_from_utf8(entry_script);
    if (!std::filesystem::exists(script_path))
    {
        return std::unexpected(util::Error::make<Kind::IoError>("Plugin script not found: " +
                                                                path_display(script_path)));
    }

    // Register before spawning: shutdown_all() flips this token, so a process
    // started in the very same instant as a shutdown is still cancelled.
    ExecToken token = register_execution();
    if (!token)
    {
        return std::unexpected(util::Error::make<Kind::PluginError>(
            "Plugin manager is stopped: cannot execute " + plugin_id));
    }
    struct Unregister
    {
        PluginManager* self;
        const ExecToken& token;
        ~Unregister()
        {
            self->unregister_execution(token);
        }
    } unregister_guard{this, token};

    return run_plugin_process(script_path, action, json_args, execution_options_,
                              [this, token, external_cancelled]()
                              {
                                  // Composite predicate: per-task cancel (task
                                  // service) OR manager-wide shutdown.
                                  return stopped_.load(std::memory_order_acquire) ||
                                         token->load(std::memory_order_acquire) ||
                                         (external_cancelled && external_cancelled());
                              });

}

// ============================================================
// Manifest parsing (moved from iplugin.h to here)
// ============================================================

namespace
{
util::Error manifest_error(std::string message)
{
    return util::Error::make<Kind::ParseError>(std::move(message));
}

/// Parse one actions array: the v1 top-level "actions" or the v2
/// "python.actions". `field` is the dotted manifest path used in messages.
util::Result<std::vector<PluginAction>> parse_actions(const nlohmann::json& doc,
                                                      const std::string& field)
{
    if (!doc.is_array())
    {
        return std::unexpected(manifest_error("manifest.json " + field + " must be an array"));
    }
    std::vector<PluginAction> actions;
    for (const auto& item : doc)
    {
        if (!item.is_object())
        {
            return std::unexpected(
                manifest_error("manifest.json " + field + " entries must be objects"));
        }
        PluginAction action;
        action.id = item.value("id", "");
        action.name = item.value("name", "");
        action.description = item.value("description", "");
        action.icon = item.value("icon", "");
        action.trigger = item.value("trigger", "inline");
        action.ui_template = item.value("template", "");
        action.analyze_action = item.value("analyze_action", "");
        action.preflight = item.value("preflight", "");
        action.protocol = item.value("protocol", "");
        if (item.contains("auto_args"))
        {
            if (!item["auto_args"].is_array())
            {
                return std::unexpected(manifest_error(
                    "manifest.json " + field + " entries: auto_args must be an array of strings"));
            }
            for (const auto& argument : item["auto_args"])
            {
                if (!argument.is_string())
                {
                    return std::unexpected(manifest_error(
                        "manifest.json " + field +
                        " entries: auto_args must be an array of strings"));
                }
                action.auto_args.push_back(argument.get<std::string>());
            }
        }
        if (item.contains("button"))
        {
            const auto& button = item["button"];
            if (!button.is_object())
            {
                return std::unexpected(manifest_error(
                    "manifest.json " + field + " entries: button must be an object"));
            }
            action.button_show = button.value("show", true);
            action.button_style = button.value("style", "both");
            action.button_location = button.value("location", "toolbar");
            action.button_tooltip = button.value("tooltip", "");
        }
        if (item.contains("cancelable"))
        {
            if (!item["cancelable"].is_boolean())
            {
                return std::unexpected(manifest_error(
                    "manifest.json " + field + " entries: cancelable must be a boolean"));
            }
            action.cancelable = item["cancelable"].get<bool>();
        }
        if (item.contains("schema"))
        {
            action.schema = item["schema"];
        }
        if (!action.id.empty())
        {
            actions.push_back(std::move(action));
        }
    }
    return actions;
}

/// Parse "frontend.provides[]" ({contract, version}); both fields are required
/// non-empty strings so a contract declaration can never be half-specified.
util::Result<std::vector<PluginContractRef>> parse_provides(const nlohmann::json& frontend)
{
    std::vector<PluginContractRef> provides;
    if (!frontend.contains("provides"))
    {
        return provides;
    }
    const auto& doc = frontend["provides"];
    if (!doc.is_array())
    {
        return std::unexpected(
            manifest_error("manifest.json frontend.provides must be an array"));
    }
    for (const auto& item : doc)
    {
        if (!item.is_object())
        {
            return std::unexpected(
                manifest_error("manifest.json frontend.provides entries must be objects"));
        }
        if (!item.contains("contract") || !item["contract"].is_string() ||
            item["contract"].get<std::string>().empty())
        {
            return std::unexpected(manifest_error(
                "manifest.json frontend.provides[].contract must be a non-empty string"));
        }
        if (!item.contains("version") || !item["version"].is_string() ||
            item["version"].get<std::string>().empty())
        {
            return std::unexpected(manifest_error(
                "manifest.json frontend.provides[].version must be a non-empty string"));
        }
        provides.push_back(
            {item["contract"].get<std::string>(), item["version"].get<std::string>()});
    }
    return provides;
}

/// Parse "frontend.requires[]" ({contract, range}); C++ reserves `requires`, so
/// the manifest field is stored in PluginManifest::contract_requires.
util::Result<std::vector<PluginContractRequirement>> parse_contract_requirements(
    const nlohmann::json& frontend)
{
    std::vector<PluginContractRequirement> requirements;
    if (!frontend.contains("requires"))
    {
        return requirements;
    }
    const auto& doc = frontend["requires"];
    if (!doc.is_array())
    {
        return std::unexpected(
            manifest_error("manifest.json frontend.requires must be an array"));
    }
    for (const auto& item : doc)
    {
        if (!item.is_object())
        {
            return std::unexpected(
                manifest_error("manifest.json frontend.requires entries must be objects"));
        }
        if (!item.contains("contract") || !item["contract"].is_string() ||
            item["contract"].get<std::string>().empty())
        {
            return std::unexpected(manifest_error(
                "manifest.json frontend.requires[].contract must be a non-empty string"));
        }
        if (!item.contains("range") || !item["range"].is_string() ||
            item["range"].get<std::string>().empty())
        {
            return std::unexpected(manifest_error(
                "manifest.json frontend.requires[].range must be a non-empty string"));
        }
        requirements.push_back(
            {item["contract"].get<std::string>(), item["range"].get<std::string>()});
    }
    return requirements;
}

/// Parse the frontend block shared by v1 and v2 manifests: phase-1 asset paths
/// plus the v2 contract declarations (provides/requires/contributes).
util::Result<PluginFrontend> parse_frontend(const nlohmann::json& frontend_doc,
                                            PluginManifest& manifest)
{
    if (!frontend_doc.is_object())
    {
        return std::unexpected(
            manifest_error("manifest.json frontend must be an object"));
    }
    PluginFrontend frontend;
    if (frontend_doc.contains("root"))
    {
        if (!frontend_doc["root"].is_string())
        {
            return std::unexpected(
                manifest_error("manifest.json frontend.root must be a string"));
        }
        frontend.root = frontend_doc["root"].get<std::string>();
    }
    if (frontend_doc.contains("entry"))
    {
        if (!frontend_doc["entry"].is_string())
        {
            return std::unexpected(
                manifest_error("manifest.json frontend.entry must be a string"));
        }
        frontend.entry = frontend_doc["entry"].get<std::string>();
    }
    if (!is_safe_relative_asset_path(frontend.root))
    {
        return std::unexpected(manifest_error(
            "manifest.json frontend.root must be a safe relative path: \"" + frontend.root +
            "\""));
    }
    if (!is_safe_relative_asset_path(frontend.entry))
    {
        return std::unexpected(manifest_error(
            "manifest.json frontend.entry must be a safe relative path: \"" + frontend.entry +
            "\""));
    }
    if (frontend_doc.contains("styles"))
    {
        const auto& styles_doc = frontend_doc["styles"];
        if (!styles_doc.is_array())
        {
            return std::unexpected(manifest_error(
                "manifest.json frontend.styles must be an array of strings"));
        }
        for (const auto& style_doc : styles_doc)
        {
            if (!style_doc.is_string())
            {
                return std::unexpected(manifest_error(
                    "manifest.json frontend.styles must be an array of strings"));
            }
            auto style = style_doc.get<std::string>();
            if (!is_safe_relative_asset_path(style))
            {
                return std::unexpected(manifest_error(
                    "manifest.json frontend.styles[] must be a safe relative path: \"" + style +
                    "\""));
            }
            frontend.styles.push_back(std::move(style));
        }
    }

    auto provides = parse_provides(frontend_doc);
    if (!provides)
    {
        return std::unexpected(provides.error());
    }
    manifest.provides = std::move(*provides);

    auto requirements = parse_contract_requirements(frontend_doc);
    if (!requirements)
    {
        return std::unexpected(requirements.error());
    }
    manifest.contract_requires = std::move(*requirements);

    if (frontend_doc.contains("contributes"))
    {
        manifest.contributes = frontend_doc["contributes"];
    }
    return frontend;
}

/// Full v1/v2 manifest parser. Every phase-2 field yields a specific
/// ParseError; type errors on legacy fields are converted by from_json().
util::Result<PluginManifest> parse_manifest_json(const nlohmann::json& doc)
{
    if (!doc.is_object())
    {
        return std::unexpected(manifest_error("manifest.json must contain a JSON object"));
    }

    PluginManifest manifest;

    // ---- manifestVersion: absent means v1; only 1 and 2 are understood ----
    if (doc.contains("manifestVersion"))
    {
        if (!doc["manifestVersion"].is_number_integer())
        {
            return std::unexpected(
                manifest_error("manifestVersion must be the integer 1 or 2"));
        }
        manifest.manifest_version = doc["manifestVersion"].get<int>();
        if (manifest.manifest_version != 1 && manifest.manifest_version != 2)
        {
            return std::unexpected(manifest_error(
                "unsupported manifestVersion: " + std::to_string(manifest.manifest_version) +
                " (supported: 1, 2)"));
        }
    }

    // ---- fields shared by v1 and v2 ---------------------------------------
    manifest.id = doc.value("id", "");
    manifest.name = doc.value("name", "");
    manifest.version = doc.value("version", "");
    manifest.author = doc.value("author", "");
    manifest.description = doc.value("description", "");
    manifest.license = doc.value("license", "");
    manifest.entry_function = doc.value("entry_function", "");
    manifest.min_app_version = doc.value("min_app_version", "");
    manifest.one_click = doc.value("one_click", false);
    manifest.icon = doc.value("icon", "");

    if (doc.contains("config"))
    {
        manifest.config = doc["config"];
    }

    if (doc.contains("capabilities"))
    {
        if (!doc["capabilities"].is_array())
        {
            return std::unexpected(
                manifest_error("manifest.json capabilities must be an array of strings"));
        }
        for (const auto& capability : doc["capabilities"])
        {
            if (!capability.is_string())
            {
                return std::unexpected(
                    manifest_error("manifest.json capabilities must be an array of strings"));
            }
            manifest.capabilities.push_back(capability.get<std::string>());
        }
    }
    if (doc.contains("platforms"))
    {
        if (!doc["platforms"].is_array())
        {
            return std::unexpected(
                manifest_error("manifest.json platforms must be an array of strings"));
        }
        for (const auto& platform : doc["platforms"])
        {
            if (!platform.is_string())
            {
                return std::unexpected(
                    manifest_error("manifest.json platforms must be an array of strings"));
            }
            manifest.platforms.push_back(platform.get<std::string>());
        }
    }

    // ---- optional v2 "python" block ---------------------------------------
    if (doc.contains("python"))
    {
        const auto& python_doc = doc["python"];
        if (!python_doc.is_object())
        {
            return std::unexpected(
                manifest_error("manifest.json python must be an object"));
        }
        PluginPython python;
        if (python_doc.contains("entry"))
        {
            if (!python_doc["entry"].is_string())
            {
                return std::unexpected(
                    manifest_error("manifest.json python.entry must be a string"));
            }
            python.entry = python_doc["entry"].get<std::string>();
        }
        if (python_doc.contains("actions"))
        {
            auto actions = parse_actions(python_doc["actions"], "python.actions");
            if (!actions)
            {
                return std::unexpected(actions.error());
            }
            python.actions = std::move(*actions);
        }
        manifest.python = std::move(python);
    }

    // ---- optional v2 "engines" block --------------------------------------
    if (doc.contains("engines"))
    {
        const auto& engines_doc = doc["engines"];
        if (!engines_doc.is_object())
        {
            return std::unexpected(
                manifest_error("manifest.json engines must be an object"));
        }
        PluginEngines engines;
        if (engines_doc.contains("kiforge"))
        {
            if (!engines_doc["kiforge"].is_string())
            {
                return std::unexpected(
                    manifest_error("manifest.json engines.kiforge must be a string"));
            }
            engines.kiforge = engines_doc["kiforge"].get<std::string>();
        }
        if (engines_doc.contains("sdk"))
        {
            if (!engines_doc["sdk"].is_string())
            {
                return std::unexpected(
                    manifest_error("manifest.json engines.sdk must be a string"));
            }
            engines.sdk = engines_doc["sdk"].get<std::string>();
        }
        manifest.engines = std::move(engines);
    }

    // ---- optional frontend (phase-1 asset paths + v2 contracts) -----------
    if (doc.contains("frontend"))
    {
        auto frontend = parse_frontend(doc["frontend"], manifest);
        if (!frontend)
        {
            return std::unexpected(frontend.error());
        }
        manifest.frontend = std::move(*frontend);
    }

    // ---- sync the executor / legacy-UI view -------------------------------
    // v2: python.entry/actions (frontend-only plugins have no Python side).
    // v1: the historical top-level entry/actions.
    if (manifest.manifest_version >= 2)
    {
        // Legacy top-level actions are not executed for v2, but a malformed
        // value is still reported instead of silently ignored.
        if (doc.contains("actions"))
        {
            auto ignored = parse_actions(doc["actions"], "actions");
            if (!ignored)
            {
                return std::unexpected(ignored.error());
            }
        }
        if (manifest.python)
        {
            manifest.entry_point = manifest.python->entry;
            manifest.actions = manifest.python->actions;
        }
        else
        {
            manifest.entry_point = "plugin.py";
            manifest.actions.clear();
        }
    }
    else
    {
        if (doc.contains("entry"))
        {
            if (!doc["entry"].is_string())
            {
                return std::unexpected(
                    manifest_error("manifest.json entry must be a string"));
            }
            manifest.entry_point = doc["entry"].get<std::string>();
        }
        else
        {
            manifest.entry_point = "plugin.py";
        }
        manifest.actions.clear();
        if (doc.contains("actions"))
        {
            auto actions = parse_actions(doc["actions"], "actions");
            if (!actions)
            {
                return std::unexpected(actions.error());
            }
            manifest.actions = std::move(*actions);
        }
    }

    if (manifest.id.empty() || manifest.name.empty())
    {
        return std::unexpected(manifest_error(
            "manifest.json missing required fields (id, name)"));
    }
    return manifest;
}
}  // namespace

util::Result<PluginManifest> PluginManifest::from_json(const nlohmann::json& doc)
{
    try
    {
        return parse_manifest_json(doc);
    }
    // Malformed field types on the legacy fields (id, name, one_click, …) still
    // surface as ParseError instead of unwinding into discovery.
    catch (const nlohmann::json::exception& e)
    {
        return std::unexpected(
            manifest_error("Invalid manifest field type: " + std::string(e.what())));
    }
}

util::Result<PluginManifest> PluginManifest::from_file(const std::filesystem::path& path)
{
    // ifstream's filesystem::path overload opens the file with the native
    // encoding: a plugin directory named in Chinese is found on Windows too.
    std::ifstream f(path);
    if (!f.is_open())
    {
        return std::unexpected(
            util::Error::make<Kind::IoError>("Cannot open manifest: " + path_display(path)));
    }
    try
    {
        auto doc = nlohmann::json::parse(f);
        return from_json(doc);
    }
    // json::exception covers parse_error AND type_error (e.g. "id": 42) —
    // a malformed manifest must be reported, never propagated as a throw.
    catch (const nlohmann::json::exception& e)
    {
        return std::unexpected(util::Error::make<Kind::ParseError>(
            "Invalid JSON in " + path_display(path) + ": " + e.what()));
    }
}

}  // namespace kforge::plugin
