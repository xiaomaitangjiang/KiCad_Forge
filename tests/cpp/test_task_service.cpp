// Tests for kforge::plugin::TaskService (phase 4): admission, cancellation /
// commit arbitration, retention, phases, revisions, real-process integration.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/plugin/plugin_manager.h"
#include "../src/plugin/task_service.h"
#include "../src/util/config_store.h"
#include "../support/test_environment.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using kforge::plugin::CancelStatus;
using kforge::plugin::TaskLimits;
using kforge::plugin::TaskPhase;
using kforge::plugin::TaskService;
using kforge::plugin::TaskState;

namespace kforge::plugin
{
struct TaskServiceTestAccess
{
    static std::size_t owned_workers(TaskService& service)
    {
        std::lock_guard lock(service.workers_mutex_);
        return service.workers_.size();
    }

    static void set_launcher(TaskService& service,
                             std::function<std::thread(std::function<void()>)> launcher)
    {
        std::lock_guard lock(service.workers_mutex_);
        service.launch_worker_ = std::move(launcher);
    }

    static std::unique_lock<std::mutex> lock_worker_ownership(TaskService& service)
    {
        return std::unique_lock(service.workers_mutex_);
    }
};
}  // namespace kforge::plugin

namespace
{
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

kforge::util::Error cancelled_error()
{
    return kforge::util::Error::make<kforge::util::Error::Kind::PluginError>("cancelled");
}

template <typename Predicate>
bool wait_until(Predicate predicate, int timeout_ms = 5000)
{
    const auto deadline = Clock::now() + Ms(timeout_ms);
    while (Clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

void write_file(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

bool python_available()
{
#ifdef _WIN32
    return std::system("python -c \"\" >NUL 2>&1") == 0;
#else
    return std::system("python -c \"\" >/dev/null 2>&1") == 0;
#endif
}

kforge::plugin::ExecutionOptions exec_options(int timeout_ms)
{
    kforge::plugin::ExecutionOptions options;
    options.timeout = Ms(timeout_ms);
    options.max_output_bytes = 1u << 20;
    return options;
}

// 安装一个最小插件（manifest + plugin.py），返回插件目录。
fs::path write_python_plugin(const fs::path& scan, const std::string& id,
                             const std::string& script, bool cancelable)
{
    const fs::path dir = scan / id;
    write_file(dir / "manifest.json",
               std::string("{\n  \"id\": \"") + id + "\",\n  \"name\": \"" + id +
                   "\",\n  \"version\": \"1.0.0\",\n  \"entry\": \"plugin.py\",\n"
                   "  \"actions\": [ { \"id\": \"run\", \"name\": \"Run\", " +
                   (cancelable ? "\"cancelable\": true" : "\"cancelable\": false") +
                   " } ]\n}\n");
    write_file(dir / "plugin.py", script);
    return dir;
}

// 父进程输出 JSON 后继续睡 5 秒；后代 0.6 秒后写 marker。取消/停用必须把整棵
// 进程树收回，marker 不出现。
const char* PY_TREE = R"PY(
import json, subprocess, sys, time, os
here = os.path.dirname(os.path.abspath(__file__))
code = "import time\ntime.sleep(0.6)\nopen(%r, 'w').write('leaked')\n" % os.path.join(here, "marker.txt")
subprocess.Popen([sys.executable, "-c", code])
print(json.dumps({"ok": True}))
sys.stdout.flush()
time.sleep(5.0)
)PY";

const char* PY_SLEEP_FOREVER = R"PY(
import time
time.sleep(5.0)
)PY";

struct Fixture
{
    test_support::TempDirectory temp;
    test_support::RestoreDefaultLogger restore;
    Fixture()
    {
        auto logger = std::make_shared<spdlog::logger>(
            "test", std::make_shared<spdlog::sinks::null_sink_mt>());
        logger->set_level(spdlog::level::debug);
        spdlog::set_default_logger(logger);
    }
    fs::path path() const { return temp.path(); }
};

// 阻塞到 release 或取消的 runner（模拟 staging）。
TaskService::Runner blocking_runner(std::atomic<bool>& release)
{
    return [&release](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
    {
        while (!release.load(std::memory_order_acquire) && !ctx.cancelled())
            std::this_thread::sleep_for(5ms);
        if (ctx.cancelled())
            return std::unexpected(cancelled_error());
        return nlohmann::json{{"ok", true}};
    };
}
}  // namespace

TEST_CASE_FIXTURE(Fixture, "T4 lifecycle: submit -> running -> succeeded with result and timestamps")
{
    TaskService tasks;
    auto id = tasks.submit("p1", "run", true,
                           [](TaskService::Context&)
                           {
                               return kforge::util::Result<nlohmann::json>(
                                   nlohmann::json{{"ok", true}, {"value", 7}});
                           });
    REQUIRE(id.ok);
    CHECK_FALSE(id.task_id.empty());
    auto record = tasks.wait_terminal(id.task_id);
    REQUIRE(record.has_value());
    CHECK(record->state == TaskState::Succeeded);
    CHECK(record->plugin_id == "p1");
    CHECK(record->action == "run");
    CHECK(record->revision == 0);
    CHECK(record->created_at_ms > 0);
    CHECK(record->started_at_ms >= record->created_at_ms);
    CHECK(record->finished_at_ms >= record->started_at_ms);
    REQUIRE(record->result.has_value());
    CHECK((*record->result)["value"] == 7);
    CHECK_FALSE(record->uncertain);

    auto fetched = tasks.get(id.task_id);
    REQUIRE(fetched.has_value());
    CHECK(fetched->state == TaskState::Succeeded);
    auto listed = tasks.list_for_plugin("p1");
    REQUIRE(listed.size() == 1u);
    CHECK(listed[0].task_id == id.task_id);
    CHECK(tasks.list_for_plugin("other").empty());
}

TEST_CASE_FIXTURE(Fixture, "T4.1 disabled plugin rejects admission; re-enable allows new tasks only")
{
    TaskService tasks;
    CHECK(tasks.is_plugin_enabled("p1"));
    auto before = tasks.plugin_state("p1");
    CHECK(before.enabled);
    CHECK(before.revision == 0);

    auto disabled = tasks.set_plugin_enabled("p1", false, std::nullopt);
    REQUIRE(disabled.ok);
    CHECK_FALSE(disabled.enabled);
    CHECK(disabled.revision == 1);
    CHECK(disabled.cancel_requested.empty());
    CHECK_FALSE(tasks.is_plugin_enabled("p1"));

    std::atomic<bool> never_released{false};
    auto rejected = tasks.submit("p1", "run", true, blocking_runner(never_released));
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.code == "disabled");

    // 旧同步入口共用同一份准入：TaskService::submit 被拒即旧入口被拒。
    auto enabled = tasks.set_plugin_enabled("p1", true, std::nullopt);
    REQUIRE(enabled.ok);
    CHECK(enabled.revision == 2);
    auto accepted = tasks.submit("p1", "run", true,
                                 [](TaskService::Context&)
                                 {
                                     return kforge::util::Result<nlohmann::json>(
                                         nlohmann::json{{"ok", true}});
                                 });
    CHECK(accepted.ok);
    REQUIRE(tasks.wait_terminal(accepted.task_id).has_value());
}

TEST_CASE_FIXTURE(Fixture, "T4.2 concurrent A/B: cancelling A leaves B untouched")
{
    TaskService tasks;
    std::atomic<bool> release_a{false};
    std::atomic<bool> release_b{false};
    auto a = tasks.submit("pa", "run", true, blocking_runner(release_a));
    auto b = tasks.submit("pb", "run", true, blocking_runner(release_b));
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    REQUIRE(wait_until([&]
                       {
                           auto ra = tasks.get(a.task_id);
                           auto rb = tasks.get(b.task_id);
                           return ra && rb && ra->state == TaskState::Running &&
                                  rb->state == TaskState::Running;
                       }));

    auto cancelled = tasks.cancel(a.task_id);
    CHECK(cancelled.status == CancelStatus::CancelRequested);
    CHECK(cancelled.task.state == TaskState::Running);
    CHECK(cancelled.task.cancel_requested);

    // 取消请求不写终态：终态只由 finish() 发布（runner 收到取消后退出）
    auto a_record = tasks.wait_terminal(a.task_id);
    REQUIRE(a_record.has_value());
    CHECK(a_record->state == TaskState::Cancelled);

    auto b_during = tasks.get(b.task_id);
    REQUIRE(b_during.has_value());
    CHECK(b_during->state == TaskState::Running);
    CHECK_FALSE(b_during->cancel_requested);

    release_b.store(true);
    auto b_record = tasks.wait_terminal(b.task_id);
    REQUIRE(b_record.has_value());
    CHECK(b_record->state == TaskState::Succeeded);
}

TEST_CASE_FIXTURE(Fixture, "T4.3 disable while running cancels cancellable task once; new task after re-enable")
{
    TaskService tasks;
    std::atomic<bool> release{false};
    auto running = tasks.submit("p1", "run", true, blocking_runner(release));
    REQUIRE(running.ok);
    REQUIRE(wait_until([&]
                       {
                           auto record = tasks.get(running.task_id);
                           return record && record->state == TaskState::Running;
                       }));

    // 停用：先关准入，再请求取消；响应不返回 cancelled
    auto off = tasks.set_plugin_enabled("p1", false, std::nullopt);
    REQUIRE(off.ok);
    CHECK_FALSE(off.enabled);
    REQUIRE(off.cancel_requested.size() == 1u);
    CHECK(off.cancel_requested[0] == running.task_id);
    CHECK(tasks.submit("p1", "run", true, blocking_runner(release)).code == "disabled");

    auto terminal = tasks.wait_terminal(running.task_id);
    REQUIRE(terminal.has_value());
    CHECK(terminal->state == TaskState::Cancelled);
    // 终态只写一次：再等/再查仍是 cancelled
    CHECK(tasks.wait_terminal(running.task_id)->state == TaskState::Cancelled);
    CHECK(tasks.get(running.task_id)->state == TaskState::Cancelled);

    auto on = tasks.set_plugin_enabled("p1", true, std::nullopt);
    REQUIRE(on.ok);
    release.store(true);
    auto fresh = tasks.submit("p1", "run", true, blocking_runner(release));
    CHECK(fresh.ok);
    CHECK(tasks.wait_terminal(fresh.task_id)->state == TaskState::Succeeded);
}

TEST_CASE_FIXTURE(Fixture, "T4.6 retention keeps 256 terminal records and never evicts running tasks")
{
    TaskLimits limits;
    limits.max_retained = 3;
    limits.max_global_concurrent = 16;
    limits.max_per_plugin_concurrent = 16;
    TaskService tasks(nullptr, limits);

    std::atomic<bool> release{false};
    auto running = tasks.submit("p1", "slow", true, blocking_runner(release));
    REQUIRE(running.ok);

    std::vector<std::string> ids;
    for (int i = 0; i < 6; ++i)
    {
        auto reply = tasks.submit("p1", "quick", true,
                                  [i](TaskService::Context&)
                                  {
                                      return kforge::util::Result<nlohmann::json>(
                                          nlohmann::json{{"ok", true}, {"i", i}});
                                  });
        REQUIRE(reply.ok);
        REQUIRE(tasks.wait_terminal(reply.task_id).has_value());
        ids.push_back(reply.task_id);
    }

    // 运行中的任务不淘汰
    auto slow = tasks.get(running.task_id);
    REQUIRE(slow.has_value());
    CHECK(slow->state == TaskState::Running);

    auto recent = tasks.list_for_plugin("p1");
    CHECK(recent.size() == 4u);  // 3 terminal + 1 running
    // 最老的终态已被淘汰
    CHECK_FALSE(tasks.get(ids.front()).has_value());
    CHECK(tasks.get(ids.back()).has_value());
    CHECK(tasks.wait_terminal(ids.front()) == std::nullopt);

    release.store(true);
    CHECK(tasks.wait_terminal(running.task_id).has_value());
}

TEST_CASE_FIXTURE(Fixture, "T4.5 failed write action can publish uncertain; cancelable read action does not")
{
    TaskService tasks;
    auto write = tasks.submit(
        "p1", "commit", false,
        [](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
        {
            ctx.set_uncertain(true);  // timeout/interruption: repository state unknown
            return std::unexpected(kforge::util::Error::make<kforge::util::Error::Kind::IoError>(
                "Plugin timed out"));
        });
    REQUIRE(write.ok);
    auto write_record = tasks.wait_terminal(write.task_id);
    REQUIRE(write_record.has_value());
    CHECK(write_record->state == TaskState::Failed);
    CHECK(write_record->uncertain);
    CHECK(write_record->error.find("timed out") != std::string::npos);

    auto read = tasks.submit(
        "p1", "status", true,
        [](TaskService::Context&) -> kforge::util::Result<nlohmann::json>
        {
            return std::unexpected(kforge::util::Error::make<kforge::util::Error::Kind::IoError>(
                "Plugin exited with code 1"));
        });
    REQUIRE(read.ok);
    auto read_record = tasks.wait_terminal(read.task_id);
    REQUIRE(read_record.has_value());
    CHECK(read_record->state == TaskState::Failed);
    CHECK_FALSE(read_record->uncertain);
}

TEST_CASE_FIXTURE(Fixture, "T4.7 phases: generic action has no phase, staged import shows staging -> committing")
{
    TaskService tasks;
    std::atomic<bool> staged_ready{false};
    std::atomic<bool> commit_ready{false};
    std::atomic<bool> release{false};
    auto staged = tasks.submit(
        "p1", "import", true,
        [&](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
        {
            ctx.set_phase(TaskPhase::Staging);
            staged_ready.store(true);
            while (!release.load() && !ctx.cancelled())
                std::this_thread::sleep_for(5ms);
            if (ctx.cancelled())
                return std::unexpected(cancelled_error());
            if (!ctx.try_enter_commit())
                return std::unexpected(cancelled_error());
            ctx.set_phase(TaskPhase::Committing);
            commit_ready.store(true);
            while (!release.load() && !ctx.cancelled())
                std::this_thread::sleep_for(5ms);
            return nlohmann::json{{"ok", true}};
        });
    REQUIRE(staged.ok);
    REQUIRE(wait_until([&] { return staged_ready.load(); }));
    auto staging = tasks.get(staged.task_id);
    REQUIRE(staging.has_value());
    CHECK(staging->state == TaskState::Running);
    CHECK(staging->phase == TaskPhase::Staging);
    CHECK(staging->cancelable);

    release.store(true);
    REQUIRE(wait_until([&] { return commit_ready.load(); }));
    auto committing = tasks.get(staged.task_id);
    REQUIRE(committing.has_value());
    CHECK(committing->phase == TaskPhase::Committing);
    CHECK_FALSE(committing->cancelable);

    REQUIRE(tasks.wait_terminal(staged.task_id).has_value());
    CHECK(tasks.get(staged.task_id)->state == TaskState::Succeeded);

    // 普通动作：running 期间 phase 保持 None
    std::atomic<bool> started{false};
    std::atomic<bool> release_generic{false};
    auto generic = tasks.submit(
        "p1", "run", true,
        [&](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
        {
            started.store(true);
            while (!release_generic.load() && !ctx.cancelled())
                std::this_thread::sleep_for(5ms);
            return nlohmann::json{{"ok", true}};
        });
    REQUIRE(generic.ok);
    REQUIRE(wait_until([&] { return started.load(); }));
    auto running = tasks.get(generic.task_id);
    REQUIRE(running.has_value());
    CHECK(running->state == TaskState::Running);
    CHECK(running->phase == TaskPhase::None);
    release_generic.store(true);
    CHECK(tasks.wait_terminal(generic.task_id).has_value());
}

TEST_CASE_FIXTURE(Fixture, "T4.8a cancel before commit: tryEnterCommit=false, task cancelled")
{
    TaskService tasks;
    std::atomic<bool> staging{false};
    std::atomic<bool> gate_result{true};
    std::atomic<bool> release{false};
    auto task = tasks.submit(
        "p1", "import", true,
        [&](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
        {
            ctx.set_phase(TaskPhase::Staging);
            staging.store(true);
            while (!release.load() && !ctx.cancelled())
                std::this_thread::sleep_for(5ms);
            gate_result.store(ctx.try_enter_commit());
            if (!gate_result.load())
                return std::unexpected(cancelled_error());
            return nlohmann::json{{"ok", true}, {"committed", true}};
        });
    REQUIRE(task.ok);
    REQUIRE(wait_until([&] { return staging.load(); }));
    auto cancel = tasks.cancel(task.task_id);
    CHECK(cancel.status == CancelStatus::CancelRequested);
    release.store(true);
    auto record = tasks.wait_terminal(task.task_id);
    REQUIRE(record.has_value());
    CHECK_FALSE(gate_result.load());
    CHECK(record->state == TaskState::Cancelled);
    // 取消先赢：不产生提交结果
    CHECK_FALSE(record->result.has_value());
}

TEST_CASE_FIXTURE(Fixture, "T4.8b commit before cancel: cancel returns not_cancelable, task reaches real result")
{
    TaskService tasks;
    std::atomic<bool> commit_entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> gate_result{false};
    auto task = tasks.submit(
        "p1", "import", true,
        [&](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
        {
            gate_result.store(ctx.try_enter_commit());
            ctx.set_phase(TaskPhase::Committing);
            commit_entered.store(true);
            while (!release.load())
                std::this_thread::sleep_for(5ms);
            return nlohmann::json{{"ok", true}, {"committed", true}};
        });
    REQUIRE(task.ok);
    REQUIRE(wait_until([&] { return commit_entered.load(); }));
    CHECK(gate_result.load());
    auto cancel = tasks.cancel(task.task_id);
    CHECK(cancel.status == CancelStatus::NotCancelable);
    CHECK_FALSE(cancel.task.cancel_requested);
    release.store(true);
    auto record = tasks.wait_terminal(task.task_id);
    REQUIRE(record.has_value());
    CHECK(record->state == TaskState::Succeeded);
    REQUIRE(record->result.has_value());
    CHECK(record->result->value("committed", false));
}

TEST_CASE_FIXTURE(Fixture, "T4.9 revision: CAS conflicts return current state; no-op toggles do not bump")
{
    TaskService tasks;
    auto initial = tasks.plugin_state("p1");
    CHECK(initial.enabled);
    CHECK(initial.revision == 0);

    auto off = tasks.set_plugin_enabled("p1", false, 0);
    REQUIRE(off.ok);
    CHECK(off.revision == 1);

    // 旧 revision 的客户端不能覆盖新状态
    auto stale = tasks.set_plugin_enabled("p1", true, 0);
    CHECK_FALSE(stale.ok);
    CHECK(stale.code == "revision_conflict");
    CHECK(stale.revision == 1);
    CHECK_FALSE(stale.enabled);
    CHECK(tasks.is_plugin_enabled("p1") == false);

    // 相同值的成功请求不递增 revision
    auto same = tasks.set_plugin_enabled("p1", false, 1);
    REQUIRE(same.ok);
    CHECK(same.revision == 1);

    auto on = tasks.set_plugin_enabled("p1", true, 1);
    REQUIRE(on.ok);
    CHECK(on.revision == 2);
    CHECK(tasks.is_plugin_enabled("p1"));
}

TEST_CASE_FIXTURE(Fixture, "T4.9 enable state persists through ConfigStore (no second store)")
{
    const fs::path config_path = path() / "config.json";
    {
        kforge::util::ConfigStore config(config_path);
        TaskService tasks(&config);
        auto off = tasks.set_plugin_enabled("persisted.plugin", false, std::nullopt);
        REQUIRE(off.ok);
        CHECK_FALSE(tasks.is_plugin_enabled("persisted.plugin"));
        CHECK(config.get_bool("plugins.persisted.plugin.enabled", true) == false);
    }
    kforge::util::ConfigStore reloaded(config_path);
    TaskService tasks_after(&reloaded);
    CHECK_FALSE(tasks_after.is_plugin_enabled("persisted.plugin"));
    auto rejected = tasks_after.submit("persisted.plugin", "run", true,
                                       [](TaskService::Context&)
                                       {
                                           return kforge::util::Result<nlohmann::json>(
                                               nlohmann::json{{"ok", true}});
                                       });
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.code == "disabled");
    // 其他插件默认启用
    CHECK(tasks_after.is_plugin_enabled("other.plugin"));
}

TEST_CASE_FIXTURE(Fixture, "T4.10 staged import task semantics: host failure is failed, not succeeded")
{
    TaskService tasks;
    // 插件暂存成功（ok 输出）但宿主提交失败 → runner 返回 error；任务必须 failed。
    auto failed = tasks.submit(
        "p1", "import", false,
        [](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
        {
            ctx.set_phase(TaskPhase::Staging);
            if (!ctx.try_enter_commit())
                return std::unexpected(cancelled_error());
            ctx.set_phase(TaskPhase::Committing);
            return std::unexpected(kforge::util::Error::make<kforge::util::Error::Kind::ValidationError>(
                "Merged library does not parse"));
        });
    REQUIRE(failed.ok);
    auto record = tasks.wait_terminal(failed.task_id);
    REQUIRE(record.has_value());
    CHECK(record->state == TaskState::Failed);
    CHECK(record->error.find("does not parse") != std::string::npos);

    // 只有宿主提交完成（runner 成功返回）才是 succeeded。
    auto succeeded = tasks.submit(
        "p1", "import", false,
        [](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
        {
            if (!ctx.try_enter_commit())
                return std::unexpected(cancelled_error());
            return nlohmann::json{{"ok", true}, {"operation_id", "op-1"}};
        });
    REQUIRE(succeeded.ok);
    auto ok_record = tasks.wait_terminal(succeeded.task_id);
    REQUIRE(ok_record.has_value());
    CHECK(ok_record->state == TaskState::Succeeded);
    REQUIRE(ok_record->result.has_value());
    CHECK(ok_record->result->value("operation_id", "") == "op-1");
}

TEST_CASE_FIXTURE(Fixture, "task/cancel lookups for unknown and evicted ids are explicit not-found")
{
    TaskLimits limits;
    limits.max_retained = 1;
    TaskService tasks(nullptr, limits);
    CHECK_FALSE(tasks.get("missing").has_value());
    CHECK(tasks.cancel("missing").status == CancelStatus::NotFound);
    CHECK(tasks.wait_terminal("missing") == std::nullopt);

    auto first = tasks.submit("p1", "run", true,
                              [](TaskService::Context&)
                              {
                                  return kforge::util::Result<nlohmann::json>(
                                      nlohmann::json{{"ok", true}});
                              });
    REQUIRE(first.ok);
    REQUIRE(tasks.wait_terminal(first.task_id).has_value());
    auto second = tasks.submit("p1", "run", true,
                               [](TaskService::Context&)
                               {
                                   return kforge::util::Result<nlohmann::json>(
                                       nlohmann::json{{"ok", true}});
                               });
    REQUIRE(second.ok);
    REQUIRE(tasks.wait_terminal(second.task_id).has_value());
    CHECK_FALSE(tasks.get(first.task_id).has_value());
    CHECK(tasks.cancel(first.task_id).status == CancelStatus::NotFound);
    auto terminal = tasks.wait_terminal(second.task_id);
    REQUIRE(terminal.has_value());
    CHECK(terminal->state == TaskState::Succeeded);
    CHECK(tasks.cancel(second.task_id).status == CancelStatus::Terminal);
}

TEST_CASE_FIXTURE(Fixture, "T4.4 real process tree is reclaimed when a task is cancelled")
{
    if (!python_available())
    {
        WARN("python not available — real process cancellation skipped");
        return;
    }
    const auto scan = path() / "plugins";
    const auto dir = write_python_plugin(scan, "tree", PY_TREE, true);
    kforge::plugin::PluginManager plugins({scan}, exec_options(120000));
    plugins.discover();
    TaskService tasks;
    auto task = tasks.submit(
        "tree", "run", true,
        [&plugins](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
        {
            auto output = plugins.execute("tree", "run", "{}",
                                          [&ctx] { return ctx.cancelled(); });
            if (!output)
                return std::unexpected(output.error());
            try
            {
                return nlohmann::json::parse(*output);
            }
            catch (const std::exception& error)
            {
                return std::unexpected(kforge::util::Error::make<
                                       kforge::util::Error::Kind::ParseError>(error.what()));
            }
        });
    REQUIRE(task.ok);
    REQUIRE(wait_until([&]
                       {
                           auto record = tasks.get(task.task_id);
                           return record && record->state == TaskState::Running;
                       }));
    std::this_thread::sleep_for(200ms);  // 让父/子进程真正起来
    auto cancel = tasks.cancel(task.task_id);
    CHECK(cancel.status == CancelStatus::CancelRequested);
    const auto cancel_start = Clock::now();
    auto record = tasks.wait_terminal(task.task_id);
    REQUIRE(record.has_value());
    CHECK(record->state == TaskState::Cancelled);
    // runner 必须有界：不能等脚本 5 秒自限
    CHECK(std::chrono::duration_cast<Ms>(Clock::now() - cancel_start).count() < 4000);
    std::this_thread::sleep_for(1000ms);
    CHECK_FALSE(fs::exists(dir / "marker.txt"));  // 后代也被进程树回收
    plugins.shutdown_all();
}

TEST_CASE_FIXTURE(Fixture, "T4.5 real process timeout becomes failed+uncertain for a write action")
{
    if (!python_available())
    {
        WARN("python not available — real timeout task skipped");
        return;
    }
    const auto scan = path() / "plugins";
    write_python_plugin(scan, "slow", PY_SLEEP_FOREVER, false);
    kforge::plugin::PluginManager plugins({scan}, exec_options(300));
    plugins.discover();
    TaskService tasks;
    auto task = tasks.submit(
        "slow", "run", false,
        [&plugins](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
        {
            auto output = plugins.execute("slow", "run", "{}",
                                          [&ctx] { return ctx.cancelled(); });
            if (!output)
            {
                const std::string message = output.error().message();
                const bool interrupted = message.find("timed out") != std::string::npos ||
                                         message.find("exceeded") != std::string::npos ||
                                         message.find("cancelled") != std::string::npos;
                if (interrupted && !ctx.cancel_requested())
                    ctx.set_uncertain(true);
                return std::unexpected(output.error());
            }
            return nlohmann::json::parse(*output);
        });
    REQUIRE(task.ok);
    const auto start = Clock::now();
    auto record = tasks.wait_terminal(task.task_id);
    REQUIRE(record.has_value());
    CHECK(std::chrono::duration_cast<Ms>(Clock::now() - start).count() < 5000);
    CHECK(record->state == TaskState::Failed);
    CHECK(record->uncertain);
    CHECK(record->error.find("timed out") != std::string::npos);
    plugins.shutdown_all();
}

TEST_CASE_FIXTURE(Fixture, "T4.11 business ok:false is failed but preserves plugin result")
{
    TaskService tasks;
    auto reply = tasks.submit("business", "run", true,
                              [](TaskService::Context&) -> kforge::util::Result<nlohmann::json>
                              {
                                  return nlohmann::json{{"ok", false}, {"error", "business rejected"}};
                              });
    REQUIRE(reply.ok);
    auto record = tasks.wait_terminal(reply.task_id);
    REQUIRE(record.has_value());
    CHECK(record->state == TaskState::Failed);
    CHECK(record->error == "business rejected");
    REQUIRE(record->result.has_value());
    CHECK(record->result->value("ok", true) == false);
}

TEST_CASE_FIXTURE(Fixture, "T4.12 shutdown joins owned workers deterministically")
{
    auto tasks = std::make_unique<TaskService>();
    std::atomic<bool> release{false};
    for (int index = 0; index < 4; ++index)
    {
        auto reply = tasks->submit("join", "run", true, blocking_runner(release));
        REQUIRE(reply.ok);
    }
    const auto started = Clock::now();
    tasks->shutdown_all();
    CHECK(std::chrono::duration_cast<Ms>(Clock::now() - started).count() < 5000);
    const auto after = tasks->list_for_plugin("join");
    for (const auto& record : after)
        CHECK(record.state == TaskState::Cancelled);
    tasks.reset();
}

TEST_CASE_FIXTURE(Fixture, "completed workers are reclaimed during normal task admission")
{
    TaskLimits limits;
    limits.max_retained = 4;
    limits.max_global_concurrent = 4;
    TaskService tasks(nullptr, limits);
    for (int index = 0; index < 200; ++index)
    {
        auto reply = tasks.submit("short", "run", true,
                                  [](TaskService::Context&) -> kforge::util::Result<nlohmann::json>
                                  {
                                      return nlohmann::json{{"ok", true}};
                                  });
        REQUIRE(reply.ok);
        REQUIRE(tasks.wait_terminal(reply.task_id).has_value());
    }
    CHECK(tasks.list_for_plugin("short").size() == limits.max_retained);
    CHECK(kforge::plugin::TaskServiceTestAccess::owned_workers(tasks) <=
          limits.max_global_concurrent);
    tasks.shutdown_all();
    CHECK(kforge::plugin::TaskServiceTestAccess::owned_workers(tasks) == 0);
}

TEST_CASE_FIXTURE(Fixture, "thread start failure rolls back task registration and admission counters")
{
    TaskLimits limits;
    limits.max_global_concurrent = 1;
    limits.max_per_plugin_concurrent = 1;
    TaskService tasks(nullptr, limits);
    bool fail_start = true;
    kforge::plugin::TaskServiceTestAccess::set_launcher(tasks,
        [&fail_start](std::function<void()> body)
        {
            if (std::exchange(fail_start, false))
                throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            return std::thread(std::move(body));
        });
    std::atomic<int> runs{0};
    auto runner = [&runs](TaskService::Context&) -> kforge::util::Result<nlohmann::json>
    {
        ++runs;
        return nlohmann::json{{"ok", true}};
    };
    auto rejected = tasks.submit("start", "run", true, runner);
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.code == "start_failed");
    CHECK(rejected.task_id.empty());
    CHECK(tasks.list_for_plugin("start").empty());
    CHECK(kforge::plugin::TaskServiceTestAccess::owned_workers(tasks) == 0);
    CHECK(runs == 0);

    auto accepted = tasks.submit("start", "run", true, runner);
    REQUIRE(accepted.ok);
    REQUIRE(tasks.wait_terminal(accepted.task_id).has_value());
    tasks.shutdown_all();
    CHECK(runs == 1);
}

TEST_CASE_FIXTURE(Fixture, "concurrent shutdown waits for an in-flight worker registration and real exit")
{
    TaskService tasks;
    std::atomic<bool> launch_entered{false};
    std::atomic<bool> release_launch{false};
    std::atomic<bool> body_completed{false};
    std::atomic<bool> release_exit{false};
    std::atomic<bool> exited{false};
    std::atomic<bool> shutdown_a_done{false};
    std::atomic<bool> shutdown_b_done{false};
    kforge::plugin::TaskServiceTestAccess::set_launcher(tasks,
        [&](std::function<void()> body)
        {
            launch_entered.store(true);
            while (!release_launch.load())
                std::this_thread::yield();
            return std::thread([&, body = std::move(body)]
            {
                body();
                body_completed.store(true);
                while (!release_exit.load())
                    std::this_thread::yield();
                exited.store(true);
            });
        });
    kforge::plugin::SubmitReply admitted;
    std::thread submitter([&]
    {
        admitted = tasks.submit("race", "run", true,
            [](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
            {
                while (!ctx.cancelled())
                    std::this_thread::yield();
                return std::unexpected(cancelled_error());
            });
    });
    CHECK(wait_until([&] { return launch_entered.load(); }));
    std::thread shutdown_a([&] { tasks.shutdown_all(); shutdown_a_done.store(true); });
    std::thread shutdown_b([&] { tasks.shutdown_all(); shutdown_b_done.store(true); });
    release_launch.store(true);
    CHECK(wait_until([&] { return body_completed.load(); }));
    CHECK_FALSE(shutdown_a_done.load());
    CHECK_FALSE(shutdown_b_done.load());
    release_exit.store(true);
    submitter.join();
    shutdown_a.join();
    shutdown_b.join();
    CHECK(admitted.ok);
    CHECK(exited.load());
    CHECK(shutdown_a_done.load());
    CHECK(shutdown_b_done.load());
    CHECK(kforge::plugin::TaskServiceTestAccess::owned_workers(tasks) == 0);
}

TEST_CASE_FIXTURE(Fixture, "runner can submit during shutdown without waiting for its own join")
{
    TaskService tasks;
    std::atomic<bool> entered{false};
    std::atomic<bool> nested_rejected{false};
    auto reply = tasks.submit("reentrant", "run", true,
        [&](TaskService::Context& ctx) -> kforge::util::Result<nlohmann::json>
        {
            entered.store(true);
            while (!ctx.cancelled())
                std::this_thread::yield();
            auto nested = tasks.submit("reentrant", "nested", true,
                [](TaskService::Context&) -> kforge::util::Result<nlohmann::json>
                {
                    return nlohmann::json{{"ok", true}};
                });
            nested_rejected.store(!nested.ok && nested.code == "stopped");
            return std::unexpected(cancelled_error());
        });
    REQUIRE(reply.ok);
    CHECK(wait_until([&] { return entered.load(); }));
    tasks.shutdown_all();
    CHECK(nested_rejected.load());
}

TEST_CASE_FIXTURE(Fixture, "shutdown releases worker ownership while a runner is already waiting to submit")
{
    TaskService tasks;
    std::atomic<bool> enter_submit{false};
    std::atomic<bool> nested_started{false};
    std::atomic<bool> nested_finished{false};
    std::atomic<bool> nested_rejected{false};
    auto reply = tasks.submit("blocked", "run", true,
        [&](TaskService::Context&) -> kforge::util::Result<nlohmann::json>
        {
            while (!enter_submit.load())
                std::this_thread::yield();
            nested_started.store(true);
            auto nested = tasks.submit("blocked", "nested", true,
                [](TaskService::Context&) -> kforge::util::Result<nlohmann::json>
                {
                    return nlohmann::json{{"ok", true}};
                });
            nested_rejected.store(!nested.ok && nested.code == "stopped");
            nested_finished.store(true);
            return nlohmann::json{{"ok", true}};
        });
    REQUIRE(reply.ok);
    auto ownership_lock = kforge::plugin::TaskServiceTestAccess::lock_worker_ownership(tasks);
    enter_submit.store(true);
    CHECK(wait_until([&] { return nested_started.load(); }));
    std::this_thread::sleep_for(30ms);  // runner is blocked at the ownership lock
    CHECK_FALSE(nested_finished.load());
    std::thread shutdown([&] { tasks.shutdown_all(); });
    CHECK(wait_until([&] { return tasks.is_stopped(); }));
    ownership_lock.unlock();
    shutdown.join();
    CHECK(nested_finished.load());
    CHECK(nested_rejected.load());
}

TEST_CASE_FIXTURE(Fixture, "T4.13 completed workers are reaped while the service stays alive")
{
    TaskLimits limits;
    limits.max_retained = 4;
    TaskService tasks(nullptr, limits);
    for (int index = 0; index < 120; ++index)
    {
        auto reply = tasks.submit("reap", "quick", true,
                                  [](TaskService::Context&)
                                  {
                                      return nlohmann::json{{"ok", true}, {"n", 1}};
                                  });
        REQUIRE(reply.ok);
        auto record = tasks.wait_terminal(reply.task_id);
        REQUIRE(record.has_value());
        CHECK(record->state == TaskState::Succeeded);
    }
    CHECK(tasks.worker_count() <= 4);
    tasks.shutdown_all();
    CHECK(tasks.worker_count() == 0);
}