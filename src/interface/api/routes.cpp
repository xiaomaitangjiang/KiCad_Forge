// Route handlers extracted from setup_routes()
#include "classifier/rule_loader.h"
#include "core/model/type_registry.h"
#include "core/repo/repositories.h"
#include "interface/api/api_server.h"
#include "interface/api/footprint_status_json.h"
#include "interface/api/library_manifest.h"
#include "interface/api/import_orchestrator.h"
#include "interface/api/symbol_summary_json.h"
#include "interface/api/routes.h"
#include "interface/api/staged_import.h"
#include "interface/api/type_matching.h"
#include "interface/api/type_mutation.h"
#include "interface/service/classification/service.h"
#include "interface/service/correspondence/service.h"
#include "interface/service/editing/export.h"
#include "interface/service/editing/history.h"
#include "interface/service/editing/move.h"
#include "interface/service/editing/paste.h"
#include "interface/service/import/pipeline.h"
#include "interface/service/import/source_path.h"
#include "interface/service/library/binding.h"
#include "interface/service/library/footprint_resolver.h"
#include "interface/service/library/service.h"
#include "interface/service/library/source_policy.h"
#include "platform/folder_dialog.h"
#include "platform/open_external.h"
#include "plugin/plugin_manager.h"
#include "plugin/task_service.h"
#include "util/config_store.h"
#include "util/logger.h"
#include "util/platform.h"
#include "util/result.h"
#include "version.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

namespace kforge::api
{

namespace
{
std::optional<json> component_path_error(const json& body)
{
    for (const auto& [field, kind] : {
             std::pair{"symbol_path", services::LibrarySourceKind::Symbols},
             std::pair{"footprint_path", services::LibrarySourceKind::Footprints},
             std::pair{"model_3d_path", services::LibrarySourceKind::Models}})
    {
        const std::string path = body.value(field, "");
        if (path.empty()) continue;
        if (auto valid = services::validate_library_source_path(path, kind, true); !valid)
            return json{{"ok", false}, {"error_code", "invalid_library_path"},
                        {"field", field}, {"error", valid.error().format_message()}};
    }
    return std::nullopt;
}

/// Content-Type for a plugin frontend asset, by extension. Plugins ship static
/// web assets; anything unknown is served as opaque bytes.
std::string plugin_asset_content_type(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".js" || extension == ".mjs") return "text/javascript; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".json" || extension == ".map") return "application/json";
    if (extension == ".html" || extension == ".htm") return "text/html; charset=utf-8";
    if (extension == ".txt") return "text/plain; charset=utf-8";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".ico") return "image/x-icon";
    if (extension == ".woff2") return "font/woff2";
    if (extension == ".woff") return "font/woff";
    if (extension == ".ttf") return "font/ttf";
    if (extension == ".otf") return "font/otf";
    if (extension == ".wasm") return "application/wasm";
    return "application/octet-stream";
}

/// Resolve a manifest-declared file at the plugin root (e.g. an icon) and keep
/// it inside the plugin directory. The legacy code joined the raw URL/manifest
/// string onto the plugin directory; on Windows a backslash segment escaped it.
/// Only regular files whose canonical path stays strictly below the canonical
/// plugin directory are accepted. The value is UTF-8 (manifest contract).
std::optional<std::filesystem::path> resolve_plugin_root_file(plugin::PluginManager* plugins,
                                                              const std::string& plugin_id,
                                                              const std::string& relative)
{
    if (plugins == nullptr || relative.empty())
        return std::nullopt;
    try
    {
        const auto plugin_dir = plugins->plugin_path(plugin_id);
        if (plugin_dir.empty())
            return std::nullopt;
        const auto root = std::filesystem::weakly_canonical(plugin_dir);
        const auto candidate =
            std::filesystem::weakly_canonical(root / services::path_from_utf8(relative));
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error) || error)
            return std::nullopt;
        auto root_it = root.begin();
        auto candidate_it = candidate.begin();
        for (; root_it != root.end(); ++root_it, ++candidate_it)
        {
            if (candidate_it == candidate.end())
                return std::nullopt;
#ifdef _WIN32
            if (::CompareStringOrdinal(root_it->native().c_str(), -1,
                                       candidate_it->native().c_str(), -1, TRUE) != CSTR_EQUAL)
                return std::nullopt;
#else
            if (*root_it != *candidate_it)
                return std::nullopt;
#endif
        }
        if (candidate_it == candidate.end())
            return std::nullopt;  // the plugin directory itself is not a file
        return candidate;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

// ------------------------------------------------------------
// Plugin configuration route dependencies
// ------------------------------------------------------------
// ApiServer::setup_routes() wires the plugin routes and the settings routes
// separately: register_plugin_routes() receives the PluginManager but not the
// ConfigStore, while register_settings_routes() receives the ConfigStore but
// not the plugins. /api/plugin-config needs both, so each registration
// publishes its dependency here and the handlers resolve them per request (all
// registrations finish before the HTTP server starts accepting connections).
// A second ConfigStore over the same file would keep its own in-memory
// snapshot and clobber concurrent writes, hence the shared instance is reused.
struct PluginConfigDeps
{
    plugin::PluginManager* plugins{nullptr};
    util::ConfigStore* config{nullptr};
};

PluginConfigDeps& plugin_config_deps()
{
    static PluginConfigDeps deps;
    return deps;
}

/// Per-plugin configuration is namespaced below the flat keys /api/config uses.
std::string plugin_config_key(const std::string& plugin_id)
{
    return "plugins." + plugin_id + ".config";
}

/// "Discovered" matches the same state /api/plugins reports (loaded or not).
bool plugin_is_discovered(plugin::PluginManager* plugins, const std::string& plugin_id)
{
    return plugins != nullptr && !plugins->plugin_path(plugin_id).empty();
}

/// 64 KiB cap on one plugin's serialized configuration document.
constexpr std::size_t kMaxPluginConfigBytes = 64u * 1024u;

void send_json(httplib::Response& response, const json& body, int status = 200)
{
    response.status = status;
    response.set_content(body.dump(), "application/json");
}

// ------------------------------------------------------------
// Phase-4 task dispatch for POST /api/tasks.
// ------------------------------------------------------------

// TaskService deps are published by register_plugin_task_routes(); other
// handlers resolve them per request, so registration order does not matter.
struct PluginTaskDeps
{
    plugin::TaskService* tasks{nullptr};
    plugin::PluginManager* plugins{nullptr};
    storage::Database* db{nullptr};
    util::ConfigStore* config{nullptr};
};

PluginTaskDeps& plugin_task_deps()
{
    static PluginTaskDeps deps;
    return deps;
}

json task_record_json(const plugin::TaskRecord& record)
{
    json out;
    out["task_id"] = record.task_id;
    out["plugin_id"] = record.plugin_id;
    out["action"] = record.action;
    out["state"] = plugin::task_state_name(record.state);
    if (record.phase != plugin::TaskPhase::None)
        out["phase"] = plugin::task_phase_name(record.phase);
    out["cancelable"] = record.cancelable;
    out["cancel_requested"] = record.cancel_requested;
    out["revision"] = record.revision;
    out["created_at"] = record.created_at_ms;
    if (record.started_at_ms > 0)
        out["started_at"] = record.started_at_ms;
    if (record.finished_at_ms > 0)
        out["finished_at"] = record.finished_at_ms;
    if (record.result)
        out["result"] = *record.result;
    if (!record.error.empty())
        out["error"] = record.error;
    out["uncertain"] = record.uncertain;
    if (record.client_request_id)
        out["client_request_id"] = *record.client_request_id;
    return out;
}

/// 旧同步入口的响应包装：外层 ok 只由任务终态决定，业务 JSON 原样放进
/// plugin_result，失败原因仍可见。任务结果里的 ok:false 不能把 failed 伪装成成功。
json legacy_task_response(const plugin::TaskRecord& record)
{
    json out;
    out["ok"] = record.state == plugin::TaskState::Succeeded;
    if (record.result)
        out["plugin_result"] = *record.result;
    if (!record.error.empty())
        out["error"] = record.error;
    if (record.uncertain)
        out["uncertain"] = true;
    return out;
}

struct PreparedTaskRequest
{
    std::string plugin_id;
    std::string action;
    bool cancelable{false};
    bool staged{false};
    std::string generic_args{"{}"};
    std::optional<StagedImporter::Request> staged_request;
};

struct TaskRequestProblem
{
    std::string code;  // plugin_unknown | action_unknown | invalid_request | unavailable
    std::string message;
};

/// Resolve the manifest action and prepare the execution request. The staged
/// protocol is dispatched to the full StagedImporter path (never plain Python);
/// everything else runs the plugin entry point with the raw args JSON.
std::optional<TaskRequestProblem> prepare_task_request(plugin::PluginManager* plugins,
                                                       storage::Database* db,
                                                       const std::string& plugin_id,
                                                       const std::string& action,
                                                       const json& args,
                                                       PreparedTaskRequest& prepared)
{
    prepared = PreparedTaskRequest{};
    prepared.plugin_id = plugin_id;
    prepared.action = action;
    if (plugins == nullptr || db == nullptr)
        return TaskRequestProblem{"unavailable", "Task service is unavailable"};

    std::optional<plugin::PluginAction> declared;
    bool plugin_found = false;
    for (const auto& manifest : plugins->available_plugins())
    {
        if (manifest.id != plugin_id)
            continue;
        plugin_found = true;
        for (const auto& candidate : manifest.actions)
        {
            if (candidate.id == action)
            {
                declared = candidate;
                break;
            }
        }
        break;
    }
    if (!plugin_found)
        return TaskRequestProblem{"plugin_unknown", "Plugin not found: " + plugin_id};
    if (!declared && action == "info")
    {
        // Legacy metadata action: every Python entry point supports `info`
        // without declaring it in manifest.actions. Keep the old synchronous
        // /execute contract working; no staged/import capability is granted.
        declared = plugin::PluginAction{};
        declared->id = "info";
        declared->cancelable = false;
    }
    if (!declared)
        return TaskRequestProblem{"action_unknown",
                                 "Unknown action for plugin " + plugin_id + ": " + action};
    // Command-line injection guard (the value travels into plugin argv).
    if (action.empty() ||
        action.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "0123456789_-") != std::string::npos)
    {
        return TaskRequestProblem{"invalid_request", "Invalid action: " + action};
    }
    prepared.cancelable = declared->cancelable;

    if (declared->protocol == "staged-import-v1")
    {
        prepared.staged = true;
        if (!args.is_object())
            return TaskRequestProblem{"invalid_request",
                                      "Staged import args must be a JSON object"};
        auto fields = parse_staged_request(args, plugin_id, action);
        if (!fields)
            return TaskRequestProblem{"invalid_request", fields.error().format_message()};
        auto request = fields->request;
        if (request.footprint_mode == "reuse" && request.footprint_id.empty())
        {
            // Legacy frontends only give library/name; resolve the unique
            // registered footprint (0 or many is an error, never "first").
            auto session = db->session();
            auto resolved = storage::FootprintRepository(session).resolve_legacy_reference(
                fields->legacy_library, fields->legacy_name);
            if (!resolved)
                return TaskRequestProblem{"invalid_request", resolved.error().format_message()};
            request.footprint_id = resolved->id();
        }
        prepared.staged_request = std::move(request);
    }
    else
    {
        prepared.generic_args = args.dump();
    }
    return std::nullopt;
}

plugin::TaskService::Runner make_task_runner(const PreparedTaskRequest& prepared,
                                             plugin::PluginManager* plugins,
                                             storage::Database* db)
{
    if (prepared.staged && prepared.staged_request)
    {
        const StagedImporter::Request request = *prepared.staged_request;
        return [plugins, db, request](plugin::TaskService::Context& ctx) -> util::Result<json>
        {
            StagedImporter importer(*db, *plugins);
            StagedImporter::RunHooks hooks;
            hooks.cancelled = [&ctx] { return ctx.cancelled(); };
            hooks.on_phase = [&ctx](const std::string& phase)
            {
                if (phase == "staging")
                    ctx.set_phase(plugin::TaskPhase::Staging);
                else if (phase == "committing")
                    ctx.set_phase(plugin::TaskPhase::Committing);
            };
            hooks.try_enter_commit = [&ctx] { return ctx.try_enter_commit(); };
            return importer.run(request, hooks);
        };
    }

    const std::string plugin_id = prepared.plugin_id;
    const std::string action = prepared.action;
    const std::string args = prepared.generic_args;
    const bool cancelable = prepared.cancelable;
    return [plugins, plugin_id, action, args, cancelable](
               plugin::TaskService::Context& ctx) -> util::Result<json>
    {
        auto output = plugins->execute(plugin_id, action, args,
                                       [&ctx] { return ctx.cancelled(); });
        if (!output)
        {
            const std::string message = output.error().message();
            const bool interrupted = message.find("timed out") != std::string::npos ||
                                     message.find("exceeded") != std::string::npos ||
                                     message.find("cancelled") != std::string::npos ||
                                     message.find("stopped") != std::string::npos;
            // Write actions (manifest cancelable=false) may have changed their
            // target before the interruption: report uncertainty, never "no change".
            if (interrupted && !cancelable && !ctx.cancel_requested())
                ctx.set_uncertain(true);
            return std::unexpected(output.error());
        }
        try
        {
            return json::parse(*output);
        }
        catch (const std::exception& error)
        {
            return std::unexpected(util::Error::make<util::Error::Kind::ParseError>(
                std::string("Plugin output is not JSON: ") + error.what()));
        }
    };
}
}  // namespace

