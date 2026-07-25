#pragma once

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
#include "util/result.h"

namespace kforge::plugin {

/// Manages plugin discovery, loading, lifecycle, and ZIP installation.
///
/// Thread-safe: all public methods use internal mutex.
class PluginManager {
public:
    /// search_paths: directories to scan for plugin bundles.
    explicit PluginManager(
        const std::vector<std::filesystem::path>& search_paths);

    ~PluginManager();

    // --- Discovery ---

    /// Scan search paths for available plugins (folders with manifest.json).
    void discover();

    /// List all discovered plugins.
    std::vector<PluginManifest> available_plugins() const;

    /// List currently loaded plugins.
    std::vector<PluginManifest> loaded_plugins() const;

    // --- Lifecycle ---

    /// Load and initialize a plugin by ID.
    util::Result<void> load(const std::string& plugin_id, IPluginContext* ctx);

    /// Unload a plugin.
    void unload(const std::string& plugin_id);

    /// Unload all plugins.
    void shutdown_all();

    /// Execute a plugin action via its manifest-defined entry point.
    /// For Python plugins: runs "python <entry> <action> '<json_args>'" and returns JSON.
    util::Result<std::string> execute(const std::string& plugin_id,
                                       const std::string& action,
                                       const std::string& json_args = "{}");

    // --- Query ---

    /// Check if a plugin is loaded/available.
    bool is_loaded(const std::string& plugin_id) const;

    /// Get the directory containing a plugin's manifest + resources.
    std::filesystem::path plugin_path(const std::string& plugin_id) const;

    /// Number of loaded plugins.
    size_t count() const;

private:
    struct LoadedPlugin {
        PluginManifest manifest;
        std::filesystem::path plugin_dir;   // folder containing manifest.json + scripts
        bool initialized{false};
    };

    mutable std::mutex mutex_;
    std::vector<std::filesystem::path> search_paths_;
    std::unordered_map<std::string, PluginManifest> available_;
    std::unordered_map<std::string, std::filesystem::path> plugin_dirs_;  // id → directory
    std::unordered_map<std::string, LoadedPlugin> loaded_;
};

}  // namespace kforge::plugin
