// Route handlers extracted from setup_routes()
#include "api/api_server.h"
#include "api/import_orchestrator.h"
#include "api/routes.h"
#include "classifier/rule_engine.h"
#include "classifier/rule_loader.h"
#include "core/type_registry.h"
#include "platform/folder_dialog.h"
#include "plugin/plugin_manager.h"
#include "services/classification_service.h"
#include "services/correspondence_service.h"
#include "services/import_pipeline.h"
#include "services/library_service.h"
#include "storage/repositories.h"
#include "util/logger.h"
#include "util/result.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

using json = nlohmann::json;

namespace kforge::api
{

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

void register_library_routes(httplib::Server& srv, sqlite3* db)
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
                    for (auto& s : *all_syms)
                        used_libs.insert(s.library_id());
                storage::ComponentLibraryRepository cl_repo(db);
                std::unordered_map<std::string, std::string> group_name;
                auto comp_libs = cl_repo.find_all();
                if (comp_libs)
                    for (auto& cl : *comp_libs)
                        group_name[cl.id] = cl.name;
                json arr = json::array();
                if (libs)
                    for (auto& l : *libs)
                    {
                        if (l.name.find("_footprints") != std::string::npos)
                            continue;
                        if (!used_libs.count(l.id))
                            continue;
                        json o;
                        o["id"] = l.id;
                        o["name"] = l.name;
                        o["file_path"] = l.file_path.string();
                        o["description"] = l.description;
                        if (!l.component_library_id.empty())
                            o["group"] = group_name[l.component_library_id];
                        int count = 0;
                        if (all_syms)
                            for (auto& s : *all_syms)
                                if (s.library_id() == l.id)
                                    count++;
                        o["symbol_count"] = count;
                        arr.push_back(o);
                    }
                r.set_content(arr.dump(), "application/json");
            });

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
                     else if (action == "delete_symbol")
                     {
                         services::LibraryService svc(
                             reinterpret_cast<storage::Database*>(static_cast<uintptr_t>(0x1)));
                         // Use direct DB access since we only have sqlite3*
                         storage::SymbolRepository sr(db);
                         std::string sym_id = body.value("id", "");
                         auto sym = sr.find_by_id(sym_id);
                         if (!sym)
                         {
                             j["ok"] = false;
                             j["error"] = "Not found";
                         }
                         else
                         {
                             if (sr.remove(sym_id))
                             {
                                 // Also try to remove from file
                                 storage::LibraryRepository lrr(db);
                                 auto libs = lrr.find_all();
                                 if (libs)
                                     for (auto& l : *libs)
                                     {
                                         if (l.id != sym->library_id() || l.file_path.empty())
                                             continue;
                                         if (!std::filesystem::exists(l.file_path))
                                             continue;
                                         std::ifstream f(l.file_path, std::ios::binary);
                                         std::string text((std::istreambuf_iterator<char>(f)),
                                                          std::istreambuf_iterator<char>());
                                         f.close();
                                         std::string search = "(symbol \"" + sym->name() + "\"";
                                         size_t pos = text.find(search);
                                         if (pos == std::string::npos)
                                             continue;
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
                                         text.erase(pos, end - pos);
                                         std::ofstream out(l.file_path, std::ios::binary);
                                         out << text;
                                         break;
                                     }
                                 j["ok"] = true;
                             }
                             else
                             {
                                 j["ok"] = false;
                                 j["error"] = "DB remove failed";
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
                     auto result = plugins->execute(plugin_id, action, body.dump());
                     if (!result)
                     {
                         j["ok"] = false;
                         j["error"] = util::error_formatter(result.error());
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
                             j["error"] = util::error_formatter(res.error());
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

void register_settings_routes(httplib::Server& srv, sqlite3* db, ImportOrchestrator* orch)
{
    srv.Get("/api/settings",
            [db](const httplib::Request&, httplib::Response& r)
            {
                storage::SettingsRepository repo(db);
                auto map = repo.all();
                json j = json::object();
                if (map)
                    for (auto& [k, v] : *map)
                        j[k] = v;
                r.set_content(j.dump(), "application/json");
            });

    srv.Post("/api/settings",
             [db](const httplib::Request& req, httplib::Response& r)
             {
                 storage::SettingsRepository repo(db);
                 try
                 {
                     auto j = json::parse(req.body);
                     for (auto& [k, v] : j.items())
                     {
                         std::string val = v.is_string() ? v.get<std::string>() : v.dump();
                         auto Error_ = repo.set(k, val);
                         if (!Error_)
                         {
                             LOG_ERROR("{}", util::error_formatter(Error_.error()));
                         }
                     }
                     bool has_sym = j.contains("symbol_lib_path");
                     bool has_fp = j.contains("footprint_lib_path");
                     if (has_sym || has_fp)
                     {
                         auto sym_path = repo.get("symbol_lib_path");
                         auto fp_path = repo.get("footprint_lib_path");
                         auto m3d_path = repo.get("model_3d_path");
                         json out;
                         out["ok"] = true;
                         int total_sym = 0, total_fp = 0, total_m3d = 0;
                         if (sym_path && !sym_path->empty())
                         {
                             auto r2 = services::ImportPipeline(db) |
                                       services::symbols_from{*sym_path, ""} | services::execute;
                             total_sym = r2.symbols;
                             total_fp = r2.footprints;
                         }
                         if (fp_path && !fp_path->empty() && *fp_path != (sym_path ? *sym_path : ""))
                         {
                             auto r2 = services::ImportPipeline(db) |
                                       services::footprints_from{*fp_path} | services::execute;
                             total_sym += r2.symbols;
                             total_fp += r2.footprints;
                         }
                         if (m3d_path && !m3d_path->empty())
                         {
                             auto r2 = services::ImportPipeline(db) |
                                       services::models_from{*m3d_path} | services::execute;
                             total_m3d = r2.models_3d;
                         }
                         out["imported_symbols"] = total_sym;
                         out["imported_footprints"] = total_fp;
                         out["imported_models_3d"] = total_m3d;
                         r.set_content(out.dump(), "application/json");
                         return;
                     }
                     json out;
                     out["ok"] = true;
                     out["imported_symbols"] = 0;
                     out["imported_footprints"] = 0;
                     r.set_content(out.dump(), "application/json");
                 }
                 catch (...)
                 {
                     json out;
                     out["ok"] = false;
                     out["error"] = "Save error";
                     r.set_content(out.dump(), "application/json");
                 }
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
                        arr.push_back(o);
                    }
                r.set_content(arr.dump(), "application/json");
            });

    srv.Post("/api/component-libraries",
             [db](const httplib::Request& req, httplib::Response& r)
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
                             j["error"] = util::error_formatter(ins.error());
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
                             storage::ComponentLibraryRepository::ComponentLibrary lib;
                             lib.id = id;
                             lib.name = body.value("name", "");
                             lib.symbol_path = body.value("symbol_path", "");
                             lib.footprint_path = body.value("footprint_path", "");
                             lib.model_3d_path = body.value("model_3d_path", "");
                             lib.enabled = body.value("enabled", true);
                             auto upd = repo.update(lib);
                             j["ok"] = upd.has_value();
                             if (!upd)
                                 j["error"] = util::error_formatter(upd.error());
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
                             auto rem = repo.remove(id);
                             j["ok"] = rem.has_value();
                             if (!rem)
                                 j["error"] = util::error_formatter(rem.error());
                         }
                     }
                     else if (action == "import")
                     {
                         auto result = services::ImportPipeline(db) |
                                       services::symbols_from{"", ""} | services::execute;
                         j["ok"] = true;
                         j["imported_symbols"] = result.symbols;
                         j["imported_footprints"] = result.footprints;
                         j["imported_models_3d"] = result.models_3d;
                         j["auto_linked"] = result.linked_symbols;
                         j["libraries"] = json::array();
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
             [db, orch](const httplib::Request&, httplib::Response& r)
             {
                 // Stop import thread before clearing to avoid race
                 if (orch) orch->stop();
                 json j;
                 sqlite3_exec(db, "DELETE FROM symbols", nullptr, nullptr, nullptr);
                 sqlite3_exec(db, "DELETE FROM footprints", nullptr, nullptr, nullptr);
                 sqlite3_exec(db, "DELETE FROM models_3d", nullptr, nullptr, nullptr);
                 sqlite3_exec(db, "DELETE FROM libraries", nullptr, nullptr, nullptr);
                 sqlite3_exec(db, "DELETE FROM symbol_footprint_links", nullptr, nullptr, nullptr);
                 sqlite3_exec(db, "DELETE FROM footprint_model_links", nullptr, nullptr, nullptr);
                 sqlite3_exec(db, "DELETE FROM settings", nullptr, nullptr, nullptr);
                 sqlite3_exec(db, "VACUUM", nullptr, nullptr, nullptr);
                 j["ok"] = true;
                 j["message"] = "All data cleared. Reimport or restart.";
                 r.set_content(j.dump(), "application/json");
                 // Restart import
                 if (orch) orch->start();
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

void register_classify_routes(httplib::Server& srv, sqlite3* db)
{
    srv.Post("/api/import",
             [db](const httplib::Request& req, httplib::Response& r)
             {
                 std::string dir = req.get_param_value("dir");
                 json j;
                 if (dir.empty())
                 {
                     j["error"] = "Missing 'dir' parameter";
                 }
                 else
                 {
                     auto result = services::ImportPipeline(db) | services::symbols_from{dir, ""} |
                                   services::execute;
                     j["ok"] = true;
                     j["symbols"] = result.symbols;
                     j["footprints"] = result.footprints;
                     j["errors"] = 0;
                 }
                 r.set_content(j.dump(), "application/json");
             });
}

}  // namespace kforge::api
