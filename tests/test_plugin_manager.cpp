// Tests for kforge::plugin::PluginManager: discovery, load/unload,
// shutdown idempotency, multi-path merge, and the create_default() factory.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/plugin/plugin_manager.h"
#include "../src/util/platform.h"
#include "support/test_environment.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace
{
void write_file(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p);  // closed when this function returns
    f << content;
}

const char* MANIFEST_HELLO = R"({
  "id": "hello",
  "name": "Hello Test",
  "version": "1.0.0",
  "entry": "plugin.py",
  "one_click": true,
  "actions": [ { "id": "ping", "name": "Ping", "trigger": "inline" } ]
})";

const char* MANIFEST_BAD = R"({ "name": "No ID Here" })";

const char* MANIFEST_AUTO = R"({
  "id": "auto",
  "name": "Auto Test",
  "version": "0.1.0",
  "one_click": false
})";

// ---- FIX05：真实 python 执行的夹具 --------------------------------------
//
// 每个夹具脚本都自带上限（最多 2 秒），即使被测实现有缺陷、测试失败退出，
// 也不会留下一个永远运行的 python 进程。

bool python_available()
{
#ifdef _WIN32
    return std::system("python -c \"\" >NUL 2>&1") == 0;
#else
    return std::system("python -c \"\" >/dev/null 2>&1") == 0;
#endif
}

kforge::plugin::ExecutionOptions exec_options(int timeout_ms, std::size_t max_output)
{
    kforge::plugin::ExecutionOptions options;
    options.timeout = std::chrono::milliseconds(timeout_ms);
    options.max_output_bytes = max_output;
    return options;
}

/// 写一个最小可执行插件（manifest.json + plugin.py）。脚本按字节原样落盘，
/// 保证中文内容就是 UTF-8，不受控制台代码页影响。
void write_exec_plugin(const fs::path& scan, const std::string& id, const std::string& name,
                       const std::string& script)
{
    const fs::path dir = scan / id;
    fs::create_directories(dir);
    write_file(dir / "manifest.json",
               std::string("{\n  \"id\": \"") + id + "\",\n  \"name\": \"" + name +
                   "\",\n  \"version\": \"1.0.0\",\n  \"entry\": \"plugin.py\",\n"
                   "  \"actions\": [ { \"id\": \"ping\", \"name\": \"Ping\", \"trigger\": \"inline\" } ]\n}\n");
    std::ofstream out(dir / "plugin.py", std::ios::binary);
    out << script;
}

// 无上限洪泛输出，但自身限时 2 秒
const char* PY_FLOOD = R"PY(
import sys, time
end = time.time() + 2.0
chunk = "x" * 4096
while time.time() < end:
    sys.stdout.write(chunk)
    sys.stdout.flush()
)PY";

// 完全无输出地睡眠 2 秒
const char* PY_SILENT_SLEEP = R"PY(
import time
time.sleep(2.0)
print('{"ok": true}')
)PY";

// 关掉 stdout/stderr（制造 EOF）后继续睡眠：EOF 不等于进程退出
const char* PY_CLOSE_STREAMS_SLEEP = R"PY(
import os, time
os.close(1)
os.close(2)
time.sleep(2.0)
)PY";

// 输出 64 KiB —— 小上限下必须先报“超限”，而不是静默截断
const char* PY_BIG_OUTPUT = R"PY(
import sys
sys.stdout.write("y" * 65536)
sys.stdout.flush()
)PY";

// 正常输出：回显 action 与 argv 里的 JSON（纯 ASCII 输出，避免控制台编码噪音）
const char* PY_ECHO_JSON = R"PY(
import json, sys
action = sys.argv[1] if len(sys.argv) > 1 else ""
raw = sys.argv[2] if len(sys.argv) > 2 else "{}"
raw = raw.encode(sys.getfilesystemencoding(), "surrogateescape").decode("utf-8", "replace")
print(json.dumps({"ok": True, "action": action, "args": raw}, ensure_ascii=True))
)PY";

// 输出合法 JSON 但 exit code != 0：绝不能当成成功
const char* PY_JSON_NONZERO = R"PY(
import json, sys
print(json.dumps({"ok": True}))
sys.stdout.flush()
sys.exit(3)
)PY";

// 非零退出且把原因写在 stderr：失败信息必须带上这段输出，调用层才能区分
// “脚本自身超时”与普通非零退出。
const char* PY_FAIL_DETAIL = R"PY(
import sys
sys.stderr.write("subprocess.TimeoutExpired: git commit timed out after 30 seconds\n")
sys.stderr.flush()
sys.exit(1)
)PY";

// 超长失败输出只保留尾部：错误信息有界，但末尾原因不能丢。
const char* PY_FAIL_LONG = R"PY(
import sys
sys.stderr.write("x" * 5000 + " TAIL-MARKER\n")
sys.stderr.flush()
sys.exit(1)
)PY";

// 慢执行：1.5 秒后输出 JSON（自限 2 秒）
const char* PY_SLOW_JSON = R"PY(
import json, time
time.sleep(1.5)
print(json.dumps({"ok": True, "slow": True}))
)PY";

// 父进程输出 JSON 后正常 exit 0，但先 spawn 一个后代：后代 0.6 秒后写 marker
// 文件（否则 2 秒自限退出）。execute() 返回后若后代还活着，marker 就会出现。
const char* PY_SPAWN_DESCENDANT = R"PY(
import json, os, subprocess, sys
marker = os.path.join(os.path.dirname(os.path.abspath(__file__)), "descendant_marker.txt")
code = "import time\ntime.sleep(0.6)\nopen(%r, 'w').write('leaked')\n" % marker
subprocess.Popen([sys.executable, "-c", code])
print(json.dumps({"ok": True}))
sys.stdout.flush()
)PY";

// 后代脚本（与父脚本同目录落盘）：持续灌空白（合法 JSON 允许尾随空白），
// 0.6 秒处写 marker，5 秒自限。execute() 返回后若后代还活着，marker 会出现。
const char* PY_FLOOD_DESCENDANT_SCRIPT = R"PY(
import os, sys, time
marker = os.path.join(os.path.dirname(os.path.abspath(__file__)), "flood_marker.txt")
t0 = time.time()
out = sys.stdout
chunk = " " * 512
while time.time() - t0 < 0.6:
    out.write(chunk)
    out.flush()
open(marker, "w").write("leaked")
while time.time() - t0 < 5.0:
    out.write(chunk)
    out.flush()
)PY";

// 父脚本：输出合法 JSON 后立即 exit 0，但先把灌空白的后代挂到同一个管道上。
// 后代持有写端：尾部 drain 若跟着它跑，就会绕过期限/取消。
const char* PY_FLOODING_PARENT = R"PY(
import json, os, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
subprocess.Popen([sys.executable, os.path.join(here, "descendant.py")],
                 stdout=sys.stdout, stderr=sys.stderr)
print(json.dumps({"ok": True}))
sys.stdout.flush()
)PY";

// 同上，但父进程打印 JSON 后继续活 1.5 秒：取消用例需要它在取消时仍在运行。
const char* PY_FLOODING_PARENT_LINGER = R"PY(
import json, os, subprocess, sys, time
here = os.path.dirname(os.path.abspath(__file__))
subprocess.Popen([sys.executable, os.path.join(here, "descendant.py")],
                 stdout=sys.stdout, stderr=sys.stderr)
