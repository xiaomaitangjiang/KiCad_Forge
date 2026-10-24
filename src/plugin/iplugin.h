#pragma once

#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <optional>

#include <nlohmann/json.hpp>

#include "core/model/types.h"
#include "core/model/symbol.h"
#include "core/model/footprint.h"
#include "core/model/model_3d.h"
#include "core/model/component.h"
#include "util/error.h"
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
    std::string trigger;         // "dialog" | "inline" | "panel" | "hidden"
    std::string ui_template;     // UI template: "form" (default) | "two_step"
    std::string analyze_action;  // two_step: action id run for the analyze step
    std::string preflight;       // dialog: action id run on open to show context (e.g. "status")
    // 宿主侧协议：非空表示该 action 由宿主接管（如 "staged-import-v1"），
    // 插件只在暂存目录里产出文件，目标库由宿主校验并提交
    std::string protocol;
    // v2 manifest: the action may be cancelled by the user (default false).
    // Read-only actions can set this; write actions stay false in phase 1.
    bool cancelable{false};
    std::vector<std::string> auto_args;  // args the host fills in automatically (e.g. "target_library")
    bool button_show{true};      // show button in toolbar
    std::string button_style;    // "icon" | "text" | "both" (default "both")
    std::string button_location; // "toolbar" | "sidebar" (default "toolbar")
    std::string button_tooltip;  // tooltip for icon-only buttons
    nlohmann::json schema;       // form fields for trigger=dialog
};

// ============================================================
// Plugin web frontend (manifest "frontend" block)
// ============================================================

/// Declares a web frontend bundled inside the plugin directory that the host
/// serves under /plugin-assets/<plugin-id>/.
///
/// All three fields are URL-style relative paths: `root` is relative to the
/// plugin directory, `entry`/`styles` are relative to `root`. They must never
/// be absolute, contain backslashes or drive letters, or contain "." / ".."
/// components — PluginManifest::from_json rejects such manifests outright.
struct PluginFrontend {
    std::string root{"dist"};          // asset root, relative to the plugin dir
    std::string entry{"index.js"};     // module loaded by the UI, relative to root
    std::vector<std::string> styles;   // stylesheets, relative to root
};

// ============================================================
// Manifest v2 declarations (contract graph, engines, python block)
// ============================================================

/// One contract the plugin frontend provides ("frontend.provides[]").
struct PluginContractRef {
    std::string contract;  // e.g. "demo.supplier"
    std::string version;   // provided version, e.g. "1.0.0"
};

/// One contract the plugin frontend requires ("frontend.requires[]").
struct PluginContractRequirement {
    std::string contract;  // e.g. "kiforge.symbols"
    std::string range;     // semver-style range, e.g. ">=1.0.0 <2.0.0"
};

/// Host/SDK compatibility ranges declared under "engines" (v2).
struct PluginEngines {
    std::string kiforge;  // e.g. ">=1.0.0-rc.1 <2.0.0"
    std::string sdk;      // e.g. ">=1.0.0-rc.1 <2.0.0"
};

/// Python half of a v2 manifest ("python" block). v1 manifests keep the
/// top-level "entry"/"actions" fields instead; from_json() copies whichever
/// form applies into PluginManifest::entry_point/actions for the executor and
/// the legacy UI, so callers never need to special-case the version.
struct PluginPython {
    std::string entry{"plugin.py"};
    std::vector<PluginAction> actions;
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
    std::optional<PluginFrontend> frontend;  // present only when manifest declares one
    // --- manifest v2 (v1 manifests keep the defaults) ---
    int manifest_version{1};                 // absent manifestVersion means 1
    std::optional<PluginPython> python;      // v2 "python" block
    std::optional<PluginEngines> engines;    // v2 "engines" block
    nlohmann::json config;                   // v2 "config" (arbitrary JSON)
    std::optional<nlohmann::json> contributes;        // frontend.contributes (arbitrary JSON)
    std::vector<PluginContractRef> provides;           // frontend.provides[]
    // "requires" is a C++ keyword — the manifest's frontend.requires[] lives
    // here as contract_requires.
    std::vector<PluginContractRequirement> contract_requires;
    std::vector<PluginAction> actions;
    bool one_click{false};
    std::vector<std::string> capabilities;
    std::vector<std::string> platforms;

    static util::Result<PluginManifest> from_json(const nlohmann::json& doc);

    /// Reads the manifest straight from a filesystem::path — the standard
    /// ifstream(path) overload keeps non-ASCII directories working on Windows
    /// (no narrowing through the system ANSI code page). Reuses from_json().
    static util::Result<PluginManifest> from_file(const std::filesystem::path& path);

    /// Back-compat overload for callers holding a narrow C string. A separate
    /// const char* (not std::string) overload is needed: for a string literal
    /// both "const char* → std::filesystem::path" and "const char* → std::string"
    /// are user-defined conversions of equal rank, which made the call
    /// ambiguous. std::string arguments keep converting to path implicitly, so
    /// no std::string overload is declared. Both forward to the path overload —
    /// the JSON parsing is not duplicated.
    static util::Result<PluginManifest> from_file(const char* path)
    {
        return from_file(std::filesystem::path(path));
    }
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
