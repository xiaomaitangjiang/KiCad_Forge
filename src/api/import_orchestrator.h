// Background import controller — owns import + link threads and atomic counters
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "services/import_pipeline.h"

namespace kforge::api {

class ImportOrchestrator {
public:
    explicit ImportOrchestrator(sqlite3* db) : db_(db) {}
    ~ImportOrchestrator();

    // Start background import. Safe to call multiple times — skips if already running.
    void start();

    // Signal stop and wait for threads to finish
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    bool is_linking() const { return linking_.load(std::memory_order_relaxed); }
    int sym_count() const { return sym_count_.load(std::memory_order_relaxed); }
    int fp_count() const { return fp_count_.load(std::memory_order_relaxed); }
    bool cancel_requested() const { return cancel_.load(std::memory_order_relaxed); }

    // Synchronous import, async post-processing (for manual "Reimport" button)
    services::ImportPipeline::Result run_now();

    // Async auto-link + 3D linking (runs in background thread, non-blocking)
    void async_link();

private:
    sqlite3* db_;
    std::thread import_thread_;
    std::thread link_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> linking_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<int> sym_count_{0};
    std::atomic<int> fp_count_{0};
};

}  // namespace kforge::api
