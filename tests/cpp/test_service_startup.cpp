#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/db/db_service.hpp"
#include "core/model/type_registry.h"
#include "interface/api/import_manager.h"
#include "interface/api/type_matching.h"
#include "interface/manager/setup/launcher.hpp"
#include "../support/test_environment.h"
#include "util/lifecycle.h"

#include <spdlog/sinks/callback_sink.h>
#include <fstream>
#include <iterator>
#include <optional>

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
        database_alive_at_shutdown = database && database->db != nullptr;
        return {};
    }
};

struct StartupFixture
{
    test_support::TempDirectory temp;
    test_support::RestoreDefaultLogger restore;
    kforge::classifier::TypeMatchingStore matching_store;
    StartupFixture()
    {
        const auto path = temp.path() / "matching.json";
        // 写文件成功是后面 load 成功的前提：模板表达式里的临时流在语句末关闭，
        // 显式判 good() 免得夹具初始化静默失败。
        std::ofstream out(path);
        out << R"({"schema_version":1,"revision":0,"rules":[],"exceptions":[]})";
        out.close();
        REQUIRE(out.good());
        REQUIRE(matching_store.load(path.string()).has_value());
        // 空规则配置同样是健康配置（有有效快照才允许业务领取）
        REQUIRE(matching_store.acquire_business_snapshot().has_value());
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
                   ImportManager{&matching_store}, ApiProbe{});
        const auto result = l.launch_all();
        REQUIRE_FALSE(result.ok());
        CHECK(failed_by<DbService>(result));
        CHECK(result.result.error().kind() == kforge::util::Error::Kind::DbError);
        CHECK(l.get<DbService>().db == nullptr);
        CHECK(l.get<ImportManager>().orchestrator() == nullptr);
        CHECK_FALSE(ApiProbe::started);
        REQUIRE(TestLogger::messages.size() == 1);
        CHECK(TestLogger::messages.front().find("Database open failed:") != std::string::npos);
    }
    CHECK_FALSE(ApiProbe::started);
}

TEST_CASE_FIXTURE(StartupFixture, "heartbeat timeout never kills a minimized window")
{
    using kforge::util::frontend_alive;
    using kforge::util::kHeartbeatTimeoutMs;

    // 窗口可见 + 心跳过期 → 判定前端已死（兜底路径仍然有效）
    CHECK_FALSE(frontend_alive(kHeartbeatTimeoutMs + 1, false));
    CHECK(frontend_alive(kHeartbeatTimeoutMs - 1, false));
    CHECK(frontend_alive(0, false));

    // 最小化时 WebView 会节流定时器：无论多久没心跳都不算死
    CHECK(frontend_alive(kHeartbeatTimeoutMs + 1, true));
    CHECK(frontend_alive(60 * 60 * 1000, true));

    // 超时必须比任何正常阻塞（导入、全库分类）都长
    CHECK(kHeartbeatTimeoutMs >= 60000);
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
                   ImportManager{&matching_store}, ApiProbe{});
        REQUIRE(l.launch_all().ok());
        CHECK(l.get<DbService>().db != nullptr);
        CHECK(l.get<ImportManager>().orchestrator() != nullptr);
        CHECK(ApiProbe::started);
    }
    CHECK(ApiProbe::database_alive_at_shutdown);
    CHECK(kforge::launcher::util::instance_store<DbService>()->db == nullptr);
}

namespace
{
using kforge::api::TypeMatchingContext;
using kforge::api::TypeMatchingStartup;

// 代替会访问真实用户目录的 TypeRegistry::build；存在性回调仍参与真正的配置编译。
struct TypeReadyProbe
{
    inline static bool ready = false;
    inline static int validations = 0;
    inline static int validations_before_ready = 0;
    inline static bool resistor_validated = false;