print(json.dumps({"ok": True}))
sys.stdout.flush()
time.sleep(1.5)
)PY";

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

long long elapsed_ms(Clock::time_point start)
{
    return std::chrono::duration_cast<Ms>(Clock::now() - start).count();
}

// 每个用例独占临时目录，并在结束后恢复默认 logger。
struct Fixture
{
    test_support::TempDirectory temp;
    test_support::RestoreDefaultLogger restore;
    fs::path g_tmp = temp.path();
    fs::path g_scan_a = g_tmp / "scan_a";
    fs::path g_scan_b = g_tmp / "scan_b";
    Fixture()
    {
        // PluginManager logs during discover/load — must install a default
        // logger first (spdlog dereferences default_logger_raw()).
        auto logger = std::make_shared<spdlog::logger>(
            "test", std::make_shared<spdlog::sinks::null_sink_mt>());
        logger->set_level(spdlog::level::debug);
        spdlog::set_default_logger(logger);

        write_file(g_scan_a / "hello" / "manifest.json", MANIFEST_HELLO);
        write_file(g_scan_a / "hello" / "plugin.py", "print(\"ok\")\n");
        write_file(g_scan_a / "bad" / "manifest.json", MANIFEST_BAD);
        write_file(g_scan_a / "stray.txt", "not a plugin\n");
        write_file(g_scan_b / "auto" / "manifest.json", MANIFEST_AUTO);
    }
};
}  // namespace

TEST_CASE_FIXTURE(Fixture, "nonexistent search path is skipped silently")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_tmp / "does_not_exist"});
    pm.discover();
    CHECK(pm.available_plugins().empty());
}

TEST_CASE_FIXTURE(Fixture, "empty directory")
{
    auto empty_dir = g_tmp / "empty";
    fs::create_directories(empty_dir);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{empty_dir});
    pm.discover();
    CHECK(pm.available_plugins().empty());
}

TEST_CASE_FIXTURE(Fixture, "valid plugin discovered + manifest fields parsed")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto avail = pm.available_plugins();
    bool found = false;
    for (auto& m : avail)
    {
        if (m.id == "hello" && m.name == "Hello Test" && m.version == "1.0.0" &&
            m.entry_point == "plugin.py" && m.one_click && m.actions.size() == 1)
            found = true;
    }
    CHECK(found);
}

TEST_CASE_FIXTURE(Fixture, "invalid manifest (missing id) skipped; stray file skipped")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto avail = pm.available_plugins();
    CHECK_EQ(avail.size(), 1u);
    CHECK_EQ(avail[0].id, "hello");
}

// from_file 的 path/string/const char* 调用都必须能编译且解析同一份真实清单
// （重载集里同时有 std::string 和 path 时，字符串字面量是二义的）。
TEST_CASE_FIXTURE(Fixture, "FIX05 manifest from_file accepts path, string and const char*")
{
    const fs::path path = g_scan_a / "hello" / "manifest.json";
    const std::string narrow = path.string();
    const char* cstr = narrow.c_str();

    auto by_path = kforge::plugin::PluginManifest::from_file(path);
    auto by_string = kforge::plugin::PluginManifest::from_file(narrow);
    auto by_cstr = kforge::plugin::PluginManifest::from_file(cstr);
    REQUIRE(by_path.has_value());
    REQUIRE(by_string.has_value());
    REQUIRE(by_cstr.has_value());
    CHECK(by_string->id == "hello");
    CHECK(by_cstr->name == "Hello Test");
    CHECK(by_string->entry_point == by_path->entry_point);
    CHECK(by_cstr->actions.size() == by_path->actions.size());
}

TEST_CASE_FIXTURE(Fixture, "load / is_loaded / count / plugin_path")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto res = pm.load("hello", nullptr);
    CHECK(res.has_value());
    CHECK(pm.is_loaded("hello"));
    CHECK_EQ(pm.count(), 1u);
    CHECK(pm.plugin_path("hello") == g_scan_a / "hello");
}

TEST_CASE_FIXTURE(Fixture, "load idempotent")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto res1 = pm.load("hello", nullptr);
    CHECK(res1.has_value());
    auto res2 = pm.load("hello", nullptr);
    CHECK(res2.has_value());
    CHECK_EQ(pm.count(), 1u);
}

TEST_CASE_FIXTURE(Fixture, "load unknown id → NotFound")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto res = pm.load("nope", nullptr);
    CHECK_FALSE(res.has_value());
    CHECK(res.error().kind() == kforge::util::Error::Kind::NotFound);
}

TEST_CASE_FIXTURE(Fixture, "unload")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto res = pm.load("hello", nullptr);
    CHECK(res.has_value());
    pm.unload("hello");
    CHECK_FALSE(pm.is_loaded("hello"));
    CHECK_EQ(pm.count(), 0u);
}

TEST_CASE_FIXTURE(Fixture, "shutdown_all clears everything and is idempotent")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto res = pm.load("hello", nullptr);
    CHECK(res.has_value());
    pm.shutdown_all();
    pm.shutdown_all();  // must not throw
    CHECK_EQ(pm.count(), 0u);
    CHECK(pm.available_plugins().empty());
}

// FIX05 回归：shutdown_all 必须一并清掉 plugin_dirs_，否则之后的 load 会在
// available_ 上解引用 end()（旧实现崩溃）。停止是终态：磁盘上的发现目录还在，
// 但 discover() 不能让它复活。
TEST_CASE_FIXTURE(Fixture, "FIX05 shutdown_all closes loading for good (discover cannot revive)")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto first = pm.load("hello", nullptr);
    CHECK(first.has_value());
    CHECK(pm.plugin_path("hello") == g_scan_a / "hello");

    pm.shutdown_all();
    CHECK_EQ(pm.count(), 0u);
    CHECK_FALSE(pm.is_loaded("hello"));
    CHECK(pm.available_plugins().empty());
    CHECK(pm.plugin_path("hello").empty());  // 陈旧目录不得残留

    // 关闭后已知 id 必须报错，而不是重新开放加载
    auto after = pm.load("hello", nullptr);
    CHECK_FALSE(after.has_value());
    CHECK(after.error().kind() == kforge::util::Error::Kind::NotFound);

    pm.shutdown_all();  // 再次关闭必须安全（幂等）
    CHECK_EQ(pm.count(), 0u);
    auto again = pm.load("hello", nullptr);
    CHECK_FALSE(again.has_value());
    CHECK(again.error().kind() == kforge::util::Error::Kind::NotFound);

    // 插件目录仍在磁盘上，但停止后的 manager 不得复活：discover 空转，load 依旧 NotFound
    CHECK(pm.is_stopped());
    pm.discover();
    CHECK(pm.available_plugins().empty());
    CHECK(pm.plugin_path("hello").empty());
    auto revived = pm.load("hello", nullptr);
    CHECK_FALSE(revived.has_value());
    CHECK(revived.error().kind() == kforge::util::Error::Kind::NotFound);
    CHECK_EQ(pm.count(), 0u);

    // 未停止的 manager 走同一段代码：默认执行上限保持旧语义
    kforge::plugin::PluginManager fresh(std::vector<fs::path>{g_scan_a});
    CHECK_FALSE(fresh.is_stopped());
    CHECK(fresh.execution_options().timeout == std::chrono::milliseconds(120000));
    CHECK(fresh.execution_options().max_output_bytes == 4u * 1024u * 1024u);
}

