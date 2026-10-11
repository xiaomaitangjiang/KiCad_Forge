// Route handlers extracted from setup_routes()
#include "classifier/rule_loader.h"
#include "core/model/type_registry.h"
#include "core/repo/repositories.h"
#include "interface/api/api_server.h"
#include "interface/api/import_orchestrator.h"
#include "interface/api/routes.h"
#include "interface/api/staged_import.h"
#include "interface/service/classification/service.h"
#include "interface/service/correspondence/service.h"
#include "interface/service/import/pipeline.h"
#include "interface/service/library/binding.h"
#include "interface/service/library/service.h"
#include "interface/service/library/source_policy.h"
#include "platform/folder_dialog.h"
#include "plugin/plugin_manager.h"
#include "util/config_store.h"
#include "util/logger.h"
#include "util/result.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

namespace kforge::api
{

// ---- Search helpers ----

// Execute a statement with one bound text parameter (?1) — no string splicing.
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
        // 统一数据版本：导入、源文件变更、删除都会自增 ——
        // 只盯导入序号会让绑定索引在别的写入之后继续用旧快照
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

// Symbol → library(id) → component_library_id → locked
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

// Footprint → component library by matching library_path prefix
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
    snprintf(buf, sizeof(buf), "%016llX", dis(gen));
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
                auto all_syms = sr.find_all();
                std::unordered_set<std::string> used_libs;
                if (all_syms)
                {
                    for (auto& s : *all_syms)
                    {
                        used_libs.insert(s.library_id());
                    }
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
                        if (!used_libs.contains(l.id))
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
                        int count = 0;
                        if (all_syms)
                        {
                            for (auto& s : *all_syms)
                            {
                                if (s.library_id() == l.id)
                                {
                                    count++;
                                }
                            }
                        }
                        o["symbol_count"] = count;
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
                             // protection lookup failed — reported above
                         }
                         else
                         {
                         int removed = 0;
                         auto syms = sr.find_by_library(id);
                         if (syms)
                             for (auto& s : *syms)
                                 if (sr.remove(s.id()))
                                     removed++;
                         // delete_file=false → unregister only (DB row removed,
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
                         j["unregistered_only"] = !delete_file;
                         if (file_deleted)
                             j["deleted_file"] = file_path;
                         }
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
                             // 源文件改写 + 行删除由统一变更服务接管（A05）：
                             // 失败时文件与库行一起回退，不再各写各的
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

void register_symbol_routes(httplib::Server& srv, storage::Database& db)
{
    srv.Get("/api/symbols",
            [&db](const httplib::Request& req, httplib::Response& r)
            {
                auto session = db.session();
                storage::SymbolRepository repo(session);
                storage::RelationshipRepository rr(session);
                // 一次取全部关联，避免每个符号两次点查（上万行时会把库锁占住数秒）
                std::unordered_map<std::string, std::string> sym_fp_links;
                if (auto links = rr.find_all_symbol_links())
                {
                    sym_fp_links = std::move(*links);
                }
                std::unordered_map<std::string, std::vector<std::string>> fp_model_links;
                if (auto links = rr.find_all_model_links())
                {
                    fp_model_links = std::move(*links);
                }
                std::string q = req.get_param_value("q");
                std::string lib_id = req.get_param_value("library");
                auto result = q.empty()
                                ? (lib_id.empty() ? repo.find_all() : repo.find_by_library(lib_id))
                                : repo.search(q);
                // Pagination: clamp to safe ranges — a negative offset or an
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
                // Resolve lock state once: library_id → locked, via
                // libraries.component_library_id → component_libraries.locked
                std::unordered_map<std::string, bool> lib_locked;
                {
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
                }
                int total = result ? (int) result->size() : 0;
                int start = std::min(offset, total);
                int end = (limit > 0) ? std::min(start + limit, total) : total;
                json arr = json::array();
                if (result)
                    for (int i = start; i < end; i++)
                    {
                        auto& sym = (*result)[i];
                        json s;
                        s["id"] = sym.id();
                        s["name"] = sym.name();
                        s["library_id"] = sym.library_id();
                        s["value"] = sym.default_value();
                        s["footprint"] = sym.footprint();
                        s["pins"] = sym.pin_count();
                        s["mpn"] = sym.mpn();
                        // type 是展示名，type_id 是身份：重命名不改筛选结果
                        s["type_id"] = sym.component_type_id;
                        s["type"] = core::TypeRegistry::instance().component_display(sym.component_type_id);
                        s["has_footprint"] = !sym.footprint().empty();
                        s["locked"] = lib_locked[sym.library_id()];
                        bool has_3d = false;
                        if (auto link = sym_fp_links.find(sym.id()); link != sym_fp_links.end())
                        {
                            has_3d = fp_model_links.contains(link->second);
                        }
                        s["has_3d_model"] = has_3d;
                        arr.push_back(s);
                    }
                r.set_content(arr.dump(), "application/json");
            });

    srv.Get("/api/footprints",
            [&db](const httplib::Request&, httplib::Response& r)
            {
                auto session = db.session();
                storage::FootprintRepository repo(session);
                auto result = repo.find_all();
                json arr = json::array();
                if (result)
                    for (auto& fp : *result)
                    {
                        json f;
                        f["id"] = fp.id();
                        f["name"] = fp.name();
                        f["description"] = fp.description();
                        f["pad_count"] = fp.pad_count();
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
                json j;
                j["footprint_id"] = b.fp_id;
                j["footprint_name"] = b.fp_name;
                j["model_id"] = b.model_id;
                j["model_name"] = b.model_name;
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
                 }
                 r.set_content(j.dump(), "application/json");
             });

    srv.Get("/api/footprints/search",
            [](const httplib::Request& req, httplib::Response& r)
            {
                auto q = req.get_param_value("q");
                auto anchor = req.get_param_value("near");  // "near" is a Windows macro
                int limit = param_limit(req, 20);
                auto mgr = g_binding_mgr.get();
                auto result = mgr->search_footprints(q.empty() ? "" : q, limit, anchor);
                json arr = json::array();
                for (auto& fp : result)
                {
                    json f;
                    f["id"] = fp.id;
                    f["name"] = fp.name;
                    f["library"] = fp.library;  // .pretty lib name — build "Lib:Name" refs
                    f["score"] = fp.score;
                    f["pad_count"] = fp.pad_count;
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
                json arr = json::array();
                for (auto& m : result)
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
                json arr = json::array();
                for (auto& s : result)
                {
                    json j;
                    j["id"] = s.id;
                    j["name"] = s.name;
                    j["score"] = s.score;
                    arr.push_back(j);
                }
                r.set_content(arr.dump(), "application/json");
            });
}

// ============================================================
// Plugin routes
// ============================================================

void register_plugin_routes(httplib::Server& srv, plugin::PluginManager* plugins,
                            storage::Database& db)
{
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
                        act["preflight"] = a.preflight;
                        act["auto_args"] = a.auto_args;
                        act["schema"] = a.schema;
                        // Always export the button block (incl. show:false) — the UI
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
                     // plugin process argv — restrict to a safe alphabet
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
                     // 声明了宿主协议的 action 由宿主接管：插件只写暂存目录，
                     // 目标库的校验、合并与数据库提交都在宿主侧完成
                     std::string protocol;
                     for (const auto& manifest : plugins->available_plugins())
                     {
                         if (manifest.id != plugin_id) continue;
                         for (const auto& declared : manifest.actions)
                             if (declared.id == action) protocol = declared.protocol;
                     }

                     if (protocol == "staged-import-v1")
                     {
                         StagedImporter::Request request;
                         request.plugin_id = plugin_id;
                         request.action = action;
                         request.provider = body.value("provider", "LCSC");
                         request.external_id = body.value("lcsc_id", body.value("source", ""));
                         request.target_library_id = body.value("target_library_id", "");
                         request.target_library_path = body.value("target_library", "");
                         request.operation_id = body.value("operation_id", "");
                         auto choice = body.value("footprint_choice", json::object());
                         request.footprint_mode = choice.value("mode", "new");
                         request.footprint_id = body.value("footprint_id", "");
                         if (request.footprint_mode == "reuse" && request.footprint_id.empty())
                         {
                             // 旧前端只给 library/name：在已登记封装里按 (库, 名) 唯一定位
                             std::string library = choice.value("library", "");
                             std::string name = choice.value("name", "");
                             if (!name.empty())
                             {
                                 auto session = db.session();
                                 if (auto all = storage::FootprintRepository(session).find_all())
                                 {
                                     for (const auto& fp : *all)
                                     {
                                         if (fp.name() != name) continue;
                                         auto parent = std::filesystem::path(fp.library_path())
                                                           .parent_path().stem().string();
                                         if (!library.empty() && parent != library) continue;
                                         request.footprint_id = fp.id();
                                         break;
                                     }
                                 }
                             }
                         }

                         StagedImporter importer(db, *plugins);
                         auto staged = importer.run(request);
                         if (!staged)
                         {
                             j["ok"] = false;
                             j["error"] = staged.error().format_message();
                         }
                         else
                         {
                             j["plugin_result"] = *staged;
                             j["ok"] = staged->value("ok", false);
                         }
                         r.set_content(j.dump(), "application/json");
                         return;
                     }

                     auto result = plugins->execute(plugin_id, action, body.dump());
                     if (!result)
                     {
                         j["ok"] = false;
                         j["error"] = result.error().format_message();
                         r.set_content(j.dump(), "application/json");
                         return;
                     }
                     auto out = json::parse(*result);
                     j["plugin_result"] = out;
                     j["ok"] = out.value("ok", false);
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

    // Plugin icon routes
    srv.Get(R"(/api/plugins/([^/]+)/icon/([^/]+))",
            [plugins](const httplib::Request& req, httplib::Response& r)
            {
                auto pid = req.matches[1];
                auto fn = req.matches[2];
                auto pp = plugins->plugin_path(pid.str());
                auto icon_path = pp / fn.str();
                if (std::filesystem::exists(icon_path))
                {
                    std::ifstream f(icon_path, std::ios::binary);
                    std::string data((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                    std::string ext = icon_path.extension().string();
                    std::string mime = (ext == ".svg") ? "image/svg+xml" : "image/png";
                    r.set_content(data, mime);
                }
                else
                {
                    r.status = 404;
                }
            });
    srv.Get(R"(/api/plugins/([^/]+)/icon)",
            [plugins](const httplib::Request& req, httplib::Response& r)
            {
                auto pid = req.matches[1];
                auto pp = plugins->plugin_path(pid.str());
                for (auto& fn : {"icon.svg", "icon.png"})
                {
                    auto icon_path = pp / fn;
                    if (std::filesystem::exists(icon_path))
                    {
                        std::ifstream f(icon_path, std::ios::binary);
                        std::string data((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
                        r.set_content(data, (std::string(fn) == "icon.svg") ? "image/svg+xml"
                                                                            : "image/png");
                        return;
                    }
                }
                r.status = 404;
            });
}

// ============================================================
// Settings / Component libraries / DB reset
// ============================================================
// Config (JSON) routes — replaces the old DB `settings` table.
// ============================================================

void register_settings_routes(httplib::Server& srv, storage::Database& db, ImportManager* import_manager,
                             util::ConfigStore* config)
{
    (void)db;
    (void)import_manager;

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

    srv.Post("/api/component-libraries",
             [&db, import_manager](const httplib::Request& req, httplib::Response& r)
             {
                 json j;
                 try
                 {
                     auto body = json::parse(req.body.empty() ? "{}" : req.body);
                     std::string action = body.value("action", "list");
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
                                     // Recompute protection from the NEW paths — pointing a
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
                             // Remove is purely an unregister operation — it does
                             // NOT touch local .kicad_sym files (those are the
                             // user's source of truth). Cascading deletes
                             // any libraries, symbols, and links under this
                             // component library so no orphan rows remain.
                             // Target must exist — a silent no-op must not be
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
                                 // All parameters bound via ?1 — never spliced.
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
                             j["ok"] = true;
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
                 // Reset clears imported data only — component_libraries
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

void register_type_routes(httplib::Server& srv, storage::Database& db)
{
    // 删除类型前先数引用：还有符号/封装在用它就拒绝，让用户先显式重分类
    auto component_refs = [](storage::Database& database, const std::string& id)
    {
        auto session = database.session();
        return storage::SymbolRepository(session).count_by_component_type(id);
    };
    auto package_refs = [](storage::Database& database, const std::string& id)
    {
        auto session = database.session();
        return storage::FootprintRepository(session).count_by_package_type(id);
    };

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
                        arr.push_back(o);
                    }
                    r.set_content(arr.dump(), "application/json");
                });

        srv.Post(base,
                 [&reg, &db, package, component_refs, package_refs](
                     const httplib::Request& req, httplib::Response& r)
                 {
                     json j;
                     try
                     {
                         auto body = json::parse(req.body.empty() ? "{}" : req.body);
                         std::string action = body.value("action", "");
                         std::string id = body.value("id", "");
                         std::string name = body.value("name", "");

                         // 旧调用只给 name：仅在名字唯一匹配时才认
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
                             entry.icon = body.value("icon", "");
                             entry.color = body.value("color", "");
                             entry.category = body.value("category", "Other");
                             auto added = package ? reg.add_package_type(entry)
                                                  : reg.add_component_type(entry);
                             if (added)
                             {
                                 (package ? reg.save_package_types() : reg.save_component_types());
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
                             core::TypeEntry fields;
                             fields.name = name;
                             fields.icon = body.value("icon", "");
                             fields.color = body.value("color", "");
                             fields.category = body.value("category", "");
                             auto updated = package ? reg.update_package_type(id, fields)
                                                    : reg.update_component_type(id, fields);
                             if (updated)
                             {
                                 (package ? reg.save_package_types() : reg.save_component_types());
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
                             bool builtin = package ? core::is_builtin_package_id(id)
                                                    : core::is_builtin_component_id(id);
                             if (id.empty())
                             {
                                 j["ok"] = false;
                                 j["error"] = "Type not found";
                             }
                             else if (builtin)
                             {
                                 // 内置类型先拒 —— 引用计数对它们没有意义
                                 j["ok"] = false;
                                 j["error"] = "Built-in types cannot be deleted";
                             }
                             else
                             {
                                 int references =
                                     package ? package_refs(db, id) : component_refs(db, id);
                                 if (references > 0)
                                 {
                                     j["ok"] = false;
                                     j["references"] = references;
                                     j["error"] = "Type is still referenced; reclassify first";
                                 }
                                 else
                                 {
                                     bool removed = package ? reg.remove_package_type(id)
                                                            : reg.remove_component_type(id);
                                     if (removed)
                                     {
                                         (package ? reg.save_package_types()
                                                  : reg.save_component_types());
                                     }
                                     j["ok"] = removed;
                                     if (!removed)
                                         j["error"] = "Type not found";
                                 }
                             }
                         }
                         else
                         {
                             j["ok"] = false;
                             j["error"] = "Invalid";
                         }
                     }
                     catch (...)
                     {
                         j["ok"] = false;
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
                     j["ok"] = true;
                     j["symbols"] = result.symbols;
                     j["footprints"] = result.footprints;
                     j["errors"] = 0;
                 }
                 r.set_content(j.dump(), "application/json");
             });
}

}  // namespace kforge::api
