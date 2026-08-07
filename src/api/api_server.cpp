#include "api/api_server.h"
#include "api/routes.h"
#include "classifier/rule_engine.h"
#include "classifier/rule_loader.h"
#include "core/type_registry.h"
#include "platform/folder_dialog.h"
#include "plugin/plugin_manager.h"
#include "services/classification_service.h"
#include "services/correspondence_service.h"
#include "services/library_service.h"
#include "storage/repositories.h"
#include "util/logger.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <sqlite3.h>
#include <sstream>
#include <unordered_set>


using json = nlohmann::json;

namespace kforge::api
{

namespace
{
// Cross-platform: get executable's own directory
std::filesystem::path get_exe_dir()
{
#ifdef _WIN32
    std::array<char, MAX_PATH> buf{};
    GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return std::filesystem::path(buf.data()).parent_path();
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

std::string find_webui_dir()
{
    auto exe_dir = get_exe_dir();
    auto pkg_dist = exe_dir / "webui" / "dist";
    if (std::filesystem::exists(pkg_dist / "index.html"))
    {
        return pkg_dist.string();
    }
    auto pkg = exe_dir / "webui";
    if (std::filesystem::exists(pkg / "index.html"))
    {
        return pkg.string();
    }

    LOG_ERROR("FATAL: webui/index.html not found next to exe at {}", exe_dir.string());
    std::exit(1);
}

// Portable: exe_dir/data/ if exists, otherwise AppData (installer mode)
std::string get_data_dir()
{
    auto exe_dir = get_exe_dir();
    auto portable = exe_dir / "data";
    if (std::filesystem::exists(portable))
    {
        return portable.string();
    }
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    return (appdata != nullptr) ? std::string(appdata) + "/KiCad_Forge/data"
                                : (exe_dir / "data").string();
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.KiCad_Forge" : "./data";
#endif
}

}  // anonymous namespace

ApiServer::ApiServer(int port) : port_(port)
{
}
ApiServer::~ApiServer()
{
    try
    {
        stop();
    }
    catch (...)
    {
        // Must not throw from destructor
    }
}

bool ApiServer::start()
{
    auto data_dir = get_data_dir();
    kforge::util::init_logger(data_dir + "/forge.log");

    std::string db_path = data_dir + "/meta.db";
    std::filesystem::create_directories(data_dir);
    auto db = storage::Database::open(db_path);
    if (!db)
    {
        LOG_ERROR("Database open failed: {}", db.error().message);
        return false;
    }
    db_ = std::move(*db);
    LOG_INFO("Server initialized (data: {})", data_dir);

    core::TypeRegistry::instance().load_component_types("config/component_types.json");
    core::TypeRegistry::instance().load_package_types("config/package_types.json");

    init_plugins();
    orchestrator_ = std::make_unique<ImportOrchestrator>(db_->handle());
    orchestrator_->start();  // multithreading import  多线程导入

    setup_routes();
    srv_.set_mount_point("/", find_webui_dir());

    // port 0 = let OS auto-assign
    if (port_ == 0)
    {
        port_ = srv_.bind_to_any_port("127.0.0.1");
        if (port_ < 0)
        {
            LOG_ERROR("Failed to bind to any port");
            return false;
        }
        thread_ = std::make_unique<std::thread>(
            [this]()
            {
                srv_.listen_after_bind();
            });
    }
    else
    {
        thread_ = std::make_unique<std::thread>(
            [this]()
            {
                srv_.listen("127.0.0.1", port_);
            });
    }
    LOG_INFO("Server bound to 127.0.0.1:{}", port_);
    return true;
}

void ApiServer::init_plugins()
{
    std::vector<std::filesystem::path> paths;

    // 1. Bundled plugins 
    auto bundled = get_exe_dir() / "plugins";
    if (std::filesystem::exists(bundled))
    {
        paths.emplace_back(bundled);
    }

    // 2. User plugins (portable=exe_dir/plugins, installed=%APPDATA%/KiCad_Forge/plugins)
    auto user_plugins = std::filesystem::path(get_data_dir()) / ".." / "plugins";
    auto canonical = std::filesystem::weakly_canonical(user_plugins);
    if (std::filesystem::exists(canonical) && canonical != std::filesystem::weakly_canonical(bundled))
    {
        paths.emplace_back(canonical);
    }

    plugins_ = std::make_unique<plugin::PluginManager>(paths);
    plugins_->discover();

    int loaded = 0;
    for (auto& m : plugins_->available_plugins())
    {
        if (m.one_click)
        {
            auto _ = plugins_->load(m.id, nullptr);
            loaded++;
        }
    }
    LOG_INFO("Plugins: {} available, {} loaded", plugins_->available_plugins().size(), loaded);
}

void ApiServer::wait()
{
    if (thread_ && thread_->joinable())
    {
        thread_->join();
    }
}
void ApiServer::stop()
{
    LOG_INFO("Shutdown: stopping plugins...");
    if (plugins_)
    {
        try
        {
            plugins_->shutdown_all();
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Plugin shutdown error: {}", e.what());
        }
        catch (...)
        {
            LOG_ERROR("Plugin shutdown error: unknown.Please send log to us");
        }
    }

    LOG_INFO("Shutdown: stopping HTTP server...");
    srv_.stop();

    if (thread_ && thread_->joinable())
    {
        LOG_INFO("Shutdown: joining server thread...");
        try
        {
            thread_->join();
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Thread join error: {}", e.what());
        }
        catch (...)
        {
            LOG_ERROR("Thread join error: unknown");
        }
        thread_.reset();
    }

    // Signal import to stop and wait
    if (orchestrator_) orchestrator_->stop();

    LOG_INFO("Shutdown: closing database...");
    db_.reset();
    LOG_INFO("Shutdown: flushing logs...");
    kforge::util::shutdown_logger();
}

int64_t ApiServer::ms_since_heartbeat() const
{
    auto last = last_heartbeat_.load(std::memory_order_relaxed);
    if (last == 0)
        return INT64_MAX;           // no heartbeat yet
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return (now - last) / 1000000;  // ns → ms
}


// tool: send JSON response
static void json_response(httplib::Response& r, const json& j)
{
    r.set_content(j.dump(), "application/json");
}

std::string read_file_str(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void ApiServer::serve_plugin_icon(const std::filesystem::path& plugin_dir,
                                  const std::string& filename, httplib::Response& r)
{
    auto icon_path = plugin_dir / filename;
    if (!std::filesystem::exists(icon_path))
    {
        r.status = 404;
        return;
    }

    std::string ext = icon_path.extension().string();
    if (ext == ".svg")
    {
        r.set_content(read_file_str(icon_path.string()), "image/svg+xml");
    }
    else if (ext == ".png")
    {
        r.set_content(read_file_str(icon_path.string()), "image/png");
    }
    else
    {
        r.status = 404;
    }
}

void ApiServer::setup_routes()  // NOLINT(readability-function-cognitive-complexity)
{
    // ---- Route groups (extracted to src/api/routes.cpp) ----
    register_library_routes(srv_, db_->handle());
    register_symbol_routes(srv_, db_->handle());
    register_plugin_routes(srv_, plugins_.get());
    register_settings_routes(srv_, db_->handle());
    register_type_routes(srv_, db_->handle());
    register_status_routes(srv_, db_->handle());
    register_classify_routes(srv_, db_->handle());

    // ---- Heartbeat + import status (need ApiServer internals) ----
    srv_.Get("/api/status", [this](const httplib::Request&, httplib::Response& r) {
        last_heartbeat_.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);
        json j;
        storage::SymbolRepository sr(db_->handle());
        j["symbols"] = sr.count();
        storage::FootprintRepository fr(db_->handle());
        j["footprints"] = fr.count();
        j["ok"] = true;
        r.set_content(j.dump(), "application/json");
    });

    srv_.Get("/api/import-status", [this](const httplib::Request&, httplib::Response& r) {
        json j;
        j["importing"] = orchestrator_ ? orchestrator_->is_running() : false;
        storage::SymbolRepository sr(db_->handle());
        j["symbols"] = sr.count();
        storage::FootprintRepository fr(db_->handle());
        j["footprints"] = fr.count();
        storage::Model3DRepository mr(db_->handle());
        j["models"] = mr.count();
        r.set_content(j.dump(), "application/json");
    });

    srv_.Post("/api/bye", [this](const httplib::Request&, httplib::Response& r) {
        shutting_down_.store(true, std::memory_order_relaxed);
        r.set_content(R"({"ok":true})", "application/json");
    });

    // ---- Classify / Check / Automatch (need Database*) ----
    srv_.Post("/api/classify", [this](const httplib::Request&, httplib::Response& r) {
        services::ClassificationService svc(db_.get());
        auto summary = svc.classify_all();
        json j;
        if (!summary) { j["error"] = summary.error().message; }
        else {
            j["total"] = summary->total; j["matched"] = summary->matched;
            j["type_updated"] = summary->type_updated;
            json arr = json::array();
            for (auto& res : summary->results) {
                json o;
                o["name"] = res.symbol_name; o["library"] = res.target_library;
                o["type"] = res.type_name; o["confidence"] = res.confidence;
                arr.push_back(o);
            }
            j["results"] = arr;
        }
        r.set_content(j.dump(), "application/json");
    });

    srv_.Post("/api/check", [this](const httplib::Request&, httplib::Response& r) {
        services::CorrespondenceService svc(db_.get());
        auto issues = svc.check_all();
        json arr = json::array();
        if (issues) for (auto& iss : *issues) {
            json o;
            o["symbol"] = iss.entity_name; o["issue"] = iss.message;
            o["severity"] = issue_severity_to_string(iss.severity);
            arr.push_back(o);
        }
        r.set_content(arr.dump(), "application/json");
    });

    srv_.Post("/api/automatch", [this](const httplib::Request&, httplib::Response& r) {
        services::CorrespondenceService svc(db_.get());
        auto _ = svc.auto_link();
        auto sug = svc.suggest_matches();
        json arr = json::array();
        if (sug) {
            int n = std::min(50, (int)sug->size());
            for (int i = 0; i < n; i++) {
                json m;
                m["symbol"] = sug->at(i).symbol_name;
                m["footprint"] = sug->at(i).footprint_name;
                m["score"] = (int)(sug->at(i).score * 100);
                arr.push_back(m);
            }
        }
        r.set_content(arr.dump(), "application/json");
    });
}

}  // namespace kforge::api