TEST_CASE_FIXTURE(Fixture, "discover idempotent (no duplicate entries)")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    pm.discover();
    CHECK_EQ(pm.available_plugins().size(), 1u);
}

TEST_CASE_FIXTURE(Fixture, "multiple search paths merged")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a, g_scan_b});
    pm.discover();
    CHECK_EQ(pm.available_plugins().size(), 2u);
}

// ------------------------------------------------------------
// FIX05：有界执行（真实 python 子进程）
// ------------------------------------------------------------

// 停止后的 execute 必须立刻拒绝，且不启动任何进程
TEST_CASE_FIXTURE(Fixture, "FIX05 execute after shutdown is rejected")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_stopped";
    write_exec_plugin(scan, "stopper", "Stopper", PY_ECHO_JSON);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(5000, 1u << 20));
    pm.discover();
    pm.shutdown_all();

    const auto start = Clock::now();
    auto res = pm.execute("stopper", "ping", "{}");
    CHECK_FALSE(res.has_value());
    CHECK(res.error().message().find("stopped") != std::string::npos);
    CHECK(elapsed_ms(start) < 1000);  // 没有启动任何进程
}

// 正常输出必须原样返回（协议不变），且只有 exit code == 0 才算成功
TEST_CASE_FIXTURE(Fixture, "FIX05 normal JSON output succeeds")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_ok";
    write_exec_plugin(scan, "echo", "Echo", PY_ECHO_JSON);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(10000, 1u << 20));
    pm.discover();

    const std::string args = R"({"text": "中文 \"quoted\" back\\slash", "n": 7})";
    const auto start = Clock::now();
    auto res = pm.execute("echo", "ping", args);
    REQUIRE(res.has_value());
    CHECK(elapsed_ms(start) < 5000);
    auto j = nlohmann::json::parse(*res);
    CHECK(j["ok"] == true);
    CHECK(j["action"] == "ping");
    CHECK(j["args"] == args);
}

// 中文目录名 + 非 ASCII entry 文件名 + 中文/引号/反斜杠参数：discover → execute 全链路。
// entry 来自 manifest（UTF-8），拼进路径时不能走窄字符构造，否则 Windows 上按
// ANSI 代码页解释就找不到脚本。
TEST_CASE_FIXTURE(Fixture, "FIX05 unicode plugin directory, entry name and JSON args round-trip")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    // C++20：u8"" 是 char8_t，path 按 UTF-8 解释 → Windows 上得到正确的宽路径
    const fs::path scan = g_tmp / fs::path(u8"插件目录");
    const fs::path dir = scan / fs::path(u8"中文_插件");
    const fs::path entry = fs::path(u8"入口脚本.py");
    fs::create_directories(dir);
    write_file(dir / "manifest.json",
               std::string("{\n  \"id\": \"unicode.echo\",\n  \"name\": \"中文 插件\",\n"
                           "  \"version\": \"1.0.0\",\n  \"entry\": \"入口脚本.py\"\n}\n"));
    {
        std::ofstream out(dir / entry, std::ios::binary);
        out << PY_ECHO_JSON;
    }

    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(10000, 1u << 20));
    pm.discover();
    auto avail = pm.available_plugins();
    REQUIRE_EQ(avail.size(), 1u);
    CHECK(avail[0].name == "中文 插件");              // 发现阶段就读到了 UTF-8 清单
    CHECK(avail[0].entry_point == "入口脚本.py");     // entry 也是 UTF-8，无损
    CHECK(pm.plugin_path("unicode.echo") == dir);     // 非 ASCII 路径没有被 ANSI 化

    const std::string args = R"({"型号": "电容 10µF", "quote": "a\"b", "path": "C:\\tmp\\x"})";
    auto res = pm.execute("unicode.echo", "ping", args);
    REQUIRE(res.has_value());
    auto j = nlohmann::json::parse(*res);
    CHECK(j["args"] == args);                         // argv 全链路无损
}

// 输出超过上限：明确报错，绝不静默截断成“成功”
TEST_CASE_FIXTURE(Fixture, "FIX05 output over the cap is an error, not a silent truncation")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_cap";
    write_exec_plugin(scan, "big", "Big", PY_BIG_OUTPUT);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(10000, 4096));
    pm.discover();

    const auto start = Clock::now();
    auto res = pm.execute("big", "ping", "{}");
    CHECK_FALSE(res.has_value());
    CHECK(res.error().message().find("exceeded") != std::string::npos);
    CHECK(res.error().message().find("4096") != std::string::npos);
    CHECK(elapsed_ms(start) < 3000);
}

// 持续大量输出 + 短超时（cap 很大，先触发超时）：必须被终止，不能绕过超时
TEST_CASE_FIXTURE(Fixture, "FIX05 a flooding plugin hits the timeout and is killed")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_flood";
    write_exec_plugin(scan, "flood", "Flood", PY_FLOOD);
    // cap 刻意放大：先触发的必须是超时，而不是输出上限
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan},
                                     exec_options(400, 256u * 1024u * 1024u));
    pm.discover();

    const auto start = Clock::now();
    auto res = pm.execute("flood", "ping", "{}");
    const auto elapsed = elapsed_ms(start);
    CHECK_FALSE(res.has_value());
    CHECK(res.error().message().find("timed out") != std::string::npos);
    CHECK(elapsed < 1500);  // 明显低于夹具 2 秒的自限
}

// 完全无输出地睡眠：同样必须在期限处终止并回收
TEST_CASE_FIXTURE(Fixture, "FIX05 a silent sleeper hits the timeout and is killed")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_silent";
    write_exec_plugin(scan, "silent", "Silent", PY_SILENT_SLEEP);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(400, 1u << 20));
    pm.discover();

    const auto start = Clock::now();
    auto res = pm.execute("silent", "ping", "{}");
    const auto elapsed = elapsed_ms(start);
    CHECK_FALSE(res.has_value());
    CHECK(res.error().message().find("timed out") != std::string::npos);
    CHECK(elapsed < 1500);
}

// 关掉 stdout/stderr 后继续睡眠：EOF 不是进程退出，不能就此当成功返回
TEST_CASE_FIXTURE(Fixture, "FIX05 closed stdout/stderr is not process exit")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_eof";
    write_exec_plugin(scan, "eof", "Eof", PY_CLOSE_STREAMS_SLEEP);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(500, 1u << 20));
    pm.discover();

    const auto start = Clock::now();
    auto res = pm.execute("eof", "ping", "{}");
    const auto elapsed = elapsed_ms(start);
    CHECK_FALSE(res.has_value());  // 旧实现在这里返回“成功但空输出”
    CHECK(elapsed < 1500);
}

// 输出合法 JSON 但 exit code != 0：必须报错
TEST_CASE_FIXTURE(Fixture, "FIX05 non-zero exit is an error even with valid JSON output")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_nonzero";
    write_exec_plugin(scan, "nonzero", "NonZero", PY_JSON_NONZERO);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(10000, 1u << 20));
    pm.discover();

    auto res = pm.execute("nonzero", "ping", "{}");
    CHECK_FALSE(res.has_value());
    CHECK(res.error().message().find("exited with code 3") != std::string::npos);
}

