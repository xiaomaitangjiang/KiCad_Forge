// Cross-platform app window — CRTP for compile-time dispatch.
// Windows: embedded WebView2 | macOS / Linux: system browser
#pragma once

#include "../util/logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
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
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>

#include "webview/webview.h"
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace kforge::platform
{

struct WindowConfig
{
    std::string url;
    std::string title{"KIForge"};
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

class UnixBrowserWindow : public AppWindow<UnixBrowserWindow>
{
public:
    explicit UnixBrowserWindow(WindowConfig cfg) : AppWindow(std::move(cfg)) {}

    using AppWindow::monitor;

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

    /// 外部浏览器进程：拿不到窗口状态，一律按「可见」处理
    [[nodiscard]] bool is_minimized() const { return false; }

private:
    pid_t pid_{-1};
};

using NativeWindow = UnixBrowserWindow;

#endif

// ============================================================
// Windows: WebView2 via webview library
// ============================================================
#ifdef _WIN32

class WinWebView2Window : public AppWindow<WinWebView2Window>
{
public:
    inline static WNDPROC original_proc_ = nullptr;

    explicit WinWebView2Window(WindowConfig cfg) : AppWindow(std::move(cfg)) {}

    ~WinWebView2Window()
    {
        close();
    }

    /// Binding called by the self-drawn top bar: minimize / maximize toggle /
    /// drag / close. Returns the resulting maximized state as JSON.
    static void window_action_bridge(const char* id, const char* req, void* arg)
    {
        static_cast<WinWebView2Window*>(arg)->handle_window_action(id, req ? req : "");
    }

    void handle_window_action(const char* id, const std::string& request)
    {
        std::string action;
        const auto first = request.find('"');
        if (first != std::string::npos)
        {
            const auto last = request.find('"', first + 1);
            if (last != std::string::npos) action = request.substr(first + 1, last - first - 1);
        }

        HWND hwnd = w_ ? static_cast<HWND>(webview_get_window(w_)) : nullptr;
        if (hwnd)
        {
            if (action == "minimize")
                ShowWindow(hwnd, SW_MINIMIZE);
            else if (action == "maximize")
            {
                if (IsZoomed(hwnd) != 0)
                    ShowWindow(hwnd, SW_RESTORE);
                else
                    ShowWindow(hwnd, SW_MAXIMIZE);
            }
            else if (action == "close")
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            else if (action == "drag")
            {
                // The top bar replaces the native caption, so start the system
                // move loop from the current mouse gesture.
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, static_cast<WPARAM>(HTCAPTION), 0);
            }
        }

        if (w_ && id)
        {
            const bool maximized = hwnd != nullptr && IsZoomed(hwnd) != 0;
            webview_return(w_, id, 0,
                           maximized ? "{\"maximized\":true}" : "{\"maximized\":false}");
        }
    }

    /// Custom client area: remove the 6px resize frame Windows still draws
    /// around a caption-less WS_THICKFRAME window, and recreate the resize
    /// grips in WM_NCHITTEST.
    static LRESULT CALLBACK frameless_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        if (msg == WM_NCCALCSIZE && wparam != 0) return 0;
        if (msg == WM_NCHITTEST)
        {
            const LRESULT hit = original_proc_
                ? CallWindowProcW(original_proc_, hwnd, msg, wparam, lparam)
                : DefWindowProcW(hwnd, msg, wparam, lparam);
            if (hit != HTCLIENT) return hit;
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &pt);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            constexpr int grip = 6;
            const bool left = pt.x < grip;
            const bool right = pt.x >= rc.right - grip;
            const bool top = pt.y < grip;
            const bool bottom = pt.y >= rc.bottom - grip;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            return HTCLIENT;
        }
        return original_proc_
            ? CallWindowProcW(original_proc_, hwnd, msg, wparam, lparam)
            : DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    bool open()
    {
        w_ = webview_create(0, nullptr);
        if (!w_)
        {
            kforge::util::log_error{}("webview: creation failed — WebView2 Runtime may be missing");
            return false;
        }
        // The app draws its own KIFORGE top bar (including window controls), so
        // the native caption is removed entirely; the window keeps its resize
        // border and taskbar/Alt-Tab icon. The native title stays "KIForge" so
        // the taskbar button and Alt-Tab entry still show an application name.
        webview_set_title(w_, cfg_.title.c_str());
        webview_bind(w_, "kforge_window", &WinWebView2Window::window_action_bridge, this);
        webview_set_size(w_, cfg_.width, cfg_.height, WEBVIEW_HINT_NONE);
        webview_navigate(w_, cfg_.url.c_str());
        kforge::util::log_info{}("webview: navigating to {}", cfg_.url);

        webview_dispatch(
            w_,
            [](webview_t w, void*)
            {
                HWND hwnd = static_cast<HWND>(webview_get_window(w));
                if (!hwnd) return;

                const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
                SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_CAPTION));
                if (!original_proc_)
                {
                    original_proc_ = reinterpret_cast<WNDPROC>(
                        GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
                    SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                      reinterpret_cast<LONG_PTR>(&WinWebView2Window::frameless_proc));
                }
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                                 SWP_FRAMECHANGED);

                // Windows 11 DWM rounding survives WS_POPUP/NCCALCSIZE custom
                // chrome; older systems simply ignore the attribute.
                if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
                {
                    using DwmSetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
                    auto set_attribute = reinterpret_cast<DwmSetAttribute>(
                        GetProcAddress(dwm, "DwmSetWindowAttribute"));
                    if (set_attribute)
                    {
                        constexpr DWORD kCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
                        constexpr int kRound = 2;               // DWMWCP_ROUND
                        set_attribute(hwnd, kCornerPreference, &kRound, sizeof(kRound));
                    }
                    FreeLibrary(dwm);
                }

                HICON app_icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101)); // IDI_ICON1
                if (app_icon)
                {
                    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(app_icon));
                    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(app_icon));
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

    /// 窗口最小化时不判心跳超时：WebView 会节流页面定时器，
    /// 稀疏心跳属于正常现象（生命周期由原生关闭事件掌握）
    [[nodiscard]] bool is_minimized() const
    {
        if (!w_)
            return false;
        HWND hwnd = static_cast<HWND>(webview_get_window(w_));
        return hwnd != nullptr && IsIconic(hwnd) != 0;
    }

    template <typename F>
    void monitor(F&& is_alive)
    {
        using namespace std::chrono;

        // Heartbeat monitor runs on a background thread — webview_run() blocks
        std::thread monitor_thread(
            [this, is_alive_copy = std::forward<F>(is_alive)]() mutable
            {
                // Wait for the first heartbeat (window may still be opening)
                auto deadline = steady_clock::now() + seconds(30);
                while (!window_closed_ && steady_clock::now() < deadline && !is_alive_copy())
                {
                    std::this_thread::sleep_for(milliseconds(200));
                }
                // Poll: exit as soon as the window is gone (webview_run has
                // returned — see window_closed_ below) or the frontend stops
                // responding while the window is still open (crash/hang).
                // 判死策略见 util/lifecycle.h：最小化时不做心跳判死。
                while (!window_closed_ && is_alive_copy())
                {
                    std::this_thread::sleep_for(milliseconds(200));
                }
                if (!window_closed_)
                {
                    kforge::util::log_info{}(
                        "webview: frontend stopped responding, terminating (window still open)");
                    // webview_terminate posts WM_QUIT to the CALLING thread's
                    // queue — dispatch it onto the UI thread, otherwise the
                    // main thread's webview_run loop never exits.
                    webview_dispatch(
                        w_,
                        [](webview_t w, void*)
                        {
                            webview_terminate(w);
                        },
                        nullptr);
                }
            });

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
