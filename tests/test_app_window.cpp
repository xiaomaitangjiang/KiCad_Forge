#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/platform/app_window.h"

#include <type_traits>

namespace
{

using kforge::platform::NativeWindow;
using kforge::platform::WindowConfig;

struct FrontendAlive
{
    bool operator()() { return false; }
};

// These calls instantiate the real platform implementation, including the
// background-thread body on Windows. Never execute them: this regression must
// not open a browser, run a GUI event loop, or wait for a heartbeat.
[[maybe_unused]] void instantiate_monitor_overloads(NativeWindow& window)
{
    FrontendAlive alive;
    window.monitor(alive);
    window.monitor(FrontendAlive{});
}

static_assert(std::is_constructible_v<NativeWindow, WindowConfig>);

}  // namespace

TEST_CASE("native window can be constructed and closed without opening a GUI")
{
    NativeWindow window(WindowConfig{"http://127.0.0.1:1", "Unopened test window", 640, 480});
    CHECK_FALSE(window.is_minimized());
    window.close();
    window.close();
    CHECK_FALSE(window.is_minimized());
}

#if defined(__APPLE__) || defined(__linux__)
TEST_CASE("unopened Unix browser window monitor does not wait for a process")
{
    NativeWindow window(WindowConfig{});
    window.monitor();
    CHECK_FALSE(window.is_minimized());
}
#endif
