#include "interface/api/import_manager.h"

#include <sqlite3.h>

#include "interface/api/import_orchestrator.h"
#include "util/config_store.h"
#include "util/logger.h"

namespace kforge::api
{

ImportManager::ImportManager(sqlite3* db, util::ConfigStore* config)
    : db_(db), config_(config), orch_(std::make_unique<ImportOrchestrator>(db, config))
{
}

ImportManager::~ImportManager() = default;

// ---- Launcher 服务接口 ----
util::Result<void> ImportManager::build(storage::DbService& db, util::ConfigStore& cfg)
{
    auto& self = kforge::launcher::util::instance_store<ImportManager>().value();
    self.db_ = db.handle();
    self.config_ = &cfg;
    self.orch_ = std::make_unique<ImportOrchestrator>(self.db_, self.config_);
    return {};
}

util::Result<void> ImportManager::destroy()
{
    auto& self = kforge::launcher::util::instance_store<ImportManager>().value();
    self.stop_async();
    return {};
}

// ---- Background auto-import (forwards to orchestrator) ----

void ImportManager::start_async()
{
    orch_->start();
}

void ImportManager::stop_async()
{
    orch_->stop();
}

bool ImportManager::is_running() const
{
    return orch_->is_running();
}

int ImportManager::sym_count() const
{
    return orch_->sym_count();
}

int ImportManager::fp_count() const
{
    return orch_->fp_count();
}

// ---- Synchronous imports ----

services::ImportPipeline::Result ImportManager::import_all(const Options& opts)
{
    if (opts.force)
    {
        clear_imported_files_cache();  // mtime snapshots gone → full re-import
    }
    return orch_->run_now();
}

services::ImportPipeline::Result ImportManager::import_directory(const std::string& dir)
{
    services::ImportPipeline pipe(db_);
    pipe | services::symbols_from{.dir = dir, .comp_lib_id = ""};
    return pipe | services::execute;
}

void ImportManager::clear_imported_files_cache()
{
    char* errmsg = nullptr;
    if (sqlite3_exec(db_, "DELETE FROM imported_files", nullptr, nullptr, &errmsg) != SQLITE_OK)
    {
        kforge::util::log_error{}("Failed to clear imported_files cache: {}",
                                  errmsg != nullptr ? errmsg : "SQL error");
        sqlite3_free(errmsg);
    }
}

}  // namespace kforge::api
