// Launcher 单元测试（doctest）：构造期 CTAD 绑定（引用槽/值槽）、build 即 start、
// 依赖注入、fail-fast 错误透传、移动语义、server_result 类型判定
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/interface/manager/setup/launcher.hpp"
#include "../src/util/logger.h"

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using kforge::launcher::Launcher;
using kforge::launcher::Server;
using kforge::launcher::failed_by;
using Result = kforge::util::Result<void>;

namespace
{

std::vector<std::string> events;

// ---- 桩类型 ----

// 纯依赖对象（无 build，不参与 launch）
struct Db : Server<Db>
{
    int conn = 1;
};

struct Logger : Server<Logger>
{
    int level = 2;
};

// build 即 start 的服务
struct HttpServer : Server<HttpServer>
{
    inline static const Db* db_seen = nullptr;
    static Result build(Db& db)
    {
        db_seen = &db;
        events.push_back("HttpServer.build");
        return {};
    }
};

struct Pipeline : Server<Pipeline>
{
    inline static const HttpServer* http_seen = nullptr;
    inline static const Db* db_seen = nullptr;
    static Result build(HttpServer& http, Db& db)
    {
        http_seen = &http;
        db_seen = &db;
        events.push_back("Pipeline.build");
        return {};
    }
};

struct FailsBuild : Server<FailsBuild>
{
    static Result build()
    {
        events.push_back("FailsBuild.build");
        return std::unexpected(
            kforge::util::Error::make<kforge::util::Error::Kind::ServiceError>("boom"));
    }
};

struct Untouched : Server<Untouched>
{
    static Result build()
    {
        events.push_back("Untouched.build");
        return {};
    }
};

// 槽位空间回归：大服务实例经引用槽绑定，槽位恒定两个指针宽
struct BigSvc : Server<BigSvc>
{
    char payload[4096];
};

static_assert(sizeof(Launcher<BigSvc&>) == 2 * sizeof(void*),
              "slot footprint fixed: two pointer widths regardless of service size");

// 不继承 Server 的裸依赖：验证继承契约已放开（owned_tag 仅作类型标签）
struct RawDep
{
    int n = 7;
};

// 可销毁服务（供 destroy 用例）
struct ManagedA : Server<ManagedA>
{
    static Result destroy()
    {
        events.push_back("A.destroy");
        return {};
    }
};

struct ManagedB : Server<ManagedB>
{
    static Result destroy()
    {
        events.push_back("B.destroy");
        return {};
    }
};

struct FailedDestroy : Server<FailedDestroy>
{
    static Result destroy()
    {
        events.push_back("FailedDestroy.destroy");
        return std::unexpected(
            kforge::util::Error::make<kforge::util::Error::Kind::ServiceError>("destroy boom"));
    }
};

}  // namespace

// —— 编译期负例（doctest 无法表达，保留 #if 0 手法手动验证）——
#if 0
static void negative_cases()
{
    struct Unknown
    {
    };
    Db db;
    Launcher<Db&> l(db);
    l.launch<Unknown>();    // concept 失败: 未注册（RegisteredSlot）
    l.get<Unknown>();       // concept 失败: 未注册（RegisteredSlot）
    l.launch<Db>();         // concept 失败: 无静态 build（Buildable）

    struct WrongReturn
    {
        static int build()
        {
            return 0;
        }
    };
    Launcher<WrongReturn> w(WrongReturn{});
    w.launch<WrongReturn>();  // concept 失败: build 必须返回 Result<void>（BuildableService）

    Launcher<Db&, Db&> dup(db, db);  // TaggedTuple requires 失败: 标签重复
}
#endif

TEST_CASE("CTAD binding: lvalue → ref slot, rvalue → value slot")
{
    Db db;
    Launcher l(db, HttpServer{});  // CTAD → Launcher<Db&, HttpServer>
    static_assert(std::is_same_v<decltype(l), Launcher<Db&, HttpServer>>);
    static_assert(std::is_lvalue_reference_v<decltype(l.get<HttpServer>())>);

    CHECK(&l.get<Db>() == &db);
}