    static Result build(DbService& db)
    {
        if (!db.db)
            return std::unexpected(kforge::util::Error::make<
                kforge::util::Error::Kind::ServiceError>("Type probe needs an open database"));
        ready = true;
        return {};
    }
    static Result destroy()
    {
        ready = false;
        return {};
    }
    static bool type_exists(const std::string& id)
    {
        ++validations;
        if (!ready) ++validations_before_ready;
        if (id == "Resistor") resistor_validated = true;
        return ready && kforge::core::is_builtin_component_id(id);
    }
};

// 探针调用正式共享 helper，证明 Launcher 装配和 API 业务接线；不监听 HTTP。
struct MatchingApiProbe
{
    inline static bool started = false;
    inline static DbService* database = nullptr;
    inline static bool database_alive_at_shutdown = false;
    inline static std::optional<TypeMatchingContext> context;
    inline static nlohmann::json initial_state;

    static Result build(ImportManager&, DbService& db)
    {
        auto configured = kforge::api::configured_type_matching_context(db.database());
        if (!configured) return std::unexpected(configured.error());
        context.emplace(*configured);
        initial_state = kforge::api::type_matching_get(*context);
        database = &db;
        started = true;
        return {};
    }
    static Result destroy()
    {
        database_alive_at_shutdown = database && database->db != nullptr;
        context.reset();
        return {};
    }
};

struct MatchingStartupFixture
{
    test_support::TempDirectory temp;
    test_support::RestoreDefaultLogger restore;
    // 初始状态保持未加载，测试必须由真实 TypeMatchingStartup::build 完成首次加载。
    kforge::classifier::TypeMatchingStore matching_store{TypeReadyProbe::type_exists};

    MatchingStartupFixture()
    {
        reset_slots();
        TypeReadyProbe::ready = false;
        TypeReadyProbe::validations = 0;
        TypeReadyProbe::validations_before_ready = 0;
        TypeReadyProbe::resistor_validated = false;
        MatchingApiProbe::started = false;
        MatchingApiProbe::database = nullptr;
        MatchingApiProbe::database_alive_at_shutdown = false;
        MatchingApiProbe::initial_state = nullptr;
    }
    ~MatchingStartupFixture() { reset_slots(); }

    static void reset_slots()
    {
        // 依赖先释放；启动槽位必须在本地 matching_store 析构前清掉。
        MatchingApiProbe::context.reset();
        kforge::launcher::util::instance_store<MatchingApiProbe>().reset();
        kforge::launcher::util::instance_store<ImportManager>().reset();
        kforge::launcher::util::instance_store<TypeMatchingStartup>().reset();
        kforge::launcher::util::instance_store<TypeReadyProbe>().reset();
        kforge::launcher::util::instance_store<DbService>().reset();
        kforge::launcher::util::instance_store<TestLogger>().reset();
    }
    std::filesystem::path matching_path() const { return temp.path() / "component_type_matching.json"; }
    static std::string config_bytes(std::uint64_t revision)
    {
        return nlohmann::json{
            {"schema_version", 1}, {"revision", revision},
            {"rules", nlohmann::json::array({{
                {"id", "startup-resistor"}, {"enabled", true}, {"field", "reference"},
                {"pattern", "^R[0-9]+$"}, {"case_sensitive", false},
                {"priority", 0}, {"type_id", "Resistor"}}})},
            {"exceptions", nlohmann::json::array()}}.dump();
    }
    void write_matching(const std::string& bytes)
    {
        std::ofstream out(matching_path(), std::ios::binary);
        out << bytes;
        out.close();
        REQUIRE(out.good());
    }
    std::string read_matching() const
    {
        std::ifstream in(matching_path(), std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }
};
}  // namespace

TEST_CASE_FIXTURE(MatchingStartupFixture, "U04-B2 startup adapter construction does no IO and missing assembly fails")
{
    auto db = kforge::storage::Database::open(temp.path() / "probe.db");
    REQUIRE(db.has_value());
    auto missing = kforge::api::configured_type_matching_context(**db);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().format_message().find("assembly error") != std::string::npos);

    auto& slot = kforge::launcher::util::instance_store<TypeMatchingStartup>();
    slot.emplace(matching_store, matching_path().string());
    CHECK_FALSE(slot->attempted);
    CHECK_FALSE(matching_store.has_snapshot());
    CHECK(matching_store.path().empty());
    CHECK_FALSE(std::filesystem::exists(matching_path()));
    CHECK(TypeReadyProbe::validations == 0);
    auto unattempted = kforge::api::configured_type_matching_context(**db);
    REQUIRE_FALSE(unattempted.has_value());
    CHECK(unattempted.error().format_message().find("not attempted") != std::string::npos);
}

