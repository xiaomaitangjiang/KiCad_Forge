#include "plugin/plugin_manager.h"
#include "util/logger.h"
#include "util/platform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#ifndef _WIN32
#include <poll.h>
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

// Windows CommandLineToArgvW-compatible quoting: a run of N backslashes
// before a quote is doubled (so the child parses them back correctly), and a
// trailing run of backslashes is doubled before the closing quote.
#ifdef _WIN32
std::string escape_win_arg(const std::string& arg)
{
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg)
    {
        if (c == '\\')
        {
            backslashes++;
        }
        else if (c == '"')
        {
            out.append((backslashes * 2) + 1, '\\');
            out += '"';
            backslashes = 0;
        }
        else
        {
            out.append(backslashes, '\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}
#endif

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

PluginManager::PluginManager(const std::vector<std::filesystem::path>& paths) : search_paths_(paths)
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
    available_.clear();
    plugin_dirs_.clear();

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
            // skip it with a warning and continue.
            try
            {
                auto m = PluginManifest::from_file(manifest_path.string());
                if (m && !m->id.empty() && !m->name.empty())
                {
                    auto id = m->id;
                    // 先扫描到的路径优先（内置插件目录在前）：同名 ID 的用户插件
                    // 不能顶掉内置项 —— 声明宿主协议的 action 只认受控来源
                    if (available_.contains(id))
                    {
                        kforge::util::log_warn{}(
                            "[plugin] Ignoring duplicate plugin id {} at {} (already loaded from {})",
                            id, entry.path().string(), plugin_dirs_[id].string());
                        continue;
                    }
                    kforge::util::log_info{}("[plugin] Discovered: {} v{} ({}) at {}", m->name,
                                             m->version, id, entry.path().string());
                    available_[id] = std::move(*m);
                    plugin_dirs_[id] = entry.path();
                }
                else if (!m)
                {
                    kforge::util::log_warn{}("[plugin] Skipping {}: {}",
                                             entry.path().string(),
                                             m.error().format_message());
                }
            }
            catch (const std::exception& e)
            {
                kforge::util::log_warn{}("[plugin] Skipping {}: malformed manifest ({})",
                                         entry.path().string(), e.what());
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

util::Result<void> PluginManager::load(const std::string& plugin_id, IPluginContext*)
{
    std::lock_guard lock(mutex_);

    auto dit = plugin_dirs_.find(plugin_id);
    if (dit == plugin_dirs_.end())
    {
        return std::unexpected(util::Error::make<Kind::NotFound>("Plugin not found: " + plugin_id));
    }

    if (loaded_.count(plugin_id))
    {
        return {};  // already loaded
    }

    auto ait = available_.find(plugin_id);
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

void PluginManager::shutdown_all()
{
    std::lock_guard lock(mutex_);
    loaded_.clear();
    available_.clear();
}

// ============================================================
// Execute — run a plugin action via CLI
// ============================================================

util::Result<std::string> PluginManager::execute(const std::string& plugin_id,
                                                 const std::string& action,
                                                 const std::string& json_args)
{
    std::filesystem::path plugin_dir;
    std::string entry_script;
    {
        std::lock_guard lock(mutex_);
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
                auto ait = available_.find(plugin_id);
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

    auto script_path = plugin_dir / entry_script;
    if (!std::filesystem::exists(script_path))
    {
        return std::unexpected(
            util::Error::make<Kind::IoError>("Plugin script not found: " + script_path.string()));
    }

    // ---- action whitelist: the value travels into the command line ----
    if (!is_valid_action(action))
    {
        return std::unexpected(
            util::Error::make<Kind::ValidationError>("Invalid action: " + action));
    }

#ifdef _WIN32
    // CommandLineToArgvW-compatible quoting per argument — no shell involved
    std::string cmd = "python " + escape_win_arg(script_path.string()) + " " +
                      escape_win_arg(action) + " " + escape_win_arg(json_args);
    kforge::util::log_debug{}("[plugin] Execute: {}", cmd);

    // CreateProcess + CREATE_NO_WINDOW: no console popup
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
        return std::unexpected(util::Error::make<Kind::IoError>("Failed to create pipe"));
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    char* cmd_copy = _strdup(cmd.c_str());
    BOOL ok = CreateProcessA(nullptr, cmd_copy, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             nullptr, &si, &pi);
    free(cmd_copy);
    CloseHandle(hWrite);

    std::string output;
    bool timed_out = false;
    if (ok)
    {
        // Drain the pipe WITH a deadline — never block indefinitely on a
        // plugin that hangs or whose descendants keep the pipe open.
        constexpr DWORD kTimeoutMs = 120000;
        constexpr size_t kMaxOutput = 4 * 1024 * 1024;  // cap memory
        const DWORD deadline = GetTickCount() + kTimeoutMs;
        char buf[4096];
        for (;;)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr))
            {
                break;  // pipe closed
            }
            if (avail > 0)
            {
                DWORD n = 0;
                if (!ReadFile(hRead, buf, sizeof(buf) - 1, &n, nullptr) || n == 0)
                {
                    break;
                }
                buf[n] = 0;
                if (output.size() < kMaxOutput)
                {
                    output += buf;
                }
                continue;
            }
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
            {
                break;  // process exited and pipe drained
            }
            if (GetTickCount() > deadline)
            {
                timed_out = true;
                TerminateProcess(pi.hProcess, 1);
                break;
            }
            Sleep(20);
        }
        if (!timed_out)
        {
            WaitForSingleObject(pi.hProcess, 5000);  // reap after EOF
        }
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (timed_out)
        {
            CloseHandle(hRead);
            return std::unexpected(
                util::Error::make<Kind::IoError>("Plugin timed out after 120s"));
        }
        if (ec != 0 && output.empty())
        {
            CloseHandle(hRead);
            return std::unexpected(
                util::Error::make<Kind::IoError>("Plugin exit: " + std::to_string(ec)));
        }
    }
    CloseHandle(hRead);
    if (!ok)
        return std::unexpected(util::Error::make<Kind::IoError>("Failed to launch plugin"));
    return output;
#else
    // fork + execvp: argv passed directly, no shell parsing, no quoting surface
    kforge::util::log_debug{}("[plugin] Execute: python {} {} {}", script_path.string(), action,
                              json_args);
    int fds[2];
    if (pipe(fds) != 0)
        return std::unexpected(util::Error::make<Kind::IoError>("Failed to create pipe"));
    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return std::unexpected(util::Error::make<Kind::IoError>("Failed to fork"));
    }
    if (pid == 0)
    {
        // child: stdout/stderr → pipe, then exec python <script> <action> <args>
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execlp("python", "python", script_path.string().c_str(), action.c_str(),
               json_args.c_str(), static_cast<char*>(nullptr));
        _exit(127);  // exec failed
    }

    // parent: drain the pipe with a deadline — a hanging plugin (or one whose
    // descendants keep the pipe open) must be killed, not block forever.
    close(fds[1]);
    std::string output;
    char buf[4096];
    constexpr auto kTimeout = std::chrono::seconds(120);
    constexpr size_t kMaxOutput = 4 * 1024 * 1024;
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    bool timed_out = false;
    bool child_done = false;
    int status = 0;

    for (;;)
    {
        struct pollfd pfd
        {
            fds[0], POLLIN, 0
        };
        int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP)))
        {
            ssize_t nr = read(fds[0], buf, sizeof(buf) - 1);
            if (nr <= 0)
            {
                break;  // EOF
            }
            if (output.size() < kMaxOutput)
            {
                buf[nr] = 0;
                output += buf;
            }
            continue;
        }
        if (pr < 0)
        {
            break;
        }
        if (!child_done && waitpid(pid, &status, WNOHANG) == pid)
        {
            child_done = true;
        }
        if (child_done)
        {
            // child exited — drain whatever is still buffered, then stop
            struct pollfd tail
            {
                fds[0], POLLIN, 0
            };
            if (poll(&tail, 1, 0) <= 0 || !(tail.revents & POLLIN))
            {
                break;
            }
            continue;
        }
        if (std::chrono::steady_clock::now() > deadline)
        {
            timed_out = true;
            break;
        }
    }
    close(fds[0]);

    if (timed_out)
    {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return std::unexpected(util::Error::make<Kind::IoError>("Plugin timed out after 120s"));
    }
    if (!child_done)
    {
        waitpid(pid, &status, 0);
    }
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (rc != 0 && output.empty())
        return std::unexpected(util::Error::make<Kind::IoError>("Plugin exit: " + std::to_string(rc)));
    return output;