TEST_CASE("launch_all: declaration-order build with dependency injection")
{
    events.clear();
    Db db;
    Launcher<Db&, Logger&, HttpServer, Pipeline> l(db, Logger{}, HttpServer{}, Pipeline{});
    HttpServer* http_slot = &l.get<HttpServer>();  // launch 前实例已在槽内
    auto r = l.launch_all();
    CHECK(r.ok());
    CHECK(events == std::vector<std::string>({"HttpServer.build", "Pipeline.build"}));
    CHECK(HttpServer::db_seen == &db);
    CHECK(Pipeline::http_seen == http_slot);
    CHECK(Pipeline::http_seen == &l.get<HttpServer>());
    CHECK(Pipeline::db_seen == &db);
}

TEST_CASE("non-Buildable pure-dependency slots are skipped")
{
    events.clear();
    Db db;
    Logger log;
    Launcher<Db&, Logger&> l(db, log);
    CHECK(l.launch_all().ok());
    CHECK(events.empty());
}

TEST_CASE("launch_all fail-fast with failing-service type tag")
{
    events.clear();
    Launcher<FailsBuild, Untouched> l(FailsBuild{}, Untouched{});
    auto r = l.launch_all();
    CHECK_FALSE(r.ok());
    CHECK(failed_by<FailsBuild>(r));
    CHECK_FALSE(failed_by<Untouched>(r));
    CHECK(r.result.error().kind() == kforge::util::Error::Kind::ServiceError);
    CHECK_EQ(r.result.error().message(), "boom");
    CHECK(events == std::vector<std::string>({"FailsBuild.build"}));
}

TEST_CASE("single launch injects deps")
{
    events.clear();
    Db db;
    Launcher<Db&, HttpServer> l(db, HttpServer{});
    CHECK(l.launch<HttpServer>().ok());
    CHECK(HttpServer::db_seen == &db);
    CHECK(events == std::vector<std::string>({"HttpServer.build"}));
}

TEST_CASE("move semantics: ref binding kept, owned slot follows")
{
    events.clear();
    Db db;
    Launcher<Db&, HttpServer> a(db, HttpServer{});
    Launcher<Db&, HttpServer> b(std::move(a));
    CHECK(&b.get<Db>() == &db);
    auto r = b.launch_all();
    CHECK(r.ok());
    CHECK(HttpServer::db_seen == &db);
}

TEST_CASE("owned instance address stable across launcher move (static storage)")
{
    events.clear();
    Db db;
    Launcher<Db&, Logger&, HttpServer> a(db, Logger{}, HttpServer{});
    CHECK(a.get<Logger>().level == 2);
    Db* dbp = &a.get<Db>();
    HttpServer* hp = &a.get<HttpServer>();
    Launcher<Db&, Logger&, HttpServer> b(std::move(a));
    CHECK(&b.get<Db>() == dbp);
    CHECK(&b.get<HttpServer>() == hp);
    CHECK(b.get<Logger>().level == 2);
    auto r = b.launch_all();
    CHECK(r.ok());
    CHECK(HttpServer::db_seen == &db);
}

TEST_CASE("raw (non-Server) dep usable — inheritance not forced")
{
    RawDep raw;
    Launcher<RawDep&> lr(raw);
    CHECK_EQ(lr.get<RawDep>().n, 7);
}

TEST_CASE("destroy_all reverse declaration order")
{
    events.clear();
    Launcher<ManagedA, ManagedB, Untouched> l(ManagedA{}, ManagedB{}, Untouched{});
    auto dr = l.destroy_all();
    CHECK(dr.ok());
    CHECK(events == std::vector<std::string>({"B.destroy", "A.destroy"}));
}

TEST_CASE("single destroy calls only the named service")
{
    events.clear();
    Launcher<ManagedA, ManagedB> ls(ManagedA{}, ManagedB{});
    CHECK(ls.destroy<ManagedA>().ok());
    CHECK(events == std::vector<std::string>({"A.destroy"}));
}

TEST_CASE("destroy_all fail-fast with failing-service type tag")
{
    events.clear();
    Launcher<ManagedA, FailedDestroy> lf(ManagedA{}, FailedDestroy{});
    auto rf = lf.destroy_all();
    CHECK_FALSE(rf.ok());
    CHECK(failed_by<FailedDestroy>(rf));
    CHECK_FALSE(failed_by<ManagedA>(rf));
    CHECK_EQ(rf.result.error().message(), "destroy boom");
    CHECK(events == std::vector<std::string>({"FailedDestroy.destroy"}));
}