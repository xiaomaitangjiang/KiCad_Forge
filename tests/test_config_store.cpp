#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "support/test_environment.h"
#include "util/config_store.h"
#include <fstream>
#include <atomic>
#include <thread>

namespace
{
struct ConfigFixture
{
    test_support::TempDirectory temp;
    std::filesystem::path path = temp.path() / "config.json";
    void write(std::string_view value) { std::ofstream(path) << value; }
};
}

TEST_CASE_FIXTURE(ConfigFixture, "config missing file uses defaults without creating a file")
{
    kforge::util::ConfigStore config(path);
    CHECK(config.get("missing").empty());
    CHECK_FALSE(config.get_bool("write_kf_id_to_file"));
    CHECK(config.get_bool("missing", true));
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE_FIXTURE(ConfigFixture, "config rejects corrupt and non-object input without rewriting it")
{
    std::string input;
    SUBCASE("corrupt") { input = "{broken"; }
    SUBCASE("array") { input = "[]"; }
    SUBCASE("null") { input = "null"; }
    write(input);
    kforge::util::ConfigStore config(path);
    CHECK(config.all().is_object());
    CHECK(config.all().empty());
    std::ifstream in(path);
    CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == input);
}

TEST_CASE_FIXTURE(ConfigFixture, "config bool values and invalid types respect caller default")
{
    write(R"({"yes":true,"no":false,"text_yes":"true","text_no":"0","invalid":"maybe","number":7})");
    kforge::util::ConfigStore config(path);
    CHECK(config.get_bool("yes"));
    CHECK_FALSE(config.get_bool("no", true));
    CHECK(config.get_bool("text_yes"));
    CHECK_FALSE(config.get_bool("text_no", true));
    CHECK(config.get_bool("invalid", true));
    CHECK(config.get_bool("number", true));
    CHECK(config.get("yes").empty());
}

TEST_CASE_FIXTURE(ConfigFixture, "config repeated writes survive reload")
{
    kforge::util::ConfigStore config(path);
    config.set("path", "first");
    config.set("path", "second");
    config.set_bool("enabled", true);
    kforge::util::ConfigStore reloaded(path);
    CHECK(reloaded.get("path") == "second");
    CHECK(reloaded.get_bool("enabled"));
    CHECK(reloaded.all() == config.all());
}

TEST_CASE_FIXTURE(ConfigFixture, "config write failure is visible and preserves memory and disk")
{
    write(R"({"path":"original"})");
    kforge::util::ConfigStore config(path);
    auto blocked = path;
    blocked += ".tmp";
    std::filesystem::create_directory(blocked);
    CHECK_THROWS(config.set("path", "changed"));
    CHECK(config.get("path") == "original");
    CHECK(kforge::util::ConfigStore(path).get("path") == "original");
    CHECK(std::filesystem::is_directory(blocked));
}

TEST_CASE_FIXTURE(ConfigFixture, "config failed replacement removes its temporary file")
{
    kforge::util::ConfigStore config(path);
    config.set("value", "before");
    auto backup = temp.path() / "saved.json";
    std::filesystem::rename(path, backup);
    std::filesystem::create_directory(path);
    CHECK_THROWS(config.set("value", "after"));
    CHECK(config.get("value") == "before");
    CHECK(kforge::util::ConfigStore(backup).get("value") == "before");
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}

TEST_CASE_FIXTURE(ConfigFixture, "config concurrent writes preserve all keys after reload")
{
    kforge::util::ConfigStore config(path);
    std::atomic<bool> failed = false;
    {
        std::vector<std::jthread> writers;
        for (int id = 0; id < 4; ++id)
            writers.emplace_back([&, id] {
                try {
                    for (int value = 0; value < 5; ++value)
                        config.set(std::to_string(id), std::to_string(value));
                } catch (...) { failed = true; }
            });
    }
    REQUIRE_FALSE(failed.load());
    kforge::util::ConfigStore reloaded(path);
    for (int id = 0; id < 4; ++id) CHECK(reloaded.get(std::to_string(id)) == "4");
}
