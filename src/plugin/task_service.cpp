// Phase 4 task service implementation. See task_service.h and
// docs/PLUGINIZATION_PLAN.md §4 for the cancellation / commit arbitration rules.
#include "plugin/task_service.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <utility>

namespace kforge::plugin
{
namespace
{
using Clock = std::chrono::system_clock;

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}

/// UUID v4 (random) for task ids — the host-generated id, separate from the
/// import operation_id. Uniqueness only needs to hold within the process; a
/// fresh random id per task is enough and needs no storage.
std::string make_task_id()
{
    static std::mt19937_64 generator(std::random_device{}());
    static std::mutex generator_mutex;
    std::uint64_t words[2];
    {
        std::lock_guard lock(generator_mutex);
        words[0] = generator();
        words[1] = generator();
    }
    unsigned char bytes[16];
    for (int i = 0; i < 8; ++i)
    {
        bytes[i] = static_cast<unsigned char>((words[0] >> (i * 8)) & 0xFF);
        bytes[8 + i] = static_cast<unsigned char>((words[1] >> (i * 8)) & 0xFF);
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);  // variant
    char text[37];
    std::snprintf(text, sizeof(text),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
                  bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
                  bytes[14], bytes[15]);
    return text;
}

std::string plugin_enabled_key(const std::string& plugin_id)
{
    return "plugins." + plugin_id + ".enabled";
}
}  // namespace

struct TaskService::Task
{
    TaskRecord record;
    std::atomic<bool> cancel_flag{false};
    std::atomic<bool> uncertain_hint{false};
    std::atomic<int> revision{0};
    std::atomic<bool> worker_completed{false};
    bool initial_cancelable{false};
    std::uint64_t sequence{0};
};

