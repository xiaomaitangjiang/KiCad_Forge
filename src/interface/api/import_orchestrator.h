// Background import controller — owns import + link threads and atomic counters
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "interface/service/import/pipeline.h"

namespace kforge::storage { class Database; }
namespace kforge::util { class ConfigStore; }

namespace kforge::api {

class ImportOrchestrator {
public:
    struct ProgressSnapshot {
        std::string phase = "idle";
        int completed = 0;
        int total = 0;
    };
    ImportOrchestrator(storage::Database& db, util::ConfigStore* config = nullptr,
                       classifier::TypeMatchingStore* matching_store = nullptr)
        : db_(&db), config_(config), matching_store_(matching_store ? matching_store
                                       : &classifier::TypeMatchingStore::instance())
    {
    }
    ~ImportOrchestrator();

    // Start background import. Safe to call multiple times — skips if already running.
    void start();

    // Signal stop and wait for threads to finish
    void stop();

    [[nodiscard]] bool is_running() const { return running_.load(std::memory_order_relaxed); }
    [[nodiscard]] int sym_count() const { return sym_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] int fp_count() const { return fp_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool cancel_requested() const { return cancel_.load(std::memory_order_relaxed); }
    // 独立于数据库会话：导入事务持有 SQLite 连接时仍可读取当前文件进度。
    [[nodiscard]] ProgressSnapshot progress_snapshot() const;

    // In-process import-completion counter — MgrHolder uses this to detect
    // when the SymbolBindingManager in-memory index is stale.
    [[nodiscard]] int64_t import_seq() const { return import_seq_.load(std::memory_order_relaxed); }

    // Synchronous import, including relationship reconciliation (manual "Reimport" button)。
    // force = 本次忽略已缓存 mtime（在同一个健康 lease 内处理，不清全局快照表）。
    services::ImportPipeline::Result run_now(bool force = false);

private:
    storage::Database* db_;
    util::ConfigStore* config_{nullptr};
    classifier::TypeMatchingStore* matching_store_;
    // Serializes lifecycle changes and manual imports. Progress has its own
    // mutex, so reading the snapshot never waits for an import to finish.
    std::mutex execution_mutex_;
    std::thread import_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<int> sym_count_{0};
    std::atomic<int> fp_count_{0};
    std::atomic<int64_t> import_seq_{0};
    mutable std::mutex progress_mutex_;
    ProgressSnapshot progress_;
    void update_progress(const std::string& phase, int completed, int total);
    void finish_progress(const services::ImportPipeline::Result& result);
    void stop_locked();
};

}  // namespace kforge::api