// ImportManager 杩斿洖 pipeline 鐨勭湡瀹炴姤鍛婏細鎬昏鏁扮嫭绔嬩簬鏈夐檺鐨勯敊璇?鍛婅鏄庣粏銆?
inline json import_result_json(const services::ImportPipeline::Result& result)
{
    json messages = json::array();
    for (const auto& error : result.errors)
        if (messages.size() < services::kMaxReportMessages) messages.push_back(error);
    for (const auto& warning : result.warnings)
        if (messages.size() < services::kMaxReportMessages) messages.push_back(warning);
    json out = {{"ok", result.ok()}, {"symbols", result.symbols},
                {"footprints", result.footprints}, {"models_3d", result.models_3d},
                {"skipped", result.skipped}, {"linked_symbols", result.linked_symbols},
                {"linked_models", result.linked_models}, {"errors", result.error_count},
                {"warnings", result.warning_count}, {"error_count", result.error_count},
                {"warning_count", result.warning_count}, {"failed_files", result.failed_files},
                {"error_messages", result.errors}, {"warning_messages", result.warnings},
                {"messages", std::move(messages)}, {"matched", result.matched},
                {"unmatched", result.unmatched}, {"conflicts", result.conflicts},
                {"input_too_long", result.input_too_long},
                {"evaluation_limited", result.evaluation_limited},
                {"evaluation_errors", result.evaluation_errors}};
    if (!result.ok())
        out["error"] = result.errors.empty() ? "Import failed; see report counts" : result.errors.front();
    return out;
}

// ---- Search helpers ----