TEST_CASE_FIXTURE(MatchingStartupFixture, "U04-B2 startup loads the existing config after types and before API helpers")
{
    const auto bytes = config_bytes(7);
    write_matching(bytes);
    kforge::util::ConfigStore config(temp.path() / "config.json");
    {
        Launcher l(TestLogger{}, DbService{(temp.path() / "test.db").string()}, config,
                   TypeReadyProbe{}, TypeMatchingStartup{matching_store, matching_path().string()},
                   ImportManager{&matching_store}, MatchingApiProbe{});
        CHECK_FALSE(l.get<TypeMatchingStartup>().attempted);
        CHECK_FALSE(matching_store.has_snapshot());
        CHECK(TypeReadyProbe::validations == 0);
        REQUIRE(l.launch_all().ok());
        CHECK(l.get<TypeMatchingStartup>().attempted);
        CHECK(TypeReadyProbe::ready);
        CHECK(TypeReadyProbe::validations > 0);
        CHECK(TypeReadyProbe::validations_before_ready == 0);
        CHECK(TypeReadyProbe::resistor_validated);
        CHECK(l.get<ImportManager>().orchestrator() != nullptr);
        REQUIRE(MatchingApiProbe::started);
        REQUIRE(MatchingApiProbe::context.has_value());
        CHECK(MatchingApiProbe::context->configured_path == matching_path().string());
        CHECK(MatchingApiProbe::initial_state.at("ok") == true);
        CHECK(MatchingApiProbe::initial_state.at("revision") == 7);
        CHECK(MatchingApiProbe::initial_state.at("has_snapshot") == true);
        CHECK(matching_store.path() == matching_path().string());
        CHECK(matching_store.load_error().empty());
        auto lease = matching_store.acquire_business_snapshot();
        REQUIRE(lease.has_value());
        kforge::classifier::MatchSample sample;
        sample.reference = "R42";
        CHECK((*lease)->evaluate(sample).type_id == "Resistor");
        CHECK(read_matching() == bytes);
    }
    CHECK(MatchingApiProbe::database_alive_at_shutdown);
    CHECK_FALSE(MatchingApiProbe::context.has_value());
    CHECK(kforge::launcher::util::instance_store<DbService>()->db == nullptr);
}

TEST_CASE_FIXTURE(MatchingStartupFixture, "U04-B2 startup installs the missing-file seed and binds the explicit path")
{
    REQUIRE_FALSE(std::filesystem::exists(matching_path()));
    kforge::util::ConfigStore config(temp.path() / "config.json");
    {
        Launcher l(TestLogger{}, DbService{(temp.path() / "test.db").string()}, config,
                   TypeReadyProbe{}, TypeMatchingStartup{matching_store, matching_path().string()},
                   ImportManager{&matching_store}, MatchingApiProbe{});
        REQUIRE(l.launch_all().ok());
        CHECK(l.get<TypeMatchingStartup>().attempted);
        CHECK(TypeReadyProbe::validations_before_ready == 0);
        CHECK(TypeReadyProbe::resistor_validated);
        REQUIRE(MatchingApiProbe::started);
        CHECK(MatchingApiProbe::initial_state.at("ok") == true);
        CHECK(MatchingApiProbe::initial_state.at("revision") == 0);
        CHECK(matching_store.path() == matching_path().string());
        auto lease = matching_store.acquire_business_snapshot();
        REQUIRE(lease.has_value());
        CHECK(nlohmann::json::parse(kforge::classifier::matching_config_to_json((*lease)->config()))
              == nlohmann::json::parse(kforge::classifier::default_matching_config_json()));
        // 首次 load 只安装内存默认值；普通保存才负责持久化。
        CHECK_FALSE(std::filesystem::exists(matching_path()));
    }
    CHECK(MatchingApiProbe::database_alive_at_shutdown);
}

