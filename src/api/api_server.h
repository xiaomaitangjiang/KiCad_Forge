#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "../third_party/httplib.h"
#include "storage/database.h"
#include "plugin/plugin_manager.h"
#include "api/import_orchestrator.h"

namespace kforge::services { class LibraryService; }

namespace kforge::api {

class ApiServer {
public:
    explicit ApiServer(int port = 20443);
    ~ApiServer();

    bool start();
    void wait();
    void stop();

    int port() const { return port_; }

    int64_t ms_since_heartbeat() const;
    bool should_stop() const { return shutting_down_.load(std::memory_order_relaxed); }

    // For route handler access
    sqlite3* db_handle() { return db_->handle(); }
    plugin::PluginManager* plugins() { return plugins_.get(); }

private:
    void setup_routes();
    void init_plugins();
    void auto_import();

    void serve_plugin_icon(const std::filesystem::path& plugin_dir,
                           const std::string& filename,
                           httplib::Response& r);

    int port_;
    httplib::Server srv_;
    std::unique_ptr<std::thread> thread_;
    std::unique_ptr<storage::Database> db_;
    std::unique_ptr<plugin::PluginManager> plugins_;
    std::unique_ptr<ImportOrchestrator> orchestrator_;
    std::atomic<bool> shutting_down_{false};
    std::atomic<int64_t> last_heartbeat_{0};
};

}  // namespace kforge::api
