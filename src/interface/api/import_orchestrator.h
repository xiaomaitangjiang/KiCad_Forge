// Background import controller — owns import + link threads and atomic counters
#pragma once

#include <atomic>
#include <thread>

#include "interface/service/import/pipeline.h"

struct sqlite3;
namespace kforge::util { class ConfigStore; }

namespace kforge::api {

class ImportOrchestrator {
public:
    ImportOrchestrator(sqlite3* db, util::ConfigStore* config = nullptr) : db_(db), config_(config) {}
    ~ImportOrchestrator();

    // Start background import. Safe to call multiple times — skips if already running.
    void start();

    // Signal stop and wait for threads to finish
    void stop();

    [[nodiscard]] bool is_running() const { return running_.load(std::memory_order_relaxed); }
    [[nodiscard]] int sym_count() const { return sym_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] int fp_count() const { return fp_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool cancel_requested() const { return cancel_.load(std::memory_order_relaxed); }

    // In-process import-completion counter — MgrHolder uses this to detect
    // when the SymbolBindingManager in-memory index is stale.
    [[nodiscard]] int64_t import_seq() const { return import_seq_.load(std::memory_order_relaxed); }

    // Synchronous import, async post-processing (for manual "Reimport" button)
    services::ImportPipeline::Result run_now();

private:
    sqlite3* db_;
    util::ConfigStore* config_{nullptr};
    std::thread import_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<int> sym_count_{0};
    std::atomic<int> fp_count_{0};
    std::atomic<int64_t> import_seq_{0};
};

}  // namespace kforge::api
