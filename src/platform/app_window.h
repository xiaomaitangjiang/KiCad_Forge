// Cross-platform app window — CRTP for compile-time dispatch.
// Windows: WebView2 via webview library | macOS: WKWebView | Linux: GTK WebKit
#pragma once

#include "../util/logger.h"

#include <atomic>
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
            kforge::util::log_error{}("webview: creation failed — WebView2 Runtime may be missing");
            return false;
        }
        webview_set_title(w_, cfg_.title.c_str());
        webview_set_size(w_, cfg_.width, cfg_.height, WEBVIEW_HINT_NONE);
        webview_navigate(w_, cfg_.url.c_str());
        kforge::util::log_info{}("webview: navigating to {}", cfg_.url);

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
                // Wait for the first heartbeat (window may still be opening)
                auto deadline = steady_clock::now() + seconds(30);
                while (!window_closed_ && steady_clock::now() < deadline && !is_alive_copy())
                {
                    std::this_thread::sleep_for(milliseconds(200));
                }
                // Poll: exit as soon as the window is gone (webview_run has
                // returned — see window_closed_ below) or the heartbeat is
                // lost (frontend crashed/hung without closing the window).
                while (!window_closed_ && is_alive_copy())
                {
                    std::this_thread::sleep_for(milliseconds(200));
                }
                if (!window_closed_)
                {
                    kforge::util::log_info{}("webview: heartbeat lost, terminating...");
                    webview_terminate(w_);
                }
            },
            std::forward<F>(is_alive));

        webview_run(w_);  // returns when the window is closed
        // Wake the monitor thread so join() below returns promptly — without
        // this, a healthy heartbeat keeps is_alive() true forever and the
        // process would hang on join() until the heartbeat timeout.
        window_closed_ = true;
        if (monitor_thread.joinable())
        {
            monitor_thread.join();
        }
    }

private:
    webview_t w_ = nullptr;
    std::atomic<bool> window_closed_{false};
};

using NativeWindow = WinWebView2Window;

#endif

}  // namespace kforge::platform