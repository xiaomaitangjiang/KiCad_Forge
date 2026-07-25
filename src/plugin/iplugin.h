#pragma once

#include <string>
#include <vector>
#include <memory>

#include <nlohmann/json.hpp>

#include "core/types.h"
#include "core/symbol.h"
#include "core/footprint.h"
#include "core/model_3d.h"
#include "core/component.h"
#include "util/result.h"
#include "plugin/iplugin_context.h"

namespace kforge::plugin {

// ============================================================
// Plugin action — declared in manifest.json, rendered as UI button
// ============================================================
struct PluginAction {
    std::string id;              // "import"
    std::string name;            // "Import LCSC"
    std::string description;     // tooltip
    std::string icon;            // override default icon, optional
    std::string trigger;         // "dialog" | "inline" | "panel"
    bool button_show{true};      // show button in toolbar
    std::string button_style;    // "icon" | "text" | "both" (default "both")
    std::string button_tooltip;  // tooltip for icon-only buttons
    nlohmann::json schema;       // form fields for trigger=dialog
};

// ============================================================
// Plugin manifest (from manifest.json)
// ============================================================
struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string license;
    std::string entry_point;     // script file to invoke (default: plugin.py)
    std::string entry_function;  // function name in script (for future use)
    std::string min_app_version;
    std::string icon;            // relative path to icon file (SVG or PNG)
    std::vector<PluginAction> actions;
    bool one_click{false};
    std::vector<std::string> capabilities;
    std::vector<std::string> platforms;

    static util::Result<PluginManifest> from_json(const nlohmann::json& doc);
    static util::Result<PluginManifest> from_file(const std::string& path);
};

// ============================================================
// Plugin capabilities flags
// ============================================================
enum class PluginCapability : uint32_t {
    None      = 0,
    Import    = 1 << 0,
    Export    = 1 << 1,
    Tool      = 1 << 2,
    Validator = 1 << 3,
};
inline PluginCapability operator|(PluginCapability a, PluginCapability b) {
    return static_cast<PluginCapability>(static_cast<uint32_t>(a) |
                                         static_cast<uint32_t>(b));
}
inline bool has_capability(PluginCapability caps, PluginCapability test) {
    return (static_cast<uint32_t>(caps) & static_cast<uint32_t>(test)) != 0;
}

// ============================================================
// Import/Export result types
// ============================================================
struct ImportResult {
    int created{0};
    int updated{0};
    int errors{0};
    std::vector<std::string> messages;
};

struct ImportContext {
    std::string source;          ///< "lcsc", "file", "url"
    nlohmann::json options;      ///< Plugin-specific options
};

struct ExportContext {
    std::string format;          ///< "kicad", "csv", "bom", "zip"
    std::string output_path;
    std::vector<core::Uuid> component_ids;
    nlohmann::json options;
};

// ============================================================
// Base plugin interface (plain C++, no Qt)
// ============================================================
class IPlugin {
public:
    virtual ~IPlugin() = default;

    /// Called after loading. Plugin receives its context for API access.
    virtual util::Result<void> initialize(IPluginContext* ctx) = 0;

    /// Called before unloading.
    virtual void shutdown() = 0;

    /// Unique identifier (from manifest).
    virtual std::string id() const = 0;
    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
    virtual std::string description() const = 0;
    virtual PluginCapability capabilities() const = 0;
};

// ============================================================
// Specialized plugin interfaces
// ============================================================
class IImportPlugin : public virtual IPlugin {
public:
    /// Execute an import operation. Called from a worker thread.
    virtual util::Result<ImportResult> execute_import(
        const ImportContext& ctx) = 0;
};

class IExportPlugin : public virtual IPlugin {
public:
    /// Execute an export operation.
    virtual util::Result<void> execute_export(
        const ExportContext& ctx) = 0;
};

class IToolPlugin : public virtual IPlugin {
public:
    /// Launch a tool window. window_parent is an opaque native window handle (HWND on Windows, GdkWindow* on Linux).
    /// Returns true if the tool ran successfully.
    virtual bool execute_tool(void* window_parent) = 0;
};

// Factory function types (after IPlugin is complete)
using PluginFactoryFn = IPlugin* (*)();
using PluginDestroyFn = void (*)(IPlugin*);

}  // namespace kforge::plugin
