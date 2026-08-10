// Cross-platform app window — CRTP for compile-time dispatch.
// Windows: WebView2 via webview library | macOS: WKWebView | Linux: GTK WebKit
#pragma once

#include "../util/logger.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
// Prevent MinGW's eventtoken.h from defining EventRegistrationToken
// inside extern "C" — webview library needs it in global C++ namespace.
#define __eventtoken_h__
struct EventRegistrationToken { INT64 value; };

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace kforge::platform
{

struct WindowConfig
{
    std::string url;
    std::string title{"KiCad Forge"};
    int width{1280};
    int height{800};
};

template <typename Derived>
class AppWindow
{
private:
    explicit AppWindow(WindowConfig cfg) : cfg_(std::move(cfg))
    {
    }

public:
    template <typename F>
    void monitor(F&& is_alive)
    {
        using std::chrono::milliseconds;
        using std::chrono::seconds;
        using std::chrono::steady_clock;

        auto deadline = steady_clock::now() + seconds(30);
        while (steady_clock::now() < deadline && !is_alive())
        {
            std::this_thread::sleep_for(milliseconds(500));
        }
        while (is_alive())
        {
            std::this_thread::sleep_for(seconds(1));
        }
    }

protected:
    WindowConfig cfg_;
    friend Derived;
};

// ============================================================
// macOS / Linux
// ============================================================
#if defined(__APPLE__) || defined(__linux__)

#include <sys/wait.h>
#include <unistd.h>

class UnixBrowserWindow : public AppWindow<UnixBrowserWindow>
{
public:
    using AppWindow::AppWindow;

    bool open()
    {
        pid_ = fork();
        if (pid_ == 0)
        {
#ifdef __APPLE__
            execlp("open", "open", cfg_.url.c_str(), nullptr);
#else
            execlp("xdg-open", "xdg-open", cfg_.url.c_str(), nullptr);
#endif
            _exit(1);
        }
        return pid_ > 0;
    }

    void monitor()
    {
        if (pid_ > 0)
            waitpid(pid_, nullptr, 0);
    }
    void close()
    {
        if (pid_ > 0)
        {
            kill(pid_, SIGTERM);
            pid_ = 0;
        }
    }

private:
    pid_t pid_{-1};
};

using NativeWindow = UnixBrowserWindow;

#endif

// ============================================================
// Windows: WebView2 via webview library
// ============================================================
#ifdef _WIN32

#include "webview/webview.h"

class WinWebView2Window : public AppWindow<WinWebView2Window>
{
public:
    explicit WinWebView2Window(WindowConfig cfg) : AppWindow(std::move(cfg)) {}

    ~WinWebView2Window()
    {
        close();
    }

    bool open()
    {
        w_ = webview_create(0, nullptr);
        if (!w_)
        {
            LOG_ERROR("webview: creation failed — WebView2 Runtime may be missing");
            return false;
        }
        webview_set_title(w_, cfg_.title.c_str());
        webview_set_size(w_, cfg_.width, cfg_.height, WEBVIEW_HINT_NONE);
        webview_navigate(w_, cfg_.url.c_str());
        LOG_INFO("webview: navigating to {}", cfg_.url);

        // Set window icon — webview uses IDI_APPLICATION by default
        webview_dispatch(
            w_,
            [](webview_t w, void*)
            {
                HWND hwnd = static_cast<HWND>(webview_get_window(w));
                HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101)); // IDI_ICON1
                if (icon)
                {
                    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
                    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
                }
            },
            nullptr);
        return true;
    }

    void close()
    {
        if (w_)
        {
            webview_destroy(w_);
            w_ = nullptr;
        }
    }

    template <typename F>
    void monitor(F&& is_alive)
    {
        using namespace std::chrono;

        // Heartbeat monitor runs on a background thread — webview_run() blocks
        std::thread monitor_thread(
            [this](F is_alive_copy)
            {
                auto deadline = steady_clock::now() + seconds(30);
                while (steady_clock::now() < deadline && !is_alive_copy())
                {
                    std::this_thread::sleep_for(milliseconds(500));
                }
                while (is_alive_copy())
                {
                    std::this_thread::sleep_for(seconds(1));
                }
                LOG_INFO("webview: heartbeat lost, terminating...");
                webview_terminate(w_);
            },
            std::forward<F>(is_alive));

        webview_run(w_);  // blocks until webview_terminate() is called
        if (monitor_thread.joinable())
        {
            monitor_thread.join();
        }
    }

private:
    webview_t w_ = nullptr;
};

using NativeWindow = WinWebView2Window;

#endif

}  // namespace kforge::platform
