#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "plugin/iplugin.h"
#include "util/error.h"

namespace kforge::plugin {

/// Resource bounds for one plugin subprocess started by execute().
///
/// Defaults preserve the historical behaviour (120 s wall clock, 4 MiB output
/// cap), so the pre-existing single-argument constructor keeps its old meaning.
/// Tests inject short timeouts / small caps through the constructor — there is
/// no environment variable, global and no timing hook involved.
struct ExecutionOptions
{
    std::chrono::milliseconds timeout{120000};
    std::size_t max_output_bytes{4u * 1024u * 1024u};
};

/// One problem detected while scanning plugin bundles. Per-plugin failures
/// never abort discovery; they are collected by discover() and exposed through
/// GET /api/plugin-diagnostics.
struct PluginDiagnostic
{
    /// Stable machine code. Phase 2 emits:
    ///   - "manifest/invalid"      manifest cannot be parsed/validated;
    ///   - "manifest/unsupported-version"  manifestVersion is not 1 or 2;
    ///   - "plugin/duplicate-id"   a plugin id was already discovered earlier.
    /// Codes are stable machine-readable identifiers; message carries details.
    std::string code;
    /// Manifest id; for a syntactically broken manifest the bundle directory
    /// name is used (the id is unknowable in that case).
    std::string plugin_id;
    std::string message;  // UTF-8 human-readable detail
};

/// Manages plugin discovery, loading, lifecycle, and ZIP installation.
///
/// Thread safety: plugin metadata (available_/loaded_/plugin_dirs_) is guarded
/// by mutex_, the set of in-flight executions by exec_mutex_, and the "stopped"
/// state is a lock-free atomic. No public method holds a mutex while waiting on
/// a child process: shutdown_all() only raises the cancellation flag, and the
/// execute() call that owns the child terminates + reaps it.
class PluginManager {
public:
    /// search_paths: directories to scan for plugin bundles.
    /// execution_options: bounds applied to every plugin subprocess.
    explicit PluginManager(const std::vector<std::filesystem::path>& search_paths,
                           const ExecutionOptions& execution_options = ExecutionOptions{});

    ~PluginManager();

    /// Create a manager over the standard search paths (bundled exe-dir plugins +
    /// user plugins, deduplicated), run discovery, and auto-load one-click plugins.
    /// Equivalent of the former ApiServer::init_plugins(); kept as a factory so
    /// tests can still construct with explicit paths.
    static std::unique_ptr<PluginManager> create_default();

    // --- Discovery ---

    /// Scan search paths for available plugins (folders with manifest.json).
    /// No-op once shutdown_all() ran: a stopped manager never revives.
    /// Every call clears the previous diagnostics before scanning (a rescan is
    /// a recomputation, not an accumulation); per-plugin failures are recorded
    /// there instead of aborting the scan.
    void discover();

    /// List all discovered plugins.
    std::vector<PluginManifest> available_plugins() const;

    /// Diagnostics collected by the most recent discover() call. Empty until
    /// discover() runs with problems; cleared again by shutdown_all().
    std::vector<PluginDiagnostic> diagnostics() const;

    /// List currently loaded plugins.
    std::vector<PluginManifest> loaded_plugins() const;

    // --- Lifecycle ---

    /// Load and initialize a plugin by ID. Fails with NotFound when the plugin
    /// is unknown or the manager has been shut down.
    util::Result<void> load(const std::string& plugin_id, IPluginContext* ctx);

    /// Unload a plugin.
    void unload(const std::string& plugin_id);

    /// Cancel all in-flight executions (they terminate + reap their child
    /// process), drop every loaded/discovered plugin, and stop the manager.
    /// Idempotent; a stopped manager refuses discover()/load()/execute().
    void shutdown_all();

    /// Execute a plugin action via its manifest-defined entry point.
    /// For Python plugins: runs "python <entry> <action> '<json_args>'" and returns JSON.
    /// Bounded by ExecutionOptions: on timeout, cancellation, output-cap overflow
    /// or a non-zero exit code the child (and its descendants) are terminated and
    /// reaped before an error is returned.
    /// external_cancelled: optional per-call cancellation predicate (task service);
    /// it is OR-ed with shutdown_all()'s token, so the Job/process-group reaping
    /// and every bound still apply.
    util::Result<std::string> execute(const std::string& plugin_id,
                                       const std::string& action,
                                       const std::string& json_args = "{}",
                                       const std::function<bool()>& external_cancelled = {});

    // --- Query ---

    /// Expose the host API endpoint to plugin subprocesses via KFORGE_API_URL.
    /// Called once the HTTP server has bound its port; plugins use it to call
    /// host search APIs (no-op for port <= 0).
    void set_api_port(int port);

    /// Check if a plugin is loaded/available.
    bool is_loaded(const std::string& plugin_id) const;

    /// Get the directory containing a plugin's manifest + resources.
    std::filesystem::path plugin_path(const std::string& plugin_id) const;

    /// Resolve a frontend asset reference (relative to the plugin's frontend
    /// root) to an absolute regular-file path, for GET /plugin-assets/... and
    /// tests. Returns:
    ///   - NotFound: unknown plugin, plugin without a frontend, or missing file;
    ///   - ValidationError: unsafe relative path (absolute/drive/backslash,
    ///     "."/".."/empty components, control characters), a symlink / junction
    ///     component, a path escaping the frontend root, or a non-regular file.
    /// Directory traversal is checked per component and again on the canonical
    /// result, so links cannot smuggle a request outside the plugin directory.
    util::Result<std::filesystem::path> resolve_frontend_asset(
        const std::string& plugin_id, const std::string& relative) const;

    /// Number of loaded plugins.
    size_t count() const;

    /// Bounds applied to new executions (diagnostics/tests).
    [[nodiscard]] ExecutionOptions execution_options() const noexcept
    {
        return execution_options_;
    }

    /// True once shutdown_all() ran.
    [[nodiscard]] bool is_stopped() const noexcept
    {
        return stopped_.load(std::memory_order_acquire);
    }

private:
    struct LoadedPlugin {
        PluginManifest manifest;
        std::filesystem::path plugin_dir;   // folder containing manifest.json + scripts
        bool initialized{false};
    };

    /// Cancellation token shared with shutdown_all(): flipping it makes the
    /// execute() call that owns the child terminate and reap it. Only an atomic
    /// is shared — no OS handle crosses threads.
    using ExecToken = std::shared_ptr<std::atomic<bool>>;
    ExecToken register_execution();                    // nullptr once stopped
    void unregister_execution(const ExecToken& token);

    mutable std::mutex mutex_;
    std::atomic<int> api_port_{0};
    std::atomic<bool> stopped_{false};   // set by shutdown_all(), never cleared
    ExecutionOptions execution_options_;
    std::vector<std::filesystem::path> search_paths_;
    std::unordered_map<std::string, PluginManifest> available_;
    std::unordered_map<std::string, std::filesystem::path> plugin_dirs_;  // id → directory
    std::unordered_map<std::string, LoadedPlugin> loaded_;
    std::vector<PluginDiagnostic> diagnostics_;  // last discover() results

    mutable std::mutex exec_mutex_;      // guards active_execs_ only
    std::vector<ExecToken> active_execs_;
};

}  // namespace kforge::plugin
