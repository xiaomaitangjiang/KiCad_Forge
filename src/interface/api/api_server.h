#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "../third_party/httplib.h"
#include "core/db/database.h"
#include "core/db/db_service.hpp"
#include "plugin/plugin_manager.h"
#include "interface/api/import_manager.h"
#include "util/config_store.h"
#include "util/error.h"

namespace kforge::services { class LibraryService; }

namespace kforge::api {

class ApiServer {
public:
    // Launcher 服务（右值槽）：build 经 instance_store<ApiServer>() 取 self 调 init。
    static util::Result<void> build(storage::DbService& db, util::ConfigStore& cfg,
                                    plugin::PluginManager& plugins, ImportManager& imports);
    // destroy 经 instance_store 取 self 调 shutdown。
    static util::Result<void> destroy();

    explicit ApiServer(int port = 0);  // 0 = OS 自动分配
    // 自定义析构+atomic 成员使 =default 移动被删除 —— 自定义移动构造（右值槽 emplace 需要；
    // atomic 无法移动，标量重置）
    ApiServer(ApiServer&& other) noexcept;
    ~ApiServer();

    // HTTP 业务装配（仅自身：routes/mount/CORS/端口线程），依赖以指针注入
    util::Result<void> init(storage::DbService& db, util::ConfigStore* cfg,
                            plugin::PluginManager* plugins, ImportManager* imports);
    void wait();
    void shutdown();  // 停 srv_/线程 + 停依赖（import/plugins）

    int port() const { return port_; }

    int64_t ms_since_heartbeat() const;
    bool should_stop() const { return shutting_down_.load(std::memory_order_relaxed); }

    // For route handler access
    sqlite3* db_handle() { return db_->handle(); }
    plugin::PluginManager* plugins() { return plugins_; }

private:
    void setup_routes();

    void serve_plugin_icon(const std::filesystem::path& plugin_dir,
                           const std::string& filename,
                           httplib::Response& r);

    int port_;
    std::unique_ptr<httplib::Server> srv_;  // unique_ptr：httplib::Server 不可移动，ApiServer 借其可移动（右值槽）
    std::unique_ptr<std::thread> thread_;
    storage::Database* db_{nullptr};
    plugin::PluginManager* plugins_{nullptr};
    util::ConfigStore* config_{nullptr};
    ImportManager* import_manager_{nullptr};
    std::atomic<bool> shutting_down_{false};
    std::atomic<int64_t> last_heartbeat_{0};
};

}  // namespace kforge::api