// 失败原因通常来自 stderr（Python 超时 traceback 就是如此）：必须进入错误
// 信息，否则上层只能看到 “exited with code 1”，无法识别不确定写入。
TEST_CASE_FIXTURE(Fixture, "FIX05 non-zero exit keeps the plugin failure detail")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_fail_detail";
    write_exec_plugin(scan, "faildetail", "FailDetail", PY_FAIL_DETAIL);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(10000, 1u << 20));
    pm.discover();

    auto res = pm.execute("faildetail", "ping", "{}");
    CHECK_FALSE(res.has_value());
    const auto message = res.error().message();
    CHECK(message.find("Plugin exited with code 1: ") != std::string::npos);
    CHECK(message.find("timed out after 30 seconds") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "FIX05 plugin failure detail is bounded and keeps the tail")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_fail_long";
    write_exec_plugin(scan, "faillong", "FailLong", PY_FAIL_LONG);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(10000, 1u << 20));
    pm.discover();

    auto res = pm.execute("faillong", "ping", "{}");
    CHECK_FALSE(res.has_value());
    const auto message = res.error().message();
    CHECK(message.size() < 1600);
    CHECK(message.find("Plugin exited with code 1: ") == 0);
    CHECK(message.find("TAIL-MARKER") != std::string::npos);
}

// 父进程正常 exit 0 也不能留下持管道/仍在工作的后代
TEST_CASE_FIXTURE(Fixture, "FIX05 no descendant outlives a successful execution")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_descendant";
    write_exec_plugin(scan, "spawner", "Spawner", PY_SPAWN_DESCENDANT);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(10000, 1u << 20));
    pm.discover();

    const auto start = Clock::now();
    auto res = pm.execute("spawner", "ping", "{}");
    const auto elapsed = elapsed_ms(start);
    REQUIRE(res.has_value());
    CHECK(elapsed < 2000);  // 后代持着管道，也不能让本次调用等它 2 秒自限

    const auto marker = scan / "spawner" / "descendant_marker.txt";
    CHECK_FALSE(fs::exists(marker));                 // 返回时后代已被收敛
    std::this_thread::sleep_for(Ms(1000));           // 后代若活着会在 0.6 秒写入
    CHECK_FALSE(fs::exists(marker));                 // 延迟 marker 证明它没能活下来
}

// 尾部 drain 必须有界：父进程已 exit 0，但后代持续灌空白（大 cap + 短期限）。
// 旧实现会在 drain 里陪着后代跑到它 5 秒自限；实现必须先收敛后代再有限 drain。
TEST_CASE_FIXTURE(Fixture, "FIX05 tail drain does not follow a flooding descendant")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_flood_desc";
    write_exec_plugin(scan, "flooder", "Flooder", PY_FLOODING_PARENT);
    write_file(scan / "flooder" / "descendant.py", PY_FLOOD_DESCENDANT_SCRIPT);
    // cap 刻意放大：先触发的只能是收敛顺序，而不是“输出超限”
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan},
                                     exec_options(3000, 256u * 1024u * 1024u));
    pm.discover();

    const auto start = Clock::now();
    auto res = pm.execute("flooder", "ping", "{}");
    const auto elapsed = elapsed_ms(start);
    CHECK(elapsed < 2000);  // 远低于后代 5 秒自限：drain 没有跟着后代跑
    REQUIRE(res.has_value());  // 父进程 exit 0；JSON + 尾随空白仍是合法 JSON
    CHECK(nlohmann::json::parse(*res).value("ok", false));

    const auto marker = scan / "flooder" / "flood_marker.txt";
    CHECK_FALSE(fs::exists(marker));   // 后代已被收敛，没来得及写 marker
    std::this_thread::sleep_for(Ms(1200));
    CHECK_FALSE(fs::exists(marker));   // 延迟检查：后代确实没有活下来
}

// 取消同理：大 cap + 长期限下 shutdown_all 必须让带后代洪泛的执行有限返回，
// 且后代一个不留。
TEST_CASE_FIXTURE(Fixture, "FIX05 shutdown during a descendant flood stays bounded")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_flood_cancel";
    write_exec_plugin(scan, "flooder2", "Flooder2", PY_FLOODING_PARENT_LINGER);
    write_file(scan / "flooder2" / "descendant.py", PY_FLOOD_DESCENDANT_SCRIPT);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan},
                                     exec_options(30000, 256u * 1024u * 1024u));
    pm.discover();

    auto future = std::async(std::launch::async,
                             [&pm]() { return pm.execute("flooder2", "ping", "{}"); });
    std::this_thread::sleep_for(Ms(400));  // 父进程仍在运行，后代正在灌
    const auto start = Clock::now();
    pm.shutdown_all();

    const bool ready = future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    CHECK(ready);
    if (ready)
    {
        auto res = future.get();
        REQUIRE_FALSE(res.has_value());  // 父进程还活着：取消必须赢
        CHECK(res.error().message().find("cancelled") != std::string::npos);
    }
    CHECK(elapsed_ms(start) < 1500);  // 明显低于父进程 1.5 秒 + 后代 5 秒自限

    const auto marker = scan / "flooder2" / "flood_marker.txt";
    std::this_thread::sleep_for(Ms(1200));
    CHECK_FALSE(fs::exists(marker));  // 取消后后代也没能活下来
}

// 关闭时正在跑的 execute 必须被取消并回收
TEST_CASE_FIXTURE(Fixture, "FIX05 shutdown cancels an in-flight execution")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_cancel";
    write_exec_plugin(scan, "slow", "Slow", PY_SILENT_SLEEP);
    // 超时故意设得很长：只有取消能结束它
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(30000, 1u << 20));
    pm.discover();

    auto future = std::async(std::launch::async,
                             [&pm]() { return pm.execute("slow", "ping", "{}"); });
    std::this_thread::sleep_for(Ms(300));  // 让子进程真正起来

    const auto start = Clock::now();
    pm.shutdown_all();
    // 有界等待：只有 execute 本身有界，这里的等待才成立
    const bool ready = future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    CHECK(ready);
    if (ready)
    {
        auto res = future.get();
        CHECK_FALSE(res.has_value());
        CHECK(res.error().message().find("cancelled") != std::string::npos);
    }
    CHECK(elapsed_ms(start) < 1500);  // 明显低于夹具 2 秒的自限

    auto after = pm.execute("slow", "ping", "{}");
    CHECK_FALSE(after.has_value());
}

// 并发执行：快速的那次不得等待慢的那次（Windows 上还验证句柄不互相继承）
TEST_CASE_FIXTURE(Fixture, "FIX05 a fast execution does not wait for a concurrent slow one")
{
    if (!python_available())
    {
        WARN("python not available — execute tests skipped");
        return;
    }
    auto scan = g_tmp / "exec_concurrent";
    write_exec_plugin(scan, "slow1", "Slow1", PY_SLOW_JSON);
    write_exec_plugin(scan, "fast1", "Fast1", PY_ECHO_JSON);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan}, exec_options(10000, 1u << 20));
    pm.discover();

    auto slow = std::async(std::launch::async,
                           [&pm]() { return pm.execute("slow1", "ping", "{}"); });
    std::this_thread::sleep_for(Ms(200));  // 让慢执行先拿到自己的管道

    const auto start = Clock::now();
    auto fast = pm.execute("fast1", "ping", "{}");
    const auto fast_elapsed = elapsed_ms(start);
    REQUIRE(fast.has_value());
    // 慢执行（1.5 秒）此刻还没结束：快执行既没等它退出，也没等它超时
    CHECK(slow.wait_for(Ms(0)) == std::future_status::timeout);
    CHECK(fast_elapsed < 1000);

    auto slow_res = slow.get();
    REQUIRE(slow_res.has_value());
    CHECK(slow_res->find("\"slow\": true") != std::string::npos);
}

