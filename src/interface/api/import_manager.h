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

namespace kforge::api {

class ImportOrchestrator;

class ImportManager {
public:
    struct Options {
        // true = 本次忽略已缓存 mtime、强制重导启用目标。
        // 不再先 DELETE 全表：那样会在健康领取之前消费别人的来源快照，
        // 坏配置下会连带丢掉停用来源的 mtime。清表只属于 db/reset。
        bool force = false;
    };

    explicit ImportManager(classifier::TypeMatchingStore* matching_store = nullptr)
        : matching_store_(matching_store ? matching_store : &classifier::TypeMatchingStore::instance()) {}
    ImportManager(ImportManager&&) = default;  // 自定义析构抑制隐式移动，显式恢复
    ImportManager(storage::Database& db, util::ConfigStore* config,
                  classifier::TypeMatchingStore* matching_store = nullptr);
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

    /// Drop the mtime snapshot table — used by db/reset (整表重建)。
    /// 重导入按钮走 import_all({.force = true})，不再经过这里。
    void clear_imported_files_cache();

private:
    storage::Database* db_{nullptr};
    util::ConfigStore* config_{nullptr};
    classifier::TypeMatchingStore* matching_store_;
    std::unique_ptr<ImportOrchestrator> orch_;
};

}  // namespace kforge::api
