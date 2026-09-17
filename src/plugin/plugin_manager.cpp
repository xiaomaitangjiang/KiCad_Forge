#include "plugin/plugin_manager.h"
#include "util/logger.h"
#include "util/platform.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

#ifndef _WIN32
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

            auto m = PluginManifest::from_file(manifest_path.string());
            if (m && !m->id.empty() && !m->name.empty())
            {
                auto id = m->id;
                kforge::util::log_info{}("[plugin] Discovered: {} v{} ({}) at {}", m->name,
                                         m->version, id, entry.path().string());
                available_[id] = std::move(*m);
                plugin_dirs_[id] = entry.path();
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
    if (ok)
    {
        char buf[4096];
        DWORD n;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &n, nullptr) && n > 0)
        {
            buf[n] = 0;
            output += buf;
        }
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
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

    // parent: read output until EOF
    close(fds[1]);
    std::string output;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = 0;
        output += buf;
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
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
    catch (const nlohmann::json::parse_error& e)
    {
        return std::unexpected(
            util::Error::make<Kind::ParseError>("Invalid JSON in " + path + ": " + e.what()));
    }
}

}  // namespace kforge::plugin
