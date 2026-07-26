#include "api/api_server.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "core/type_registry.h"
#include "storage/repositories.h"
#include "classifier/rule_engine.h"
#include "services/library_service.h"
#include "services/classification_service.h"
#include "services/correspondence_service.h"
#include "classifier/rule_loader.h"
#include "plugin/plugin_manager.h"

using json = nlohmann::json;

namespace kforge::api {

// Cross-platform: get executable's own directory
static std::filesystem::path get_exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
    return std::filesystem::path(buf).parent_path();
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
        return std::filesystem::path(buf).parent_path();
    return ".";
#else
    return std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
}

static std::string find_webui_dir() {
    auto exe_dir = get_exe_dir();
    auto pkg_dist = exe_dir / "webui" / "dist";
    if (std::filesystem::exists(pkg_dist / "index.html")) return pkg_dist.string();
    auto pkg = exe_dir / "webui";
    if (std::filesystem::exists(pkg / "index.html")) return pkg.string();

    printf("FATAL: webui/index.html not found next to exe at %s\n",
           exe_dir.string().c_str());
    std::exit(1);
}

// Portable: exe_dir/data/ if exists, otherwise AppData (installer mode)
static std::string get_data_dir() {
    auto exe_dir = get_exe_dir();
    auto portable = exe_dir / "data";
    if (std::filesystem::exists(portable)) return portable.string();
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    return appdata ? std::string(appdata) + "/KiCad_Forge/data" : (exe_dir / "data").string();
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.KiCad_Forge" : "./data";
#endif
}

ApiServer::ApiServer(int port) : port_(port) {}
ApiServer::~ApiServer() { try { stop(); } catch (...) {} }

bool ApiServer::start() {
    auto data_dir = get_data_dir();
    std::string db_path = data_dir + "/meta.db";
    std::filesystem::create_directories(data_dir);
    auto db = storage::Database::open(db_path);
    if (!db) { fprintf(stderr, "DB: %s\n", db.error().message.c_str()); return false; }
    db_ = std::move(*db);

    core::TypeRegistry::instance().load_component_types("config/component_types.json");
    core::TypeRegistry::instance().load_package_types("config/package_types.json");

    init_plugins();
    auto_import();

    setup_routes();
    srv_.set_mount_point("/", find_webui_dir());

    thread_ = std::make_unique<std::thread>([this]() { srv_.listen("127.0.0.1", port_); });
    return true;
}

void ApiServer::init_plugins() {
    std::vector<std::filesystem::path> paths;

    // Plugins live next to exe: exe_dir/plugins
    auto plugin_dir = get_exe_dir() / "plugins";
    if (std::filesystem::exists(plugin_dir)) {
        paths.emplace_back(plugin_dir);
    }

    plugins_ = std::make_unique<plugin::PluginManager>(paths);
    plugins_->discover();

    for (auto& m : plugins_->available_plugins()) {
        if (m.one_click) { auto _ = plugins_->load(m.id, nullptr); }
    }
}

void ApiServer::wait() { if (thread_ && thread_->joinable()) thread_->join(); }
void ApiServer::stop() {
    if (plugins_) { try { plugins_->shutdown_all(); } catch (...) {} }
    srv_.stop();
    if (thread_ && thread_->joinable()) {
        try { thread_->join(); } catch (...) {}
        thread_.reset();
    }
    db_.reset();
}

int64_t ApiServer::ms_since_heartbeat() const {
    auto last = last_heartbeat_.load(std::memory_order_relaxed);
    if (last == 0) return INT64_MAX;  // no heartbeat yet
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return (now - last) / 1000000;  // ns → ms
}

