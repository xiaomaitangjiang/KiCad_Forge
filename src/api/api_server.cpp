#include "api/api_server.h"

#include <cstdio>
#include <filesystem>

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

static std::string find_webui_dir() {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
    auto exe_dir = std::filesystem::path(exe_path).parent_path();
    auto pkg_dist = exe_dir / "webui" / "dist";
    auto pkg = exe_dir / "webui";
    if (std::filesystem::exists(pkg_dist / "index.html")) return pkg_dist.string();
    if (std::filesystem::exists(pkg / "index.html")) return pkg.string();
#endif

    for (auto* d : {"../../../../../webui/dist", "../../../../webui/dist",
                    "../../../webui/dist", "../../webui/dist", "../webui/dist",
                    "webui/dist", "webui"}) {
        if (std::filesystem::exists(std::filesystem::path(d) / "index.html")) return d;
    }
    return "webui/dist";
}

ApiServer::ApiServer(int port) : port_(port) {}
ApiServer::~ApiServer() { stop(); }

bool ApiServer::start() {
    std::string db_path;
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    db_path = appdata ? std::string(appdata) + "/kicad_forge/meta.db" : "kicad_forge_meta.db";
#else
    const char* home = std::getenv("HOME");
    db_path = home ? std::string(home) + "/.kicad_forge/meta.db" : "kicad_forge_meta.db";
#endif
    std::filesystem::create_directories(std::filesystem::path(db_path).parent_path());
    auto db = storage::Database::open(db_path);
    if (!db) { fprintf(stderr, "DB: %s\n", db.error().message.c_str()); return false; }
    db_ = std::move(*db);

    core::TypeRegistry::instance().load_component_types("config/component_types.json");
    core::TypeRegistry::instance().load_package_types("config/package_types.json");

    init_plugins();
    auto_import();

    setup_routes();
    srv_.set_doc_root(find_webui_dir());

    thread_ = std::make_unique<std::thread>([this]() { srv_.listen(port_); });
    return true;
}

