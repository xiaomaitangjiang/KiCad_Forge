// KiCad Forge — graphical symbol/footprint library manager
// Entry point: starts HTTP server, opens native app window, manages lifecycle.
#include "api/api_server.h"
#include "platform/app_window.h"

#include <chrono>
#include <thread>

#include "../third_party/httplib.h"

#ifdef _WIN32
#include <windows.h>
#endif

// --------------- main entry point ---------------

static int run_server() {
    (void)setvbuf(stdout, nullptr, _IONBF, 0);  
    // unbuffered — printf visible in debugger
    const int PORT = 20443;

    // Check for stale process
    httplib::Client probe("127.0.0.1", PORT);
    probe.set_connection_timeout(0, 500000);
    if (probe.Get("/api/status")) {
        printf("Port %d is already in use. Is another instance running?\n", PORT);
        return 1;
    }
    

    // 1. Start HTTP backend
    kforge::api::ApiServer server(PORT);
    if (!server.start()) { printf("ERROR: Server start failed\n"); return 1; }

    printf("KiCad Forge starting...\n"); fflush(stdout);

    // 2. Wait for server to respond to HTTP requests
    {
        using namespace std::chrono;
        auto deadline = steady_clock::now() + seconds(8);
        bool ready = false;
        while (steady_clock::now() < deadline) {
            httplib::Client cli("127.0.0.1", PORT);
            cli.set_connection_timeout(0, 200000);
            if (cli.Get("/api/status")) { ready = true; break; }
            std::this_thread::sleep_for(milliseconds(200));
        }
        if (!ready) { printf("ERROR: Server not responding\n"); return 1; }
    }
    printf("KiCad Forge running at http://127.0.0.1:%d\n", PORT);

    // 3. Open native app window, then block until user closes it.
    //    Uses frontend heartbeat — works across all platforms.
    kforge::platform::WindowConfig cfg;
    cfg.url = "http://127.0.0.1:20443";

    kforge::platform::NativeWindow win(cfg);
    if (!win.open()) {
        printf("ERROR: Could not open browser window\n");
        return 1;
    }
    printf("Close browser window or press Ctrl+C to stop\n"); fflush(stdout);
    win.monitor([&] { return server.ms_since_heartbeat() < 1500; });
    return 0;
}

// Windows subsystem: release builds use WinMain (no console)
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return run_server(); }
#endif
int main() { return run_server(); }
