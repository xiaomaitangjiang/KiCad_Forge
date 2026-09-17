// Route handlers extracted from setup_routes()
#include "classifier/rule_loader.h"
#include "core/model/type_registry.h"
#include "core/repo/repositories.h"
#include "interface/api/api_server.h"
#include "interface/api/import_orchestrator.h"
#include "interface/api/routes.h"
#include "interface/service/classification/service.h"
#include "interface/service/correspondence/service.h"
#include "interface/service/import/pipeline.h"
#include "interface/service/library/binding.h"
#include "interface/service/library/service.h"
#include "platform/folder_dialog.h"
#include "plugin/plugin_manager.h"
#include "util/config_store.h"
#include "util/logger.h"
#include "util/result.h"

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <unordered_map>

using json = nlohmann::json;

namespace kforge::api
{

// ---- In-memory index holder: rebuilt when an import has finished, so
// footprints/models imported after process start become searchable.
// Defined at namespace scope so its static instance can outlive any local
// lambda capture and survive the static-init order issues local classes have.
struct BindingMgrHolder
{
    sqlite3* db{nullptr};
    ImportOrchestrator* orch_ptr{nullptr};
    std::mutex mtx;
    int64_t last_seq{0};
    std::shared_ptr<services::SymbolBindingManager> mgr;

    void init(sqlite3* d, ImportOrchestrator* o)
    {
        db = d;
        orch_ptr = o;
    }