// 这两个环境检查访问真实应用路径，需显式 --no-skip 才运行。
TEST_CASE_FIXTURE(Fixture, "create_default smoke" * doctest::skip())
{
    auto pm = kforge::plugin::PluginManager::create_default();
    CHECK(pm != nullptr);
}

TEST_CASE_FIXTURE(Fixture, "platform helpers smoke" * doctest::skip())
{
    CHECK_FALSE(kforge::util::get_exe_dir().empty());
    CHECK_FALSE(kforge::util::get_data_dir().empty());
}

// ------------------------------------------------------------
// 阶段1：manifest.frontend 解析与 resolve_frontend_asset
// 这里只覆盖解析器和资源解析 helper；HTTP 路由在 routes.cpp，不属于本目标。
// ------------------------------------------------------------

namespace
{
// 带 frontend 的清单：root=dist，entry=main.mjs，styles 含子目录形式
const char* MANIFEST_WEB = R"({
  "id": "web",
  "name": "Web Test",
  "version": "2.3.4",
  "frontend": { "root": "dist", "entry": "main.mjs",
                "styles": ["style.css", "css/theme.css"] }
})";

// 落盘一个完整 web 插件：dist/ 下是入口、样式和嵌套资源；插件外层放一份
// “秘密”文件，供目录穿越用例尝试逃逸。
void write_web_plugin(const std::filesystem::path& scan)
{
    const auto dir = scan / "web";
    write_file(dir / "manifest.json", MANIFEST_WEB);
    write_file(dir / "dist" / "index.js", "console.log('index');\n");
    write_file(dir / "dist" / "main.mjs", "export const ok = true;\n");
    write_file(dir / "dist" / "style.css", "body { color: red; }\n");
    write_file(dir / "dist" / "css" / "theme.css", "body { margin: 0; }\n");
    write_file(scan / "secret.txt", "top secret\n");
}

bool kind_is(const kforge::util::Error& error, kforge::util::Error::Kind kind)
{
    return error.kind() == kind;
}
}  // namespace

TEST_CASE_FIXTURE(Fixture, "frontend manifest defaults: root dist, entry index.js, no styles")
{
    using nlohmann::json;
    const auto doc = json{{"id", "plain"}, {"name", "Plain"}, {"frontend", json::object()}};
    auto parsed = kforge::plugin::PluginManifest::from_json(doc);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->frontend.has_value());
    CHECK(parsed->frontend->root == "dist");
    CHECK(parsed->frontend->entry == "index.js");
    CHECK(parsed->frontend->styles.empty());
}

TEST_CASE_FIXTURE(Fixture, "manifest without frontend has no frontend block")
{
    auto parsed = kforge::plugin::PluginManifest::from_json(nlohmann::json::parse(MANIFEST_HELLO));
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->frontend.has_value());
}

TEST_CASE_FIXTURE(Fixture, "frontend manifest fields parsed in declaration order")
{
    auto parsed = kforge::plugin::PluginManifest::from_json(nlohmann::json::parse(MANIFEST_WEB));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->frontend.has_value());
    CHECK(parsed->frontend->root == "dist");
    CHECK(parsed->frontend->entry == "main.mjs");
    REQUIRE(parsed->frontend->styles.size() == 2u);
    CHECK(parsed->frontend->styles[0] == "style.css");
    CHECK(parsed->frontend->styles[1] == "css/theme.css");
}

TEST_CASE_FIXTURE(Fixture, "frontend manifest accepts nested relative root/entry/styles")
{
    using nlohmann::json;
    const auto doc = json::parse(R"({
      "id": "nested", "name": "Nested",
      "frontend": { "root": "web/dist", "entry": "js/app.js",
                    "styles": ["css/a.css"] }
    })");
    auto parsed = kforge::plugin::PluginManifest::from_json(doc);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->frontend.has_value());
    CHECK(parsed->frontend->root == "web/dist");
    CHECK(parsed->frontend->entry == "js/app.js");
    REQUIRE(parsed->frontend->styles.size() == 1u);
    CHECK(parsed->frontend->styles[0] == "css/a.css");
}

TEST_CASE_FIXTURE(Fixture, "frontend manifest rejects unsafe relative paths")
{
    using nlohmann::json;
    const std::vector<std::string> invalid{
        R"({"root": ""})",
        R"({"root": "/dist"})",
        R"({"root": "C:/dist"})",
        R"({"root": "C:\\dist"})",
        R"({"root": "../dist"})",
        R"({"root": "dist/../assets"})",
        R"({"root": "dist/./assets"})",
        R"({"root": "dist\\assets"})",
        R"({"entry": ""})",
        R"({"entry": "/main.js"})",
        R"({"entry": "../main.js"})",
        R"({"entry": "js/../main.js"})",
        R"({"entry": "js\\main.js"})",
        R"({"styles": ["style.css", ".."]})",
        R"({"styles": [""]})",
        R"({"styles": ["css//theme.css"]})",
        R"({"styles": ["C:/theme.css"]})",
        R"({"styles": "style.css"})",
        R"({"root": "dist\u0001"})",
        R"({"entry": "main\u0001.js"})",
    };
    for (const auto& body : invalid)
    {
        CAPTURE(body);
        const auto doc =
            json::parse(std::string(R"({"id":"x","name":"X","frontend":)") + body + "}");
        auto parsed = kforge::plugin::PluginManifest::from_json(doc);
        CHECK_FALSE(parsed.has_value());
        if (!parsed)
            CHECK(kind_is(parsed.error(), kforge::util::Error::Kind::ParseError));
    }

    // frontend 本身必须是对象，root/entry 必须是字符串，styles 必须是字符串数组
    for (const std::string body : {R"("dist")", "[]", "42", R"({"root": 5})",
                                   R"({"entry": null})", R"({"styles": [1]})"})
    {
        CAPTURE(body);
        const auto doc =
            json::parse(std::string(R"({"id":"x","name":"X","frontend":)") + body + "}");
        auto parsed = kforge::plugin::PluginManifest::from_json(doc);
        CHECK_FALSE(parsed.has_value());
        if (!parsed)
            CHECK(kind_is(parsed.error(), kforge::util::Error::Kind::ParseError));
    }
}

