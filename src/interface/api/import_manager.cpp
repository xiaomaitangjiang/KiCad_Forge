#include "interface/api/import_manager.h"

#include <sqlite3.h>

#include "interface/api/import_orchestrator.h"
#include "util/config_store.h"
#include "util/logger.h"

namespace kforge::api
{

ImportManager::ImportManager(storage::Database& db, util::ConfigStore* config,
                             classifier::TypeMatchingStore* matching_store)
    : db_(&db), config_(config), matching_store_(matching_store ? matching_store
                                      : &classifier::TypeMatchingStore::instance()),
      orch_(std::make_unique<ImportOrchestrator>(db, config, matching_store_))
{
}

ImportManager::~ImportManager() = default;

// ---- Launcher 服务接口 ----
util::Result<void> ImportManager::build(storage::DbService& db, util::ConfigStore& cfg)
{
    auto& self = kforge::launcher::util::instance_store<ImportManager>().value();
    self.db_ = &db.database();
    self.config_ = &cfg;
    self.orch_ = std::make_unique<ImportOrchestrator>(*self.db_, self.config_, self.matching_store_);
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
    if (orch_)
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
        // 只停止后台线程；清空快照表会让「还没健康领取」的坏配置消费掉别人的
        // mtime，force 语义改为在同一个健康 lease 内忽略本次已缓存 mtime。
        orch_->stop();
    }
    return orch_->run_now(opts.force);
}

services::ImportPipeline::Result ImportManager::import_directory(const std::string& dir)
{
    services::ImportPipeline pipe(*db_, nullptr, false, matching_store_);
    pipe | services::symbols_from{.dir = dir, .comp_lib_id = ""};
    return pipe | services::execute;
}

void ImportManager::clear_imported_files_cache()
{
    auto session = db_->session();
    char* errmsg = nullptr;
    if (sqlite3_exec(session.handle(), "DELETE FROM imported_files", nullptr, nullptr, &errmsg) !=
        SQLITE_OK)
    {
        kforge::util::log_error{}("Failed to clear imported_files cache: {}",
                                  errmsg != nullptr ? errmsg : "SQL error");
        sqlite3_free(errmsg);
    }
}

}  // namespace kforge::api
