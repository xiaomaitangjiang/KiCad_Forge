#include <doctest/doctest.h>

#include "interface/manager/setup/launcher.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using kforge::launcher::Launcher;
using kforge::launcher::failed_by;
using Result = kforge::util::Result<void>;

struct LifecycleFixture
{
    inline static std::vector<std::string> events;
    inline static int fail_build = -1;
    inline static int throw_build = -1;
    inline static int fail_destroy = -1;
    inline static int throw_destroy = -1;

    LifecycleFixture()
    {
        events.clear();
        fail_build = throw_build = fail_destroy = throw_destroy = -1;
    }
};

Result failure(const char* message)
{
    return std::unexpected(kforge::util::Error::make<
        kforge::util::Error::Kind::ServiceError>(message));
}

template <int Id>
struct Service
{
    static Result build()
    {
        LifecycleFixture::events.push_back("build" + std::to_string(Id));
        if (LifecycleFixture::throw_build == Id)
            throw std::runtime_error("build threw");
        return LifecycleFixture::fail_build == Id ? failure("build failed") : Result{};
    }
    static Result destroy()
    {
        LifecycleFixture::events.push_back("destroy" + std::to_string(Id));
        if (LifecycleFixture::throw_destroy == Id)
            throw std::runtime_error("destroy threw");
        return LifecycleFixture::fail_destroy == Id ? failure("destroy failed") : Result{};
    }
};
using A = Service<0>;
using B = Service<1>;
using C = Service<2>;

struct Unknown {};
struct WrongReturn { static int build(); };
struct NeedsUnknown { static Result build(Unknown&); };
template <class L, class S>
concept CanLaunch = requires(L& l) { l.template launch<S>(); };
template <class L, class S>
concept CanGet = requires(L& l) { l.template get<S>(); };
template <class L, class S>
concept CanDestroy = requires(L& l) { l.template destroy<S>(); };
}

TEST_CASE_FIXTURE(LifecycleFixture, "successful outcomes identify no failing service")
{
    Launcher l(A{}, B{});
    auto result = l.launch_all();
    REQUIRE(result.ok());
    CHECK_FALSE(failed_by<A>(result));
    CHECK_FALSE(failed_by<B>(result));
    result = l.destroy_all();
    CHECK_FALSE(failed_by<A>(result));
    CHECK_FALSE(failed_by<B>(result));
}

TEST_CASE_FIXTURE(LifecycleFixture, "destructor cleans all services in reverse despite failure or exception")
{
    SUBCASE("success") {}
    SUBCASE("returned error") { fail_destroy = 1; }
    SUBCASE("exception") { throw_destroy = 1; }
    {
        Launcher l(A{}, B{}, C{});
        REQUIRE(l.launch_all().ok());
        events.clear();
    }
    CHECK(events == std::vector<std::string>{"destroy2", "destroy1", "destroy0"});
}

TEST_CASE_FIXTURE(LifecycleFixture, "explicit cleanup is not repeated by destructor")
{
    {
        Launcher l(A{}, B{});
        REQUIRE(l.launch_all().ok());
        events.clear();
        CHECK(l.destroy<B>().ok());
        CHECK(l.destroy_all().ok());
        CHECK(l.destroy_all().ok());
    }
    CHECK(events == std::vector<std::string>{"destroy1", "destroy0"});
}

TEST_CASE_FIXTURE(LifecycleFixture, "fail-fast cleanup reports its location and destructor finishes remaining work")
{
    fail_destroy = 1;
    {
        Launcher l(A{}, B{}, C{});
        REQUIRE(l.launch_all().ok());
        events.clear();
        const auto result = l.destroy_all();
        REQUIRE_FALSE(result.ok());
        CHECK(failed_by<B>(result));
        CHECK(result.result.error().message() == "destroy failed");
        CHECK(events == std::vector<std::string>{"destroy2", "destroy1"});
    }
    CHECK(events == std::vector<std::string>{"destroy2", "destroy1", "destroy0"});
}

