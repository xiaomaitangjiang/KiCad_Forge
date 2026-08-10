// Cross-platform app window — CRTP for compile-time dispatch.
// Windows: Edge --app mode | macOS: WKWebView | Linux: GTK WebKit
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
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

// PKEY_AppUserModel_ID GUID — not in MinGW's propkey.h
const PROPERTYKEY PKEY_AppUserModel_ID = {
    .fmtid = {.Data1 = 0x9F4C2855,
              .Data2 = 0x9F79,
              .Data3 = 0x4B39,
              .Data4 = {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}},
    .pid = 5};
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
// Windows: Edge --app mode via .lnk shortcut (独立任务栏图标)
// ============================================================
#ifdef _WIN32

class WinEdgeWindow : public AppWindow<WinEdgeWindow>
{
public:
    using AppWindow::AppWindow;

    bool open()
    {
        std::string exe = find_edge();
        if (exe.empty())
        {
            LOG_ERROR("FATAL: Could not find msedge.exe. Is Edge installed?");
            return false;
        }
        LOG_DEBUG("AppWindow: {}", exe);

        auto data_dir = std::filesystem::temp_directory_path() / "KiCad_Forge_Edge";
        std::filesystem::create_directories(data_dir);

        std::string args = "--app=" + cfg_.url + " --app-id=Kicad_Forge.App" +
                           " --window-size=" + std::to_string(cfg_.width) + "," +
                           std::to_string(cfg_.height) + " --user-data-dir=\"" + data_dir.string() +
                           "\"";

        std::string cmd = "\"" + exe + "\" " + args;
        std::vector<char> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back('\0');

        STARTUPINFOA si{.cb = sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOW;
        PROCESS_INFORMATION pi{};

        if (CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                           &si, &pi) == 0)
            return false;

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

private:
    static std::string find_edge()
    {
        namespace fs = std::filesystem;

        for (auto* key :
             {R"(SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\msedge.exe)",
              R"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\msedge.exe)"})
        {
            char path[MAX_PATH];
            DWORD len = sizeof(path);
            if (ERROR_SUCCESS ==
                RegGetValueA(HKEY_LOCAL_MACHINE, key, "", RRF_RT_REG_SZ, nullptr, path, &len))
                if (fs::exists(path))
                    return path;
            if (ERROR_SUCCESS ==
                RegGetValueA(HKEY_CURRENT_USER, key, "", RRF_RT_REG_SZ, nullptr, path, &len))
                if (fs::exists(path))
                    return path;
        }

        for (auto* base : {R"(C:\Program Files (x86)\Microsoft\Edge\Application)",
                           R"(C:\Program Files\Microsoft\Edge\Application)"})
        {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(base, ec))
            {
                if (!entry.is_directory())
                    continue;
                auto exe = entry.path() / "msedge.exe";
                if (fs::exists(exe))
                    return exe.string();
            }
        }
        return "";
    }
};

using NativeWindow = WinEdgeWindow;

#endif

}  // namespace kforge::platform
