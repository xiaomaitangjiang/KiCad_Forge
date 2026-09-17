// Facade for all import entry points (route handlers + startup).
// Owns the ImportOrchestrator (background auto-import) and exposes
// synchronous one-shot imports — routes no longer construct ImportPipeline
// inline (previously duplicated in three POST handlers).
#pragma once

#include <memory>
#include <string>

#include "core/db/db_service.hpp"
#include "interface/api/import_orchestrator.h"
#include "interface/service/import/pipeline.h"
#include "util/config_store.h"
#include "util/service_runtime.hpp"

struct sqlite3;

namespace kforge::api {

class ImportOrchestrator;

class ImportManager {
public:
    struct Options {
        bool force = false;  // true = drop mtime snapshots, re-import everything
    };

    ImportManager() = default;  // 右值槽：build 内初始化
    ImportManager(ImportManager&&) = default;  // 自定义析构抑制隐式移动，显式恢复
    ImportManager(sqlite3* db, util::ConfigStore* config);
    ~ImportManager();

    // Launcher 服务接口
    static util::Result<void> build(storage::DbService& db, util::ConfigStore& cfg);
    static util::Result<void> destroy();

    // Background auto-import (app startup / after db reset) — forwards to orchestrator
    void start_async();
    void stop_async();
    [[nodiscard]] bool is_running() const;
    [[nodiscard]] int sym_count() const;
    [[nodiscard]] int fp_count() const;
    [[nodiscard]] ImportOrchestrator* orchestrator() const { return orch_.get(); }

    // Synchronous imports for route handlers
    /// All enabled component libraries (+ legacy settings fallback).
    /// (No default argument here: Options' default member initializer needs
    /// the complete type, which is unavailable inside the class definition.)
    services::ImportPipeline::Result import_all(const Options& opts);
    /// One directory of symbols (POST /api/import?dir=).
    services::ImportPipeline::Result import_directory(const std::string& dir);

    /// Drop the mtime snapshot table — used by db/reset and force re-import.
    void clear_imported_files_cache();

private:
    sqlite3* db_{nullptr};
    util::ConfigStore* config_{nullptr};
    std::unique_ptr<ImportOrchestrator> orch_;
};

}  // namespace kforge::api