// 发现阶段：一个插件的 frontend 校验失败只跳过它自己，其他插件照常可用，
// 且错误通过日志/返回值暴露而不是抛到全局。
TEST_CASE_FIXTURE(Fixture, "invalid frontend skips only the offending plugin during discovery")
{
    const auto scan = g_tmp / "fe_discovery";
    write_web_plugin(scan);
    write_file(scan / "evil" / "manifest.json",
               R"({"id":"evil","name":"Evil","frontend":{"root":"../outside"}})");
    write_file(scan / "plain" / "manifest.json", MANIFEST_AUTO);

    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan});
    pm.discover();

    auto available = pm.available_plugins();
    REQUIRE_EQ(available.size(), 2u);
    bool web = false;
    bool evil = false;
    bool plain = false;
    for (auto& m : available)
    {
        if (m.id == "web")
        {
            web = true;
            REQUIRE(m.frontend.has_value());
            CHECK(m.frontend->entry == "main.mjs");
        }
        else if (m.id == "evil")
            evil = true;
        else if (m.id == "auto")
            plain = true;
    }
    CHECK(web);
    CHECK_FALSE(evil);
    CHECK(plain);
}

TEST_CASE_FIXTURE(Fixture, "resolve_frontend_asset returns absolute paths inside the frontend root")
{
    const auto scan = g_tmp / "fe_resolve";
    write_web_plugin(scan);
    const auto plugin_dir = scan / "web";

    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan});
    pm.discover();
    REQUIRE(pm.plugin_path("web") == plugin_dir);

    auto entry = pm.resolve_frontend_asset("web", "main.mjs");
    REQUIRE(entry.has_value());
    CHECK(entry->is_absolute());
    CHECK(entry->filename() == "main.mjs");
    CHECK(fs::equivalent(*entry, plugin_dir / "dist" / "main.mjs"));

    auto nested = pm.resolve_frontend_asset("web", "css/theme.css");
    REQUIRE(nested.has_value());
    CHECK(fs::equivalent(*nested, plugin_dir / "dist" / "css" / "theme.css"));

    // 资源名是 UTF-8：不能按 ANSI 代码页解释（Windows 上非 ASCII 文件名）
    const std::string unicode_name(reinterpret_cast<const char*>(u8"中文资源.js"));
    write_file(plugin_dir / "dist" / fs::path(u8"中文资源.js"), "export default 1;\n");
    auto unicode = pm.resolve_frontend_asset("web", unicode_name);
    REQUIRE(unicode.has_value());
    CHECK(fs::equivalent(*unicode, plugin_dir / "dist" / fs::path(u8"中文资源.js")));
}

TEST_CASE_FIXTURE(Fixture, "resolve_frontend_asset rejects traversal and malformed relative paths")
{
    const auto scan = g_tmp / "fe_traversal";
    write_web_plugin(scan);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan});
    pm.discover();

    std::vector<std::string> invalid{
        "",
        ".",
        "..",
        "../secret.txt",
        "/secret.txt",
        "C:/secret.txt",
        "C:\\secret.txt",
        "dist\\main.mjs",
        "css/../main.mjs",
        "main.mjs/..",
        "css//theme.css",
        "css/",
        "index.js/",
    };
    invalid.push_back(std::string("a\x01") + "b.js");  // 控制字符

    for (const auto& bad : invalid)
    {
        CAPTURE(bad);
        auto resolved = pm.resolve_frontend_asset("web", bad);
        CHECK_FALSE(resolved.has_value());
        if (!resolved)
            CHECK(kind_is(resolved.error(), kforge::util::Error::Kind::ValidationError));
    }

    // 语法合法但文件不存在：NotFound（路由映射 404，而不是 400）
    auto missing = pm.resolve_frontend_asset("web", "missing.js");
    CHECK_FALSE(missing.has_value());
    if (!missing)
        CHECK(kind_is(missing.error(), kforge::util::Error::Kind::NotFound));

    // 目录不是可提供的资源（必须是普通文件）
    fs::create_directories(scan / "web" / "dist" / "folder");
    auto directory = pm.resolve_frontend_asset("web", "folder");
    CHECK_FALSE(directory.has_value());
    if (!directory)
        CHECK(kind_is(directory.error(), kforge::util::Error::Kind::ValidationError));
}

TEST_CASE_FIXTURE(Fixture, "resolve_frontend_asset requires a frontend and a live manager")
{
    const auto scan = g_tmp / "fe_requires";
    write_web_plugin(scan);

    // hello（来自 Fixture）没有 frontend；web 有
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a, scan});
    pm.discover();

    auto no_frontend = pm.resolve_frontend_asset("hello", "main.mjs");
    CHECK_FALSE(no_frontend.has_value());
    if (!no_frontend)
        CHECK(kind_is(no_frontend.error(), kforge::util::Error::Kind::NotFound));

    auto unknown = pm.resolve_frontend_asset("nope", "main.mjs");
    CHECK_FALSE(unknown.has_value());
    if (!unknown)
        CHECK(kind_is(unknown.error(), kforge::util::Error::Kind::NotFound));

    pm.shutdown_all();
    auto stopped = pm.resolve_frontend_asset("web", "main.mjs");
    CHECK_FALSE(stopped.has_value());
    if (!stopped)
        CHECK(kind_is(stopped.error(), kforge::util::Error::Kind::NotFound));
}

// frontend.root 本身也不能是符号链接/junction：否则一个 dist -> C:\ 的链接
// 就能让 /plugin-assets 提供插件目录之外的文件（要求拒绝链接逃逸）。
TEST_CASE_FIXTURE(Fixture, "resolve_frontend_asset rejects a frontend root that links outside the plugin")
{
    const auto scan = g_tmp / "fe_root_link";
    const auto plugin_dir = scan / "linked";
    write_file(plugin_dir / "manifest.json", R"({
      "id": "linked", "name": "Linked",
      "frontend": { "root": "dist", "entry": "index.js" }
    })");
    const auto outside = g_tmp / "fe_root_outside";
    write_file(outside / "index.js", "export default 'leak';\n");

    const auto dist = plugin_dir / "dist";
    std::error_code ec;
#ifdef _WIN32
    const std::wstring command =
        L"cmd /c mklink /J \"" + dist.wstring() + L"\" \"" + outside.wstring() + L"\" >NUL 2>&1";
    _wsystem(command.c_str());
    if (!fs::exists(dist))
    {
        WARN("junction creation unavailable — frontend root link check skipped");
        return;
    }
#else
    fs::create_directory_symlink(outside, dist, ec);
    if (ec)
    {
        WARN("directory symlink creation unavailable — frontend root link check skipped");
        return;
    }
#endif

    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan});
    pm.discover();
    auto resolved = pm.resolve_frontend_asset("linked", "index.js");
    CHECK_FALSE(resolved.has_value());
    if (!resolved)
        CHECK(kind_is(resolved.error(), kforge::util::Error::Kind::ValidationError));

    fs::remove(dist, ec);  // 先摘掉链接，避免临时目录递归清理时跟随
}

