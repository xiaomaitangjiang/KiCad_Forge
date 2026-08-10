// Route groups — extracted from setup_routes()
#pragma once

struct sqlite3;

namespace httplib { class Server; struct Request; struct Response; }
namespace kforge::plugin { class PluginManager; }

namespace kforge::api {

void register_library_routes(httplib::Server&, sqlite3* db);
void register_symbol_routes(httplib::Server&, sqlite3* db);
void register_plugin_routes(httplib::Server&, plugin::PluginManager*);
void register_settings_routes(httplib::Server&, sqlite3* db, class ImportOrchestrator* orch = nullptr);
void register_type_routes(httplib::Server&, sqlite3* db);
void register_status_routes(httplib::Server&, sqlite3* db);
void register_classify_routes(httplib::Server&, sqlite3* db);

}  // namespace kforge::api