// Execute a statement with one bound text parameter (?1) 鈥?no string splicing.
// Returns false and fills `error` on failure.
inline bool exec_param(sqlite3* db, const char* sql, const std::string& param,
                       std::string& error)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        error = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_text(stmt, 1, param.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
    {
        error = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

// Clamped "limit" query param (1..500); returns default when absent/invalid.
inline int param_limit(const httplib::Request& req, int default_limit)
{
    auto v = req.get_param_value("limit");
    if (v.empty())
    {
        return default_limit;
    }
    try
    {
        return std::clamp(std::stoi(v), 1, 500);
    }
    catch (const std::exception&)
    {
        return default_limit;
    }
}

// ---- In-memory index holder: rebuilt when an import has finished, so
// footprints/models imported after process start become searchable.
// Defined at namespace scope so its static instance can outlive any local
// lambda capture and survive the static-init order issues local classes have.
struct BindingMgrHolder
{
    storage::Database* db{nullptr};
    ImportOrchestrator* orch_ptr{nullptr};
    std::mutex mtx;
    int64_t last_seq{0};
    int64_t last_import_seq{0};
    std::shared_ptr<services::SymbolBindingManager> mgr;

    void init(storage::Database& d, ImportOrchestrator* o)
    {
        db = &d;
        orch_ptr = o;
    }

    std::shared_ptr<services::SymbolBindingManager> get()
    {
        std::lock_guard lock(mtx);
        // 缁熶竴鏁版嵁鐗堟湰锛氬鍏ャ€佹簮鏂囦欢鍙樻洿銆佸垹闄ら兘浼氳嚜澧?鈥斺€?
        // 鍙洴瀵煎叆搴忓彿浼氳缁戝畾绱㈠紩鍦ㄥ埆鐨勫啓鍏ヤ箣鍚庣户缁敤鏃у揩鐓?
        auto cur = db ? db->data_version() : 0;
        auto imported = orch_ptr ? orch_ptr->import_seq() : 0;
        if (!mgr || cur != last_seq || imported != last_import_seq)
        {
            mgr = std::make_shared<services::SymbolBindingManager>(*db);
            last_seq = cur;
            last_import_seq = imported;
        }
        return mgr;
    }
};
static BindingMgrHolder g_binding_mgr;

// ---- Lock helpers: a locked component library protects its symbols from
// delete/modify/bind/classify. Official KiCad libs are locked & protected.
static bool library_locked(const storage::DbSession& session,
                           const std::string& component_lib_id)
{
    storage::ComponentLibraryRepository repo(session);
    if (auto all = repo.find_all())
    {
        for (auto& l : *all)
        {
            if (l.id == component_lib_id && l.locked)
            {
                return true;
            }
        }
    }
    return false;
}

// Symbol 鈫?library(id) 鈫?component_library_id 鈫?locked
static bool symbol_locked(const storage::DbSession& session, const std::string& symbol_id)
{
    storage::SymbolRepository sr(session);
    auto sym = sr.find_by_id(symbol_id);
    if (!sym)
    {
        return false;
    }
    storage::LibraryRepository lr(session);
    if (auto all = lr.find_all())
    {
        for (auto& l : *all)
        {
            if (l.id == sym->library_id() && !l.component_library_id.empty())
            {
                return library_locked(session, l.component_library_id);
            }
        }
    }
    return false;
}

// Footprint 鈫?component library by matching library_path prefix
static bool footprint_locked(const storage::DbSession& session, const std::string& footprint_id)
{
    storage::FootprintRepository fr(session);
    auto fp = fr.find_by_id(footprint_id);
    if (!fp || fp->library_path().empty())
    {
        return false;
    }
    const auto& fp_path = fp->library_path();
    storage::ComponentLibraryRepository repo(session);
    if (auto all = repo.find_all())
    {
        for (auto& l : *all)
        {
            if (!l.footprint_path.empty() && l.locked &&
                fp_path.find(l.footprint_path) != std::string::npos)
            {
                return true;
            }
        }
    }
    return false;
}

static std::string short_uuid()
{
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(dis(gen)));
    return buf;
}

// ============================================================
// Library routes
// ============================================================

void register_library_routes(httplib::Server& srv, storage::Database& db, ImportOrchestrator* import_orch)
{
    srv.Get("/api/libraries",
            [&db](const httplib::Request&, httplib::Response& r)
            {
                auto session = db.session();
                storage::SymbolRepository sr(session);
                storage::LibraryRepository lr(session);
                auto libs = lr.find_all();
                auto symbol_counts = sr.count_by_library();
                if (!symbol_counts)
                {
                    r.status = 500;
                    r.set_content(json{{"ok", false},
                                       {"error", symbol_counts.error().format_message()}}.dump(),
                                  "application/json");
                    return;
                }
                storage::ComponentLibraryRepository cl_repo(session);
                std::unordered_map<std::string, std::string> group_name;
                std::unordered_map<std::string, bool> cl_locked;
                auto comp_libs = cl_repo.find_all();
                if (comp_libs)
                {
                    for (auto& cl : *comp_libs)
                    {
                        group_name[cl.id] = cl.name;
                        cl_locked[cl.id] = cl.locked;
                    }
                }
                json arr = json::array();
                if (libs)
                {
                    for (auto& l : *libs)
                    {
                        if (l.name.contains("_footprints"))
                        {
                            continue;
                        }
                        json o;
                        o["id"] = l.id;
                        o["name"] = l.name;
                        o["file_path"] = l.file_path.string();
                        o["description"] = l.description;
                        o["component_library_id"] = l.component_library_id;  // workspace filter
                        if (!l.component_library_id.empty())
                        {
                            o["group"] = group_name[l.component_library_id];
                            o["locked"] = cl_locked[l.component_library_id];
                        }
                        else
                        {
                            o["locked"] = false;
                        }
                        const auto count = symbol_counts->find(l.id);
                        o["symbol_count"] = count == symbol_counts->end() ? 0 : count->second;
                        arr.push_back(o);
                    }
                }
                r.set_content(arr.dump(), "application/json");
            });

    // Init the binding manager holder once with the orchestrator pointer
    // before any /api/symbols/{id}/binding route runs.
    {
        auto* the_orch = import_orch;
        g_binding_mgr.init(db, the_orch);
    }

    srv.Post("/api/libraries",
             [&db](const httplib::Request& req, httplib::Response& r)
             {
                 json j;
                 try
                 {
                     auto body = json::parse(req.body.empty() ? "{}" : req.body);
                     std::string action = body.value("action", "");
                     if (action == "delete")
                     {
                         bool history_changed = false;
                         {
                             auto session = db.session();
                             std::string id = body.value("id", "");
                             storage::LibraryRepository lrr(session);
                             storage::SymbolRepository sr(session);
                             auto libs = lrr.find_all();
                             std::string file_path;
                             std::string component_library_id;
                             if (libs)
                                 for (auto& l : *libs)
                                     if (l.id == id)
                                     {
                                         file_path = l.file_path.string();
                                         component_library_id = l.component_library_id;
                                     }
                             // Unified protection: owner lock OR the file living in
                             // an official/locked source path (share/kicad, any
                             // locked component library root). File deletion must
                             // never bypass the app-level guard via a missing owner.
                             bool protected_source = false;
                             if (!component_library_id.empty() &&
                                 library_locked(session, component_library_id))
                             {
                                 protected_source = true;
                             }
                             if (!protected_source && !file_path.empty())
                             {
                                 if (auto locked = services::source_is_locked(session, file_path))
                                 {
                                     protected_source = *locked;
                                 }
                                 else
                                 {
                                     j["ok"] = false;
                                     j["error"] = locked.error().format_message();
                                 }
                             }
                             if (protected_source)
                             {
                                 j["ok"] = false;
                                 j["error"] = "Library is locked";
                             }
                             else if (j.contains("error"))
                             {
                                 // protection lookup failed 鈥?reported above
                             }
                             else
                             {
                                 int removed = 0;
                                 auto syms = sr.find_by_library(id);
                                 if (syms)
                                     for (auto& s : *syms)
                                         if (sr.remove(s.id()))
                                             removed++;
                                 // delete_file=false 鈫?unregister only (DB row removed,
                                 // the .kicad_sym file on disk stays)
                                 const bool delete_file = body.value("delete_file", true);
                                 bool file_deleted = false;
                                 if (delete_file && !file_path.empty() &&
                                     std::filesystem::exists(file_path))
                                 {
                                     std::filesystem::remove(file_path);
                                     file_deleted = true;
                                 }
                                 auto db_r = lrr.remove(id);
                                 j["ok"] = db_r.has_value();
                                 history_changed = db_r.has_value();
                                 j["unregistered_only"] = !delete_file;
                                 if (file_deleted)
                                     j["deleted_file"] = file_path;
                             }
                         }
                         // 先释放 DbSession 再记历史：锁序固定为
                         // editing_commit_mutex -> DB session，避免与 undo 倒序互等。
                         if (history_changed)
                             services::note_successful_edit();
                     }
                     else if (action == "delete_symbol")
                     {
                         std::string sym_id = body.value("id", "");
                         bool locked = false;
                         {
                             auto session = db.session();
                             locked = symbol_locked(session, sym_id);
                         }
                         if (locked)
                         {
                             j["ok"] = false;
                             j["error"] = "Symbol belongs to a locked library";
                         }
                         else
                         {
                             // 婧愭枃浠舵敼鍐?+ 琛屽垹闄ょ敱缁熶竴鍙樻洿鏈嶅姟鎺ョ锛圓05锛夛細
                             // 澶辫触鏃舵枃浠朵笌搴撹涓€璧峰洖閫€锛屼笉鍐嶅悇鍐欏悇鐨?
                             services::LibraryService library_service(&db);
                             auto removed = library_service.delete_symbol(sym_id);
                             j["ok"] = removed.has_value();
                             if (!removed)
                                 j["error"] = removed.error().format_message();
                         }
                     }
                     else if (!action.empty() || body.contains("name"))
                     {
                         std::string name = body.value("name", "");
                         if (!name.empty())
                         {
                             auto session = db.session();
                             storage::LibraryRepository repo(session);
                             core::LibraryMeta lib;
                             lib.name = name;
                             lib.file_path = name + ".kicad_sym";
                             auto ins = repo.insert(lib);
                             j["ok"] = ins.has_value();
                         }
                         else
                         {
                             j["ok"] = false;
                         }
                     }
                 }
                 catch (...)
                 {
                     j["ok"] = false;
                     j["error"] = "Parse error";
                 }
                 r.set_content(j.dump(), "application/json");
             });
}

// ============================================================
// Symbol routes
// ============================================================

void register_library_manifest_routes(httplib::Server& srv, storage::Database& db)
{
    srv.Post("/api/library-manifest",
             [&db](const httplib::Request& req, httplib::Response& r)
             {
                 json body;
                 try
                 {
                     body = json::parse(req.body.empty() ? "{}" : req.body);
                 }
                 catch (const std::exception&)
                 {
                     send_json(r, {{"ok", false}, {"error", "Malformed JSON body"}}, 400);
                     return;
                 }
                 const std::string root = body.value("root", "");
                 const std::string target = body.value("target", "");
                 if (root.empty())
                 {
                     send_json(r, {{"ok", false}, {"error", "Repository root is required"}}, 400);
                     return;
                 }
                 auto manifest = build_library_manifest(db, root, target);
                 const bool ok = manifest.value("ok", false);
                 send_json(r, manifest, ok ? 200 : 400);
             });
}

void register_symbol_routes(httplib::Server& srv, storage::Database& db)
{
    srv.Get("/api/symbols",
            [&db](const httplib::Request& req, httplib::Response& r)
            {
                std::string q = req.get_param_value("q");
                std::string lib_id = req.get_param_value("library");
                // 鍒嗛〉/绛涢€?locked/蹇収鍦ㄥ悓涓€涓?session 閲屽彇瀹岋紝閲婃斁浼氳瘽涔嬪悗鍐嶅仛纾佺洏妫€鏌?
                services::Snapshot snapshot;
                std::unordered_map<std::string, bool> lib_locked;
                std::vector<core::Symbol> page_symbols;
                const auto request_started = std::chrono::steady_clock::now();
                {
                    const auto wait_started = std::chrono::steady_clock::now();
                    auto session = db.session();
                    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - wait_started);
                    kforge::util::log_debug{}("symbols list: db session acquired after {} ms", waited.count());
                    storage::SymbolRepository repo(session);
                    // 列表页只读摘要列：properties_json 是全量快照里最贵的部分，
                    // 分页发生在 SQL 之后，先读全量会把 session 锁住几十秒。
                    auto result = repo.find_summaries(q, lib_id);
                    if (!result)
                    {
                        r.status = 500;
                        json j;
                        j["error"] = result.error().format_message();
                        r.set_content(j.dump(), "application/json");
                        return;
                    }
                    // Pagination: clamp to safe ranges 鈥?a negative offset or an
                    // oversized limit must never index outside the vector or
                    // overflow start+limit.
                    int limit = 0, offset = 0;
                    try
                    {
                        auto lv = req.get_param_value("limit");
                        if (!lv.empty())
                            limit = std::stoi(lv);
                        auto ov = req.get_param_value("offset");
                        if (!ov.empty())
                            offset = std::stoi(ov);
                    }
                    catch (const std::exception&)
                    {
                    }
                    offset = std::max(0, offset);
                    limit = std::clamp(limit, 0, 10000);  // 0 = no limit
                    int total = static_cast<int>(result->size());
                    int start = std::min(offset, total);
                    int end = (limit > 0) ? std::min(start + limit, total) : total;
                    page_symbols.assign(result->begin() + start, result->begin() + end);
                    // Resolve lock state once: library_id 鈫?locked, via
                    // libraries.component_library_id 鈫?component_libraries.locked
                    storage::ComponentLibraryRepository clr(session);
                    std::unordered_map<std::string, bool> cl_locked;
                    if (auto cls = clr.find_all())
                        for (auto& cl : *cls)
                            cl_locked[cl.id] = cl.locked;
                    storage::LibraryRepository lr(session);
                    if (auto libs = lr.find_all())
                        for (auto& l : *libs)
                        {
                            auto it = cl_locked.find(l.component_library_id);
                            lib_locked[l.id] = it != cl_locked.end() && it->second;
                        }
                    // 一次取完关联表：读失败整体 500，DB 错误绝不能被当成
                    // 「这些符号都没有封装」。
                    auto loaded = services::load_snapshot_light(session);
                    if (!loaded)
                    {
                        r.status = 500;
                        json j;
                        j["error"] = loaded.error().format_message();
                        r.set_content(j.dump(), "application/json");
                        return;
                    }
                    snapshot = std::move(*loaded);
                }
                services::FileCheckContext files;  // 鏈〉娑夊強鐨勬枃浠跺悇 stat 涓€娆?
                json arr = json::array();
                for (auto& sym : page_symbols)
                {
                    json s = symbol_summary_json(sym, lib_locked[sym.library_id()]);
                    // 鐘舵€佺敱缁熶竴 resolver 鍐冲畾锛氬紩鐢ㄩ潪绌?鈮?灏佽瀛樺湪
                    auto resolved = services::resolve_symbol(sym, snapshot, files);
                    api::apply_footprint_status(s, resolved);
                    arr.push_back(s);
                }
                r.set_content(arr.dump(), "application/json");
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - request_started);
                kforge::util::log_debug{}("symbols list: {} rows in {} ms", arr.size(), elapsed.count());
            });

    srv.Get("/api/symbols/([^/]+)",
            [&db](const httplib::Request& req, httplib::Response& response)
            {
                auto session = db.session();
                storage::SymbolRepository symbols(session);
                auto symbol = symbols.find_by_id(req.matches[1]);
                if (!symbol)
                {
                    const bool missing = symbol.error().kind() == util::Error::Kind::NotFound;
                    send_json(response, {{"error", symbol.error().format_message()}}, missing ? 404 : 500);
                    return;
                }
                auto locked = symbols.is_library_locked(symbol->id());
                if (!locked)
                {
                    send_json(response, {{"error", locked.error().format_message()}}, 500);
                    return;
                }
                send_json(response, symbol_summary_json(*symbol, *locked));
            });

    srv.Get("/api/footprints",
            [&db](const httplib::Request&, httplib::Response& r)
            {
                services::Snapshot snapshot;
                std::vector<core::Footprint> rows;
                {
                    auto session = db.session();
                    auto result = storage::FootprintRepository(session).find_all();
                    if (!result)
                    {
                        r.status = 500;
                        json j;
                        j["error"] = result.error().format_message();
                        r.set_content(j.dump(), "application/json");
                        return;
                    }
                    rows = std::move(*result);
                    auto loaded = services::load_snapshot(session);
                    if (!loaded)
                    {
                        r.status = 500;
                        json j;
                        j["error"] = loaded.error().format_message();
                        r.set_content(j.dump(), "application/json");
                        return;
                    }
                    snapshot = std::move(*loaded);
                }
                services::FileCheckContext files;
                json arr = json::array();
                for (auto& fp : rows)
                {
                    json f;
                    f["id"] = fp.id();
                    f["name"] = fp.name();
                    f["description"] = fp.description();
                    f["pad_count"] = fp.pad_count();
                    // 鍒楄〃鍙垽瀹氬皝瑁呮湰韬細涓嶄负鏁村簱鍊欓€夊幓 stat 鍏ㄩ儴妯″瀷鏂囦欢
                    auto resolved = services::inspect_footprint(fp, snapshot, files, false);
                    api::apply_footprint_candidate_status(f, resolved);
                    arr.push_back(f);
                }
                r.set_content(arr.dump(), "application/json");
            });

    srv.Get("/api/symbols/([^/]+)/binding",
            [](const httplib::Request& req, httplib::Response& r)
            {
                auto id = req.matches[1];
                auto mgr = g_binding_mgr.get();
                auto b = mgr->get(id);
                if (!b)
                {
                    // 璇诲け璐?绗﹀彿涓嶅瓨鍦細闈?2xx + JSON error锛岀粷涓嶈兘琚綋鎴愩€屾病鏈夌粦瀹氥€?
                    r.status = (b.error().kind() == util::Error::Kind::NotFound) ? 404 : 500;
                    json j;
                    j["error"] = b.error().format_message();
                    r.set_content(j.dump(), "application/json");
                    return;
                }
                json j;
                j["footprint_id"] = b->footprint_id;
                j["footprint_name"] = b->footprint_name;
                j["model_id"] = b->model_id;
                j["model_name"] = b->model_name;
                api::apply_footprint_status(j, *b);
                r.set_content(j.dump(), "application/json");
            });

    srv.Post("/api/symbols/([^/]+)/bind-footprint",
             [&db](const httplib::Request& req, httplib::Response& r)
             {
                 auto id = req.matches[1];
                 bool locked = false;
                 {
                     auto session = db.session();
                     locked = symbol_locked(session, id);
                 }
                 if (locked)
                 {
                     json j{{"ok", false}, {"error", "Symbol belongs to a locked library"}};
                     r.set_content(j.dump(), "application/json");
                     return;
                 }
                 auto body = json::parse(req.body);
                 auto mgr = g_binding_mgr.get();
                 auto res = mgr->assign_footprint(id, body["footprint_id"].get<std::string>());
                 json j;
                 if (!res)
                 {
                     j["ok"] = false;
                     j["error"] = res.error().format_message();
                 }
                 else
                 {
                     j["ok"] = true;
                     services::note_successful_edit();
                 }
                 r.set_content(j.dump(), "application/json");
             });

    srv.Post("/api/footprints/([^/]+)/bind-model",
             [&db](const httplib::Request& req, httplib::Response& r)
             {
                 auto id = req.matches[1];
                 bool locked = false;
                 {
                     auto session = db.session();
                     locked = footprint_locked(session, id);
                 }
                 if (locked)
                 {
                     json j{{"ok", false}, {"error", "Footprint belongs to a locked library"}};
                     r.set_content(j.dump(), "application/json");
                     return;
                 }
                 auto body = json::parse(req.body);
                 auto mgr = g_binding_mgr.get();
                 auto res = mgr->assign_model(id, body["model_id"].get<std::string>());
                 json j;
                 if (!res)
                 {
                     j["ok"] = false;
                     j["error"] = res.error().format_message();
                 }
                 else
                 {
                     j["ok"] = true;
                     services::note_successful_edit();
                 }
                 r.set_content(j.dump(), "application/json");
             });

    srv.Get("/api/footprints/search",
            [&db](const httplib::Request& req, httplib::Response& r)
            {
                auto q = req.get_param_value("q");
                auto anchor = req.get_param_value("near");  // "near" is a Windows macro
                int limit = param_limit(req, 20);
                auto mgr = g_binding_mgr.get();
                auto result = mgr->search_footprints(q.empty() ? "" : q, limit, anchor);
                if (!result)
                {
                    // 绱㈠紩璇诲け璐ヤ笉鏄€屾病鏈夊€欓€夈€嶏細璁╄皟鐢ㄦ柟鐪嬪埌閿欒
                    r.status = 500;
                    json j;
                    j["error"] = result.error().format_message();
                    r.set_content(j.dump(), "application/json");
                    return;
                }
                // 鍊欓€夋槸鍚﹀瓨鍦ㄧ敱缁熶竴 resolver 鍒ゅ畾锛氬け鏁堥」鐓у父鏄剧ず锛堝彲瑙ｉ噴锛夛紝
                // 浣嗗墠绔細绂佺敤閫夋嫨
                services::Snapshot snapshot;
                {
                    auto session = db.session();
                    auto loaded = services::load_snapshot(session);
                    if (!loaded)
                    {
                        r.status = 500;
                        json j;
                        j["error"] = loaded.error().format_message();
                        r.set_content(j.dump(), "application/json");
                        return;
                    }
                    snapshot = std::move(*loaded);
                }
                services::FileCheckContext files;
                json arr = json::array();
                for (auto& fp : *result)
                {
                    json f;
                    f["id"] = fp.id;
                    f["name"] = fp.name;
                    f["library"] = fp.library;  // .pretty lib name 鈥?build "Lib:Name" refs
                    f["score"] = fp.score;
                    f["pad_count"] = fp.pad_count;
                    if (const auto* row = snapshot.footprint(fp.id))
                    {
                        auto resolved = services::inspect_footprint(*row, snapshot, files, false);
                        api::apply_footprint_candidate_status(f, resolved);
                    }
                    else
                    {
                        // 绱㈠紩閲屾湁銆佸簱閲屽凡缁忔病鏈夌殑琛岋細涓嶆槸鍙敤鍊欓€?
                        f["footprint_status"] = services::to_string(services::FootprintStatus::Stale);
                        f["footprint_reason"] = services::fp_reason::kMissingFile;
                    }
                    arr.push_back(f);
                }
                r.set_content(arr.dump(), "application/json");
            });

    srv.Get("/api/models/search",
            [](const httplib::Request& req, httplib::Response& r)
            {
                auto q = req.get_param_value("q");
                auto anchor = req.get_param_value("near");  // "near" is a Windows macro
                int limit = param_limit(req, 20);
                auto mgr = g_binding_mgr.get();
                auto result = mgr->search_models(q.empty() ? "" : q, limit, anchor);
                if (!result)
                {
                    r.status = 500;
                    json j;
                    j["error"] = result.error().format_message();
                    r.set_content(j.dump(), "application/json");
                    return;
                }
                json arr = json::array();
                for (auto& m : *result)
                {
                    json j;
                    j["id"] = m.id;
                    j["name"] = m.name;
                    j["format"] = m.format;
                    j["score"] = m.score;
                    arr.push_back(j);
                }
                r.set_content(arr.dump(), "application/json");
            });

    srv.Get("/api/symbols/search",
            [](const httplib::Request& req, httplib::Response& r)
            {
                auto q = req.get_param_value("q");
                auto anchor = req.get_param_value("near");  // "near" is a Windows macro
                int limit = param_limit(req, 20);
                auto mgr = g_binding_mgr.get();
                auto result = mgr->search_symbols(q.empty() ? "" : q, limit, anchor);
                if (!result)
                {
                    r.status = 500;
                    json j;
                    j["error"] = result.error().format_message();
                    r.set_content(j.dump(), "application/json");
                    return;
                }
                json arr = json::array();
                for (auto& s : *result)
                {
                    json j;
                    j["id"] = s.id;
                    j["name"] = s.name;
                    j["score"] = s.score;
                    arr.push_back(j);
                }
                r.set_content(arr.dump(), "application/json");
            });

    // POST /api/editing/export —— 对象复制的只读导出（ED-030）。
    // 不写 DB/文件，不取写锁：锁定库、只读库、官方库的对象同样可以导出。
    srv.Post("/api/editing/export",
             [&db](const httplib::Request& req, httplib::Response& r)
             {
                 json body;
                 try
                 {
                     body = req.body.empty() ? json::object() : json::parse(req.body);
                 }
                 catch (const std::exception& error)
                 {
                     send_json(r, {{"ok", false},
                                   {"code", "invalid_request"},
                                   {"error", std::string("Invalid JSON body: ") + error.what()}},
                               400);
                     return;
                 }
                 if (!body.is_object() || body.find("ids") == body.end() || !body["ids"].is_array())
                 {
                     send_json(r, {{"ok", false},
                                   {"code", "invalid_request"},
                                   {"error", "Body must be an object with an ids array"}},
                               400);
                     return;
                 }
                 std::vector<core::Uuid> ids;
                 for (const auto& value : body["ids"])
                 {
                     if (!value.is_string())
                     {
                         send_json(r, {{"ok", false},
                                       {"code", "invalid_request"},
                                       {"error", "ids must contain only strings"}},
                                   400);
                         return;
                     }
                     auto id = value.get<std::string>();
                     if (id.empty() || id.size() > 64)
                     {
                         send_json(r, {{"ok", false},
                                       {"code", "invalid_request"},
                                       {"error", "Invalid symbol id"}},
                                   400);
                         return;
                     }
                     ids.push_back(std::move(id));
                 }
                 if (ids.empty() || ids.size() > 200)
                 {
                     send_json(r, {{"ok", false},
                                   {"code", "invalid_request"},
                                   {"error", "ids must contain between 1 and 200 symbol ids"}},
                                   400);
                     return;
                 }
                 auto exported = services::export_symbols(db, ids);
                 if (!exported)
                 {
                     const auto kind = exported.error().kind();
                     const char* code = "export_failed";
                     int status = 500;
                     if (kind == util::Error::Kind::NotFound)
                     {
                         code = "symbol_not_found";
                         status = 404;
                     }
                     else if (kind == util::Error::Kind::ValidationError)
                     {
                         code = "invalid_request";
                         status = 400;
                     }
                     send_json(r,
                               {{"ok", false},
                                {"code", code},
                                {"error", exported.error().format_message()}},
                               status);
                     return;
                 }
                 json items = json::array();
                 for (const auto& item : exported->items)
                 {
                     items.push_back({{"uuid", item.uuid},
                                      {"kf_id", item.kf_id},
                                      {"name", item.name},
                                      {"library_id", item.library_id},
                                      {"library_name", item.library_name},
                                      {"source_path", item.source_path},
                                      {"source_digest", item.source_digest},
                                      {"sexpr", item.sexpr},
                                      {"dependency", item.dependency}});
                 }
                 send_json(r, {{"ok", true},
                               {"version", exported->version},
                               {"kind", exported->kind},
                               {"items", std::move(items)}});
             });

    // POST /api/editing/paste —— 对象粘贴（ED-031..036）。
    // dry_run 只读预检；真正提交由 SourceMutationService::apply 一次完成文件追加与
    // DB 行插入（失败整体回滚）。剪贴板内容不携带写授权，目标权限按目标库重新校验。
    srv.Post("/api/editing/paste",
             [&db](const httplib::Request& req, httplib::Response& r)
             {
                 if (req.body.size() > services::kMaxPasteBytes)
                 {
                     send_json(r,
                               {{"ok", false},
                                {"code", "payload_too_large"},
                                {"error", "Paste payload exceeds the 4 MiB limit"}},
                               413);
                     return;
                 }
                 json body;
                 try
                 {
                     body = req.body.empty() ? json::object() : json::parse(req.body);
                 }
                 catch (const std::exception& error)
                 {
                     send_json(r,
                               {{"ok", false},
                                {"code", "invalid_request"},
                                {"error", std::string("Invalid JSON body: ") + error.what()}},
                               400);
                     return;
                 }
                 auto invalid = [&r](const std::string& message)
                 {
                     send_json(r, {{"ok", false}, {"code", "invalid_request"}, {"error", message}}, 400);
                 };
                 if (!body.is_object())
                 {
                     invalid("Body must be a JSON object");
                     return;
                 }
                 const auto string_of = [&body](const char* key) -> std::optional<std::string>
                 {
                     const auto it = body.find(key);
                     if (it == body.end() || !it->is_string())
                     {
                         return std::nullopt;
                     }
                     return it->get<std::string>();
                 };
                 const auto target_library_id = string_of("target_library_id");
                 if (!target_library_id || target_library_id->empty() || target_library_id->size() > 64)
                 {
                     invalid("target_library_id must be a non-empty string of at most 64 characters");
                     return;
                 }
                 const auto operation_id = string_of("operation_id");
                 if (!operation_id || operation_id->empty() ||
                     operation_id->size() > services::kMaxPasteOperationIdLength)
                 {
                     invalid("operation_id must be a non-empty string of at most 64 characters");
                     return;
                 }
                 services::PasteRequest request;
                 request.target_library_id = *target_library_id;
                 request.operation_id = *operation_id;
                 if (body.contains("target_digest"))
                 {
                     if (!body["target_digest"].is_string())
                     {
                         invalid("target_digest must be a string");
                         return;
                     }
                     request.target_digest = body["target_digest"].get<std::string>();
                     if (request.target_digest.size() > 64)
                     {
                         invalid("target_digest must be at most 64 characters");
                         return;
                     }
                 }
                 if (body.contains("dry_run"))
                 {
                     if (!body["dry_run"].is_boolean())
                     {
                         invalid("dry_run must be a boolean");
                         return;
                     }
                     request.dry_run = body["dry_run"].get<bool>();
                 }
                 if (body.contains("renames"))
                 {
                     if (!body["renames"].is_object())
                     {
                         invalid("renames must be an object");
                         return;
                     }
                     for (auto it = body["renames"].begin(); it != body["renames"].end(); ++it)
                     {
                         const std::string from = it.key();
                         if (from.empty() || from.size() > services::kMaxPasteNameLength ||
                             !it.value().is_string())
                         {
                             invalid("renames must map non-empty names to new names");
                             return;
                         }
                         const auto to = it.value().get<std::string>();
                         if (to.empty() || to.size() > services::kMaxPasteNameLength)
                         {
                             invalid("renames must map names to non-empty names of at most 255 characters");
                             return;
                         }
                         request.renames.emplace(from, to);
                     }
                 }
                 if (!body.contains("items") || !body["items"].is_array())
                 {
                     invalid("items must be an array");
                     return;
                 }
                 const auto& items_json = body["items"];
                 if (items_json.empty() || items_json.size() > services::kMaxPasteItems)
                 {
                     invalid("items must contain between 1 and 200 entries");
                     return;
                 }
                 for (const auto& value : items_json)
                 {
                     if (!value.is_object() || !value.contains("name") || !value["name"].is_string() ||
                         !value.contains("sexpr") || !value["sexpr"].is_string())
                     {
                         invalid("each item must provide string name and sexpr fields");
                         return;
                     }
                     services::PasteItem item;
                     item.name = value["name"].get<std::string>();
                     item.sexpr = value["sexpr"].get<std::string>();
                     if (item.name.empty() || item.name.size() > services::kMaxPasteNameLength ||
                         item.sexpr.empty())
                     {
                         invalid("item name must be 1..255 characters and sexpr must not be empty");
                         return;
                     }
                     if (value.contains("dependency"))
                     {
                         if (!value["dependency"].is_boolean())
                         {
                             invalid("dependency must be a boolean");
                             return;
                         }
                         item.dependency = value["dependency"].get<bool>();
                     }
                     request.items.push_back(std::move(item));
                 }
                 auto send_paste_error = [&r](const util::Error& error)
                 {
                     const auto code = services::paste_error_code(error);
                     std::string message = error.message();
                     const std::string prefix = code + ": ";
                     if (message.rfind(prefix, 0) == 0)
                     {
                         message.erase(0, prefix.size());
                     }
                     send_json(r, {{"ok", false}, {"code", code}, {"error", message}},
                               services::paste_error_status(code));
                 };
                 if (request.dry_run)
                 {
                     auto preview = services::preview_paste(db, request);
                     if (!preview)
                     {
                         send_paste_error(preview.error());
                         return;
                     }
                     json creates = json::array();
                     for (const auto& create : preview->creates)
                     {
                         creates.push_back({{"source_name", create.source_name},
                                            {"new_name", create.new_name}});
                     }
                     json conflicts = json::array();
                     for (const auto& conflict : preview->conflicts)
                     {
                         conflicts.push_back({{"name", conflict}});
                     }
                     json warnings = json::array();
                     for (const auto& warning : preview->warnings)
                     {
                         warnings.push_back(warning);
                     }
                     send_json(r,
                               {{"ok", true},
                                {"plan",
                                 {{"target",
                                   {{"library_id", preview->target.library_id},
                                    {"name", preview->target.name},
                                    {"file_path", preview->target.file_path},
                                    {"digest", preview->target.digest}}},
                                  {"creates", std::move(creates)},
                                  {"conflicts", std::move(conflicts)},
                                  {"warnings", std::move(warnings)}}}});
                     return;
                 }
                 auto pasted = services::paste_symbols(db, request);
                 if (!pasted)
                 {
                     send_paste_error(pasted.error());
                     return;
                 }
                 json created = json::array();
                 for (const auto& item : pasted->created)
                 {
                     created.push_back({{"uuid", item.uuid}, {"name", item.name}});
                 }
                 send_json(r,
                           {{"ok", true}, {"created", std::move(created)}, {"replayed", pasted->replayed}});
             });

    // POST /api/editing/cut —— 剪切意图（ED-040）。只读：不删文件、不改 DB，只生成
    // 一次性、短期、进程内不透明 token，并导出完整节点与 extends 闭包（同 export）。
    srv.Post("/api/editing/cut",
             [&db](const httplib::Request& req, httplib::Response& r)
             {
                 json body;
                 try
                 {
                     body = req.body.empty() ? json::object() : json::parse(req.body);
                 }
                 catch (const std::exception& error)
                 {
                     send_json(r,
                               {{"ok", false},
                                {"code", "invalid_request"},
                                {"error", std::string("Invalid JSON body: ") + error.what()}},
                               400);
                     return;
                 }
                 if (!body.is_object() || body.find("ids") == body.end() || !body["ids"].is_array())
                 {
                     send_json(r,
                               {{"ok", false},
                                {"code", "invalid_request"},
                                {"error", "Body must be an object with an ids array"}},
                               400);
                     return;
                 }
                 std::vector<core::Uuid> ids;
                 for (const auto& value : body["ids"])
                 {
                     if (!value.is_string())
                     {
                         send_json(r,
                                   {{"ok", false},
                                    {"code", "invalid_request"},
                                    {"error", "ids must contain only strings"}},
                                   400);
                         return;
                     }
                     auto id = value.get<std::string>();
                     if (id.empty() || id.size() > 64)
                     {
                         send_json(r,
                                   {{"ok", false},
                                    {"code", "invalid_request"},
                                    {"error", "Invalid symbol id"}},
                                   400);
                         return;
                     }
                     ids.push_back(std::move(id));
                 }
                 if (ids.empty() || ids.size() > services::kMaxCutItems)
                 {
                     send_json(r,
                               {{"ok", false},
                                {"code", "invalid_request"},
                                {"error", "ids must contain between 1 and 200 symbol ids"}},
                               400);
                     return;
                 }
                 auto cut = services::cut_symbols(db, ids);
                 if (!cut)
                 {
                     const auto code = services::cut_error_code(cut.error());
                     std::string message = cut.error().message();
                     const std::string prefix = code + ": ";
                     if (message.rfind(prefix, 0) == 0)
                     {
                         message.erase(0, prefix.size());
                     }
                     send_json(r, {{"ok", false}, {"code", code}, {"error", message}},
                               services::cut_error_status(code));
                     return;
                 }
                 json items = json::array();
                 for (const auto& item : cut->items)
                 {
                     items.push_back({{"uuid", item.uuid},
                                      {"kf_id", item.kf_id},
                                      {"name", item.name},
                                      {"library_id", item.library_id},
                                      {"library_name", item.library_name},
                                      {"source_path", item.source_path},
                                      {"source_digest", item.source_digest},
                                      {"sexpr", item.sexpr},
                                      {"dependency", item.dependency}});
                 }
                 json warnings = json::array();
                 for (const auto& warning : cut->warnings)
                 {
                     warnings.push_back(warning);
                 }
                 send_json(r, {{"ok", true},
                               {"version", cut->version},
                               {"kind", cut->kind},
                               {"token", cut->token},
                               {"summary", cut->summary},
                               {"items", std::move(items)},
                               {"warnings", std::move(warnings)}});
             });

    // POST /api/editing/cut/clear —— 释放剪切意图（Escape）。token 不存在也返回 ok。
    srv.Post("/api/editing/cut/clear",
             [](const httplib::Request& req, httplib::Response& r)
             {
                 json body;
                 try
                 {
                     body = req.body.empty() ? json::object() : json::parse(req.body);
                 }
                 catch (const std::exception& error)
                 {
                     send_json(r,
                               {{"ok", false},
                                {"code", "invalid_request"},
                                {"error", std::string("Invalid JSON body: ") + error.what()}},
                               400);
                     return;
                 }
                 if (!body.is_object())
                 {
                     send_json(r,
                               {{"ok", false},
                                {"code", "invalid_request"},
                                {"error", "Body must be a JSON object"}},
                               400);
                     return;
                 }
                 if (body.contains("token"))
                 {
                     if (!body["token"].is_string() || body["token"].get<std::string>().size() > 128)
                     {
                         send_json(r,
                                   {{"ok", false},
                                    {"code", "invalid_request"},
                                    {"error", "token must be a string of at most 128 characters"}},
                                   400);
                         return;
                     }
                     services::clear_cut_token(body["token"].get<std::string>());
                 }
                 send_json(r, {{"ok", true}});
             });

    // POST /api/editing/move —— 跨库移动（ED-041..045）。dry_run 只读预检、不消费
    // token；提交时目标追加 + 源删除 + DB 归属更新在同一次 SourceMutationService::apply。
    srv.Post("/api/editing/move",
             [&db](const httplib::Request& req, httplib::Response& r)
             {
                 json body;
                 try
                 {
                     body = req.body.empty() ? json::object() : json::parse(req.body);
                 }
                 catch (const std::exception& error)
                 {
                     send_json(r,
                               {{"ok", false},
                                {"code", "invalid_request"},
                                {"error", std::string("Invalid JSON body: ") + error.what()}},
                               400);
                     return;
                 }
                 auto send_move_error = [&r](const util::Error& error)
                 {
                     const auto code = services::move_error_code(error);
                     std::string message = error.message();
                     const std::string prefix = code + ": ";
                     if (message.rfind(prefix, 0) == 0)
                     {
                         message.erase(0, prefix.size());
                     }
                     send_json(r, {{"ok", false}, {"code", code}, {"error", message}},
                               services::move_error_status(code));
                 };
                 auto invalid = [&r](const std::string& message)
                 {
                     send_json(r, {{"ok", false}, {"code", "invalid_request"}, {"error", message}}, 400);
                 };
                 if (!body.is_object())
                 {
                     invalid("Body must be a JSON object");
                     return;
                 }
                 const auto string_of = [&body](const char* key) -> std::optional<std::string>
                 {
                     const auto it = body.find(key);
                     if (it == body.end() || !it->is_string())
                     {
                         return std::nullopt;
                     }
                     return it->get<std::string>();
                 };
                 const auto token = string_of("token");
                 if (!token || token->empty() || token->size() > 128)
                 {
                     invalid("token must be a non-empty string of at most 128 characters");
                     return;
                 }
                 const auto target_library_id = string_of("target_library_id");
                 if (!target_library_id || target_library_id->empty() || target_library_id->size() > 64)
                 {
                     invalid("target_library_id must be a non-empty string of at most 64 characters");
                     return;
                 }
                 const auto operation_id = string_of("operation_id");
                 if (!operation_id || operation_id->empty() ||
                     operation_id->size() > services::kMaxMoveOperationIdLength)
                 {
                     invalid("operation_id must be a non-empty string of at most 64 characters");
                     return;
                 }
                 services::MoveRequest request;
                 request.token = *token;
                 request.target_library_id = *target_library_id;
                 request.operation_id = *operation_id;
                 if (body.contains("target_digest"))
                 {
                     if (!body["target_digest"].is_string())
                     {
                         invalid("target_digest must be a string");
                         return;
                     }
                     request.target_digest = body["target_digest"].get<std::string>();
                     if (request.target_digest.size() > 64)
                     {
                         invalid("target_digest must be at most 64 characters");
                         return;
                     }
                 }
                 if (body.contains("dry_run"))
                 {
                     if (!body["dry_run"].is_boolean())
                     {
                         invalid("dry_run must be a boolean");
                         return;
                     }
                     request.dry_run = body["dry_run"].get<bool>();
                 }
                 if (body.contains("renames"))
                 {
                     if (!body["renames"].is_object())
                     {
                         invalid("renames must be an object");
                         return;
                     }
                     for (auto it = body["renames"].begin(); it != body["renames"].end(); ++it)
                     {
                         const std::string from = it.key();
                         if (from.empty() || from.size() > 255 || !it.value().is_string())
                         {
                             invalid("renames must map non-empty names to new names");
                             return;
                         }
                         const auto to = it.value().get<std::string>();
                         if (to.empty() || to.size() > 255)
                         {
                             invalid("renames must map names to non-empty names of at most 255 characters");
                             return;
                         }
                         request.renames.emplace(from, to);
                     }
                 }
                 if (request.dry_run)
                 {
                     auto preview = services::preview_move(db, request);
                     if (!preview)
                     {
                         send_move_error(preview.error());
                         return;
                     }
                     json moves = json::array();
                     for (const auto& move : preview->moves)
                     {
                         moves.push_back({{"uuid", move.uuid},
                                          {"name", move.name},
                                          {"new_name", move.new_name}});
                     }
                     json dependencies = json::array();
                     for (const auto& dependency : preview->dependencies)
                     {
                         dependencies.push_back({{"name", dependency.name},
                                                 {"dependency", dependency.dependency}});
                     }
                     json conflicts = json::array();
                     for (const auto& conflict : preview->conflicts)
                     {
                         conflicts.push_back({{"name", conflict.name}, {"reason", conflict.reason}});
                     }
                     json warnings = json::array();
                     for (const auto& warning : preview->warnings)
                     {
                         warnings.push_back(warning);
                     }
                     send_json(r,
                               {{"ok", true},
                                {"plan",
                                 {{"noop", preview->noop},
                                  {"target",
                                   {{"library_id", preview->target.library_id},
                                    {"name", preview->target.name},
                                    {"file_path", preview->target.file_path},
                                    {"digest", preview->target.digest}}},
                                  {"moves", std::move(moves)},
                                  {"dependencies", std::move(dependencies)},
                                  {"conflicts", std::move(conflicts)},
                                  {"warnings", std::move(warnings)}}}});
                     return;
                 }
                 auto moved = services::move_symbols(db, request);
                 if (!moved)
                 {
                     send_move_error(moved.error());
                     return;
                 }
                 json moved_items = json::array();
                 for (const auto& item : moved->moved)
                 {
                     moved_items.push_back({{"uuid", item.uuid},
                                            {"name", item.new_name},
                                            {"source_name", item.name}});
                 }
                 send_json(r,
                           {{"ok", true},
                            {"moved", std::move(moved_items)},
                            {"count", moved->moved.size()},
                            {"noop", moved->noop},
                            {"replayed", moved->replayed}});
             });
    // GET /api/editing/history —— 会话内编辑历史快照（只读，不轮询、不写）。
    // 进程重启后历史为空；前端据此启用/禁用撤销与重做。
    srv.Get("/api/editing/history",
            [](const httplib::Request&, httplib::Response& r)
            {
                const auto state = services::history_state();
                const auto summary_json = [](const services::HistoryEntrySummary& entry)
                {
                    return json{{"kind", entry.kind},
                                {"count", entry.count},
                                {"target_library_name", entry.target_library_name},
                                {"source_library_name", entry.source_library_name},
                                {"created_at_ms", entry.created_at_ms}};
                };
                json entries = json::array();
                for (const auto& entry : state.entries)
                    entries.push_back(summary_json(entry));
                json undo = nullptr;
                if (state.can_undo && state.undo_index < state.entries.size())
                    undo = summary_json(state.entries[state.undo_index]);
                json redo = nullptr;
                if (state.can_redo && state.redo_index < state.entries.size())
                    redo = summary_json(state.entries[state.redo_index]);
                send_json(r,
                          {{"ok", true},
                           {"can_undo", state.can_undo},
                           {"can_redo", state.can_redo},
                           {"count", state.count},
                           {"bytes", state.bytes},
                           {"undo", std::move(undo)},
                           {"redo", std::move(redo)},
                           {"entries", std::move(entries)}});
            });

    // POST /api/editing/history/undo 与 /redo —— 重新授权并复核文件摘要/DB 行状态；
    // 冲突整体拒绝，历史指针不前移。
    const auto register_history_apply = [&srv, &db](const char* path, bool undo)
    {
        srv.Post(path,
                 [&db, undo](const httplib::Request&, httplib::Response& r)
                 {
                     const auto send_history_error = [&r](const util::Error& error)
                     {
                         const auto code = services::history_error_code(error);
                         std::string message = error.message();
                         const std::string prefix = code + ": ";
                         if (message.rfind(prefix, 0) == 0)
                             message.erase(0, prefix.size());
                         send_json(r, {{"ok", false}, {"code", code}, {"error", message}},
                                   services::history_error_status(code));
                     };
                     auto outcome = undo ? services::undo_history(db) : services::redo_history(db);
                     if (!outcome)
                     {
                         send_history_error(outcome.error());
                         return;
                     }
                     json restored = json::array();
                     for (const auto& item : outcome->restored)
                         restored.push_back({{"uuid", item.uuid}, {"name", item.name}});
                     send_json(r,
                               {{"ok", true},
                                {"kind", outcome->kind},
                                {"count", outcome->count},
                                {"restored", std::move(restored)},
                                {"can_undo", outcome->can_undo},
                                {"can_redo", outcome->can_redo}});
                 });
    };
    register_history_apply("/api/editing/history/undo", true);
    register_history_apply("/api/editing/history/redo", false);
}


