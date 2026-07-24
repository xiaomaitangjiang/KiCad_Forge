#pragma once

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
    explicit ApiServer(int port = 8080);
    ~ApiServer();

    bool start();
    void wait();
    void stop();

private:
    void setup_routes();
    void init_plugins();
    void auto_import();

    int port_;
    httplib::Server srv_;
    std::unique_ptr<std::thread> thread_;
    std::unique_ptr<storage::Database> db_;
    std::unique_ptr<plugin::PluginManager> plugins_;
};

}  // namespace kforge::api