TEST_CASE_FIXTURE(LifecycleFixture, "startup failure stops later builds and only cleans attempted services")
{
    SUBCASE("first") { fail_build = 0; }
    SUBCASE("middle") { fail_build = 1; }
    SUBCASE("last") { fail_build = 2; }
    {
        Launcher l(A{}, B{}, C{});
        const auto result = l.launch_all();
        REQUIRE_FALSE(result.ok());
        CHECK(failed_by<A>(result) == (fail_build == 0));
        CHECK(failed_by<B>(result) == (fail_build == 1));
        CHECK(failed_by<C>(result) == (fail_build == 2));
        CHECK(result.result.error().message() == "build failed");
        CHECK(events.size() == static_cast<std::size_t>(fail_build + 1));
    }
    std::vector<std::string> expected;
    for (int i = 0; i <= fail_build; ++i) expected.push_back("build" + std::to_string(i));
    for (int i = fail_build; i >= 0; --i) expected.push_back("destroy" + std::to_string(i));
    CHECK(events == expected);
}

TEST_CASE_FIXTURE(LifecycleFixture, "unstarted services need no cleanup")
{
    { Launcher l(A{}, B{}); }
    CHECK(events.empty());
}

TEST_CASE_FIXTURE(LifecycleFixture, "move transfers cleanup responsibility and keeps instance address")
{
    std::optional<Launcher<A>> destination;
    {
        Launcher source(A{});
        REQUIRE(source.launch_all().ok());
        const auto* address = &source.get<A>();
        destination.emplace(std::move(source));
        CHECK(&destination->get<A>() == address);
        events.clear();
    }
    CHECK(events.empty());
    destination.reset();
    CHECK(events == std::vector<std::string>{"destroy0"});
}

TEST_CASE_FIXTURE(LifecycleFixture, "move assignment releases destination and transfers cleanup")
{
    // Reference slots avoid overwriting the per-type owned storage.
    A first, second;
    {
        Launcher destination(first);
        Launcher source(second);
        REQUIRE(destination.launch_all().ok());
        REQUIRE(source.launch_all().ok());
        events.clear();
        destination = std::move(source);
        CHECK(events == std::vector<std::string>{"destroy0"});
        CHECK(&destination.get<A>() == &second);
    }
    CHECK(events == std::vector<std::string>{"destroy0", "destroy0"});
}

TEST_CASE_FIXTURE(LifecycleFixture, "successful relaunch restores cleanup responsibility")
{
    {
        Launcher l(A{});
        REQUIRE(l.launch_all().ok());
        REQUIRE(l.destroy_all().ok());
        REQUIRE(l.launch_all().ok());
    }
    CHECK(events == std::vector<std::string>{"build0", "destroy0", "build0", "destroy0"});
}

TEST_CASE("launcher constraints reject invalid service operations")
{
    CHECK_FALSE((CanLaunch<Launcher<A>, Unknown>));
    CHECK_FALSE((CanGet<Launcher<A>, Unknown>));
    CHECK_FALSE((CanDestroy<Launcher<A>, Unknown>));
    CHECK_FALSE((CanLaunch<Launcher<WrongReturn>, WrongReturn>));
    CHECK_FALSE((CanLaunch<Launcher<NeedsUnknown>, NeedsUnknown>));
    CHECK((CanLaunch<Launcher<A>, A>));
}

TEST_CASE_FIXTURE(LifecycleFixture, "throwing build still cleans partial resources")
{
    throw_build = 1;
    {
        Launcher l(A{}, B{}, C{});
        CHECK_THROWS_AS(l.launch_all(), std::runtime_error);
    }
    CHECK(events == std::vector<std::string>{"build0", "build1", "destroy1", "destroy0"});
}

TEST_CASE_FIXTURE(LifecycleFixture, "explicit throwing destroy is not retried during unwinding")
{
    throw_destroy = 1;
    {
        Launcher l(A{}, B{});
        REQUIRE(l.launch_all().ok());
        events.clear();
        CHECK_THROWS_AS(l.destroy_all(), std::runtime_error);
    }
    CHECK(events == std::vector<std::string>{"destroy1", "destroy0"});
}

TEST_CASE_FIXTURE(LifecycleFixture, "self move assignment keeps cleanup responsibility")
{
    {
        Launcher l(A{});
        REQUIRE(l.launch_all().ok());
        auto* alias = &l;
        l = std::move(*alias);
        CHECK(events == std::vector<std::string>{"build0"});
    }
    CHECK(events == std::vector<std::string>{"build0", "destroy0"});
}