    std::shared_ptr<services::SymbolBindingManager> get()
    {
        std::lock_guard lock(mtx);
        auto cur = orch_ptr ? orch_ptr->import_seq() : 0;
        if (!mgr || cur != last_seq)
        {
            mgr = std::make_shared<services::SymbolBindingManager>(db);
            last_seq = cur;
        }
        return mgr;
    }
};
static BindingMgrHolder g_binding_mgr;

// ---- Lock helpers: a locked component library protects its symbols from
// delete/modify/bind/classify. Official KiCad libs are locked & protected.
static bool library_locked(sqlite3* db, const std::string& component_lib_id)
{
    storage::ComponentLibraryRepository repo(db);
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
static bool symbol_locked(sqlite3* db, const std::string& symbol_id)
{
    storage::SymbolRepository sr(db);
    auto sym = sr.find_by_id(symbol_id);
    if (!sym)
    {
        return false;
    }
    storage::LibraryRepository lr(db);
    if (auto all = lr.find_all())
    {
        for (auto& l : *all)
        {
            if (l.id == sym->library_id() && !l.component_library_id.empty())
            {
                return library_locked(db, l.component_library_id);
            }
        }
    }
    return false;
}

// Footprint → component library by matching library_path prefix
static bool footprint_locked(sqlite3* db, const std::string& footprint_id)
{
    storage::FootprintRepository fr(db);
    auto fp = fr.find_by_id(footprint_id);
    if (!fp || fp->library_path().empty())
    {
        return false;
    }
    const auto& fp_path = fp->library_path();
    storage::ComponentLibraryRepository repo(db);
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

void register_library_routes(httplib::Server& srv, sqlite3* db, ImportOrchestrator* import_orch)
{
    srv.Get("/api/libraries",
            [db](const httplib::Request&, httplib::Response& r)
            {
                storage::SymbolRepository sr(db);
                storage::LibraryRepository lr(db);
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
                storage::ComponentLibraryRepository cl_repo(db);
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
             [db](const httplib::Request& req, httplib::Response& r)
             {
                 json j;
                 try
                 {
                     auto body = json::parse(req.body.empty() ? "{}" : req.body);
                     std::string action = body.value("action", "");
                     storage::LibraryRepository lr(db);
                     if (action == "delete")
                     {
                         std::string id = body.value("id", "");
                         bool owner_locked = false;
                         {
                             storage::LibraryRepository lrr(db);
                             if (auto libs = lrr.find_all())
                                 for (auto& l : *libs)
                                     if (l.id == id && !l.component_library_id.empty() &&
                                         library_locked(db, l.component_library_id))
                                     {
                                         owner_locked = true;
                                         break;
                                     }
                         }
                         if (owner_locked)
                         {
                             j["ok"] = false;
                             j["error"] = "Library is locked";
                         }
                         else
                         {
                         storage::LibraryRepository lrr(db);
                         storage::SymbolRepository sr(db);
                         auto libs = lrr.find_all();
                         std::string file_path;
                         if (libs)
                             for (auto& l : *libs)
                                 if (l.id == id)
                                     file_path = l.file_path.string();
                         int removed = 0;
                         auto syms = sr.find_by_library(id);
                         if (syms)
                             for (auto& s : *syms)
                                 if (sr.remove(s.id()))
                                     removed++;
                         bool file_deleted = false;
                         if (!file_path.empty() && std::filesystem::exists(file_path))
                         {
                             std::filesystem::remove(file_path);
                             file_deleted = true;
                         }
                         auto db_r = lrr.remove(id);
                         j["ok"] = db_r.has_value();
                         if (file_deleted)
                             j["deleted_file"] = file_path;
                         }
                     }
                     else if (action == "delete_symbol")
                     {
                         storage::SymbolRepository sr(db);
                         std::string sym_id = body.value("id", "");
                         if (symbol_locked(db, sym_id))
                         {
                             j["ok"] = false;
                             j["error"] = "Symbol belongs to a locked library";
                         }
                         else
                         {
                             auto sym = sr.find_by_id(sym_id);
                             if (!sym)
                             {
                                 j["ok"] = false;
                                 j["error"] = "Not found";
                             }
                             else
                             {
                                 // The library file is the source of truth:
                                 // rewrite it FIRST, and only remove the DB
                                 // row once the file update succeeded — a
                                 // failed rewrite must not leave DB and file
                                 // inconsistent.
                                 bool file_error = false;
                                 std::string file_error_msg;
                                 storage::LibraryRepository lrr(db);
                                 auto libs = lrr.find_all();
                                 if (libs)
                                     for (auto& l : *libs)
                                     {
                                         if (l.id != sym->library_id() || l.file_path.empty())
                                             continue;
                                         if (!std::filesystem::exists(l.file_path))
                                             continue;  // file gone — nothing to sync
                                         file_error = true;
                                         std::ifstream f(l.file_path, std::ios::binary);
                                         std::string text((std::istreambuf_iterator<char>(f)),
                                                          std::istreambuf_iterator<char>());
                                         f.close();
                                         std::string search = "(symbol \"" + sym->name() + "\"";
                                         size_t pos = text.find(search);
                                         if (pos == std::string::npos)
                                         {
                                             file_error_msg = "Symbol node not found in library file";
                                             break;
                                         }
                                         // Reject hits inside comment lines (leading ';')
                                         size_t line_start = text.rfind('\n', pos);
                                         line_start = (line_start == std::string::npos)
                                                          ? 0
                                                          : line_start + 1;
                                         size_t first_nonspace = text.find_first_not_of(" \t", line_start);
                                         if (first_nonspace != std::string::npos &&
                                             text[first_nonspace] == ';')
                                         {
                                             file_error_msg = "Symbol node not found in library file";
                                             break;
                                         }
                                         // Quote-aware depth walk to the closing ')'
                                         int depth = 0;
                                         bool in_s = false;
                                         size_t end = pos;
                                         while (end < text.size())
                                         {
                                             char c = text[end];
                                             if (c == '"' && (end == 0 || text[end - 1] != '\\'))
                                                 in_s = !in_s;
                                             else if (!in_s)
                                             {
                                                 if (c == '(')
                                                     depth++;
                                                 else if (c == ')')
                                                 {
                                                     depth--;
                                                     if (depth == 0)
                                                     {
                                                         end++;
                                                         break;
                                                     }
                                                 }
                                             }
                                             end++;
                                         }
                                         if (depth != 0)
                                         {
                                             file_error_msg = "Malformed symbol node in library file";
                                             break;
                                         }
                                         text.erase(pos, end - pos);
                                         std::ofstream out(l.file_path, std::ios::binary);
                                         out << text;
                                         if (!out)
                                         {
                                             file_error_msg = "Failed to write library file";
                                             break;
                                         }
                                         file_error = false;
                                         break;
                                     }
                                 if (file_error)
                                 {
                                     j["ok"] = false;
                                     j["error"] = file_error_msg.empty() ? "File update failed"
                                                                         : file_error_msg;
                                 }
                                 else if (!sr.remove(sym_id))
                                 {
                                     j["ok"] = false;
                                     j["error"] = "DB remove failed";
                                 }
                                 else
                                 {
                                     j["ok"] = true;
                                 }
                             }
                         }
                     }
                     else if (!action.empty() || body.contains("name"))
                     {
                         std::string name = body.value("name", "");
                         if (!name.empty())
                         {
                             storage::LibraryRepository repo(db);
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

void register_symbol_routes(httplib::Server& srv, sqlite3* db)
{
    srv.Get("/api/symbols",
            [db](const httplib::Request& req, httplib::Response& r)
            {
                storage::SymbolRepository repo(db);
                storage::RelationshipRepository rr(db);
                std::string q = req.get_param_value("q");
                std::string lib_id = req.get_param_value("library");
                auto result = q.empty()
                                ? (lib_id.empty() ? repo.find_all() : repo.find_by_library(lib_id))
                                : repo.search(q);
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
                catch (...)
                {
                }
                // Resolve lock state once: library_id → locked, via
                // libraries.component_library_id → component_libraries.locked
                std::unordered_map<std::string, bool> lib_locked;
                {
                    storage::ComponentLibraryRepository clr(db);
                    std::unordered_map<std::string, bool> cl_locked;
                    if (auto cls = clr.find_all())
                        for (auto& cl : *cls)
                            cl_locked[cl.id] = cl.locked;
                    storage::LibraryRepository lr(db);
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
                        s["type"] = core::component_type_name(static_cast<int>(sym.component_type));
                        s["has_footprint"] = !sym.footprint().empty();
                        s["locked"] = lib_locked[sym.library_id()];
                        bool has_3d = false;
                        auto fp_opt = rr.find_footprint_for_symbol(sym.id());
                        if (fp_opt && *fp_opt)
                        {
                            auto models = rr.find_models_for_footprint(**fp_opt);
                            has_3d = (models && !models->empty());
                        }
                        s["has_3d_model"] = has_3d;
                        arr.push_back(s);
                    }
                r.set_content(arr.dump(), "application/json");
            });

    srv.Get("/api/footprints",
            [db](const httplib::Request&, httplib::Response& r)
            {
                storage::FootprintRepository repo(db);
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
            [db](const httplib::Request& req, httplib::Response& r)
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
             [db](const httplib::Request& req, httplib::Response& r)
             {
                 auto id = req.matches[1];
                 if (symbol_locked(db, id))
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
             [db](const httplib::Request& req, httplib::Response& r)
             {
                 auto id = req.matches[1];
                 if (footprint_locked(db, id))
                 {
                     json j{{"ok", false}, {"error", "Footprint belongs to a locked library"}};
                     r.set_content(j.dump(), "application/json");
                     return;
                 }
                 auto body = json::parse(req.body);
                 auto mgr = g_binding_mgr.get();
                 mgr->assign_model(id, body["model_id"].get<std::string>());
                 json j{{"ok", true}};
                 r.set_content(j.dump(), "application/json");
             });

    srv.Get("/api/footprints/search",
            [db](const httplib::Request& req, httplib::Response& r)
            {
                auto q = req.get_param_value("q");
                auto anchor = req.get_param_value("near");  // "near" is a Windows macro
                auto mgr = g_binding_mgr.get();
                auto result = mgr->search_footprints(q.empty() ? "" : q, 20, anchor);
                json arr = json::array();
                for (auto& fp : result)
                {
                    json f;
                    f["id"] = fp.id;
                    f["name"] = fp.name;
                    f["pad_count"] = fp.pad_count;
                    arr.push_back(f);
                }
                r.set_content(arr.dump(), "application/json");
            });

    srv.Get("/api/models/search",
            [db](const httplib::Request& req, httplib::Response& r)
            {
                auto q = req.get_param_value("q");
                auto anchor = req.get_param_value("near");  // "near" is a Windows macro
                auto mgr = g_binding_mgr.get();
                auto result = mgr->search_models(q.empty() ? "" : q, 20, anchor);
                json arr = json::array();
                for (auto& m : result)
                {
                    json j;
                    j["id"] = m.id;
                    j["name"] = m.name;
                    j["format"] = m.format;
                    arr.push_back(j);
                }
                r.set_content(arr.dump(), "application/json");
            });
}

// ============================================================
// Plugin routes
// ============================================================

void register_plugin_routes(httplib::Server& srv, plugin::PluginManager* plugins)
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
                        act["trigger"] = a.trigger;
                        act["schema"] = a.schema;
                        if (a.button_show)
                        {
                            json btn;
                            btn["show"] = a.button_show;
                            btn["style"] = a.button_style;
                            btn["location"] = a.button_location;
                            btn["tooltip"] = a.button_tooltip;
                            act["button"] = btn;
                        }
                        actions.push_back(act);
                    }
                    p["actions"] = actions;
                    arr.push_back(p);
                }
                r.set_content(arr.dump(), "application/json");
            });

    srv.Post("/api/plugins/execute",
             [plugins](const httplib::Request& req, httplib::Response& r)
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

void register_settings_routes(httplib::Server& srv, sqlite3* db, ImportManager* import_manager,
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
            [db](const httplib::Request&, httplib::Response& r)
            {
                storage::ComponentLibraryRepository repo(db);
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
             [db, import_manager](const httplib::Request& req, httplib::Response& r)
             {
                 storage::ComponentLibraryRepository repo(db);
                 json j;
                 try
                 {
                     auto body = json::parse(req.body.empty() ? "{}" : req.body);
                     std::string action = body.value("action", "list");
                     if (action == "add")
                     {
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
                                     lib.is_protected = cur.is_protected;
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
                             // Remove is purely an unregister operation — it does
                             // NOT touch local .kicad_sym files (those are the
                             // user's source of truth). Cascading deletes
                             // any libraries, symbols, and links under this
                             // component library so no orphan rows remain.
                             auto exec = [&](const std::string& sql) -> bool
                             {
                                 char* errmsg = nullptr;
                                 if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg) != SQLITE_OK)
                                 {
                                     j["error"] = errmsg != nullptr ? errmsg : "SQL error";
                                     sqlite3_free(errmsg);
                                     return false;
                                 }
                                 return true;
                             };
                             sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
                             bool ok = true;
                             ok = ok && exec(
                                 "DELETE FROM symbols WHERE library_id IN "
                                 "(SELECT id FROM libraries WHERE component_library_id = '" + id + "')");
                             ok = ok && exec(
                                 "DELETE FROM symbol_footprint_links WHERE symbol_id IN "
                                 "(SELECT id FROM symbols WHERE library_id IN "
                                 "(SELECT id FROM libraries WHERE component_library_id = '" + id + "'))");
                             ok = ok && exec(
                                 "DELETE FROM footprint_model_links WHERE footprint_id IN "
                                 "(SELECT id FROM footprints WHERE library_path IN "
                                 "(SELECT file_path FROM libraries WHERE component_library_id = '" + id +
                                 "' AND file_path != ''))");
                             ok = ok && exec(
                                 "DELETE FROM libraries WHERE component_library_id = '" + id + "'");
                             if (ok)
                             {
                                 auto rem = repo.remove(id);
                                 if (!rem)
                                 {
                                     j["error"] = rem.error().format_message();
                                     ok = false;
                                 }
                             }
                             sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
                             j["ok"] = ok;
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
             [db, import_manager](const httplib::Request& req, httplib::Response& r)
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
                 auto exec = [&](const char* sql) -> bool
                 {
                     char* errmsg = nullptr;
                     if (sqlite3_exec(db, sql, nullptr, nullptr, &errmsg) != SQLITE_OK)
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

void register_type_routes(httplib::Server& srv, sqlite3* /*db*/)
{
    srv.Get("/api/component-types",
            [](const httplib::Request&, httplib::Response& r)
            {
                auto& reg = core::TypeRegistry::instance();
                json arr = json::array();
                for (auto& ct : reg.component_types())
                {
                    json o;
                    o["name"] = ct.name;
                    o["icon"] = ct.icon;
                    o["color"] = ct.color;
                    arr.push_back(o);
                }
                r.set_content(arr.dump(), "application/json");
            });
    srv.Post("/api/component-types",
             [](const httplib::Request& req, httplib::Response& r)
             {
                 auto& reg = core::TypeRegistry::instance();
                 json j;
                 try
                 {
                     auto body = json::parse(req.body.empty() ? "{}" : req.body);
                     std::string action = body.value("action", "");
                     std::string name = body.value("name", "");
                     if (action == "add" && !name.empty())
                     {
                         core::TypeEntry e;
                         e.name = name;
                         e.icon = body.value("icon", "");
                         j["ok"] = reg.add_component_type(e);
                         if (j["ok"])
                             reg.save_component_types();
                         else
                             j["error"] = "Name exists";
                     }
                     else if (action == "remove" && !name.empty())
                     {
                         j["ok"] = reg.remove_component_type(name);
                         if (j["ok"])
                             reg.save_component_types();
                         else
                             j["error"] = "Not found";
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
    srv.Get("/api/package-types",
            [](const httplib::Request&, httplib::Response& r)
            {
                auto& reg = core::TypeRegistry::instance();
                json arr = json::array();
                for (auto& pt : reg.package_types())
                {
                    json o;
                    o["name"] = pt.name;
                    o["category"] = pt.category;
                    arr.push_back(o);
                }
                r.set_content(arr.dump(), "application/json");
            });
    srv.Post("/api/package-types",
             [](const httplib::Request& req, httplib::Response& r)
             {
                 auto& reg = core::TypeRegistry::instance();
                 json j;
                 try
                 {
                     auto body = json::parse(req.body.empty() ? "{}" : req.body);
                     std::string action = body.value("action", ""), name = body.value("name", "");
                     if (action == "add" && !name.empty())
                     {
                         core::TypeEntry e;
                         e.name = name;
                         e.category = body.value("category", "Other");
                         j["ok"] = reg.add_package_type(e);
                         if (j["ok"])
                             reg.save_package_types();
                         else
                             j["error"] = "Name exists";
                     }
                     else if (action == "remove" && !name.empty())
                     {
                         j["ok"] = reg.remove_package_type(name);
                         if (j["ok"])
                             reg.save_package_types();
                         else
                             j["error"] = "Not found";
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
}

// ============================================================
// Status / heartbeat routes
// ============================================================

void register_status_routes(httplib::Server& srv, sqlite3* db)
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

void register_classify_routes(httplib::Server& srv, sqlite3* db, ImportManager* import_manager)
{
    srv.Post("/api/import",
             [db, import_manager](const httplib::Request& req, httplib::Response& r)
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