// 符号链接 / junction 逃逸：目标落在 frontend root 之外时拒绝，即使文件名
// 本身完全合法。Windows 上创建符号链接可能需要开发者模式/管理员权限，创建
// 失败时跳过对应断言（用 WARN 记录），不让测试因环境差异变红。
TEST_CASE_FIXTURE(Fixture, "resolve_frontend_asset rejects symlink and junction escapes")
{
    const auto scan = g_tmp / "fe_links";
    write_web_plugin(scan);
    const auto dir = scan / "web";
    const auto outside = g_tmp / "fe_outside";
    write_file(outside / "leak.txt", "leak\n");

    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan});
    pm.discover();

    std::error_code ec;
    fs::create_symlink(outside / "leak.txt", dir / "dist" / "leak-link.js", ec);
    if (ec)
    {
        WARN("symlink creation unavailable — file symlink escape check skipped");
    }
    else
    {
        auto resolved = pm.resolve_frontend_asset("web", "leak-link.js");
        CHECK_FALSE(resolved.has_value());
        if (!resolved)
            CHECK(kind_is(resolved.error(), kforge::util::Error::Kind::ValidationError));
    }

    ec.clear();
    fs::create_directory_symlink(outside, dir / "dist" / "out", ec);
    if (ec)
    {
        WARN("directory symlink creation unavailable — directory escape check skipped");
    }
    else
    {
        auto resolved = pm.resolve_frontend_asset("web", "out/leak.txt");
        CHECK_FALSE(resolved.has_value());
        if (!resolved)
            CHECK(kind_is(resolved.error(), kforge::util::Error::Kind::ValidationError));
    }

#ifdef _WIN32
    // junction 不是 std::filesystem::is_symlink 能识别的类型，必须由 reparse
    // point 检查拦下；mklink /J 不需要管理员权限。
    const auto junction = dir / "dist" / "junction";
    const std::wstring command = L"cmd /c mklink /J \"" + junction.wstring() + L"\" \"" +
                                 outside.wstring() + L"\" >NUL 2>&1";
    _wsystem(command.c_str());
    if (fs::exists(junction))
    {
        auto resolved = pm.resolve_frontend_asset("web", "junction/leak.txt");
        CHECK_FALSE(resolved.has_value());
        if (!resolved)
            CHECK(kind_is(resolved.error(), kforge::util::Error::Kind::ValidationError));
        std::error_code remove_ec;
        fs::remove(junction, remove_ec);  // 先摘掉链接，再让临时目录析构
    }
    else
    {
        WARN("junction creation unavailable — junction escape check skipped");
    }
#endif
}

// ------------------------------------------------------------
// 阶段2：manifest v2、cancelable、契约声明、engines/config、诊断
// ------------------------------------------------------------

namespace
{
kforge::util::Result<kforge::plugin::PluginManifest> parse_manifest_json(const std::string& text)
{
    return kforge::plugin::PluginManifest::from_json(nlohmann::json::parse(text));
}

// 阶段2 的混合清单：python + frontend + 契约声明 + engines + config
const char* MANIFEST_V2_MIXED = R"({
  "manifestVersion": 2,
  "id": "phase2.mixed",
  "name": "Phase 2 Mixed",
  "version": "2.0.0",
  "python": {
    "entry": "main.py",
    "actions": [
      { "id": "run", "name": "Run", "trigger": "inline", "cancelable": true },
      { "id": "status", "name": "Status", "trigger": "inline" }
    ]
  },
  "frontend": {
    "root": "ui",
    "entry": "app.mjs",
    "styles": ["theme.css"],
    "provides": [ { "contract": "demo.supplier", "version": "1.0.0" } ],
    "requires": [ { "contract": "kiforge.symbols", "range": ">=1.0.0-rc.1 <2.0.0" } ],
    "contributes": { "views": ["demo.supply"], "slots": ["demo.supply.tabs"] }
  },
  "engines": { "kiforge": ">=1.0.0-rc.1 <2.0.0", "sdk": ">=1.0.0-rc.1 <2.0.0" },
  "config": { "schema": { "type": "object" } }
})";
}  // namespace

TEST_CASE_FIXTURE(Fixture, "v1 manifest keeps top-level entry/actions and v2 defaults are absent")
{
    auto parsed = parse_manifest_json(MANIFEST_HELLO);
    REQUIRE(parsed.has_value());
    CHECK_EQ(parsed->manifest_version, 1);
    CHECK(parsed->entry_point == "plugin.py");
    REQUIRE(parsed->actions.size() == 1u);
    CHECK(parsed->actions[0].id == "ping");
    CHECK_FALSE(parsed->actions[0].cancelable);
    CHECK_FALSE(parsed->python.has_value());
    CHECK_FALSE(parsed->engines.has_value());
    CHECK_FALSE(parsed->frontend.has_value());
    CHECK_FALSE(parsed->contributes.has_value());
    CHECK(parsed->provides.empty());
    CHECK(parsed->contract_requires.empty());
    CHECK(parsed->config.is_null());
}

TEST_CASE_FIXTURE(Fixture, "v1 manifest accepts explicit manifestVersion 1 and cancelable")
{
    auto parsed = parse_manifest_json(R"({
      "manifestVersion": 1,
      "id": "legacy.v1",
      "name": "Legacy V1",
      "entry": "legacy.py",
      "actions": [ { "id": "go", "name": "Go", "cancelable": true } ]
    })");
    REQUIRE(parsed.has_value());
    CHECK_EQ(parsed->manifest_version, 1);
    CHECK(parsed->entry_point == "legacy.py");
    REQUIRE(parsed->actions.size() == 1u);
    CHECK(parsed->actions[0].cancelable);
    CHECK_FALSE(parsed->python.has_value());
}

TEST_CASE_FIXTURE(Fixture, "v2 mixed manifest parses python, contracts, engines and config")
{
    auto parsed = parse_manifest_json(MANIFEST_V2_MIXED);
    REQUIRE(parsed.has_value());
    CHECK_EQ(parsed->manifest_version, 2);

    REQUIRE(parsed->python.has_value());
    CHECK(parsed->python->entry == "main.py");
    REQUIRE(parsed->python->actions.size() == 2u);
    CHECK(parsed->python->actions[0].cancelable);
    CHECK_FALSE(parsed->python->actions[1].cancelable);
    // 同步到旧执行路径字段
    CHECK(parsed->entry_point == "main.py");
    REQUIRE(parsed->actions.size() == 2u);
    CHECK(parsed->actions[0].id == "run");
    CHECK(parsed->actions[0].cancelable);

    REQUIRE(parsed->frontend.has_value());
    CHECK(parsed->frontend->root == "ui");
    CHECK(parsed->frontend->entry == "app.mjs");
    REQUIRE(parsed->frontend->styles.size() == 1u);
    CHECK(parsed->frontend->styles[0] == "theme.css");

    REQUIRE(parsed->provides.size() == 1u);
    CHECK(parsed->provides[0].contract == "demo.supplier");
    CHECK(parsed->provides[0].version == "1.0.0");
    REQUIRE(parsed->contract_requires.size() == 1u);
    CHECK(parsed->contract_requires[0].contract == "kiforge.symbols");
    CHECK(parsed->contract_requires[0].range == ">=1.0.0-rc.1 <2.0.0");
    REQUIRE(parsed->contributes.has_value());
    CHECK((*parsed->contributes)["views"][0] == "demo.supply");

    REQUIRE(parsed->engines.has_value());
    CHECK(parsed->engines->kiforge == ">=1.0.0-rc.1 <2.0.0");
    CHECK(parsed->engines->sdk == ">=1.0.0-rc.1 <2.0.0");
    CHECK(parsed->config["schema"]["type"] == "object");
}