void ApiServer::init_plugins() {
    std::vector<std::filesystem::path> paths;

    // 1. Next to exe (for packaged releases)
#ifdef _WIN32
    char exe_buf[512];
    GetModuleFileNameA(nullptr, exe_buf, sizeof(exe_buf));
    auto exe_dir = std::filesystem::path(exe_buf).parent_path();
    paths.push_back(exe_dir / "plugins");
    // 2. Project root plugins/ (for development)
    paths.push_back(exe_dir / ".." / ".." / ".." / ".." / "plugins");
#endif
    // 3. User plugins directory
#ifdef _WIN32
    paths.push_back(std::string(std::getenv("APPDATA") ? std::getenv("APPDATA") : ".") + "/kicad_forge/plugins");
#else
    paths.push_back(std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.kicad_forge/plugins");
#endif
    plugins_ = std::make_unique<plugin::PluginManager>(paths);
    plugins_->discover();

    for (auto& m : plugins_->available_plugins()) {
        if (m.one_click) { auto _ = plugins_->load(m.id, nullptr); }
    }
}

void ApiServer::wait() { if (thread_ && thread_->joinable()) thread_->join(); }
void ApiServer::stop() {
    if (plugins_) plugins_->shutdown_all();
    srv_.stop();
    if (thread_ && thread_->joinable()) thread_->join();
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
}

// Helper: send JSON response
static void json_response(net::Response& r, const json& j) {
    r.body = j.dump();
    r.content_type = "application/json";
}

void ApiServer::setup_routes() {
    // ======== Import ========
    srv_.post("/api/import", [this](const net::Request& req, net::Response& r) {
        services::LibraryService svc(db_.get());
        std::string dir = req.param("dir");
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
        json_response(r, j);
    });

    // ======== Status ========
    srv_.get("/api/status", [this](const net::Request&, net::Response& r) {
        services::LibraryService svc(db_.get());
        json j; j["symbols"] = svc.symbol_count();
        j["footprints"] = svc.footprint_count(); j["ok"] = true;
        json_response(r, j);
    });

    // ======== Symbols ========
    srv_.get("/api/symbols", [this](const net::Request& req, net::Response& r) {
        storage::SymbolRepository repo(db_->handle());
        storage::RelationshipRepository rr(db_->handle());
        std::string q = req.param("q");
        auto result = q.empty() ? repo.find_all() : repo.search(q);
        json arr = json::array();
        if (result) for (auto& sym : *result) {
            json s;
            s["id"] = sym.id(); s["name"] = sym.name();
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
        r.body = arr.dump();
        r.content_type = "application/json";
    });

    // ======== Classify ========
    srv_.post("/api/classify", [this](const net::Request&, net::Response& r) {
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
        json_response(r, j);
    });

    // ======== Check ========
    srv_.post("/api/check", [this](const net::Request&, net::Response& r) {
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
        r.body = arr.dump();
        r.content_type = "application/json";
    });

    // ======== Automatch ========
    srv_.post("/api/automatch", [this](const net::Request&, net::Response& r) {
        services::CorrespondenceService svc(db_.get());
        svc.auto_link();
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
        r.body = arr.dump();
        r.content_type = "application/json";
    });

    // ======== Rules ========
    srv_.get("/api/rules", [this](const net::Request&, net::Response& r) {
        auto rules = classifier::RuleLoader::default_rules();
        json arr = json::array();
        for (auto& rule : rules) {
            json o; o["name"] = rule.name; o["priority"] = rule.priority;
            o["target"] = rule.target_library; o["confidence"] = rule.confidence;
            arr.push_back(o);
        }
        r.body = arr.dump();
        r.content_type = "application/json";
    });

    // ======== Settings ========
    srv_.get("/api/settings", [this](const net::Request&, net::Response& r) {
        storage::SettingsRepository repo(db_->handle());
        auto map = repo.all();
        json j = json::object();
        if (map) for (auto& [k, v] : *map) j[k] = v;
        json_response(r, j);
    });

    srv_.post("/api/settings", [this](const net::Request& req, net::Response& r) {
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
        r.body = "{\"ok\":true}";
        r.content_type = "application/json";
    });

    // ======== Component Types ========
    srv_.get("/api/component-types", [this](const net::Request&, net::Response& r) {
        auto& reg = core::TypeRegistry::instance();
        json arr = json::array();
        for (auto& e : reg.component_types()) {
            json item; item["name"] = e.name; item["icon"] = e.icon; item["color"] = e.color;
            arr.push_back(item);
        }
        json_response(r, arr);
    });

    srv_.post("/api/component-types", [this](const net::Request& req, net::Response& r) {
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
        json_response(r, j);
    });

    // ======== Package Types ========
    srv_.get("/api/package-types", [this](const net::Request&, net::Response& r) {
        auto& reg = core::TypeRegistry::instance();
        json arr = json::array();
        for (auto& e : reg.package_types()) {
            json item; item["name"] = e.name; item["category"] = e.category;
            arr.push_back(item);
        }
        json_response(r, arr);
    });

    srv_.post("/api/package-types", [this](const net::Request& req, net::Response& r) {
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
        json_response(r, j);
    });

    // ======== Plugin execution ========
    srv_.post("/api/plugins/execute", [this](const net::Request& req, net::Response& r) {
        json j;
        try {
            auto body = json::parse(req.body.empty() ? "{}" : req.body);
            std::string plugin_id = req.param("id");
            if (plugin_id.empty()) plugin_id = body.value("id", "");
            if (plugin_id.empty()) { j["ok"] = false; j["error"] = "Missing plugin id"; json_response(r, j); return; }

            // Build args JSON for the plugin script
            json args;
            args["source"] = body.value("lcsc_id", body.value("source", ""));
            if (body.contains("options")) args["options"] = body["options"];

            auto result = plugins_->execute(plugin_id, "import", args.dump());
            if (!result) {
                j["ok"] = false; j["error"] = result.error().message;
                json_response(r, j); return;
            }

            // Parse plugin output
            auto out = json::parse(*result);
            j["ok"] = out.value("ok", false);
            j["lcsc_id"] = out.value("lcsc_id", "");
            j["plugin_output"] = out;

            // Import generated files into database
            if (out.value("ok", false) && out.contains("output_dir")) {
                std::string dir = out["output_dir"].get<std::string>();
                services::LibraryService svc(db_.get());
                auto ir = svc.import_directory(dir);
                if (ir) {
                    j["imported_symbols"] = ir->symbols;
                    j["imported_footprints"] = ir->footprints;
                    j["imported_models_3d"] = 0;
                }
            }
        } catch (const json::parse_error& e) {
            j["ok"] = false; j["error"] = std::string("Parse error: ") + e.what();
        } catch (...) {
            j["ok"] = false; j["error"] = "Unknown error";
        }
        json_response(r, j);
    });

    // ======== Plugins ========
    srv_.get("/api/plugins", [this](const net::Request&, net::Response& r) {
        json arr = json::array();
        if (plugins_) {
            for (auto& m : plugins_->loaded_plugins()) {
                json p; p["id"] = m.id; p["name"] = m.name;
                p["version"] = m.version; p["status"] = "loaded"; arr.push_back(p);
            }
            for (auto& m : plugins_->available_plugins()) {
                if (!plugins_->is_loaded(m.id)) {
                    json p; p["id"] = m.id; p["name"] = m.name;
                    p["version"] = m.version; p["status"] = "available"; arr.push_back(p);
                }
            }
        }
        json_response(r, arr);
    });

    srv_.post("/api/plugins/load", [this](const net::Request& req, net::Response& r) {
        json j; std::string id = req.param("id");
        if (plugins_ && !id.empty()) {
            auto res = plugins_->load(id, nullptr);
            j["ok"] = res.has_value();
            if (!res) j["error"] = res.error().message;
        } else j["ok"] = false;
        json_response(r, j);
    });
}

}  // namespace kforge::api
