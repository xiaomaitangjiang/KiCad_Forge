#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/db/db_service.hpp"
#include "interface/api/import_manager.h"
#include "interface/manager/setup/launcher.hpp"
#include "support/test_environment.h"

#include <spdlog/sinks/callback_sink.h>

namespace
{
using kforge::launcher::Launcher;
using kforge::launcher::failed_by;
using kforge::storage::DbService;
using kforge::api::ImportManager;
using Result = kforge::util::Result<void>;

struct TestLogger
{
    inline static std::vector<std::string> messages;
    static Result build()
    {
        messages.clear();
        auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [](const spdlog::details::log_msg& message) {
                messages.emplace_back(message.payload.data(), message.payload.size());
            });
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("startup-test", sink));
        return {};
    }
};

struct ApiProbe
{
    inline static bool started = false;
    inline static DbService* database = nullptr;
    inline static bool database_alive_at_shutdown = false;
    static Result build(ImportManager&, DbService& db)
    {
        started = true;
        database = &db;
        return {};
    }
    static Result destroy()
    {
        database_alive_at_shutdown = database && database->handle();
        return {};
    }
};

struct StartupFixture
{
    test_support::TempDirectory temp;
    test_support::RestoreDefaultLogger restore;
    StartupFixture()
    {
        ApiProbe::started = false;
        ApiProbe::database = nullptr;
        ApiProbe::database_alive_at_shutdown = false;
    }
    ~StartupFixture()
    {
        kforge::launcher::util::instance_store<ImportManager>().reset();
        kforge::launcher::util::instance_store<DbService>().reset();
    }
};
}

TEST_CASE_FIXTURE(StartupFixture, "DB failure is logged and prevents import and API startup")
{
    // An existing directory cannot be opened as an SQLite database file.
    kforge::util::ConfigStore config(temp.path() / "config.json");
    {
        Launcher l(TestLogger{}, DbService{temp.path().string()}, config,
                   ImportManager{}, ApiProbe{});
        const auto result = l.launch_all();
        REQUIRE_FALSE(result.ok());
        CHECK(failed_by<DbService>(result));
        CHECK(result.result.error().kind() == kforge::util::Error::Kind::DbError);
        CHECK(l.get<DbService>().handle() == nullptr);
        CHECK(l.get<ImportManager>().orchestrator() == nullptr);
        CHECK_FALSE(ApiProbe::started);
        REQUIRE(TestLogger::messages.size() == 1);
        CHECK(TestLogger::messages.front().find("Database open failed:") != std::string::npos);
    }
    CHECK_FALSE(ApiProbe::started);
}

TEST_CASE_FIXTURE(StartupFixture, "uninitialized import manager can be stopped repeatedly")
{
    ImportManager manager;
    CHECK_NOTHROW(manager.stop_async());
    CHECK_NOTHROW(manager.stop_async());
}

TEST_CASE_FIXTURE(StartupFixture, "DB stays alive for dependents and closes on launcher exit")
{
    kforge::util::ConfigStore config(temp.path() / "config.json");
    {
        Launcher l(TestLogger{}, DbService{(temp.path() / "test.db").string()}, config,
                   ImportManager{}, ApiProbe{});
        REQUIRE(l.launch_all().ok());
        CHECK(l.get<DbService>().handle() != nullptr);
        CHECK(l.get<ImportManager>().orchestrator() != nullptr);
        CHECK(ApiProbe::started);
    }
    CHECK(ApiProbe::database_alive_at_shutdown);
    CHECK(kforge::launcher::util::instance_store<DbService>()->handle() == nullptr);
}