const char* task_state_name(TaskState state)
{
    switch (state)
    {
        case TaskState::Queued:
            return "queued";
        case TaskState::Running:
            return "running";
        case TaskState::Succeeded:
            return "succeeded";
        case TaskState::Failed:
            return "failed";
        case TaskState::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

const char* task_phase_name(TaskPhase phase)
{
    switch (phase)
    {
        case TaskPhase::None:
            return "";
        case TaskPhase::Queued:
            return "queued";
        case TaskPhase::Staging:
            return "staging";
        case TaskPhase::Committing:
            return "committing";
    }
    return "";
}

bool task_state_is_terminal(TaskState state)
{
    return state == TaskState::Succeeded || state == TaskState::Failed ||
           state == TaskState::Cancelled;
}

const char* cancel_status_name(CancelStatus status)
{
    switch (status)
    {
        case CancelStatus::CancelRequested:
            return "cancel_requested";
        case CancelStatus::AlreadyRequested:
            return "already_requested";
        case CancelStatus::NotCancelable:
            return "not_cancelable";
        case CancelStatus::Terminal:
            return "terminal";
        case CancelStatus::NotFound:
            return "not_found";
    }
    return "not_found";
}

// ------------------------------------------------------------
// Context
// ------------------------------------------------------------

TaskService::Context::Context(TaskService* service, std::shared_ptr<Task> task)
    : service_(service), task_(std::move(task))
{
}

bool TaskService::Context::cancelled() const noexcept
{
    return task_->cancel_flag.load(std::memory_order_acquire) ||
           service_->stopped_.load(std::memory_order_acquire);
}

bool TaskService::Context::cancel_requested() const noexcept
{
    return task_->cancel_flag.load(std::memory_order_acquire);
}

bool TaskService::Context::try_enter_commit()
{
    return service_->enter_commit(task_);
}

void TaskService::Context::set_phase(TaskPhase phase)
{
    service_->set_task_phase(task_, phase);
}

void TaskService::Context::set_uncertain(bool uncertain)
{
    task_->uncertain_hint.store(uncertain, std::memory_order_release);
}

int TaskService::Context::revision() const noexcept
{
    return task_->revision.load(std::memory_order_relaxed);
}

const std::string& TaskService::Context::plugin_id() const noexcept
{
    return task_->record.plugin_id;
}

const std::string& TaskService::Context::action() const noexcept
{
    return task_->record.action;
}

// ------------------------------------------------------------
// TaskService
// ------------------------------------------------------------

TaskService::TaskService(util::ConfigStore* config, TaskLimits limits)
    : config_(config), limits_(limits)
{
    if (limits_.max_retained == 0)
        limits_.max_retained = 1;  // keep at least the just-finished record
    if (limits_.max_global_concurrent == 0)
        limits_.max_global_concurrent = 1;
    if (limits_.max_per_plugin_concurrent == 0)
        limits_.max_per_plugin_concurrent = 1;
}

TaskService::~TaskService()
{
    shutdown_all();
}

TaskService::PluginStateEntry& TaskService::plugin_state_locked(const std::string& plugin_id) const
{
    auto [entry, inserted] = plugin_states_.try_emplace(plugin_id);
    if (inserted && config_ != nullptr)
    {
        try
        {
            // ConfigStore is the single persisted owner of plugins.<id>.enabled;
            // a missing key means enabled (phase-4 default).
            entry->second.enabled = config_->get_bool(plugin_enabled_key(plugin_id), true);
        }
        catch (const std::exception&)
        {
            entry->second.enabled = true;  // unreadable config must not disable plugins
        }
    }
    return entry->second;
}

bool TaskService::persist_enabled(const std::string& plugin_id, bool enabled)
{
    if (config_ == nullptr)
        return true;  // memory-only (tests): nothing to persist
    try
    {
        config_->set_bool(plugin_enabled_key(plugin_id), enabled);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

SubmitReply TaskService::submit(const std::string& plugin_id, const std::string& action,
                                bool cancelable, Runner runner,
                                std::optional<std::string> client_request_id)
{
    SubmitReply reply;
    if (!runner)
    {
        reply.code = "invalid";
        reply.error = "Task runner is empty";
        return reply;
    }

    // A worker may call submit() while shutdown is joining it. Reject before
    // acquiring workers_mutex_ so this never blocks the worker's own shutdown.
    if (stopped_.load(std::memory_order_acquire))
    {
        reply.code = "stopped";
        reply.error = "Task service is stopped";
        return reply;
    }

    std::lock_guard workers_lock(workers_mutex_);
    reap_completed_workers();

    std::shared_ptr<Task> task;
    {
        std::lock_guard lock(mutex_);
        if (stopped_.load(std::memory_order_acquire))
        {
            reply.code = "stopped";
            reply.error = "Task service is stopped";
            return reply;
        }
        const auto& state = plugin_state_locked(plugin_id);
        if (!state.enabled)
        {
            reply.code = "disabled";
            reply.error = "Plugin is disabled: " + plugin_id;
            return reply;
        }
        if (active_global_ >= limits_.max_global_concurrent ||
            workers_.size() >= limits_.max_global_concurrent)
        {
            reply.code = "busy";
            reply.error = "Too many concurrent tasks";
            return reply;
        }
        if (active_per_plugin_[plugin_id] >= limits_.max_per_plugin_concurrent)
        {
            reply.code = "busy";
            reply.error = "Too many concurrent tasks for plugin " + plugin_id;
            return reply;
        }

        task = std::make_shared<Task>();
        task->record.task_id = make_task_id();
        task->record.plugin_id = plugin_id;
        task->record.action = action;
        task->record.state = TaskState::Queued;
        task->record.phase = TaskPhase::Queued;
        task->record.cancelable = cancelable;
        task->record.revision = state.revision;
        task->revision.store(state.revision, std::memory_order_relaxed);
        task->record.created_at_ms = now_ms();
        task->record.client_request_id = std::move(client_request_id);
        task->initial_cancelable = cancelable;
        task->sequence = next_sequence_++;

        // Allocate the ownership slot before starting a thread. No vector growth
        // or throwing thread move can leave a joinable local thread unowned.
        workers_.push_back(Worker{task, {}});
        try
        {
            tasks_.emplace(task->record.task_id, task);
        }
        catch (...)
        {
            workers_.pop_back();
            throw;
        }
        ++active_global_;
        ++active_per_plugin_[plugin_id];

        auto rollback_start = [&]
        {
            tasks_.erase(task->record.task_id);
            --active_global_;
            if (--active_per_plugin_[plugin_id] == 0)
                active_per_plugin_.erase(plugin_id);
            workers_.pop_back();
        };
        try
        {
            auto body = [this, task, runner = std::move(runner)]() mutable
            {
                run_task(task, std::move(runner));
                // Publish only after run_task and its runner captures have been
                // destroyed; a reaper can now join without blocking task logic.
                {
                    std::lock_guard lock(mutex_);
                    task->worker_completed.store(true, std::memory_order_release);
                }
                finished_cv_.notify_all();
            };
            workers_.back().thread = launch_worker_ ? launch_worker_(std::move(body))
                                                   : std::thread(std::move(body));
        }
        catch (const std::exception& error)
        {
            rollback_start();
            reply.code = "start_failed";
            reply.error = std::string("Cannot start task worker: ") + error.what();
            return reply;
        }
        catch (...)
        {
            rollback_start();
            reply.code = "start_failed";
            reply.error = "Cannot start task worker";
            return reply;
        }

        reply.ok = true;
        reply.task_id = task->record.task_id;
        reply.state = TaskState::Queued;
    }

    return reply;
}

std::size_t TaskService::worker_count() const
{
    std::lock_guard lock(workers_mutex_);
    return workers_.size();
}

void TaskService::reap_completed_workers()
{
    for (auto worker = workers_.begin(); worker != workers_.end();)
    {
        if (!worker->task->worker_completed.load(std::memory_order_acquire))
        {
            ++worker;
            continue;
        }
        if (worker->thread.joinable())
            worker->thread.join();
        worker = workers_.erase(worker);
    }
}

void TaskService::run_task(std::shared_ptr<Task> task, Runner runner)
{
    {
        std::lock_guard lock(mutex_);
        if (!task_state_is_terminal(task->record.state))
        {
            task->record.state = TaskState::Running;
            task->record.started_at_ms = now_ms();
            task->record.phase = TaskPhase::None;
        }
    }

    Context context(this, task);
    util::Result<nlohmann::json> result =
        std::unexpected(util::Error::make<util::Error::Kind::PluginError>("Task did not run"));
    try
    {
        result = runner(context);
    }
    catch (const std::exception& error)
    {
        result = std::unexpected(util::Error::make<util::Error::Kind::PluginError>(
            std::string("Task runner threw: ") + error.what()));
    }
    catch (...)
    {
        result = std::unexpected(util::Error::make<util::Error::Kind::PluginError>(
            "Task runner threw an unknown exception"));
    }

    if (result)
    {
        nlohmann::json value = std::move(*result);
        std::string business_error;
        bool business_ok = true;
        if (value.is_object() && value.contains("ok") && value["ok"].is_boolean())
        {
            business_ok = value["ok"].get<bool>();
            if (!business_ok)
            {
                business_error = value.contains("error") && value["error"].is_string()
                                     ? value["error"].get<std::string>()
                                     : "Plugin reported ok:false";
            }
        }
        if (business_ok)
            finish(task, TaskState::Succeeded, std::move(value), std::string(), false);
        else
            finish(task, TaskState::Failed, std::move(value), business_error, false);
    }
    else
        finish(task, TaskState::Failed, std::nullopt, result.error().format_message(),
               task->uncertain_hint.load(std::memory_order_acquire));
}

void TaskService::finish(const std::shared_ptr<Task>& task, TaskState outcome,
                         std::optional<nlohmann::json> result, std::string error, bool uncertain)
{
    {
        std::lock_guard lock(mutex_);
        if (task_state_is_terminal(task->record.state))
            return;  // terminal is written exactly once
        // A registered cancel request always wins over the runner's outcome: the
        // cancelled terminal is published only here, after the runner stopped.
        task->record.state =
            task->record.cancel_requested ? TaskState::Cancelled : outcome;
        task->record.result = std::move(result);
        task->record.error = std::move(error);
        task->record.uncertain = task->record.state == TaskState::Failed && uncertain;
        task->record.finished_at_ms = now_ms();

        if (active_global_ > 0)
            --active_global_;
        auto plugin_active = active_per_plugin_.find(task->record.plugin_id);
        if (plugin_active != active_per_plugin_.end())
        {
            if (plugin_active->second > 0)
                --plugin_active->second;
            if (plugin_active->second == 0)
                active_per_plugin_.erase(plugin_active);
        }
        ++terminal_count_;
        evict_terminal_locked();
    }
    finished_cv_.notify_all();
}

void TaskService::evict_terminal_locked()
{
    while (terminal_count_ > limits_.max_retained)
    {
        auto oldest = tasks_.end();
        for (auto it = tasks_.begin(); it != tasks_.end(); ++it)
        {
            if (!task_state_is_terminal(it->second->record.state))
                continue;
            if (oldest == tasks_.end() ||
                it->second->record.finished_at_ms < oldest->second->record.finished_at_ms ||
                (it->second->record.finished_at_ms == oldest->second->record.finished_at_ms &&
                 it->second->sequence < oldest->second->sequence))
            {
                oldest = it;
            }
        }
        if (oldest == tasks_.end())
            break;  // only running tasks left — never evict them
        tasks_.erase(oldest);
        --terminal_count_;
    }
}

bool TaskService::enter_commit(const std::shared_ptr<Task>& task)
{
    std::lock_guard lock(mutex_);
    if (task_state_is_terminal(task->record.state))
        return false;
    // Same lock as requestCancel(): a cancel accepted before this point always
    // wins, so the caller must not start writing.
    if (task->record.cancel_requested || task->cancel_flag.load(std::memory_order_acquire))
        return false;
    task->record.cancelable = false;
    task->record.phase = TaskPhase::Committing;
    return true;
}

void TaskService::set_task_phase(const std::shared_ptr<Task>& task, TaskPhase phase)
{
    std::lock_guard lock(mutex_);
    if (task_state_is_terminal(task->record.state))
        return;
    if (task->record.phase == TaskPhase::Committing && phase != TaskPhase::Committing)
        return;  // never regress out of the commit phase
    task->record.phase = phase;
}

void TaskService::set_task_uncertain(const std::shared_ptr<Task>& task, bool uncertain)
{
    std::lock_guard lock(mutex_);
    task->record.uncertain = uncertain;
}

TaskRecord TaskService::snapshot_locked(const Task& task) const
{
    TaskRecord record = task.record;
    return record;
}

std::optional<TaskRecord> TaskService::get(const std::string& task_id) const
{
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end())
        return std::nullopt;
    return snapshot_locked(*it->second);
}

std::vector<TaskRecord> TaskService::list_for_plugin(const std::string& plugin_id) const
{
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::uint64_t, TaskRecord>> found;
    for (const auto& [id, task] : tasks_)
    {
        if (task->record.plugin_id != plugin_id)
            continue;
        found.emplace_back(task->sequence, snapshot_locked(*task));
    }
    std::sort(found.begin(), found.end(),
              [](const auto& left, const auto& right) { return left.first > right.first; });
    std::vector<TaskRecord> records;
    records.reserve(found.size());
    for (auto& [sequence, record] : found)
        records.push_back(std::move(record));
    return records;
}

CancelReply TaskService::cancel(const std::string& task_id)
{
    std::lock_guard lock(mutex_);
    CancelReply reply;
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end())
    {
        reply.status = CancelStatus::NotFound;
        return reply;
    }
    auto& task = *it->second;
    reply.task = snapshot_locked(task);
    if (task_state_is_terminal(task.record.state))
    {
        reply.status = CancelStatus::Terminal;
        return reply;
    }
    if (!task.record.cancelable || task.record.phase == TaskPhase::Committing)
    {
        reply.status = CancelStatus::NotCancelable;
        return reply;
    }
    if (task.record.cancel_requested)
    {
        reply.status = CancelStatus::AlreadyRequested;
        return reply;
    }
    task.record.cancel_requested = true;
    task.cancel_flag.store(true, std::memory_order_release);
    reply.status = CancelStatus::CancelRequested;
    reply.task = snapshot_locked(task);
    return reply;
}

std::optional<TaskRecord> TaskService::wait_terminal(const std::string& task_id)
{
    std::shared_ptr<Task> task;
    {
        std::lock_guard lock(mutex_);
        const auto it = tasks_.find(task_id);
        if (it == tasks_.end())
            return std::nullopt;
        task = it->second;
        if (task_state_is_terminal(task->record.state) &&
            task->worker_completed.load(std::memory_order_acquire))
            return snapshot_locked(*task);
    }
    std::unique_lock lock(mutex_);
    finished_cv_.wait(lock,
                      [&task]
                      {
                          return task_state_is_terminal(task->record.state) &&
                                 task->worker_completed.load(std::memory_order_acquire);
                      });
    return snapshot_locked(*task);
}

PluginState TaskService::plugin_state(const std::string& plugin_id) const
{
    std::lock_guard lock(mutex_);
    const auto& state = plugin_state_locked(plugin_id);
    return PluginState{state.enabled, state.revision};
}

bool TaskService::is_plugin_enabled(const std::string& plugin_id) const
{
    return plugin_state(plugin_id).enabled;
}

int TaskService::plugin_revision(const std::string& plugin_id) const
{
    return plugin_state(plugin_id).revision;
}

PluginStateReply TaskService::set_plugin_enabled(const std::string& plugin_id, bool enabled,
                                                 std::optional<int> if_revision)
{
    PluginStateReply reply;
    std::lock_guard lock(mutex_);
    if (stopped_.load(std::memory_order_acquire))
    {
        reply.code = "stopped";
        reply.error = "Task service is stopped";
        return reply;
    }
    auto& state = plugin_state_locked(plugin_id);
    if (if_revision.has_value() && *if_revision != state.revision)
    {
        reply.code = "revision_conflict";
        reply.error = "Plugin state revision changed";
        reply.enabled = state.enabled;
        reply.revision = state.revision;
        return reply;
    }

    if (enabled != state.enabled)
    {
        const bool previous = state.enabled;
        state.enabled = enabled;
        ++state.revision;
        if (!persist_enabled(plugin_id, enabled))
        {
            state.enabled = previous;
            --state.revision;
            reply.code = "persist_failed";
            reply.error = "Cannot persist plugin enabled state";
            reply.enabled = state.enabled;
            reply.revision = state.revision;
            return reply;
        }
        if (!enabled)
        {
            // Admission is already closed (the state flip above is under the same
            // lock as submit); now ask cancellable tasks to stop. committing and
            // non-cancelable tasks are left to their real terminal state.
            for (auto& [task_id, task] : tasks_)
            {
                if (task->record.plugin_id != plugin_id)
                    continue;
                if (task_state_is_terminal(task->record.state))
                    continue;
                if (!task->record.cancelable || task->record.phase == TaskPhase::Committing)
                    continue;
                if (task->record.cancel_requested)
                    continue;
                task->record.cancel_requested = true;
                task->cancel_flag.store(true, std::memory_order_release);
                reply.cancel_requested.push_back(task_id);
            }
        }
    }

    reply.ok = true;
    reply.enabled = state.enabled;
    reply.revision = state.revision;
    return reply;
}

void TaskService::shutdown_all()
{
    {
        std::unique_lock lock(mutex_);
        if (shutdown_joining_)
        {
            finished_cv_.wait(lock, [this] { return shutdown_finished_; });
            return;
        }
        if (shutdown_finished_)
            return;
        shutdown_joining_ = true;
        if (!stopped_.exchange(true, std::memory_order_acq_rel))
        {
            for (auto& [task_id, task] : tasks_)
            {
                if (task_state_is_terminal(task->record.state))
                    continue;
                // Global fallback: cancellable tasks end as "cancelled"; writes
                // keep their real outcome and uncertainty after interruption.
                if (task->record.cancelable && task->record.phase != TaskPhase::Committing &&
                    !task->record.cancel_requested)
                {
                    task->record.cancel_requested = true;
                    task->cancel_flag.store(true, std::memory_order_release);
                }
            }
        }
    }

    // Admission cannot hand a late worker to us after this point. Release the
    // ownership lock before joining: an existing runner may already be waiting
    // for this lock inside submit() and must be able to observe stopped_. Other
    // shutdown callers wait for this same join operation, never return early.
    std::vector<Worker> workers;
    {
        std::lock_guard workers_lock(workers_mutex_);
        workers.swap(workers_);
    }
    for (auto& worker : workers)
    {
        if (worker.thread.joinable())
            worker.thread.join();
    }
    {
        std::lock_guard lock(mutex_);
        shutdown_finished_ = true;
        shutdown_joining_ = false;
    }
    finished_cv_.notify_all();
}

}  // namespace kforge::plugin
