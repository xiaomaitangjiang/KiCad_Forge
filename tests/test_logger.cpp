// Tests for kforge::util logging: level mapping, formatting, and
// source_location correctness (must point at the call site, not logger.h).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/util/logger.h"
#include "support/test_environment.h"

#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

namespace
{
struct Entry
{
    std::string payload;
    spdlog::level::level_enum lvl;
    int line;
    std::string file;
};

std::vector<Entry> g_msgs;

// Must install a default logger before any log call — spdlog dereferences
// default_logger_raw() which is null until set.
void capture()
{
    g_msgs.clear();
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [](const spdlog::details::log_msg& m)
        {
            g_msgs.push_back({std::string(m.payload.data(), m.payload.size()),
                              m.level, m.source.line,
                              m.source.filename != nullptr ? m.source.filename : ""});
        });
    auto logger = std::make_shared<spdlog::logger>("test", sink);
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
}
}  // namespace

struct LoggerFixture
{
    test_support::RestoreDefaultLogger restore;
    LoggerFixture() { capture(); }
};

TEST_CASE_FIXTURE(LoggerFixture, "level mapping + payload")
{
    kforge::util::log_debug{}("d");
    kforge::util::log_info{}("i");
    kforge::util::log_warn{}("w");
    kforge::util::log_error{}("e");
    REQUIRE_EQ(g_msgs.size(), 4u);
    CHECK(g_msgs[0].lvl == spdlog::level::debug);
    CHECK(g_msgs[1].lvl == spdlog::level::info);
    CHECK(g_msgs[2].lvl == spdlog::level::warn);
    CHECK(g_msgs[3].lvl == spdlog::level::err);
    CHECK_EQ(g_msgs[0].payload, "d");
    CHECK_EQ(g_msgs[1].payload, "i");
    CHECK_EQ(g_msgs[2].payload, "w");
    CHECK_EQ(g_msgs[3].payload, "e");
}

TEST_CASE_FIXTURE(LoggerFixture, "formatting with args")
{
    kforge::util::log_info{}("payload {} {}", 1, "x");
    REQUIRE_FALSE(g_msgs.empty());
    CHECK_EQ(g_msgs.back().payload, "payload 1 x");
}

TEST_CASE_FIXTURE(LoggerFixture, "error location == call site")
{
    int expected = __LINE__;
    kforge::util::log_error{}("loc marker {}", 7);  // must stay directly below
    REQUIRE_FALSE(g_msgs.empty());
    auto& e = g_msgs.back();
    CHECK_EQ(e.line, expected + 1);
    CHECK(e.file.find("test_logger.cpp") != std::string::npos);
    CHECK(e.file.find("logger.h") == std::string::npos);
}

TEST_CASE_FIXTURE(LoggerFixture, "info has no location")
{
    kforge::util::log_info{}("plain");
    REQUIRE_FALSE(g_msgs.empty());
    CHECK_EQ(g_msgs.back().line, 0);
    CHECK(g_msgs.back().file.empty());
}

TEST_CASE_FIXTURE(LoggerFixture, "zero-arg call")
{
    kforge::util::log_warn{}("bare");
    REQUIRE_FALSE(g_msgs.empty());
    CHECK_EQ(g_msgs.back().payload, "bare");
}