// ============================================================
// Plugin routes
// ============================================================

void register_plugin_routes(httplib::Server& srv, plugin::PluginManager* plugins,
                            storage::Database& db)
{
    plugin_config_deps().plugins = plugins;  // read by /api/plugin-config per request

    srv.Get("/api/plugins",
            [plugins](const httplib::Request&, httplib::Response& r)
            {
                json arr = json::array();
                for (auto& m : plugins->available_plugins())
                {
                    json p;
                    p["id"] = m.id;
                    p["name"] = m.name;
                    p["version"] = m.version;
                    p["author"] = m.author;
                    p["description"] = m.description;
                    p["status"] = plugins->is_loaded(m.id) ? "loaded" : "available";
                    // Phase-4 frontend wiring: admission state comes from the
                    // TaskService (single owner). Without a task service the
                    // plugin is reported enabled with revision 0, so the old
                    // array shape/fields stay valid.
                    bool plugin_enabled = true;
                    int plugin_revision = 0;
                    if (auto* tasks = plugin_task_deps().tasks)
                    {
                        const auto state = tasks->plugin_state(m.id);
                        plugin_enabled = state.enabled;
                        plugin_revision = state.revision;
                    }
                    p["enabled"] = plugin_enabled;
                    p["revision"] = plugin_revision;
                    p["manifest_version"] = m.manifest_version;
                    if (!m.config.is_null())
                        p["config"] = m.config;  // manifest "config" (schema), verbatim
                    if (m.engines)
                    {
                        json engines;
                        engines["kiforge"] = m.engines->kiforge;
                        engines["sdk"] = m.engines->sdk;
                        p["engines"] = std::move(engines);
                    }
                    if (m.frontend)
                    {
                        // URL prefix already contains the frontend root: entry
                        // and styles are relative to it, so they are appended
                        // verbatim (never root + entry).
                        const std::string base_url = "/plugin-assets/" + m.id + "/";
                        json frontend;
                        frontend["base_url"] = base_url;
                        frontend["entry_url"] = base_url + m.frontend->entry;
                        json styles = json::array();
                        for (const auto& style : m.frontend->styles)
                            styles.push_back(base_url + style);
                        frontend["styles"] = std::move(styles);
                        frontend["version"] = m.version;
                        // v2 contract declarations live inside the frontend block
                        if (!m.provides.empty())
                        {
                            json provides = json::array();
                            for (const auto& contract : m.provides)
                                provides.push_back({{"contract", contract.contract},
                                                    {"version", contract.version}});
                            frontend["provides"] = std::move(provides);
                        }
                        if (!m.contract_requires.empty())
                        {
                            json requirements = json::array();
                            for (const auto& requirement : m.contract_requires)
                                requirements.push_back({{"contract", requirement.contract},
                                                        {"range", requirement.range}});
                            frontend["requires"] = std::move(requirements);
                        }
                        if (m.contributes)
                            frontend["contributes"] = *m.contributes;
                        p["frontend"] = std::move(frontend);
                    }
                    if (!m.icon.empty())
                    {
                        auto pp = plugins->plugin_path(m.id);
                        p["icon_url"] = "/api/plugins/" + m.id + "/icon/" + m.icon;
                    }
                    json actions = json::array();
                    for (auto& a : m.actions)
                    {
                        json act;
                        act["id"] = a.id;
                        act["name"] = a.name;
                        act["description"] = a.description;
                        act["trigger"] = a.trigger;
                        act["template"] = a.ui_template;
                        act["analyze_action"] = a.analyze_action;
                        act["protocol"] = a.protocol;
                        act["preflight"] = a.preflight;
                        act["cancelable"] = a.cancelable;
                        act["auto_args"] = a.auto_args;
                        act["schema"] = a.schema;
                        // Always export the button block (incl. show:false) 鈥?the UI
                        // filters on it to hide non-button actions like "analyze".
                        json btn;
                        btn["show"] = a.button_show;
                        btn["style"] = a.button_style;
                        btn["location"] = a.button_location;
                        btn["tooltip"] = a.button_tooltip;
                        act["button"] = btn;
                        actions.push_back(act);
                    }
                    p["actions"] = actions;
                    arr.push_back(p);
                }
                r.set_content(arr.dump(), "application/json");
            });

    // Per-plugin discovery problems (bad manifest, unsupported manifestVersion,
    // duplicate id). Kept as a separate array endpoint so the legacy
    // /api/plugins array shape stays untouched.
    srv.Get("/api/plugin-diagnostics",
            [plugins](const httplib::Request&, httplib::Response& r)
            {
                json arr = json::array();
                if (plugins != nullptr)
                {
                    for (const auto& diagnostic : plugins->diagnostics())
                    {
                        arr.push_back({{"code", diagnostic.code},
                                       {"plugin_id", diagnostic.plugin_id},
                                       {"message", diagnostic.message}});
                    }
                }
                r.set_content(arr.dump(), "application/json");
            });

    srv.Post("/api/plugins/execute",
             [plugins, &db](const httplib::Request& req, httplib::Response& r)
             {
                 json j;
                 try
                 {
                     std::string plugin_id = req.get_param_value("id");
                     if (plugin_id.empty())
                     {
                         j["ok"] = false;
                         j["error"] = "Missing plugin id";
                         r.set_content(j.dump(), "application/json");
                         return;
                     }
                     auto body = json::parse(req.body.empty() ? "{}" : req.body);
                     std::string action = body.value("action", "import");
                     // Command-line injection guard: action travels into the
                     // plugin process argv 鈥?restrict to a safe alphabet
                     // (PluginManager::execute re-validates, defense in depth)
                     if (action.empty() ||
                         action.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                  "0123456789_-") != std::string::npos)
                     {
                         j["ok"] = false;
                         j["error"] = "Invalid action";
                         r.set_content(j.dump(), "application/json");
                         return;
                     }
                     // Phase 4: the legacy synchronous entry creates a task and
                     // waits for its real terminal state; the response structure
                     // is unchanged. Staged-import actions are dispatched to the
                     // full host StagedImporter, everything else to the Python
                     // entry point 鈥?same TaskService as POST /api/tasks.
                     auto& deps = plugin_task_deps();
                     if (deps.tasks == nullptr || deps.plugins == nullptr || deps.db == nullptr)
                     {
                         j["ok"] = false;
                         j["error"] = "Task service is unavailable";
                     }
                     else
                     {
                         PreparedTaskRequest prepared;
                         if (auto problem = prepare_task_request(deps.plugins, deps.db, plugin_id,
                                                                 action, body, prepared))
                         {
                             j["ok"] = false;
                             j["error"] = problem->message;
                         }
                         else
                         {
                             auto submitted = deps.tasks->submit(
                                 plugin_id, action, prepared.cancelable,
                                 make_task_runner(prepared, deps.plugins, deps.db));
                             if (!submitted.ok)
                             {
                                 j["ok"] = false;
                                 j["error"] = submitted.error;
                             }
                             else
                             {
                                 auto record = deps.tasks->wait_terminal(submitted.task_id);
                                 if (!record)
                                 {
                                     j["ok"] = false;
                                     j["error"] = "Task result is unavailable";
                                 }
                                 else
                                     j = legacy_task_response(*record);
                             }
                         }
                     }
                 }
                 catch (...)
                 {
                     j["ok"] = false;
                     j["error"] = "Execute error";
                 }
                 r.set_content(j.dump(), "application/json");
             });

    srv.Post("/api/plugins/load",
             [plugins](const httplib::Request& req, httplib::Response& r)
             {
                 json j;
                 try
                 {
                     auto body = json::parse(req.body.empty() ? "{}" : req.body);
                     std::string id = body.value("id", "");
                     if (!id.empty())
                     {
                         auto res = plugins->load(id, nullptr);
                         j["ok"] = res.has_value();
                         if (!res)
                             j["error"] = res.error().format_message();
                     }
                     else
                         j["ok"] = false;
                 }
                 catch (...)
                 {
                     j["ok"] = false;
                 }
                 r.set_content(j.dump(), "application/json");
             });

    // Plugin icon routes. The file name comes from the manifest/URL, so it is
    // resolved through the containment helper instead of a raw path join.
    srv.Get(R"(/api/plugins/([^/]+)/icon/([^/]+))",
            [plugins](const httplib::Request& req, httplib::Response& r)
            {
                const auto icon_path = resolve_plugin_root_file(
                    plugins, req.matches[1].str(), req.matches[2].str());
                if (!icon_path)
                {
                    r.status = 404;
                    return;
                }
                std::ifstream f(*icon_path, std::ios::binary);
                if (!f.is_open())
                {
                    r.status = 404;
                    return;
                }
                std::string data((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
                r.set_content(std::move(data), icon_path->extension() == ".svg" ? "image/svg+xml"
                                                                               : "image/png");
            });
    srv.Get(R"(/api/plugins/([^/]+)/icon)",
            [plugins](const httplib::Request& req, httplib::Response& r)
            {
                const std::string plugin_id = req.matches[1].str();
                for (const auto* name : {"icon.svg", "icon.png"})
                {
                    const auto icon_path = resolve_plugin_root_file(plugins, plugin_id, name);
                    if (!icon_path)
                        continue;
                    std::ifstream f(*icon_path, std::ios::binary);
                    if (!f.is_open())
                        continue;
                    std::string data((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                    r.set_content(std::move(data), icon_path->extension() == ".svg"
                                                       ? "image/svg+xml"
                                                       : "image/png");
                    return;
                }
                r.status = 404;
            });

    // Frontend assets: /plugin-assets/<plugin-id>/<path relative to frontend.root>.
    // The manager resolves and contains the path (no traversal, no links); here
    // we only choose an HTTP status and the Content-Type by extension.
    srv.Get(R"(/plugin-assets/([^/]+)/(.+))",
            [plugins](const httplib::Request& req, httplib::Response& r)
            {
                if (plugins == nullptr)
                {
                    r.status = 404;
                    return;
                }
                const std::string plugin_id = req.matches[1].str();
                const std::string relative = req.matches[2].str();
                auto resolved = plugins->resolve_frontend_asset(plugin_id, relative);
                if (!resolved)
                {
                    r.status = resolved.error().kind() == util::Error::Kind::ValidationError
                                   ? 400
                                   : 404;
                    return;
                }
                std::ifstream file(*resolved, std::ios::binary);
                if (!file.is_open())
                {
                    r.status = 404;  // vanished between resolve and open
                    return;
                }
                std::string data((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
                const std::string content_type = plugin_asset_content_type(*resolved);
                r.set_content(std::move(data), content_type);
                r.set_header("Cache-Control", "no-store");
            });
}

// ============================================================
// Phase-4 task / plugin state / import operation routes
// ============================================================

// Declared in api_server.cpp (routes.h is not part of this change): ApiServer
// owns the TaskService and publishes it here before the server starts.
void register_plugin_task_routes(httplib::Server& srv, plugin::TaskService* tasks,
                                 plugin::PluginManager* plugins, storage::Database& db,
                                 util::ConfigStore* config)
{
    plugin_task_deps().tasks = tasks;
    plugin_task_deps().plugins = plugins;
    plugin_task_deps().db = &db;
    plugin_task_deps().config = config;

    // POST /api/tasks {plugin_id, action, args?, client_request_id?}
    srv.Post("/api/tasks",
             [](const httplib::Request& req, httplib::Response& r)
             {
                 auto& deps = plugin_task_deps();
                 if (deps.tasks == nullptr || deps.plugins == nullptr || deps.db == nullptr)
                 {
                     send_json(r, {{"ok", false}, {"error_code", "unavailable"},
                                   {"error", "Task service is unavailable"}},
                               503);
                     return;
                 }
                 json body;
                 try
                 {
                     body = json::parse(req.body.empty() ? "{}" : req.body);
                 }
                 catch (const std::exception&)
                 {
                     send_json(r, {{"ok", false}, {"error_code", "invalid_request"},
                                   {"error", "Request body must be a JSON object"}},
                               400);
                     return;
                 }
                 if (!body.is_object())
                 {
                     send_json(r, {{"ok", false}, {"error_code", "invalid_request"},
                                   {"error", "Request body must be a JSON object"}},
                               400);
                     return;
                 }
                 std::string plugin_id;
                 std::string action;
                 std::optional<std::string> client_request_id;
                 json args = json::object();
                 try
                 {
                     plugin_id = body.value("plugin_id", std::string());
                     action = body.value("action", std::string());
                     if (body.contains("args"))
                     {
                         if (!body["args"].is_object())
                         {
                             send_json(r, {{"ok", false}, {"error_code", "invalid_request"},
                                           {"error", "args must be a JSON object"}},
                                       400);
                             return;
                         }
                         args = body["args"];
                     }
                     if (body.contains("client_request_id"))
                     {
                         if (!body["client_request_id"].is_string())
                         {
                             send_json(r, {{"ok", false}, {"error_code", "invalid_request"},
                                           {"error", "client_request_id must be a string"}},
                                       400);
                             return;
                         }
                         client_request_id = body["client_request_id"].get<std::string>();
                     }
                 }
                 catch (const std::exception& error)
                 {
                     send_json(r, {{"ok", false}, {"error_code", "invalid_request"},
                                   {"error", std::string("Invalid task request: ") + error.what()}},
                               400);
                     return;
                 }
                 if (plugin_id.empty() || action.empty())
                 {
                     send_json(r, {{"ok", false}, {"error_code", "invalid_request"},
                                   {"error", "plugin_id and action are required"}},
                               400);
                     return;
                 }

                 PreparedTaskRequest prepared;
                 if (auto problem = prepare_task_request(deps.plugins, deps.db, plugin_id, action,
                                                         args, prepared))
                 {
                     const bool unknown = problem->code == "plugin_unknown" ||
                                          problem->code == "action_unknown";
                     send_json(r, {{"ok", false}, {"error_code", problem->code},
                                   {"error", problem->message}},
                               unknown ? 404 : 400);
                     return;
                 }
                 auto reply = deps.tasks->submit(
                     plugin_id, action, prepared.cancelable,
                     make_task_runner(prepared, deps.plugins, deps.db), client_request_id);
                 if (!reply.ok)
                 {
                     int status = 400;
                     std::string error_code = reply.code;
                     if (reply.code == "disabled")
                     {
                         status = 409;
                         error_code = "plugin_disabled";
                     }
                     else if (reply.code == "busy")
                     {
                         status = 429;
                     }
                     else if (reply.code == "stopped")
                     {
                         status = 503;
                     }
                     send_json(r, {{"ok", false}, {"error_code", error_code},
                                   {"error", reply.error}},
                               status);
                     return;
                 }
                 send_json(r, {{"ok", true},
                               {"task_id", reply.task_id},
                               {"state", plugin::task_state_name(reply.state)}});
             });

    // GET /api/tasks/{task_id}
    srv.Get(R"(/api/tasks/([^/]+))",
            [](const httplib::Request& req, httplib::Response& r)
            {
                auto& deps = plugin_task_deps();
                if (deps.tasks == nullptr)
                {
                    send_json(r, {{"ok", false}, {"error_code", "unavailable"},
                                  {"error", "Task service is unavailable"}},
                              503);
                    return;
                }
                auto record = deps.tasks->get(req.matches[1].str());
                if (!record)
                {
                    send_json(r, {{"ok", false}, {"error_code", "task_not_found"},
                                  {"error", "Task not found"}},
                              404);
                    return;
                }
                json out = task_record_json(*record);
                out["ok"] = true;
                send_json(r, out);
            });

    // POST /api/tasks/{task_id}/cancel
    srv.Post(R"(/api/tasks/([^/]+)/cancel)",
             [](const httplib::Request& req, httplib::Response& r)
             {
                 auto& deps = plugin_task_deps();
                 if (deps.tasks == nullptr)
                 {
                     send_json(r, {{"ok", false}, {"error_code", "unavailable"},
                                   {"error", "Task service is unavailable"}},
                               503);
                     return;
                 }
                 auto reply = deps.tasks->cancel(req.matches[1].str());
                 if (reply.status == plugin::CancelStatus::NotFound)
                 {
                     send_json(r, {{"ok", false}, {"error_code", "task_not_found"},
                                   {"error", "Task not found"}},
                               404);
                     return;
                 }
                 json out = task_record_json(reply.task);
                 out["ok"] = true;
                 out["status"] = plugin::cancel_status_name(reply.status);
                 send_json(r, out);
             });

    // GET /api/plugins/{plugin_id}/tasks 鈥?recent task summaries.
    srv.Get(R"(/api/plugins/([^/]+)/tasks)",
            [](const httplib::Request& req, httplib::Response& r)
            {
                auto& deps = plugin_task_deps();
                const std::string plugin_id = req.matches[1].str();
                if (deps.tasks == nullptr || deps.plugins == nullptr)
                {
                    send_json(r, {{"ok", false}, {"error_code", "unavailable"},
                                  {"error", "Task service is unavailable"}},
                              503);
                    return;
                }
                if (!plugin_is_discovered(deps.plugins, plugin_id))
                {
                    send_json(r, {{"ok", false}, {"error_code", "plugin_unknown"},
                                  {"error", "Plugin not found: " + plugin_id}},
                              404);
                    return;
                }
                json list = json::array();
                for (const auto& record : deps.tasks->list_for_plugin(plugin_id))
                    list.push_back(task_record_json(record));
                send_json(r, {{"ok", true}, {"plugin_id", plugin_id}, {"tasks", std::move(list)}});
            });

    // GET /api/plugins/{plugin_id}/state 鈥?current admission state for the UI.
    srv.Get(R"(/api/plugins/([^/]+)/state)",
            [](const httplib::Request& req, httplib::Response& r)
            {
                auto& deps = plugin_task_deps();
                const std::string plugin_id = req.matches[1].str();
                if (!plugin_is_discovered(deps.plugins, plugin_id))
                {
                    send_json(r, {{"ok", false}, {"error", "plugin_unknown"}}, 404);
                    return;
                }
                if (deps.tasks == nullptr)
                {
                    send_json(r, {{"ok", false}, {"error", "unavailable"}}, 503);
                    return;
                }
                const auto state = deps.tasks->plugin_state(plugin_id);
                send_json(r, {{"ok", true},
                              {"plugin_id", plugin_id},
                              {"enabled", state.enabled},
                              {"revision", state.revision}});
            });

    // POST /api/plugins/{plugin_id}/state {enabled, if_revision?}
    srv.Post(R"(/api/plugins/([^/]+)/state)",
             [](const httplib::Request& req, httplib::Response& r)
             {
                 auto& deps = plugin_task_deps();
                 const std::string plugin_id = req.matches[1].str();
                 if (deps.tasks == nullptr || deps.plugins == nullptr)
                 {
                     send_json(r, {{"ok", false}, {"error_code", "unavailable"},
                                   {"error", "Task service is unavailable"}},
                               503);
                     return;
                 }
                 if (!plugin_is_discovered(deps.plugins, plugin_id))
                 {
                     send_json(r, {{"ok", false}, {"error_code", "plugin_unknown"},
                                   {"error", "Plugin not found: " + plugin_id}},
                               404);
                     return;
                 }
                 json body;
                 bool enabled = true;
                 std::optional<int> if_revision;
                 try
                 {
                     body = json::parse(req.body.empty() ? "{}" : req.body);
                     if (!body.is_object() || !body.contains("enabled") ||
                         !body["enabled"].is_boolean())
                     {
                         send_json(r, {{"ok", false}, {"error_code", "invalid_request"},
                                       {"error", "enabled must be a boolean"}},
                                   400);
                         return;
                     }
                     enabled = body["enabled"].get<bool>();
                     if (body.contains("if_revision"))
                     {
                         if (!body["if_revision"].is_number_integer())
                         {
                             send_json(r, {{"ok", false}, {"error_code", "invalid_request"},
                                           {"error", "if_revision must be an integer"}},
                                       400);
                             return;
                         }
                         if_revision = body["if_revision"].get<int>();
                     }
                 }
                 catch (const std::exception& error)
                 {
                     send_json(r, {{"ok", false}, {"error_code", "invalid_request"},
                                   {"error", std::string("Invalid state request: ") + error.what()}},
                               400);
                     return;
                 }

                 auto reply = deps.tasks->set_plugin_enabled(plugin_id, enabled, if_revision);
                 if (!reply.ok)
                 {
                     int status = 400;
                     if (reply.code == "revision_conflict")
                         status = 409;
                     else if (reply.code == "persist_failed")
                         status = 500;
                     else if (reply.code == "stopped")
                         status = 503;
                     json out = {{"ok", false}, {"error_code", reply.code},
                                 {"error", reply.error},
                                 {"enabled", reply.enabled},
                                 {"revision", reply.revision},
                                 {"cancel_requested", json::array()}};
                     send_json(r, out, status);
                     return;
                 }
                 json out = {{"ok", true},
                             {"enabled", reply.enabled},
                             {"revision", reply.revision},
                             {"cancel_requested", reply.cancel_requested}};
                 send_json(r, out);
             });

    // GET /api/imports/operations/{operation_id}: read-only replay of a committed
    // import. Never creates a record; unknown ids are 404.
    srv.Get(R"(/api/imports/operations/([^/]+))",
            [&db](const httplib::Request& req, httplib::Response& r)
            {
                const std::string operation_id = req.matches[1].str();
                if (operation_id.empty())
                {
                    send_json(r, {{"ok", false}, {"error_code", "invalid_request"},
                                  {"error", "Missing operation id"}},
                              400);
                    return;
                }
                auto session = db.session();
                sqlite3_stmt* stmt = nullptr;
                const char* sql =
                    "SELECT request_digest, result_json FROM import_operations WHERE id = ?1";
                if (sqlite3_prepare_v2(session.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK)
                {
                    send_json(r, {{"ok", false}, {"error_code", "db_error"},
                                  {"error", "Cannot read operation log: " +
                                                std::string(sqlite3_errmsg(session.handle()))}},
                              500);
                    return;
                }
                sqlite3_bind_text(stmt, 1, operation_id.c_str(), -1, SQLITE_TRANSIENT);
                std::string stored_result;
                bool found = false;
                int step = sqlite3_step(stmt);
                if (step == SQLITE_ROW)
                {
                    const unsigned char* text = sqlite3_column_text(stmt, 1);
                    stored_result = text != nullptr
                                        ? std::string(reinterpret_cast<const char*>(text))
                                        : std::string();
                    found = true;
                }
                sqlite3_finalize(stmt);
                if (!found)
                {
                    send_json(r, {{"ok", false}, {"error_code", "operation_not_found"},
                                  {"error", "Operation not found: " + operation_id}},
                              404);
                    return;
                }
                try
                {
                    json result =
                        json::parse(stored_result.empty() ? "{}" : stored_result);
                    send_json(r, {{"ok", true},
                                  {"operation_id", operation_id},
                                  {"result", std::move(result)}});
                }
                catch (const std::exception& error)
                {
                    send_json(r, {{"ok", false}, {"error_code", "db_error"},
                                  {"error", "Stored operation result is not JSON: " +
                                                std::string(error.what())}},
                              500);
                }
            });
}

// ============================================================
// Settings / Component libraries / DB reset
// ============================================================
// Config (JSON) routes 鈥?replaces the old DB `settings` table.
// ============================================================

void register_settings_routes(httplib::Server& srv, storage::Database& db, ImportManager* import_manager,
                             util::ConfigStore* config)
{
    (void)db;
    (void)import_manager;

    plugin_config_deps().config = config;  // read by /api/plugin-config per request
    if (!config) return;  // safety: skip config endpoints if not wired

    srv.Get("/api/config",
            [config](const httplib::Request&, httplib::Response& r)
            {
                auto j = config->all();
                r.set_content(j.dump(), "application/json");
            });

    srv.Post("/api/config",
             [config](const httplib::Request& req, httplib::Response& r)
             {
                 json j;
                 try
                 {
                     auto body = json::parse(req.body.empty() ? "{}" : req.body);
                     for (auto& [k, v] : body.items())
                     {
                         if (v.is_boolean())
                         {
                             config->set_bool(k, v.get<bool>());
                         }
                         else
                         {
                             config->set(k, v.is_string() ? v.get<std::string>() : v.dump());
                         }
                     }
                     j["ok"] = true;
                 }
                 catch (const std::exception& e)
                 {
                     j["ok"] = false;
                     j["error"] = e.what();
                 }
                 r.set_content(j.dump(), "application/json");
             });

    // Per-plugin configuration: a JSON object persisted in the same ConfigStore
    // as /api/config under "plugins.<id>.config". Values are stored as the JSON
    // text string (the representation /api/config uses for non-primitive
    // values), so read/write parse/serialize here. Only discovered plugins are
    // addressable, and a missing key reads as {} so the UI always gets an object.
    srv.Get(R"(/api/plugin-config/([^/]+))",
            [](const httplib::Request& req, httplib::Response& r)
            {
                const std::string plugin_id = req.matches[1].str();
                auto& deps = plugin_config_deps();
                if (!plugin_is_discovered(deps.plugins, plugin_id))
                {
                    send_json(r, {{"ok", false}, {"error", "Plugin not found: " + plugin_id}}, 404);
                    return;
                }
                if (deps.config == nullptr)
                {
                    send_json(r,
                              {{"ok", false}, {"error", "Plugin configuration store is unavailable"}},
                              503);
                    return;
                }
                try
                {
                    const auto snapshot = deps.config->all();
                    const auto stored = snapshot.find(plugin_config_key(plugin_id));
                    if (stored == snapshot.end())
                    {
                        send_json(r, {{"ok", true}, {"config", json::object()}});
                        return;
                    }
                    if (!stored->is_string())
                    {
                        send_json(r,
                                  {{"ok", false},
                                   {"error", "Stored plugin configuration is not a JSON string"}},
                                  500);
                        return;
                    }
                    auto parsed = json::parse(stored->get<std::string>());
                    if (!parsed.is_object())
                    {
                        send_json(r,
                                  {{"ok", false},
                                   {"error", "Stored plugin configuration must be a JSON object"}},
                                  500);
                        return;
                    }
                    send_json(r, {{"ok", true}, {"config", std::move(parsed)}});
                }
                catch (const std::exception& error)
                {
                    // Corrupt value in the config file: report it, never throw
                    // out of the HTTP handler.
                    send_json(r,
                              {{"ok", false},
                               {"error", "Stored plugin configuration is invalid JSON: " +
                                             std::string(error.what())}},
                              500);
                }
            });

    srv.Post(R"(/api/plugin-config/([^/]+))",
             [](const httplib::Request& req, httplib::Response& r)
             {
                 const std::string plugin_id = req.matches[1].str();
                 auto& deps = plugin_config_deps();
                 if (!plugin_is_discovered(deps.plugins, plugin_id))
                 {
                     send_json(r, {{"ok", false}, {"error", "Plugin not found: " + plugin_id}}, 404);
                     return;
                 }
                 if (deps.config == nullptr)
                 {
                     send_json(r,
                               {{"ok", false}, {"error", "Plugin configuration store is unavailable"}},
                               503);
                     return;
                 }
                 json body;
                 try
                 {
                     body = json::parse(req.body);
                 }
                 catch (const std::exception&)
                 {
                     send_json(r, {{"ok", false}, {"error", "Request body must be a JSON object"}},
                               400);
                     return;
                 }
                 if (!body.is_object())
                 {
                     send_json(r, {{"ok", false}, {"error", "Request body must be a JSON object"}},
                               400);
                     return;
                 }
                 const std::string serialized = body.dump();
                 if (serialized.size() > kMaxPluginConfigBytes)
                 {
                     send_json(r, {{"ok", false},
                                   {"error", "Plugin configuration exceeds the 64 KiB limit"}},
                               400);
                     return;
                 }
                 try
                 {
                     deps.config->set(plugin_config_key(plugin_id), serialized);
                 }
                 catch (const std::exception& error)
                 {
                     send_json(r,
                               {{"ok", false},
                                {"error", "Failed to save plugin configuration: " +
                                              std::string(error.what())}},
                               500);
                     return;
                 }
                 send_json(r, {{"ok", true}});
             });


    srv.Get("/api/component-libraries",
            [&db](const httplib::Request&, httplib::Response& r)
            {
                auto session = db.session();
                storage::ComponentLibraryRepository repo(session);
                auto libs = repo.find_all();
                json arr = json::array();
                if (libs)
                    for (auto& l : *libs)
                    {
                        json o;
                        o["id"] = l.id;
                        o["name"] = l.name;
                        o["symbol_path"] = l.symbol_path;
                        o["footprint_path"] = l.footprint_path;
                        o["model_3d_path"] = l.model_3d_path;
                        o["enabled"] = l.enabled;
                        o["sort_order"] = l.sort_order;
                        o["locked"] = l.locked;
                        o["is_protected"] = l.is_protected;
                        arr.push_back(o);
                    }
                r.set_content(arr.dump(), "application/json");
            });

    srv.Get("/api/component-libraries/([^/]+)/symbol-libraries",
            [&db](const httplib::Request& req, httplib::Response& r)
            {
                json response;
                try
                {
                    auto result = list_component_symbol_libraries(db, req.matches[1].str());
                    if (result)
                        response = std::move(*result);
                    else
                    {
                        const auto kind = result.error().kind();
                        r.status = kind == util::Error::Kind::NotFound ? 404 :
                                   kind == util::Error::Kind::ValidationError ? 400 : 500;
                        response = {{"ok", false}, {"error", result.error().format_message()}};
                    }
                }
                catch (const std::exception& error)
                {
                    r.status = 500;
                    response = {{"ok", false}, {"error", error.what()}};
                }
                r.set_content(response.dump(), "application/json");
            });

    srv.Post("/api/component-libraries",
             [&db, import_manager](const httplib::Request& req, httplib::Response& r)
             {
                 json j;
                 try
                 {
                     auto body = json::parse(req.body.empty() ? "{}" : req.body);
                     std::string action = body.value("action", "list");
                     // Inspect paths before taking the database session. Lock-only
                     // updates preserve the configured roots and need no filesystem I/O.
                     if (action == "add" || (action == "update" && !body.contains("locked")))
                     {
                         if (auto error = component_path_error(body))
                         {
                             r.set_content(error->dump(), "application/json");
                             return;
                         }
                     }
                     if (action == "add")
                     {
                         auto session = db.session();
                         storage::ComponentLibraryRepository repo(session);
                         storage::ComponentLibraryRepository::ComponentLibrary lib;
                         lib.id = short_uuid();
                         lib.name = body.value("name", "New Library");
                         lib.symbol_path = body.value("symbol_path", "");
                         lib.footprint_path = body.value("footprint_path", "");
                         lib.model_3d_path = body.value("model_3d_path", "");
                         lib.enabled = body.value("enabled", true);
                         auto ins = repo.insert(lib);
                         j["ok"] = ins.has_value();
                         if (ins)
                             j["library"] = {{"id", ins->id}, {"name", ins->name}};
                         else
                             j["error"] = ins.error().format_message();
                     }
                     else if (action == "update")
                     {
                         std::string id = body.value("id", "");
                         if (id.empty())
                         {
                             j["ok"] = false;
                             j["error"] = "Missing id";
                         }
                         else
                         {
                             auto session = db.session();
                             storage::ComponentLibraryRepository repo(session);
                             // Load existing to preserve lock state
                             storage::ComponentLibraryRepository::ComponentLibrary cur;
                             bool found = false;
                             if (auto all = repo.find_all())
                                 for (auto& l : *all)
                                     if (l.id == id)
                                     {
                                         cur = l;
                                         found = true;
                                         break;
                                     }
                             if (!found)
                             {
                                 j["ok"] = false;
                                 j["error"] = "Not found";
                             }
                             else
                             {
                                 bool toggling_lock = body.contains("locked");
                                 if (!toggling_lock && cur.locked)
                                 {
                                     j["ok"] = false;
                                     j["error"] = "Library is locked";
                                 }
                                 else if (toggling_lock && cur.is_protected &&
                                          !body["locked"].get<bool>())
                                 {
                                     j["ok"] = false;
                                     j["error"] = "Official KiCad library cannot be unlocked";
                                 }
                                 else
                                 {
                                     storage::ComponentLibraryRepository::ComponentLibrary lib;
                                     lib.id = id;
                                     lib.name = toggling_lock ? cur.name
                                                              : body.value("name", "");
                                     lib.symbol_path = toggling_lock ? cur.symbol_path
                                                                     : body.value("symbol_path", "");
                                     lib.footprint_path = toggling_lock ? cur.footprint_path
                                                                        : body.value("footprint_path", "");
                                     lib.model_3d_path = toggling_lock ? cur.model_3d_path
                                                                       : body.value("model_3d_path", "");
                                     lib.enabled = toggling_lock ? cur.enabled
                                                                 : body.value("enabled", true);
                                     lib.sort_order = cur.sort_order;
                                     lib.locked = toggling_lock ? body["locked"].get<bool>()
                                                                : cur.locked;
                                     // Recompute protection from the NEW paths 鈥?pointing a
                                     // library at the official KiCad directories must (re)set
                                     // the protected flag, otherwise the app-level guard is
                                     // silently bypassed for a "user" library.
                                     if (!toggling_lock)
                                     {
                                         auto is_official = [](const std::string& p) -> bool
                                         {
                                             if (p.empty())
                                             {
                                                 return false;
                                             }
                                             auto norm = services::normalized_source_path(p)
                                                             .generic_string();
                                             return norm.contains("/share/kicad");
                                         };
                                         lib.is_protected = is_official(lib.symbol_path) ||
                                                            is_official(lib.footprint_path) ||
                                                            is_official(lib.model_3d_path);
                                         if (lib.is_protected)
                                         {
                                             lib.locked = true;
                                         }
                                     }
                                     else
                                     {
                                         lib.is_protected = cur.is_protected;
                                     }
                                     auto upd = repo.update(lib);
                                     j["ok"] = upd.has_value();
                                     if (!upd)
                                     {
                                         j["error"] = upd.error().format_message();
                                     }
                                 }
                             }
                         }
                     }
                     else if (action == "remove")
                     {
                         std::string id = body.value("id", "");
                         if (id.empty())
                         {
                             j["ok"] = false;
                             j["error"] = "Missing id";
                         }
                         else
                         {
                             auto session = db.session();
                             storage::ComponentLibraryRepository repo(session);
                             // Remove is purely an unregister operation 鈥?it does
                             // NOT touch local .kicad_sym files (those are the
                             // user's source of truth). Cascading deletes
                             // any libraries, symbols, and links under this
                             // component library so no orphan rows remain.
                             // Target must exist 鈥?a silent no-op must not be
                             // reported as success.
                             bool exists = false;
                             if (auto all = repo.find_all())
                             {
                                 for (auto& cl : *all)
                                 {
                                     if (cl.id == id)
                                     {
                                         exists = true;
                                         break;
                                     }
                                 }
                             }
                             if (!exists)
                             {
                                 j["ok"] = false;
                                 j["error"] = "Component library not found: " + id;
                             }
                             else
                             {
                                 // All parameters bound via ?1 鈥?never spliced.
                                 auto exec = [&](const char* sql) -> bool
                                 {
                                     std::string error;
                                     if (!exec_param(session.handle(), sql, id, error))
                                     {
                                         j["error"] = error;
                                         return false;
                                     }
                                     return true;
                                 };
                                 if (auto begun = session.begin(); !begun)
                                 {
                                     j["ok"] = false;
                                     j["error"] = begun.error().format_message();
                                 }
                                 else
                                 {
                                     bool ok = true;
                                     ok = ok && exec(
                                         "DELETE FROM symbols WHERE library_id IN "
                                         "(SELECT id FROM libraries WHERE component_library_id = ?1)");
                                     ok = ok && exec(
                                         "DELETE FROM symbol_footprint_links WHERE symbol_id IN "
                                         "(SELECT id FROM symbols WHERE library_id IN "
                                         "(SELECT id FROM libraries WHERE component_library_id = ?1))");
                                     ok = ok && exec(
                                         "DELETE FROM footprint_model_links WHERE footprint_id IN "
                                         "(SELECT id FROM footprints WHERE library_path IN "
                                         "(SELECT file_path FROM libraries WHERE component_library_id = ?1 "
                                         "AND file_path != ''))");
                                     ok = ok && exec(
                                         "DELETE FROM libraries WHERE component_library_id = ?1");
                                     if (ok)
                                     {
                                         auto rem = repo.remove(id);
                                         if (!rem)
                                         {
                                             j["error"] = rem.error().format_message();
                                             ok = false;
                                         }
                                     }
                                     auto finished = ok ? session.commit() : session.rollback();
                                     if (!finished)
                                     {
                                         j["error"] = finished.error().format_message();
                                         ok = false;
                                     }
                                     j["ok"] = ok;
                                 }
                             }
                         }
                     }
                     else if (action == "import")
                     {
                         // Re-import all enabled component libraries. force=true:
                         // a manual "reimport" must re-scan regardless of mtime.
                         if (!import_manager)
                         {
                             j["ok"] = false;
                             j["error"] = "Import manager unavailable";
                         }
                         else
                         {
                             auto result =
                                 import_manager->import_all(ImportManager::Options{.force = true});
                             j = import_result_json(result);
                             j["imported_symbols"] = result.symbols;
                             j["imported_footprints"] = result.footprints;
                             j["imported_models_3d"] = result.models_3d;
                             j["auto_linked"] = result.linked_symbols;
                             j["libraries"] = json::array();
                         }
                     }
                     else
                     {
                         j["ok"] = false;
                         j["error"] = "Unknown action: " + action;
                     }
                 }
                 catch (...)
                 {
                     j["ok"] = false;
                     j["error"] = "Parse error";
                 }
                 r.set_content(j.dump(), "application/json");
             });

    srv.Post("/api/db/reset",
             [&db, import_manager](const httplib::Request& req, httplib::Response& r)
             {
                 json j;
                 // Confirmation guard: this endpoint wipes all data
                 bool confirmed = false;
                 try
                 {
                     confirmed = json::parse(req.body.empty() ? "{}" : req.body)
                                     .value("confirm", false);
                 }
                 catch (const nlohmann::json::parse_error&)
                 {
                     confirmed = false;
                 }
                 if (!confirmed)
                 {
                     j["ok"] = false;
                     j["error"] = "Confirmation required (send {\"confirm\":true})";
                     r.set_content(j.dump(), "application/json");
                     return;
                 }
                 // Stop import thread before clearing to avoid race
                 if (import_manager)
                     import_manager->stop_async();
                 auto session = db.session();
                 auto exec = [&](const char* sql) -> bool
                 {
                     char* errmsg = nullptr;
                     if (sqlite3_exec(session.handle(), sql, nullptr, nullptr, &errmsg) != SQLITE_OK)
                     {
                         j["error"] = errmsg != nullptr ? errmsg : "SQL error";
                         sqlite3_free(errmsg);
                         return false;
                     }
                     return true;
                 };
                 // Reset clears imported data only 鈥?component_libraries
                 // (library config + lock flags) and settings are preserved.
                 // imported_files (mtime snapshots) is cleared too, otherwise
                 // the next import would skip everything as "unchanged".
                 const char* clears[] = {"DELETE FROM symbols",
                                         "DELETE FROM footprints",
                                         "DELETE FROM models_3d",
                                         "DELETE FROM libraries",
                                         "DELETE FROM symbol_footprint_links",
                                         "DELETE FROM footprint_model_links",
                                         "DELETE FROM imported_files"};
                 for (const char* sql : clears)
                 {
                     if (!exec(sql))
                     {
                         j["ok"] = false;
                         r.set_content(j.dump(), "application/json");
                         return;
                     }
                 }
                 if (!exec("VACUUM"))
                 {
                     j["ok"] = false;
                     r.set_content(j.dump(), "application/json");
                     return;
                 }
                 j["ok"] = true;
                 j["message"] = "All data cleared. Reimport or restart.";
                 r.set_content(j.dump(), "application/json");
                 // Restart import
                 if (import_manager)
                     import_manager->start_async();
             });
}

// ============================================================
// Type routes
// ============================================================

// 瀛楁璇诲彇涓€寰嬪尯鍒嗐€岀己鐪併€嶄笌銆屾樉寮忕粰鍊笺€嶏細缂虹渷 = 涓嶆敼锛屾樉寮忥紙鍚┖涓诧級= 瑕嗙洊锛?
// 绫诲瀷涓嶅鏁翠綋鎷掔粷骞剁粰鍑哄叿浣撳瓧娈靛悕锛岃€屼笉鏄 json 鎶涘嚭鍘诲彉鎴愭棤淇℃伅鐨勫け璐ャ€?
inline bool read_text_field(const json& body, const char* key, std::string& out,
                            std::string& error)
{
    if (!body.contains(key) || body[key].is_null()) return true;
    if (!body[key].is_string())
    {
        error = std::string("Field must be a string: ") + key;
        return false;
    }
    out = body[key].get<std::string>();
    return true;
}

inline bool read_patch_field(const json& body, const char* key, std::optional<std::string>& out,
                             std::string& error)
{
    if (!body.contains(key) || body[key].is_null()) return true;
    if (!body[key].is_string())
    {
        error = std::string("Field must be a string: ") + key;
        return false;
    }
    out = body[key].get<std::string>();
    return true;
}

void register_type_routes(httplib::Server& srv, storage::Database& db)
{
    // 娉ㄥ唽鏃舵嫹璐濇寮忚矾寰勶紱鍧忛厤缃緷鐒舵敞鍐岋紝缂哄惎鍔ㄨ閰嶅垯璇锋眰杩斿洖鏄庣‘閿欒銆?
    const auto matching_context = configured_type_matching_context(db);
    srv.Get("/api/component-type-matching",
            [matching_context](const httplib::Request&, httplib::Response& response)
            {
                auto out = matching_context ? type_matching_get(*matching_context)
                    : type_matching_error(matching_context.error().format_message());
                response.set_content(out.dump(), "application/json");
            });
    using MatchingOperation = json (*)(const TypeMatchingContext&, const json&);
    auto register_matching_post = [&](const char* path, MatchingOperation operation)
    {
        srv.Post(path, [matching_context, operation](const httplib::Request& request,
                                                    httplib::Response& response)
        {
            json out;
            if (!matching_context)
                out = type_matching_error(matching_context.error().format_message());
            else
            {
                auto body = type_matching_parse_request_body(request.body);
                out = body ? operation(*matching_context, *body)
                           : type_matching_error(body.error().format_message());
            }
            response.set_content(out.dump(), "application/json");
        });
    };
    register_matching_post("/api/component-type-matching", type_matching_save);
    register_matching_post("/api/component-type-matching/reload", type_matching_reload);
    register_matching_post("/api/component-type-matching/preview", type_matching_preview);
    register_matching_post("/api/component-type-matching/apply", type_matching_apply);

    // 鍒犻櫎鐨勫疄闄呭叆鍙ｅ湪 api/type_mutation.h锛氬紩鐢ㄦ鏌?+ 鎸佷箙鍖栧垹闄ゅ悓 session锛?
    // 娴嬭瘯鐩存帴璋冨畠锛屽洜姝?琚紩鐢ㄦ椂鎷掑垹"鏄湪鐪熷疄鐢熶骇璺緞涓婇獙璇佺殑銆?

    auto register_kind = [&](const char* base, bool package)
    {
        auto& reg = core::TypeRegistry::instance();
        srv.Get(base,
                [&reg, package](const httplib::Request&, httplib::Response& r)
                {
                    json arr = json::array();
                    for (auto& entry : package ? reg.package_types() : reg.component_types())
                    {
                        json o;
                        o["id"] = entry.id;
                        o["name"] = entry.name;
                        o["icon"] = entry.icon;
                        o["color"] = entry.color;
                        if (package) o["category"] = entry.category;
                        // 鍐呯疆鍒ゅ畾鐢辨湇鍔＄缁欏嚭锛氱晫闈笉蹇呰嚜宸辩淮鎶や竴浠?ID 娓呭崟锛?
                        // 涔熷氨涓嶄細鎶婂唴缃」閿欐斁鎴愬彲鍒狅紙鐪熸鐨勬嫆缁濅粛鍦ㄦ湇鍔＄锛?
                        o["builtin"] = package ? core::is_builtin_package_id(entry.id)
                                               : core::is_builtin_component_id(entry.id);
                        arr.push_back(o);
                    }
                    r.set_content(arr.dump(), "application/json");
                });

        srv.Post(base,
                 [&reg, &db, package](const httplib::Request& req, httplib::Response& r)
                 {
                     json j;
                     try
                     {
                         auto body = json::parse(req.body.empty() ? "{}" : req.body);
                         std::string error;
                         std::string action;
                         std::string id;
                         std::string name;
                         if (!read_text_field(body, "action", action, error) ||
                             !read_text_field(body, "id", id, error) ||
                             !read_text_field(body, "name", name, error))
                         {
                             j["ok"] = false;
                             j["error"] = error;
                             r.set_content(j.dump(), "application/json");
                             return;
                         }
                         // 鐪佺暐瀛楁 = 涓嶆敼锛涙樉寮忕┖涓?= 娓呯┖ 鈥斺€?鍙敼鍚嶄笉浼氬啀鎶规帀鍥炬爣/棰滆壊
                         core::TypePatch patch;
                         if (!read_patch_field(body, "name", patch.name, error) ||
                             !read_patch_field(body, "icon", patch.icon, error) ||
                             !read_patch_field(body, "color", patch.color, error) ||
                             !read_patch_field(body, "category", patch.category, error))
                         {
                             j["ok"] = false;
                             j["error"] = error;
                             r.set_content(j.dump(), "application/json");
                             return;
                         }

                         // 鏃ц皟鐢ㄥ彧缁?name锛氫粎鍦ㄥ悕瀛楀敮涓€鍖归厤鏃舵墠璁?
                         if (id.empty() && !name.empty())
                         {
                             auto found = package ? reg.package_by_name(name)
                                                  : reg.component_by_name(name);
                             if (found) id = found->id;
                         }

                         if (action == "add")
                         {
                             core::TypeEntry entry;
                             entry.name = name;
                             entry.icon = patch.icon.value_or("");
                             entry.color = patch.color.value_or("");
                             entry.category = patch.category.value_or("Other");
                             // 鍐呭瓨涓庢枃浠朵竴璧锋彁浜わ細鍐欑洏澶辫触灏变笉鍐嶆姤 ok
                             auto added = package ? reg.add_package_type_persistent(entry)
                                                  : reg.add_component_type_persistent(entry);
                             if (added)
                             {
                                 j["ok"] = true;
                                 j["id"] = *added;
                             }
                             else
                             {
                                 j["ok"] = false;
                                 j["error"] = added.error().format_message();
                             }
                         }
                         else if (action == "rename" || action == "update")
                         {
                             auto updated = package
                                                ? reg.update_package_type_persistent(id, patch)
                                                : reg.update_component_type_persistent(id, patch);
                             if (updated)
                             {
                                 j["ok"] = true;
                                 j["id"] = id;
                             }
                             else
                             {
                                 j["ok"] = false;
                                 j["error"] = updated.error().format_message();
                             }
                         }
                         else if (action == "remove")
                         {
                             // 鍐呯疆鍒ゅ畾銆佸紩鐢ㄦ鏌ヤ笌鎸佷箙鍖栧垹闄ら兘鍦ㄧ敓浜?helper 閲岋紝
                             // 璁℃暟澶辫触鏄敊璇紙涓嶆斁琛岋級锛岃寮曠敤鍒欏瀹炲洖鎶ュ紩鐢ㄦ暟
                             auto outcome = package ? api::remove_package_type(db, id)
                                                    : api::remove_component_type(db, id);
                             if (!outcome)
                             {
                                 j["ok"] = false;
                                 j["error"] = outcome.error().format_message();
                             }
                             else
                             {
                                 j["ok"] = outcome->removed;
                                 j["references"] = outcome->references;
                                 j["matching_referenced"] = outcome->matching_referenced;
                                 j["reason"] = outcome->reason;
                                 if (outcome->removed)
                                 {
                                     j["id"] = id;
                                 }
                                 else
                                 {
                                     j["error"] = outcome->reason;
                                 }
                             }
                         }
                         else
                         {
                             j["ok"] = false;
                             j["error"] = "Invalid";
                         }
                     }
                     catch (const std::exception& e)
                     {
                         j["ok"] = false;
                         j["error"] = std::string("Invalid request: ") + e.what();
                     }
                     catch (...)
                     {
                         j["ok"] = false;
                         j["error"] = "Invalid request";
                     }
                     r.set_content(j.dump(), "application/json");
                 });
    };

    register_kind("/api/component-types", false);
    register_kind("/api/package-types", true);
}

// ============================================================
// Status / heartbeat routes
// ============================================================

void register_status_routes(httplib::Server& srv, storage::Database& /*db*/)
{
    srv.Post("/api/pick-folder",
             [](const httplib::Request&, httplib::Response& r)
             {
                 auto path = platform::NativeFolderDialog::pick_folder();
                 json j;
                 if (!path.empty())
                 {
                     j["ok"] = true;
                     j["path"] = path;
                 }
                 else
                 {
                     j["ok"] = false;
                     j["path"] = "";
                 }
                 r.set_content(j.dump(), "application/json");
             });

    srv.Get("/api/rules",
            [](const httplib::Request&, httplib::Response& r)
            {
                auto rules = classifier::RuleLoader::default_rules();
                json arr = json::array();
                for (auto& rule : rules)
                {
                    json o;
                    o["name"] = rule.name;
                    o["target"] = rule.target_library;
                    o["confidence"] = rule.confidence;
                    arr.push_back(o);
                }
                r.set_content(arr.dump(), "application/json");
            });
}

// ============================================================
// Classify / check / automatch / import routes
// ============================================================

void register_classify_routes(httplib::Server& srv, storage::Database& /*db*/,
                              ImportManager* import_manager)
{
    srv.Post("/api/import",
             [import_manager](const httplib::Request& req, httplib::Response& r)
             {
                 std::string dir = req.get_param_value("dir");
                 json j;
                 if (dir.empty())
                 {
                     j["ok"] = false;
                     j["error"] = "Missing 'dir' parameter";
                 }
                 else if (!std::filesystem::is_directory(dir))
                 {
                     j["ok"] = false;
                     j["error"] = "Not a directory";
                 }
                 else if (!import_manager)
                 {
                     j["ok"] = false;
                     j["error"] = "Import manager unavailable";
                 }
                 else
                 {
                     auto result = import_manager->import_directory(dir);
                     j = import_result_json(result);
                 }
                 r.set_content(j.dump(), "application/json");
             });
}

}  // namespace kforge::api
