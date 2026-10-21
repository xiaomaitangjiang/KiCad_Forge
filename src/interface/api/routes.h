// Route groups — extracted from setup_routes()
#pragma once

namespace httplib { class Server; struct Request; struct Response; }
namespace kforge::plugin { class PluginManager; }
namespace kforge::api { class ImportManager; }
namespace kforge::storage { class Database; }
namespace kforge::util { class ConfigStore; }

namespace kforge::api {

// 路由只拿连接持有者；每次数据库操作在处理器内自取 session
void register_library_routes(httplib::Server&, storage::Database& db, class ImportOrchestrator* import_orch = nullptr);
void register_symbol_routes(httplib::Server&, storage::Database& db);
void register_plugin_routes(httplib::Server&, plugin::PluginManager*, storage::Database& db);
void register_settings_routes(httplib::Server&, storage::Database& db, class ImportManager* import_manager = nullptr,
                              util::ConfigStore* config = nullptr);
void register_type_routes(httplib::Server&, storage::Database& db);
void register_status_routes(httplib::Server&, storage::Database& db);
void register_classify_routes(httplib::Server&, storage::Database& db, class ImportManager* import_manager = nullptr);

}  // namespace kforge::api