TEST_CASE_FIXTURE(Fixture, "v2 frontend-only manifest has no python and default entry_point")
{
    auto parsed = parse_manifest_json(R"({
      "manifestVersion": 2,
      "id": "phase2.front",
      "name": "Phase 2 Front",
      "version": "2.0.0",
      "frontend": { "root": "dist", "entry": "index.js", "styles": ["style.css"] }
    })");
    REQUIRE(parsed.has_value());
    CHECK_EQ(parsed->manifest_version, 2);
    CHECK_FALSE(parsed->python.has_value());
    CHECK(parsed->entry_point == "plugin.py");
    CHECK(parsed->actions.empty());
    REQUIRE(parsed->frontend.has_value());
    CHECK(parsed->frontend->entry == "index.js");
    CHECK_FALSE(parsed->engines.has_value());
    CHECK(parsed->provides.empty());
    CHECK(parsed->contract_requires.empty());
    CHECK_FALSE(parsed->contributes.has_value());
}

TEST_CASE_FIXTURE(Fixture, "v2 python-only manifest keeps the frontend absent")
{
    auto parsed = parse_manifest_json(R"({
      "manifestVersion": 2,
      "id": "phase2.py",
      "name": "Phase 2 Python",
      "python": { "entry": "tool.py",
                  "actions": [ { "id": "go", "name": "Go" } ] }
    })");
    REQUIRE(parsed.has_value());
    CHECK_EQ(parsed->manifest_version, 2);
    REQUIRE(parsed->python.has_value());
    CHECK(parsed->python->entry == "tool.py");
    CHECK(parsed->entry_point == "tool.py");
    REQUIRE(parsed->actions.size() == 1u);
    CHECK(parsed->actions[0].id == "go");
    CHECK_FALSE(parsed->frontend.has_value());
}

TEST_CASE_FIXTURE(Fixture, "unsupported or malformed manifestVersion is rejected")
{
    using nlohmann::json;
    for (const std::string value : {"3", "0", "-1", "2.5", "\"2\"", "true", "null"})
    {
        CAPTURE(value);
        const auto doc = json::parse(std::string(R"({"manifestVersion":)") + value +
                                     R"(,"id":"x","name":"X"})");
        auto parsed = kforge::plugin::PluginManifest::from_json(doc);
        CHECK_FALSE(parsed.has_value());
        if (!parsed)
        {
            CHECK(kind_is(parsed.error(), kforge::util::Error::Kind::ParseError));
            CHECK(parsed.error().message().find("manifestVersion") != std::string::npos);
        }
    }
}

TEST_CASE_FIXTURE(Fixture, "v2 manifest type errors are rejected with ParseError")
{
    using nlohmann::json;
    const std::vector<std::string> invalid{
        R"("python": 5)",
        R"("python": {"entry": 7})",
        R"("python": {"actions": {}})",
        R"("python": {"actions": [{"id":"a","name":"A","cancelable":"yes"}]})",
        R"("frontend": {"provides": {}})",
        R"("frontend": {"provides": ["x"]})",
        R"("frontend": {"provides": [{"version": "1.0.0"}]})",
        R"("frontend": {"provides": [{"contract": "c"}]})",
        R"("frontend": {"provides": [{"contract": 5, "version": "1"}]})",
        R"("frontend": {"provides": [{"contract": "", "version": "1"}]})",
        R"("frontend": {"requires": [{"contract": "c"}]})",
        R"("frontend": {"requires": [{"contract": "c", "range": 5}]})",
        R"("frontend": {"requires": [{"contract": "", "range": ">=1"}]})",
        R"("engines": "x")",
        R"("engines": {"kiforge": 5})",
        R"("engines": {"sdk": []})",
        R"("actions": {})",
    };
    for (const auto& block : invalid)
    {
        CAPTURE(block);
        const auto doc = json::parse(std::string(R"({"manifestVersion":2,"id":"x","name":"X",)") +
                                     block + "}");
        auto parsed = kforge::plugin::PluginManifest::from_json(doc);
        CHECK_FALSE(parsed.has_value());
        if (!parsed)
            CHECK(kind_is(parsed.error(), kforge::util::Error::Kind::ParseError));
    }
}

TEST_CASE_FIXTURE(Fixture, "v2 contributes accepts arbitrary JSON")
{
    using nlohmann::json;
    for (const std::string value : {R"({"a":1})", R"(["x"])", R"("text")", "null", "42"})
    {
        CAPTURE(value);
        const auto doc = json::parse(
            std::string(R"({"manifestVersion":2,"id":"x","name":"X",)"
                        R"("frontend":{"root":"dist","entry":"index.js","contributes":)") +
            value + "}}");
        auto parsed = kforge::plugin::PluginManifest::from_json(doc);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->contributes.has_value());
        CHECK(*parsed->contributes == json::parse(value));
    }
}

TEST_CASE_FIXTURE(Fixture, "discovery records diagnostics for bad manifests, unknown versions and duplicate ids")
{
    const auto scan = g_tmp / "phase2_diagnostics";
    write_file(scan / "good" / "manifest.json", R"({
      "manifestVersion": 2, "id": "good", "name": "Good", "version": "2.0.0",
      "frontend": { "root": "dist", "entry": "index.js" }
    })");
    // 语义错误（name 类型不对）但 JSON 合法：诊断里能取到 manifest id
    write_file(scan / "broken" / "manifest.json", R"({ "id": "broken", "name": 7 })");
    // 语法错误：无法取 id，回退到目录名
    write_file(scan / "syntax" / "manifest.json", "{ not json");
    write_file(scan / "future" / "manifest.json",
               R"({ "manifestVersion": 3, "id": "future.plugin", "name": "Future" })");
    write_file(scan / "dup_a" / "manifest.json", R"({ "id": "dup", "name": "Dup A" })");
    write_file(scan / "dup_b" / "manifest.json", R"({ "id": "dup", "name": "Dup B" })");

    kforge::plugin::PluginManager pm(std::vector<fs::path>{scan});
    pm.discover();

    // 坏清单不影响其他插件：good + 一个 dup 可见
    CHECK_EQ(pm.available_plugins().size(), 2u);
    CHECK_FALSE(pm.plugin_path("dup").empty());

    const auto diagnostics = pm.diagnostics();
    auto count_code = [&diagnostics](const std::string& code)
    {
        std::size_t count = 0;
        for (const auto& diagnostic : diagnostics)
            if (diagnostic.code == code)
                ++count;
        return count;
    };
    CHECK_EQ(count_code("manifest/invalid"), 2u);
    CHECK_EQ(count_code("manifest/unsupported-version"), 1u);
    CHECK_EQ(count_code("plugin/duplicate-id"), 1u);

    for (const auto& diagnostic : diagnostics)
    {
        CHECK_FALSE(diagnostic.message.empty());
        if (diagnostic.code == "manifest/invalid")
            CHECK((diagnostic.plugin_id == "broken" || diagnostic.plugin_id == "syntax"));
        if (diagnostic.code == "manifest/unsupported-version")
            CHECK(diagnostic.plugin_id == "future.plugin");
        if (diagnostic.code == "plugin/duplicate-id")
            CHECK(diagnostic.plugin_id == "dup");
    }

    // 重新 discover 是「重算」而不是累加
    pm.discover();
    CHECK_EQ(pm.diagnostics().size(), diagnostics.size());

    pm.shutdown_all();
    CHECK(pm.diagnostics().empty());
}
