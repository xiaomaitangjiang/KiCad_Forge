// Phase 4: unified Python/import task service.
//
// One TaskService owns every asynchronous plugin action (generic Python actions
// and full staged-import tasks), the per-plugin enable/disable admission gate
// and the cancellation/commit arbitration. All task state transitions happen
// under one service mutex, which makes requestCancel()/tryEnterCommit()/finish()
// atomic with respect to each other (see docs/PLUGINIZATION_PLAN.md §4.2).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/config_store.h"
#include "util/error.h"

namespace kforge::plugin
{

enum class TaskState
{
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

enum class TaskPhase
{
    None,       // generic actions: state "running", no sub-phase
    Queued,     // accepted but not started yet
    Staging,    // staged-import: plugin writes its private staging dir (cancellable)
    Committing, // staged-import: host validation/merge/commit (not cancellable)
};

const char* task_state_name(TaskState state);
const char* task_phase_name(TaskPhase phase);
bool task_state_is_terminal(TaskState state);

/// Public snapshot of one task. Copies are safe to hand to HTTP handlers.
struct TaskRecord
{
    std::string task_id;
    std::string plugin_id;
    std::string action;
    TaskState state{TaskState::Queued};
    TaskPhase phase{TaskPhase::Queued};
    bool cancelable{false};
    bool cancel_requested{false};
    int revision{0};  // plugin state revision captured when the task was created
    std::int64_t created_at_ms{0};
    std::int64_t started_at_ms{0};
    std::int64_t finished_at_ms{0};
    std::optional<nlohmann::json> result;
    std::string error;
    bool uncertain{false};
    std::optional<std::string> client_request_id;
};

enum class CancelStatus
{
    CancelRequested,
    AlreadyRequested,
    NotCancelable,
    Terminal,
    NotFound,
};
const char* cancel_status_name(CancelStatus status);

struct CancelReply
{
    CancelStatus status{CancelStatus::NotFound};
    TaskRecord task;
};

/// Result of admission + registration. `code` is a stable machine-readable
/// reason: "disabled", "busy", "stopped", "invalid", "start_failed".
struct SubmitReply
{
    bool ok{false};
    std::string code;
    std::string error;
    std::string task_id;
    TaskState state{TaskState::Queued};
};

struct TaskLimits
{
    std::size_t max_retained{256};          // terminal records kept (oldest evicted first)
    std::size_t max_global_concurrent{16};  // owned workers across all plugins, including finalization
    std::size_t max_per_plugin_concurrent{4};
};

struct PluginState
{
    bool enabled{true};
    int revision{0};
};

struct PluginStateReply
{
    bool ok{false};
    std::string code;  // "revision_conflict", "stopped", "persist_failed"
    std::string error;
    bool enabled{true};
    int revision{0};
    std::vector<std::string> cancel_requested;  // tasks newly asked to cancel
};

class TaskService
{
    struct Task;

public:
    /// Context handed to a Runner. It is the only way a task observes
    /// cancellation and publishes staged-import phases / commit intent.
    class Context
    {
    public:
        /// True once a cancel was requested for THIS task, or the service is
        /// shutting down (global fallback, same as PluginManager::execute).
        bool cancelled() const noexcept;
        bool cancel_requested() const noexcept;

        /// Atomically cross from "staging" (cancellable) to "committing"
        /// (not cancellable). Returns false when a cancel won the race — the
        /// caller must not write anything.
        bool try_enter_commit();
        /// Publish a sub-phase for progress display (no-op after commit).
        void set_phase(TaskPhase phase);
        /// Mark a failed result as "repository may have changed" (write action
        /// interrupted by timeout/overflow/shutdown).
        void set_uncertain(bool uncertain);

        int revision() const noexcept;
        const std::string& plugin_id() const noexcept;
        const std::string& action() const noexcept;

    private:
        friend class TaskService;
        Context(TaskService* service, std::shared_ptr<Task> task);
        TaskService* service_;
        std::shared_ptr<Task> task_;
    };

    /// Executes one task. Must observe Context::cancelled() and must return a
    /// JSON result on success (the task then becomes "succeeded").
    using Runner = std::function<util::Result<nlohmann::json>(Context&)>;

