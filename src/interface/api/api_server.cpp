#include "core/model/type_registry.h"
#include "core/repo/repositories.h"
#include "interface/api/api_server.h"
#include "interface/api/routes.h"
#include "interface/manager/setup/launcher.hpp"
#include "interface/service/classification/service.h"
#include "interface/service/correspondence/service.h"
#include "interface/service/library/service.h"
#include "plugin/plugin_manager.h"
#include "util/logger.h"
#include "util/platform.h"
#include "util/result.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <sstream>
#include <string_view>


using json = nlohmann::json;

namespace kforge::api
{

namespace
{
// ---- Local-request guard: the server binds 127.0.0.1 and serves the webui
//      same-origin. CORS headers are echoed only for local origins, and any
//      request carrying a foreign Origin/Host (malicious web page, DNS
//      rebinding) is rejected before it reaches a handler — browsers still
//      send simple requests without a preflight, so header-only CORS is not
//      enough.

bool is_local_hostname(const std::string& host)
{
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

// "http://host[:port][/path]" → true when host is a loopback address.
bool is_local_origin(const std::string& origin)
{
    constexpr std::string_view kHttpPrefix = "http://";
    if (!origin.starts_with(kHttpPrefix))
    {
        return false;
    }
    auto rest = origin.substr(kHttpPrefix.size());
    auto host_end = rest.find_first_of(":/");
    auto host = rest.substr(0, host_end);
    return is_local_hostname(host);
}

// Host header "host[:port]" → true when host is loopback (DNS-rebinding guard).
bool is_local_host(const std::string& host)
{
    auto colon = host.find(':');
    return is_local_hostname(host.substr(0, colon));
}

std::string find_webui_dir()
{
    auto exe_dir = kforge::util::get_exe_dir();
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

    kforge::util::log_error{}("FATAL: webui/index.html not found next to exe at {}", exe_dir.string());
    std::abort();
}

}  // anonymous namespace

ApiServer::ApiServer(int port) : port_(port), srv_(std::make_unique<httplib::Server>())
{
}

ApiServer::ApiServer(ApiServer&& other) noexcept
    : port_(other.port_),
      srv_(std::move(other.srv_)),
      thread_(std::move(other.thread_)),
      db_(other.db_),
      plugins_(other.plugins_),
      config_(other.config_),
      import_manager_(other.import_manager_),
      shutting_down_(false),   // atomic 不可移动 —— 标量重置
      last_heartbeat_(0)
{
}
ApiServer::~ApiServer()
{
    // 幂等兜底：launcher destroy 已调 shutdown；析构再调一次无害
    try
    {
        shutdown();
    }
    catch (const std::exception& e)
    {
        kforge::util::log_error{}("Shutdown error in destructor: {}", e.what());
    }
    catch (...)
    {
        kforge::util::log_error{}("Shutdown error in destructor: unknown exception");
    }
}

// ---- Launcher 服务接口：实例入右值槽（构造时已存在），build/destroy 只做启动/停止 ----
util::Result<void> ApiServer::build(storage::DbService& db, util::ConfigStore& cfg,
                                    plugin::PluginManager& plugins, ImportManager& imports)
{
    auto& self = kforge::launcher::util::instance_store<ApiServer>().value();
    return self.init(db, &cfg, &plugins, &imports);
}

util::Result<void> ApiServer::destroy()
{
    auto& self = kforge::launcher::util::instance_store<ApiServer>().value();
    self.shutdown();
    return {};
}

util::Result<void> ApiServer::init(storage::DbService& db, util::ConfigStore* cfg,
                                   plugin::PluginManager* plugins, ImportManager* imports)
{
    db_ = db.db.get();
    config_ = cfg;
    plugins_ = plugins;
    import_manager_ = imports;

    setup_routes();
    srv_->set_mount_point("/", find_webui_dir());

    // CORS: 仅对本机 origin 回显响应头；外来 Origin/Host 请求直接拒绝。
    srv_->set_pre_routing_handler(
        [](const httplib::Request& req, httplib::Response& res)
        {
            if (req.has_header("Origin") &&
                !is_local_origin(req.get_header_value("Origin")))
            {
                res.status = httplib::StatusCode::Forbidden_403;
                res.set_content("Forbidden", "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.has_header("Host") && !is_local_host(req.get_header_value("Host")))
            {
                res.status = httplib::StatusCode::Forbidden_403;
                res.set_content("Forbidden", "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.method == "OPTIONS")
            {
                auto origin = req.get_header_value("Origin");
                if (!origin.empty())
                {
                    res.set_header("Access-Control-Allow-Origin", origin);
                }
                res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                res.set_header("Access-Control-Allow-Headers", "Content-Type");
                res.status = 204;
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
    srv_->set_post_routing_handler(
        [](const httplib::Request& req, httplib::Response& res)
        {
            if (req.has_header("Origin"))
            {
                auto origin = req.get_header_value("Origin");
                if (is_local_origin(origin))
                {
                    res.set_header("Access-Control-Allow-Origin", origin);
                }
            }
        });

    // port 0 = auto-assign
    if (port_ == 0)
    {
        port_ = srv_->bind_to_any_port("127.0.0.1");
        if (port_ < 0)
        {
            kforge::util::log_error{}("Failed to bind to any port");
            return std::unexpected(
                kforge::util::Error::make<kforge::util::Error::Kind::ServiceError>("bind failed"));
        }
        thread_ = std::make_unique<std::thread>(
            [this]()
            {
                srv_->listen_after_bind();
            });
    }
    else
    {
        thread_ = std::make_unique<std::thread>(
            [this]()
            {
                srv_->listen("127.0.0.1", port_);
            });
    }
    kforge::util::log_info{}("Server bound to 127.0.0.1:{}", port_);
    return {};
}

void ApiServer::wait()
{
    if (thread_ && thread_->joinable())
    {
        thread_->join();
    }
}
// 停 HTTP 服务与线程；依赖的导入/插件由各自服务销毁（此处仅作为 ApiServer 依赖的连带停止）
void ApiServer::shutdown()
{
    kforge::util::log_info{}("Shutdown: stopping plugins...");
    if (plugins_)
    {
        try
        {
            plugins_->shutdown_all();
        }
        catch (const std::exception& e)
        {
            kforge::util::log_error{}("Plugin shutdown error: {}", e.what());
        }
        catch (...)
        {
            kforge::util::log_error{}("Plugin shutdown error: unknown.Please send log to us");
        }
    }

    kforge::util::log_info{}("Shutdown: stopping HTTP server...");
    if (srv_)  // 移动构造后源对象 srv_ 为 null（析构兜底时跳过）
    {
        srv_->stop();
    }

    if (thread_ && thread_->joinable())
    {
        kforge::util::log_info{}("Shutdown: joining server thread...");
        try
        {
            thread_->join();
        }
        catch (const std::exception& e)
        {
            kforge::util::log_error{}("Thread join error: {}", e.what());
        }
        catch (...)
        {
            kforge::util::log_error{}("Thread join error: unknown");
        }
        thread_.reset();
    }

    if (import_manager_)
    {
        import_manager_->stop_async();
    }
}

int64_t ApiServer::ms_since_heartbeat() const
{
    auto last = last_heartbeat_.load(std::memory_order_relaxed);
    if (last == 0)
    {
        return 0;  // no heartbeat yet → treat as fresh (give browser time to connect)
    }
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return (now - last) / 1000000;  // ns → ms
}


// tool: send JSON response
static void json_response(httplib::Response& r, const json& j)
{
    r.set_content(j.dump(), "application/json");
}

static std::string read_file_str(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
    {
        return {};
    }
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
    register_library_routes(*srv_, db_->handle(),
                            import_manager_ ? import_manager_->orchestrator() : nullptr);
    register_symbol_routes(*srv_, db_->handle());
    register_plugin_routes(*srv_, plugins_);
    register_settings_routes(*srv_, db_->handle(), import_manager_, config_);
    register_type_routes(*srv_, db_->handle());
    register_status_routes(*srv_, db_->handle());
    register_classify_routes(*srv_, db_->handle(), import_manager_);

    // ---- Heartbeat + import status (need ApiServer internals) ----
    srv_->Get("/api/status",
             [this](const httplib::Request&, httplib::Response& r)
             {
                 last_heartbeat_.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                       std::memory_order_relaxed);
                 json j;
                 storage::SymbolRepository sr(db_->handle());
                 j["symbols"] = sr.count();
                 storage::FootprintRepository fr(db_->handle());
                 j["footprints"] = fr.count();
                 j["ok"] = true;
                 r.set_content(j.dump(), "application/json");
             });

    srv_->Get("/api/import-status",
             [this](const httplib::Request&, httplib::Response& r)
             {
                 json j;
                 j["importing"] = import_manager_ ? import_manager_->is_running() : false;
                 storage::SymbolRepository sr(db_->handle());
                 j["symbols"] = sr.count();
                 storage::FootprintRepository fr(db_->handle());
                 j["footprints"] = fr.count();
                 storage::Model3DRepository mr(db_->handle());
                 j["models"] = mr.count();
                 r.set_content(j.dump(), "application/json");
             });

    srv_->Post("/api/bye",
              [this](const httplib::Request&, httplib::Response& r)
              {
                  shutting_down_.store(true, std::memory_order_relaxed);
                  r.set_content(R"({"ok":true})", "application/json");
              });

    // ---- Classify / Check / Automatch (need Database*) ----
    srv_->Post("/api/classify",
              [this](const httplib::Request&, httplib::Response& r)
              {
                  services::ClassificationService svc(db_);
                  auto summary = svc.classify_all();
                  json j;
                  if (!summary)
                  {
                      j["error"] = summary.error().format_message();
                  }
                  else
                  {
                      j["total"] = summary->total;
                      j["matched"] = summary->matched;
                      j["type_updated"] = summary->type_updated;
                      json arr = json::array();
                      for (auto& res : summary->results)
                      {
                          json o;
                          o["name"] = res.symbol_name;
                          o["library"] = res.target_library;
                          o["type"] = res.type_name;
                          o["confidence"] = res.confidence;
                          arr.push_back(o);
                      }
                      j["results"] = arr;
                  }
                  r.set_content(j.dump(), "application/json");
              });

    srv_->Post("/api/check",
              [this](const httplib::Request&, httplib::Response& r)
              {
                  services::CorrespondenceService svc(db_);
                  auto issues = svc.check_all();
                  json arr = json::array();
                  if (issues)
                      for (auto& iss : *issues)
                      {
                          json o;
                          o["symbol"] = iss.entity_name;
                          o["issue"] = iss.message;
                          o["severity"] = issue_severity_to_string(iss.severity);
                          arr.push_back(o);
                      }
                  r.set_content(arr.dump(), "application/json");
              });

    srv_->Post("/api/automatch",
              [this](const httplib::Request&, httplib::Response& r)
              {
                  services::CorrespondenceService svc(db_);
                  auto sug = svc.suggest_matches();
                  json arr = json::array();
                  if (sug)
                  {
                      int n = std::min(50, (int) sug->size());
                      for (int i = 0; i < n; i++)
                      {
                          json m;
                          m["symbol"] = sug->at(i).symbol_name;
                          m["footprint"] = sug->at(i).footprint_name;
                          m["score"] = (int) (sug->at(i).score * 100);
                          arr.push_back(m);
                      }
                  }
                  r.set_content(arr.dump(), "application/json");
              });

    // Per-symbol match suggestions — lazy, computed on click
    srv_->Post("/api/symbols/([^/]+)/matches",
              [this](const httplib::Request& req, httplib::Response& r)
              {
                  auto id = req.matches[1];
                  services::CorrespondenceService svc(db_);
                  auto sug = svc.suggest_matches_for(id, 10);
                  json arr = json::array();
                  if (sug)
                  {
                      for (auto& s : *sug)
                      {
                          json m;
                          m["footprint_id"] = s.footprint_id;
                          m["footprint"] = s.footprint_name;
                          m["score"] = (int) (s.score * 100);
                          arr.push_back(m);
                      }
                  }
                  r.set_content(arr.dump(), "application/json");
              });
}

}  // namespace kforge::api