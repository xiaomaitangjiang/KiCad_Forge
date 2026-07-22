#include "api/api_server.h"

#include <cstdio>
#include <thread>
#include <atomic>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include "platform/webview_win.h"
#endif

static std::atomic<bool> g_running{true};

// Wait until the HTTP server is actually accepting connections
static bool wait_for_server(int port, int timeout_secs) {
    using namespace std::chrono;
    auto deadline = steady_clock::now() + seconds(timeout_secs);

    while (steady_clock::now() < deadline) {
        // Try a simple TCP connect to check if port is listening
#ifdef _WIN32
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock != INVALID_SOCKET) {
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons((u_short)port);
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");

            // Set non-blocking for short timeout
            u_long mode = 1;
            ioctlsocket(sock, FIONBIO, &mode);

            connect(sock, (sockaddr*)&addr, sizeof(addr));

            fd_set set;
            FD_ZERO(&set);
            FD_SET(sock, &set);
            timeval tv = {0, 200000};  // 200ms

            if (select(0, nullptr, &set, nullptr, &tv) > 0) {
                closesocket(sock);
                return true;
            }
            closesocket(sock);
        }
#else
        // Simple check: try to connect
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                close(sock);
                return true;
            }
            close(sock);
        }
#endif
        std::this_thread::sleep_for(milliseconds(200));
    }
    return false;
}

static int run_server() {
    // Need WSA for socket checks on startup
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    kforge::api::ApiServer server(8080);

    if (!server.start()) {
        printf("ERROR: Failed to start server\n");
        return 1;
    }

    printf("KiCad Forge starting...\n");
    fflush(stdout);

    // Wait for the HTTP server to actually be ready (up to 5 seconds)
    if (!wait_for_server(8080, 5)) {
        printf("ERROR: Server failed to start listening on port 8080\n");
        printf("Check if another process is using port 8080.\n");
        return 1;
    }

    printf("KiCad Forge running at http://localhost:8080\n");
    fflush(stdout);

#ifdef _WIN32
    // Open native window with embedded WebView2 browser.
    // If WebView2 is available, blocks until the window is closed.
    // If not available, falls back to browser and keeps the server running.
    kforge::platform::show_webview_window(
        "http://127.0.0.1:8080",
        "KiCad Forge",
        1280, 800);

    WSACleanup();
#else
    // Linux/macOS: open system browser
#ifdef __APPLE__
    system("open http://localhost:8080");
#else
    system("xdg-open http://localhost:8080 2>/dev/null");
#endif
#endif
    printf("Press Ctrl+C to stop\n");
    fflush(stdout);
    server.wait();


    g_running = false;
    server.stop();
    return 0;
}

#ifdef _WIN32
// Release mode uses -mwindows which requires WinMain (no console)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return run_server();
}
#endif

int main() {
    return run_server();
}
