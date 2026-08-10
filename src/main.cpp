#include "httplib.h"
#include "api/api_server.h"
#include "platform/app_window.h"
#include "util/logger.h"

#include <chrono>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

// --------------- main entry point ---------------

static int run_server()
{
    (void) setvbuf(stdout, nullptr, _IONBF, 0);
    // 关闭输出缓冲区

    // 0 = 让 OS 自动分配空闲端口
    kforge::api::ApiServer server(0);
    if (!server.start())
    {
        LOG_ERROR("Server start failed");
        return 1;
    }
    int PORT = server.port();
    LOG_INFO("Server started on port {}", PORT);

    //等待服务器响应
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
            LOG_ERROR("Server did not respond within 8s");
            return 1;
        }
    }
    LOG_INFO("Server responding on http://127.0.0.1:{}", PORT);

    //打开应用界面, 界面关闭时关闭后端进程.
    kforge::platform::WindowConfig cfg;
    cfg.url = "http://127.0.0.1:" + std::to_string(PORT);

    kforge::platform::NativeWindow win(cfg);
    if (!win.open())
    {
        LOG_ERROR("Failed to open browser window");
        return 1;
    }
    LOG_INFO("Browser window opened");
    win.monitor(
        [&]
        {
            return !server.should_stop() && server.ms_since_heartbeat() < 30000;
        });  // 30s 心跳超时 — 前端递归 setTimeout 不受浏览器节流
    LOG_INFO("Shutting down (heartbeat stopped or bye signal received)");
    // server destructor will join import thread, close DB, flush logs
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
