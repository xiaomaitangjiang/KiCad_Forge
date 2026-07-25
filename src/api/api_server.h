#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "../third_party/httplib.h"
#include "storage/database.h"
#include "plugin/plugin_manager.h"

namespace kforge::api {

/// Thin HTTP layer — routes requests to services, serves webui/.
class ApiServer {
public:
    explicit ApiServer(int port = 20443);
    ~ApiServer();

    bool start();
    void wait();
    void stop();

    // Heartbeat — frontend calls /api/status periodically.
    // Returns milliseconds since last heartbeat. Used to decide when to shut down.
    int64_t ms_since_heartbeat() const;

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
    std::atomic<int64_t> last_heartbeat_{0};
};

}  // namespace kforge::api
