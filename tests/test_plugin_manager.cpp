// Tests for kforge::plugin::PluginManager: discovery, load/unload,
// shutdown idempotency, multi-path merge, and the create_default() factory.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/plugin/plugin_manager.h"
#include "../src/util/platform.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace
{
fs::path g_tmp;
fs::path g_scan_a;  // hello/ (valid) + bad/ (missing id) + stray file
fs::path g_scan_b;  // auto/ — second search path for merge test

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

// 全局 fixture：main 前建临时目录 + 写 manifest + 装 null logger，main 后清理。
struct Fixture
{
    Fixture()
    {
        // PluginManager logs during discover/load — must install a default
        // logger first (spdlog dereferences default_logger_raw()).
        auto logger = std::make_shared<spdlog::logger>(
            "test", std::make_shared<spdlog::sinks::null_sink_mt>());
        logger->set_level(spdlog::level::debug);
        spdlog::set_default_logger(logger);

#ifdef _WIN32
        auto pid = ::GetCurrentProcessId();
#else
        auto pid = ::getpid();
#endif
        g_tmp = fs::temp_directory_path() / ("kf_plugin_test_" + std::to_string(pid));
        g_scan_a = g_tmp / "scan_a";
        g_scan_b = g_tmp / "scan_b";
        fs::remove_all(g_tmp);

        write_file(g_scan_a / "hello" / "manifest.json", MANIFEST_HELLO);
        write_file(g_scan_a / "hello" / "plugin.py", "print(\"ok\")\n");
        write_file(g_scan_a / "bad" / "manifest.json", MANIFEST_BAD);
        write_file(g_scan_a / "stray.txt", "not a plugin\n");
        write_file(g_scan_b / "auto" / "manifest.json", MANIFEST_AUTO);
    }
    ~Fixture()
    {
        fs::remove_all(g_tmp);
        spdlog::drop_all();
    }
};
// 全局静态：构造在 RUN_ALL_TESTS 前（建目录 + 装 null logger），
// 析构在所有测试结束后（清理临时目录 + 释放 logger）。
static Fixture g_fixture;
}  // namespace

TEST_CASE("nonexistent search path is skipped silently")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_tmp / "does_not_exist"});
    pm.discover();
    CHECK(pm.available_plugins().empty());
}

TEST_CASE("empty directory")
{
    auto empty_dir = g_tmp / "empty";
    fs::create_directories(empty_dir);
    kforge::plugin::PluginManager pm(std::vector<fs::path>{empty_dir});
    pm.discover();
    CHECK(pm.available_plugins().empty());
}

TEST_CASE("valid plugin discovered + manifest fields parsed")
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

TEST_CASE("invalid manifest (missing id) skipped; stray file skipped")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto avail = pm.available_plugins();
    CHECK_EQ(avail.size(), 1u);
    CHECK_EQ(avail[0].id, "hello");
}

TEST_CASE("load / is_loaded / count / plugin_path")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto res = pm.load("hello", nullptr);
    CHECK(res.has_value());
    CHECK(pm.is_loaded("hello"));
    CHECK_EQ(pm.count(), 1u);
    CHECK(pm.plugin_path("hello") == g_scan_a / "hello");
}

TEST_CASE("load idempotent")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto res1 = pm.load("hello", nullptr);
    CHECK(res1.has_value());
    auto res2 = pm.load("hello", nullptr);
    CHECK(res2.has_value());
    CHECK_EQ(pm.count(), 1u);
}

TEST_CASE("load unknown id → NotFound")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto res = pm.load("nope", nullptr);
    CHECK_FALSE(res.has_value());
    CHECK(res.error().kind() == kforge::util::Error::Kind::NotFound);
}

TEST_CASE("unload")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    auto res = pm.load("hello", nullptr);
    CHECK(res.has_value());
    pm.unload("hello");
    CHECK_FALSE(pm.is_loaded("hello"));
    CHECK_EQ(pm.count(), 0u);
}

TEST_CASE("shutdown_all clears everything and is idempotent")
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

TEST_CASE("discover idempotent (no duplicate entries)")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a});
    pm.discover();
    pm.discover();
    CHECK_EQ(pm.available_plugins().size(), 1u);
}

TEST_CASE("multiple search paths merged")
{
    kforge::plugin::PluginManager pm(std::vector<fs::path>{g_scan_a, g_scan_b});
    pm.discover();
    CHECK_EQ(pm.available_plugins().size(), 2u);
}

TEST_CASE("create_default smoke")
{
    auto pm = kforge::plugin::PluginManager::create_default();
    CHECK(pm != nullptr);
}

TEST_CASE("platform helpers smoke")
{
    CHECK_FALSE(kforge::util::get_exe_dir().empty());
    CHECK_FALSE(kforge::util::get_data_dir().empty());
}