// Tests for kforge::plugin::PluginManager: discovery, load/unload,
// shutdown idempotency, multi-path merge, and the create_default() factory.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/plugin/plugin_manager.h"
#include "../src/util/platform.h"
#include "support/test_environment.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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
