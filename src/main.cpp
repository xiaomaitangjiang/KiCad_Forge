#include "core/db/db_service.hpp"
#include "core/model/type_registry.h"
#include "interface/api/api_server.h"
#include "interface/api/import_manager.h"
#include "interface/manager/setup/launcher.hpp"
#include "platform/app_window.h"
#include "util/config_store.h"
#include "util/logger.h"
#include "util/platform.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

// --------------- main entry point ---------------

// Frontend sends heartbeats via recursive setTimeout (immune to browser
// background-tab throttling), so a 5s timeout is safe; the old 30s made the
// process linger for ~30s after the window closed.
static constexpr auto kHeartbeatTimeoutMs = 5000;

static int run_server()
{
    (void) setvbuf(stdout, nullptr, _IONBF, 0);

    using namespace kforge;

    // --- 初始化全部在 main：服务实例先构造 ---
    auto data_dir = util::get_data_dir();
    std::filesystem::create_directories(data_dir);

    util::ConfigStore cfg(data_dir + "/config.json");
    auto plugins = plugin::PluginManager::create_default();

    // ---- Launcher 编排：声明序 build（logger→db→…→apiserver，依赖注入）。
    //      CTAD：左值→T&（引用槽）、右值→T（右值槽入静态 store）
    kforge::launcher::Launcher l(util::Logger{},
                                 storage::DbService{data_dir + "/meta.db"},
                                 cfg,
                                 *plugins,
                                 api::ImportManager{},
                                 core::TypeRegistry::instance(),
                                 api::ApiServer{});

    auto r = l.launch_all();  // 经 Launcher 触发各服务 build；失败带服务类型
    if (!r.ok())
    {
        kforge::util::log_error{}("Launch failed: {}", r.result.error().format_message());
        return 1;
    }

    int PORT = l.get<api::ApiServer>().port();
    kforge::util::log_info{}("Server started on port {}", PORT);

    // 等待服务器响应
    {
        using std::chrono::milliseconds;
        using std::chrono::seconds;
        using std::chrono::steady_clock;
        auto deadline = steady_clock::now() + seconds(8);
        bool ready = false;
        while (steady_clock::now() < deadline)
        {
            httplib::Client cli("127.0.0.1", PORT);
            cli.set_connection_timeout(0, 200000);
            if (cli.Get("/api/status"))
            {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(milliseconds(200));
        }
        if (!ready)
        {
            kforge::util::log_error{}("Server did not respond within 8s");
            return 1;
        }
    }
    kforge::util::log_info{}("Server responding on http://127.0.0.1:{}", PORT);

    //打开应用界面, 界面关闭时关闭后端进程
    kforge::platform::WindowConfig win_cfg;
    win_cfg.url = "http://127.0.0.1:" + std::to_string(PORT);

    kforge::platform::NativeWindow win(win_cfg);
    if (!win.open())
    {
        kforge::util::log_error{}("Failed to open browser window");
        return 1;
    }
    kforge::util::log_info{}("Browser window opened");
    l.get<api::ImportManager>().start_async();  // 导入归 ImportManager（窗口后启动）
    win.monitor(
        [&]
        {
            return !l.get<api::ApiServer>().should_stop() &&
                   l.get<api::ApiServer>().ms_since_heartbeat() < kHeartbeatTimeoutMs;
        });
    kforge::util::log_info{}("Shutting down (heartbeat stopped or bye signal received)");
    // 栈析构：~Launcher → 自动调用destroy_all（忽略报错）
    return 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int ret = run_server();
    CoUninitialize();
    return ret;
}
#endif
int main()
{
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int ret = run_server();
    CoUninitialize();
    return ret;
#else
    return run_server();
#endif
}