#endif
}

// ============================================================
// Manifest parsing (moved from iplugin.h to here)
// ============================================================

util::Result<PluginManifest> PluginManifest::from_json(const nlohmann::json& doc)
{
    PluginManifest m;
    m.id = doc.value("id", "");
    m.name = doc.value("name", "");
    m.version = doc.value("version", "");
    m.author = doc.value("author", "");
    m.description = doc.value("description", "");
    m.license = doc.value("license", "");
    m.entry_point = doc.value("entry", "plugin.py");
    m.min_app_version = doc.value("min_app_version", "");
    m.one_click = doc.value("one_click", false);
    m.icon = doc.value("icon", "");

    // Parse actions from manifest
    if (doc.contains("actions") && doc["actions"].is_array())
    {
        for (const auto& a : doc["actions"])
        {
            PluginAction act;
            act.id = a.value("id", "");
            act.name = a.value("name", "");
            act.description = a.value("description", "");
            act.icon = a.value("icon", "");
            act.trigger = a.value("trigger", "inline");
            act.ui_template = a.value("template", "");
            act.analyze_action = a.value("analyze_action", "");
            act.preflight = a.value("preflight", "");
            act.protocol = a.value("protocol", "");
            if (a.contains("auto_args") && a["auto_args"].is_array())
            {
                for (const auto& k : a["auto_args"])
                {
                    act.auto_args.push_back(k.get<std::string>());
                }
            }
            // Button display options
            if (a.contains("button"))
            {
                const auto& btn = a["button"];
                act.button_show = btn.value("show", true);
                act.button_style = btn.value("style", "both");
                act.button_location = btn.value("location", "toolbar");
                act.button_tooltip = btn.value("tooltip", "");
            }
            if (a.contains("schema"))
                act.schema = a["schema"];
            if (!act.id.empty())
                m.actions.push_back(std::move(act));
        }
    }

    if (doc.contains("capabilities") && doc["capabilities"].is_array())
    {
        for (auto& c : doc["capabilities"])
            m.capabilities.push_back(c.get<std::string>());
    }
    if (doc.contains("platforms") && doc["platforms"].is_array())
    {
        for (auto& p : doc["platforms"])
            m.platforms.push_back(p.get<std::string>());
    }
    if (m.id.empty() || m.name.empty())
    {
        return std::unexpected(util::Error::make<Kind::ParseError>(
            "manifest.json missing required fields (id, name)"));
    }
    return m;
}

util::Result<PluginManifest> PluginManifest::from_file(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open())
    {
        return std::unexpected(util::Error::make<Kind::IoError>("Cannot open manifest: " + path));
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
        return std::unexpected(
            util::Error::make<Kind::ParseError>("Invalid JSON in " + path + ": " + e.what()));
    }
}

}  // namespace kforge::plugin