TEST_CASE_FIXTURE(MatchingStartupFixture, "U04-B2 startup bad initial JSON keeps API helpers open and reload repairs the configured path")
{
    const std::string bad_bytes = "{broken startup matching config\n";
    write_matching(bad_bytes);
    kforge::util::ConfigStore config(temp.path() / "config.json");
    {
        Launcher l(TestLogger{}, DbService{(temp.path() / "test.db").string()}, config,
                   TypeReadyProbe{}, TypeMatchingStartup{matching_store, matching_path().string()},
                   ImportManager{&matching_store}, MatchingApiProbe{});
        REQUIRE(l.launch_all().ok());
        CHECK(l.get<TypeMatchingStartup>().attempted);
        CHECK(l.get<ImportManager>().orchestrator() != nullptr);
        REQUIRE(MatchingApiProbe::started);
        REQUIRE(MatchingApiProbe::context.has_value());
        CHECK(MatchingApiProbe::initial_state.at("ok") == false);
        CHECK(MatchingApiProbe::initial_state.at("has_snapshot") == false);
        CHECK_FALSE(MatchingApiProbe::initial_state.at("load_error").get<std::string>().empty());
        CHECK_FALSE(MatchingApiProbe::initial_state.contains("rules"));
        CHECK(matching_store.path().empty());
        CHECK_FALSE(matching_store.acquire_business_snapshot().has_value());
        CHECK(read_matching() == bad_bytes);
        bool load_error_logged = false;
        for (const auto& message : TestLogger::messages)
            if (message.find("Type matching config load failed") != std::string::npos &&
                message.find(matching_path().string()) != std::string::npos)
                load_error_logged = true;
        CHECK(load_error_logged);

        const auto repaired_bytes = config_bytes(11);
        write_matching(repaired_bytes);
        auto reloaded = kforge::api::type_matching_reload(
            *MatchingApiProbe::context, nlohmann::json::object());
        REQUIRE(reloaded.at("ok") == true);
        CHECK(reloaded.at("revision") == 11);
        CHECK(reloaded.at("has_snapshot") == true);
        CHECK(reloaded.at("load_error") == "");
        CHECK(matching_store.path() == MatchingApiProbe::context->configured_path);
        CHECK(matching_store.path() == matching_path().string());
        CHECK(TypeReadyProbe::validations_before_ready == 0);
        CHECK(TypeReadyProbe::resistor_validated);
        CHECK(read_matching() == repaired_bytes);
        auto lease = matching_store.acquire_business_snapshot();
        REQUIRE(lease.has_value());
        kforge::classifier::MatchSample sample;
        sample.reference = "R42";
        CHECK((*lease)->evaluate(sample).type_id == "Resistor");
    }
    CHECK(MatchingApiProbe::database_alive_at_shutdown);
}

TEST_CASE_FIXTURE(MatchingStartupFixture, "U04-B2 startup DB failure prevents matching load and API startup")
{
    const auto bytes = config_bytes(7);
    write_matching(bytes);
    kforge::util::ConfigStore config(temp.path() / "config.json");
    {
        Launcher l(TestLogger{}, DbService{temp.path().string()}, config,
                   TypeReadyProbe{}, TypeMatchingStartup{matching_store, matching_path().string()},
                   ImportManager{&matching_store}, MatchingApiProbe{});
        const auto result = l.launch_all();
        REQUIRE_FALSE(result.ok());
        CHECK(failed_by<DbService>(result));
        CHECK_FALSE(TypeReadyProbe::ready);
        CHECK_FALSE(l.get<TypeMatchingStartup>().attempted);
        CHECK(TypeReadyProbe::validations == 0);
        CHECK_FALSE(matching_store.has_snapshot());
        CHECK(matching_store.path().empty());
        CHECK(l.get<ImportManager>().orchestrator() == nullptr);
        CHECK_FALSE(MatchingApiProbe::started);
        CHECK_FALSE(MatchingApiProbe::context.has_value());
        CHECK(read_matching() == bytes);
    }
    CHECK_FALSE(MatchingApiProbe::started);
}