void ApiServer::auto_import() {
    storage::SettingsRepository settings(db_->handle());
    services::LibraryService svc(db_.get());

    auto sym_path = settings.get("symbol_lib_path");
    if (sym_path && !sym_path->empty()) {
        auto r = svc.import_directory(*sym_path);
        if (r) printf("Auto-import: %d symbols, %d footprints from %s\n",
                       r->symbols, r->footprints, sym_path->c_str());
    }

    auto fp_path = settings.get("footprint_lib_path");
    if (fp_path && !fp_path->empty() && *fp_path != *sym_path) {
        auto r = svc.import_directory(*fp_path);
        if (r) printf("Auto-import: %d symbols, %d footprints from %s\n",
                       r->symbols, r->footprints, fp_path->c_str());
    }

    // Auto-link symbols to footprints
    services::CorrespondenceService cs(db_.get());
    int linked = 0;
    auto link_result = cs.auto_link();
    if (link_result) linked = *link_result;
    printf("Auto-link: %d symbol<->footprint links created\n", linked);
}

// Helper: send JSON response
static void json_response(httplib::Response& r, const json& j) {
    r.set_content(j.dump(), "application/json");

}

static std::string read_file_str(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

void ApiServer::serve_plugin_icon(const std::filesystem::path& plugin_dir,
                                   const std::string& filename,
                                   httplib::Response& r) {
    auto icon_path = plugin_dir / filename;
    if (!std::filesystem::exists(icon_path)) { r.status = 404; return; }

    std::string ext = icon_path.extension().string();
    if (ext == ".svg") r.set_content(read_file_str(icon_path.string()), "image/svg+xml");
    else if (ext == ".png") r.set_content(read_file_str(icon_path.string()), "image/png");
    else r.status = 404;
}

void ApiServer::setup_routes() {
    // ======== Import ========
    srv_.Post("/api/import", [this](const httplib::Request& req, httplib::Response& r) {
        services::LibraryService svc(db_.get());
        std::string dir = req.get_param_value("dir");
        json j;
        if (dir.empty()) {
            j["error"] = "Missing 'dir' parameter";
        } else {
            auto result = svc.import_directory(dir);
            if (!result) { j["error"] = result.error().message; }
            else {
                j["ok"] = true; j["symbols"] = result->symbols;
                j["footprints"] = result->footprints; j["errors"] = result->errors;
                j["messages"] = result->messages;
            }
        }
        r.set_content(j.dump(), "application/json");
    });

    // ======== Libraries ========
    srv_.Get("/api/libraries", [this](const httplib::Request&, httplib::Response& r) {
        storage::SymbolRepository sr(db_->handle());
        storage::LibraryRepository lr(db_->handle());
        auto libs = lr.find_all();
        // Only show libraries that contain symbols (not footprint-only libs)
        auto all_syms = sr.find_all();
        std::unordered_set<std::string> used_libs;
        if (all_syms) for (auto& s : *all_syms) used_libs.insert(s.library_id());

        json arr = json::array();
        if (libs) for (auto& l : *libs) {
            // Skip footprint-only libraries and unused libraries
            if (l.name.find("_footprints") != std::string::npos) continue;
            if (!used_libs.count(l.id)) continue;  // only show libraries with symbols

            json o;
            o["id"] = l.id; o["name"] = l.name;
            o["file_path"] = l.file_path.string();
            o["description"] = l.description;
            // Count symbols in this library
            int count = 0;
            if (all_syms) for (auto& s : *all_syms) if (s.library_id() == l.id) count++;
            o["symbol_count"] = count;
            arr.push_back(o);
        }
        r.set_content(arr.dump(), "application/json");
    });

    srv_.Post("/api/libraries", [this](const httplib::Request& req, httplib::Response& r) {
        try {
            auto body = json::parse(req.body);
            std::string action = body.value("action", "create");
            storage::LibraryRepository lr(db_->handle());
            storage::SymbolRepository sr(db_->handle());
            json j;

            if (action == "delete") {
                std::string lib_id = body.value("id", "");
                if (lib_id.empty()) {
                    j["ok"] = false; j["error"] = "Missing library id";
                } else {
                    services::LibraryService svc(db_.get());
                    auto result = svc.delete_library(lib_id);
                    j["ok"] = result.has_value();
                    if (result) {
                        j["symbols_deleted"] = result->symbols_removed;
                        if (!result->deleted_file.empty()) j["deleted_file"] = result->deleted_file;
                    } else {
                        j["error"] = result.error().message;
                    }
                }
            } else if (action == "delete_symbol") {
                std::string sym_id = body.value("id", "");
                services::LibraryService svc(db_.get());
                auto result = svc.delete_symbol(sym_id);
                j["ok"] = result.has_value();
                if (!result) j["error"] = result.error().message;
            } else {
                core::LibraryMeta m;
                m.name = body.value("name", "New Library");
                m.description = body.value("description", "");
                m.file_path = body.value("file_path", "");
                auto ins = lr.insert(m);
                if (ins) { j["ok"] = true; j["id"] = ins->id; j["name"] = ins->name; }
                else { j["ok"] = false; j["error"] = ins.error().message; }
            }
            r.set_content(j.dump(), "application/json");
        } catch (...) { r.set_content("{\"ok\":false}", "application/json");  }
    });

    // ======== Status ========
    srv_.Get("/api/status", [this](const httplib::Request&, httplib::Response& r) {
        // Update heartbeat — main.cpp uses this to know a window is still open
        last_heartbeat_.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);

        services::LibraryService svc(db_.get());
        json j; j["symbols"] = svc.symbol_count();
        j["footprints"] = svc.footprint_count(); j["ok"] = true;
        r.set_content(j.dump(), "application/json");
    });

    // ======== Symbols ========
    srv_.Get("/api/symbols", [this](const httplib::Request& req, httplib::Response& r) {
        storage::SymbolRepository repo(db_->handle());
        storage::RelationshipRepository rr(db_->handle());
        storage::LibraryRepository lr(db_->handle());
        std::string q = req.get_param_value("q");
        std::string lib_id = req.get_param_value("library");
        auto result = q.empty()
            ? (lib_id.empty() ? repo.find_all() : repo.find_by_library(lib_id))
            : repo.search(q);
        json arr = json::array();
        if (result) for (auto& sym : *result) {
            json s;
            s["id"] = sym.id(); s["name"] = sym.name();
            s["library_id"] = sym.library_id();
            s["value"] = sym.default_value(); s["footprint"] = sym.footprint();
            s["pins"] = sym.pin_count(); s["mpn"] = sym.mpn();
            s["type"] = core::component_type_name(static_cast<int>(sym.component_type));
            s["has_footprint"] = !sym.footprint().empty();
            bool has_3d = false;
            auto fp_opt = rr.find_footprint_for_symbol(sym.id());
            if (fp_opt && *fp_opt) {
                auto models = rr.find_models_for_footprint(**fp_opt);
                has_3d = (models && !models->empty());
            }
            s["has_3d_model"] = has_3d;
            arr.push_back(s);
        }
        r.set_content(arr.dump(), "application/json");
        
    });

    // ======== Classify ========
    srv_.Post("/api/classify", [this](const httplib::Request&, httplib::Response& r) {
        services::ClassificationService svc(db_.get());
        auto summary = svc.classify_all();
        json j;
        if (!summary) { j["error"] = summary.error().message; }
        else {
            j["total"] = summary->total; j["matched"] = summary->matched;
            j["type_updated"] = summary->type_updated;
            json arr = json::array();
            for (auto& result : summary->results) {
                json o; o["name"] = result.symbol_name; o["library"] = result.target_library;
                o["type"] = result.type_name; o["confidence"] = result.confidence;
                arr.push_back(o);
            }
            j["results"] = arr;
        }
        r.set_content(j.dump(), "application/json");
    });

    // ======== Check ========
    srv_.Post("/api/check", [this](const httplib::Request&, httplib::Response& r) {
        services::CorrespondenceService svc(db_.get());
        auto issues = svc.check_all();
        json arr = json::array();
        if (issues) for (auto& issue : *issues) {
            json i;
            i["type"] = correspondence::issue_type_to_string(issue.type);
            i["severity"] = correspondence::issue_severity_to_string(issue.severity);
            i["name"] = issue.entity_name; i["message"] = issue.message;
            i["fix"] = issue.suggested_fix; arr.push_back(i);
        }
        r.set_content(arr.dump(), "application/json");
        
    });

    // ======== Automatch ========
    srv_.Post("/api/automatch", [this](const httplib::Request&, httplib::Response& r) {
        services::CorrespondenceService svc(db_.get());
        auto _ = svc.auto_link();
        auto sug = svc.suggest_matches();
        json arr = json::array();
        if (sug) {
            int n = std::min(50, (int)sug->size());
            for (int i = 0; i < n; i++) {
                json m; m["symbol"] = sug->at(i).symbol_name;
                m["footprint"] = sug->at(i).footprint_name;
                m["score"] = (int)(sug->at(i).score * 100); arr.push_back(m);
            }
        }
        r.set_content(arr.dump(), "application/json");
        
    });

    // ======== Rules ========
    srv_.Get("/api/rules", [this](const httplib::Request&, httplib::Response& r) {
        auto rules = classifier::RuleLoader::default_rules();
        json arr = json::array();
        for (auto& rule : rules) {
            json o; o["name"] = rule.name; o["priority"] = rule.priority;
            o["target"] = rule.target_library; o["confidence"] = rule.confidence;
            arr.push_back(o);
        }
        r.set_content(arr.dump(), "application/json");
        
    });

    
    // ======== Database reset (truncate tables, keep file) ========
    srv_.Post("/api/db/reset", [this](const httplib::Request&, httplib::Response& r) {
        json j;
        auto _1 = db_->execute("DELETE FROM footprint_model_links");
        auto _2 = db_->execute("DELETE FROM symbol_footprint_links");
        auto _3 = db_->execute("DELETE FROM models_3d");
        auto _4 = db_->execute("DELETE FROM footprints");
        auto _5 = db_->execute("DELETE FROM symbols");
        auto _6 = db_->execute("DELETE FROM libraries");
        j["ok"] = true; j["message"] = "All data cleared. Reimport or restart.";
        r.set_content(j.dump(), "application/json");
    });

    // ======== Settings ========
    srv_.Get("/api/settings", [this](const httplib::Request&, httplib::Response& r) {
        storage::SettingsRepository repo(db_->handle());
        auto map = repo.all();
        json j = json::object();
        if (map) for (auto& [k, v] : *map) j[k] = v;
        r.set_content(j.dump(), "application/json");
    });

    srv_.Post("/api/settings", [this](const httplib::Request& req, httplib::Response& r) {
        storage::SettingsRepository repo(db_->handle());
        try {
            auto j = json::parse(req.body);
            for (auto& [k, v] : j.items()) {
                std::string val = v.is_string() ? v.get<std::string>() : v.dump();
                repo.set(k, val);
            }
            bool has_sym = j.contains("symbol_lib_path");
            bool has_fp  = j.contains("footprint_lib_path");
            if (has_sym || has_fp) {
                services::LibraryService svc(db_.get());
                auto sym_path = repo.get("symbol_lib_path");
                auto fp_path  = repo.get("footprint_lib_path");
                json out;
                out["ok"] = true;
                int total_sym = 0, total_fp = 0;
                if (sym_path && !sym_path->empty()) {
                    auto s = svc.import_directory(*sym_path);
                    if (s) {
                        total_sym = s->symbols; total_fp = s->footprints;
                        out["debug_errors"] = s->errors;
                        out["debug_msgs"] = s->messages;
                    } else { out["debug_error"] = s.error().message; }
                }
                if (fp_path && !fp_path->empty()) {
                    auto s = svc.import_directory(*fp_path);
                    if (s) { total_sym += s->symbols; total_fp += s->footprints; }
                }
                // Auto-link after settings import
                services::CorrespondenceService cs(db_.get());
                auto linked = cs.auto_link();
                if (linked) printf("Auto-link: %d links from settings import\n", *linked);
                int total_m3d = 0;
                auto m3d_path = repo.get("model_3d_path");
                if (m3d_path && !m3d_path->empty()) {
                    auto s = svc.scan_3d_models(*m3d_path);
                    if (s) { total_m3d = *s; svc.link_3d_models_to_symbols(); }
                }
                out["imported_symbols"] = total_sym;
                out["imported_footprints"] = total_fp;
                out["imported_models_3d"] = total_m3d;
                json_response(r, out);
                return;
            }
        } catch(...) {}
        r.set_content("{\"ok\":true}", "application/json");
        
    });

    // ======== Component Types ========
    srv_.Get("/api/component-types", [this](const httplib::Request&, httplib::Response& r) {
        auto& reg = core::TypeRegistry::instance();
        json arr = json::array();
        for (auto& e : reg.component_types()) {
            json item; item["name"] = e.name; item["icon"] = e.icon; item["color"] = e.color;
            arr.push_back(item);
        }
        json_response(r, arr);
    });

    srv_.Post("/api/component-types", [this](const httplib::Request& req, httplib::Response& r) {
        json j;
        try {
            auto body = json::parse(req.body);
            std::string action = body.value("action", "");
            std::string name = body.value("name", "");
            auto& reg = core::TypeRegistry::instance();
            if (action == "add" && !name.empty()) {
                core::TypeEntry entry; entry.name = name;
                entry.icon = body.value("icon", "O"); entry.color = body.value("color", "#8E8E93");
                j["ok"] = reg.add_component_type(entry);
                if (j["ok"]) reg.save_component_types(); else j["error"] = "Name exists";
            } else if (action == "remove" && !name.empty()) {
                j["ok"] = reg.remove_component_type(name);
                if (j["ok"]) reg.save_component_types(); else j["error"] = "Not found";
            } else { j["ok"] = false; j["error"] = "Invalid"; }
        } catch (...) { j["ok"] = false; j["error"] = "Invalid JSON"; }
        r.set_content(j.dump(), "application/json");
    });

    // ======== Package Types ========
    srv_.Get("/api/package-types", [this](const httplib::Request&, httplib::Response& r) {
        auto& reg = core::TypeRegistry::instance();
        json arr = json::array();
        for (auto& e : reg.package_types()) {
            json item; item["name"] = e.name; item["category"] = e.category;
            arr.push_back(item);
        }
        json_response(r, arr);
    });

    srv_.Post("/api/package-types", [this](const httplib::Request& req, httplib::Response& r) {
        json j;
        try {
            auto body = json::parse(req.body);
            std::string action = body.value("action", "");
            std::string name = body.value("name", "");
            auto& reg = core::TypeRegistry::instance();
            if (action == "add" && !name.empty()) {
                core::TypeEntry entry; entry.name = name;
                entry.category = body.value("category", "Other");
                j["ok"] = reg.add_package_type(entry);
                if (j["ok"]) reg.save_package_types(); else j["error"] = "Name exists";
            } else if (action == "remove" && !name.empty()) {
                j["ok"] = reg.remove_package_type(name);
                if (j["ok"]) reg.save_package_types(); else j["error"] = "Not found";
            } else { j["ok"] = false; j["error"] = "Invalid"; }
        } catch (...) { j["ok"] = false; j["error"] = "Invalid JSON"; }
        r.set_content(j.dump(), "application/json");
    });

    // ======== Plugin execution ========
    srv_.Post("/api/plugins/execute", [this](const httplib::Request& req, httplib::Response& r) {
        json j;
        try {
            auto body = json::parse(req.body.empty() ? "{}" : req.body);
            std::string plugin_id = req.get_param_value("id");
            if (plugin_id.empty()) plugin_id = body.value("id", "");
            if (plugin_id.empty()) { j["ok"] = false; j["error"] = "Missing plugin id"; r.set_content(j.dump(), "application/json"); return; }

            auto result = plugins_->execute(plugin_id, "import", body.dump());
            if (!result) {
                j["ok"] = false; j["error"] = result.error().message;
                r.set_content(j.dump(), "application/json"); return;
            }

            // Parse plugin output, reimport if ok
            auto out = json::parse(*result);
            j["plugin_result"] = out;
            j["ok"] = out.value("ok", false);

            if (out.value("ok", false)) {
                storage::SettingsRepository settings(db_->handle());
                auto sym_path = settings.get("symbol_lib_path");
                if (sym_path && !sym_path->empty()) {
                    services::LibraryService svc(db_.get());
                    // Don't set target_library here — each file auto-creates its own library
                    auto ir = svc.import_directory(*sym_path);
                    if (ir) {
                        j["imported_symbols"] = ir->symbols;
                        j["imported_footprints"] = ir->footprints;
                    }
                }
            }
        } catch (const json::parse_error& e) {
            j["ok"] = false; j["error"] = std::string("Parse error: ") + e.what();
        } catch (...) {
            j["ok"] = false; j["error"] = "Unknown error";
        }
        r.set_content(j.dump(), "application/json");
    });

    // ======== Plugins ========
    // Helper: build a plugin JSON object with all UI-relevant fields
    auto build_plugin_json = [](const plugin::PluginManifest& m, const std::string& status) {
        json p;
        p["id"] = m.id; p["name"] = m.name;
        p["version"] = m.version; p["description"] = m.description;
        p["status"] = status;
        if (!m.icon.empty()) {
            p["icon_url"] = "/api/plugins/" + m.id + "/icon";
        }
        if (!m.actions.empty()) {
            json acts = json::array();
            for (auto& a : m.actions) {
                json act;
                act["id"] = a.id; act["name"] = a.name;
                act["description"] = a.description;
                act["trigger"] = a.trigger;
                // Per-action icon URL only when different from plugin default
                if (!a.icon.empty() && a.icon != m.icon)
                    act["icon_url"] = "/api/plugins/" + m.id + "/icon/" + a.id;
                if (!a.schema.is_null()) act["schema"] = a.schema;
                // Button display preferences
                act["button"] = { {"show", a.button_show},
                                  {"style", a.button_style.empty() ? "both" : a.button_style},
                                  {"tooltip", a.button_tooltip} };
                acts.push_back(act);
            }
            p["actions"] = acts;
        }
        return p;
    };

    srv_.Get("/api/plugins", [this, build_plugin_json](const httplib::Request&, httplib::Response& r) {
        json arr = json::array();
        if (plugins_) {
            for (auto& m : plugins_->loaded_plugins()) {
                arr.push_back(build_plugin_json(m, "loaded"));
            }
            for (auto& m : plugins_->available_plugins()) {
                if (!plugins_->is_loaded(m.id)) {
                    arr.push_back(build_plugin_json(m, "available"));
                }
            }
        }
        json_response(r, arr);
    });

    // Serve plugin icon files (SVG or PNG)
    srv_.Get(R"(/api/plugins/([^/]+)/icon/([^/]+))", [this](const httplib::Request& req, httplib::Response& r) {
        std::string pid = req.matches[1];
        std::string action_id = req.matches[2];
        if (!plugins_) { r.status = 404; return; }
        auto dir = plugins_->plugin_path(pid);
        if (dir.empty()) { r.status = 404; return; }
        serve_plugin_icon(dir, action_id + ".svg", r);
        if (r.status == 404) serve_plugin_icon(dir, action_id + ".png", r);
    });

    srv_.Get(R"(/api/plugins/([^/]+)/icon)", [this](const httplib::Request& req, httplib::Response& r) {
        std::string pid = req.matches[1];
        if (!plugins_) { r.status = 404; return; }
        auto dir = plugins_->plugin_path(pid);
        if (dir.empty()) { r.status = 404; return; }
        serve_plugin_icon(dir, "icon.svg", r);
        if (r.status == 404) serve_plugin_icon(dir, "icon.png", r);
    });

    srv_.Post("/api/plugins/load", [this](const httplib::Request& req, httplib::Response& r) {
        json j; std::string id = req.get_param_value("id");
        if (plugins_ && !id.empty()) {
            auto res = plugins_->load(id, nullptr);
            j["ok"] = res.has_value();
            if (!res) j["error"] = res.error().message;
        } else j["ok"] = false;
        r.set_content(j.dump(), "application/json");
    });
}

}  // namespace kforge::api