    /// config may be nullptr (tests): enable state is then memory-only.
    explicit TaskService(util::ConfigStore* config = nullptr, TaskLimits limits = {});
    ~TaskService();

    TaskService(const TaskService&) = delete;
    TaskService& operator=(const TaskService&) = delete;

    /// Admit and start a task. Rejects when the plugin is disabled, the
    /// concurrency caps are reached, or the service was shut down.
    SubmitReply submit(const std::string& plugin_id, const std::string& action,
                       bool cancelable, Runner runner,
                       std::optional<std::string> client_request_id = std::nullopt);

    std::optional<TaskRecord> get(const std::string& task_id) const;
    std::vector<TaskRecord> list_for_plugin(const std::string& plugin_id) const;

    /// Register a cancel request. Never writes a terminal state; the cancelled
    /// terminal state is published by finish() after the runner has stopped.
    CancelReply cancel(const std::string& task_id);

    /// Block until the task is terminal and its runner has returned (or was
    /// evicted → nullopt). Used by the legacy synchronous execute entry.
    std::optional<TaskRecord> wait_terminal(const std::string& task_id);

    PluginState plugin_state(const std::string& plugin_id) const;
    bool is_plugin_enabled(const std::string& plugin_id) const;
    int plugin_revision(const std::string& plugin_id) const;

    /// Enable/disable admission for one plugin (persisted to
    /// plugins.<id>.enabled). Disabling closes admission under the same lock and
    /// then requests cancellation for cancellable, non-committing tasks; the
    /// reply lists only the newly requested task ids.
    PluginStateReply set_plugin_enabled(const std::string& plugin_id, bool enabled,
                                        std::optional<int> if_revision = std::nullopt);

    /// Close admission, request cancel on cancellable tasks, wait for every
    /// worker to finish. Idempotent. Terminal records are kept.
    void shutdown_all();
    bool is_stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }
    std::size_t worker_count() const;

private:
    friend struct TaskServiceTestAccess;

    struct PluginStateEntry
    {
        bool enabled{true};
        int revision{0};
    };

    // All record/state mutations (and finish/CAS) use mutex_.
    mutable std::mutex mutex_;
    std::condition_variable finished_cv_;  // task completion and shutdown convergence
    bool shutdown_joining_{false};  // protected by mutex_
    bool shutdown_finished_{false};
    std::atomic<bool> stopped_{false};
    util::ConfigStore* config_{nullptr};
    TaskLimits limits_;

    std::unordered_map<std::string, std::shared_ptr<Task>> tasks_;
    mutable std::unordered_map<std::string, PluginStateEntry> plugin_states_;
    std::size_t terminal_count_{0};
    std::size_t active_global_{0};
    std::unordered_map<std::string, std::size_t> active_per_plugin_;
    std::uint64_t next_sequence_{0};
    struct Worker
    {
        std::shared_ptr<Task> task;
        std::thread thread;
    };
    // Separate from mutex_: worker ownership/start/reaping/shutdown are serialized,
    // but joining must never hold the mutex needed by run_task()/finish().
    mutable std::mutex workers_mutex_;
    std::vector<Worker> workers_;
    // Empty in production: launch the lambda directly with std::thread. Tests
    // inject a factory to cover start failure and shutdown interleaving.
    std::function<std::thread(std::function<void()>)> launch_worker_;

    PluginStateEntry& plugin_state_locked(const std::string& plugin_id) const;
    void reap_completed_workers();  // requires workers_mutex_, never mutex_
    void run_task(std::shared_ptr<Task> task, Runner runner);
    void finish(const std::shared_ptr<Task>& task, TaskState outcome,
                std::optional<nlohmann::json> result, std::string error, bool uncertain);
    bool enter_commit(const std::shared_ptr<Task>& task);
    void set_task_phase(const std::shared_ptr<Task>& task, TaskPhase phase);
    void set_task_uncertain(const std::shared_ptr<Task>& task, bool uncertain);
    void evict_terminal_locked();
    bool persist_enabled(const std::string& plugin_id, bool enabled);
    TaskRecord snapshot_locked(const Task& task) const;
};

}  // namespace kforge::plugin
