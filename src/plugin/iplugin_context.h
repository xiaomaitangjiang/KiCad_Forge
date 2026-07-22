#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <functional>

#include <nlohmann/json.hpp>

#include "core/types.h"
#include "core/symbol.h"
#include "core/footprint.h"
#include "core/model_3d.h"
#include "util/result.h"

namespace kforge::plugin::api {

// ============================================================
// Sandboxed API interfaces for plugins
// ============================================================

/// Read-only access to library data.
class ILibraryAccess {
public:
    virtual ~ILibraryAccess() = default;

    /// List all registered libraries.
    virtual std::vector<core::LibraryMeta> list_libraries() const = 0;

    /// Get all symbols in a library.
    virtual util::Result<std::vector<core::Symbol>>
        symbols_in_library(const core::Uuid& lib_id) const = 0;

    /// Get all known footprints.
    virtual util::Result<std::vector<core::Footprint>>
        all_footprints() const = 0;

    /// Search symbols by keyword.
    virtual util::Result<std::vector<core::Symbol>>
        search_symbols(const std::string& keyword) const = 0;
};

/// Create new library entries (symbols, footprints, 3D models).
class IComponentCreator {
public:
    virtual ~IComponentCreator() = default;

    /// Create a new symbol in the target library.
    virtual util::Result<core::Uuid>
        create_symbol(const core::Symbol& sym,
                      const core::Uuid& target_library_id) = 0;

    /// Create a new footprint.
    virtual util::Result<core::Uuid>
        create_footprint(const core::Footprint& fp) = 0;

    /// Create a new 3D model entry.
    virtual util::Result<core::Uuid>
        create_3d_model(const core::Model3D& model) = 0;

    /// Link a symbol to a footprint.
    virtual util::Result<void>
        link_symbol_to_footprint(const core::Uuid& sym_id,
                                 const core::Uuid& fp_id) = 0;

    /// Attach a 3D model to a footprint.
    virtual util::Result<void>
        attach_3d_model(const core::Uuid& fp_id,
                        const std::filesystem::path& model_path) = 0;
};

/// Network access (rate-limited, logged).
using ProgressCallback = std::function<void(int percent, const std::string& status)>;

class INetworkAccess {
public:
    virtual ~INetworkAccess() = default;

    virtual util::Result<nlohmann::json>
        http_get_json(const std::string& url) = 0;

    virtual util::Result<std::vector<uint8_t>>
        http_download(const std::string& url,
                      ProgressCallback progress = nullptr) = 0;
};

/// Log to the application log.
class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void debug(const std::string& msg) = 0;
    virtual void info(const std::string& msg) = 0;
    virtual void warn(const std::string& msg) = 0;
    virtual void error(const std::string& msg) = 0;
};

/// Plugin-scoped settings.
class ISettingsAccess {
public:
    virtual ~ISettingsAccess() = default;
    virtual std::string get(const std::string& key,
                            const std::string& default_val = "") const = 0;
    virtual void set(const std::string& key, const std::string& value) = 0;
};

}  // namespace kforge::plugin::api

// ============================================================
// Full context passed to each plugin
// ============================================================
namespace kforge::plugin {

class IPluginContext {
public:
    virtual ~IPluginContext() = default;

    virtual api::ILibraryAccess& library_access() = 0;
    virtual api::IComponentCreator& component_creator() = 0;
    virtual api::INetworkAccess& network_access() = 0;
    virtual api::ILogger& logger() = 0;
    virtual api::ISettingsAccess& settings() = 0;

    /// Check if the application is shutting down (plugins should abort work).
    virtual bool is_shutting_down() const = 0;
};

}  // namespace kforge::plugin
