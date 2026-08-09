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
#include <vector>

#ifdef _WIN32
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>
#endif

namespace kforge::platform {

struct WindowConfig {
    std::string url;
    std::string title{"KiCad Forge"};
    int width{1280};
    int height{800};
};

template <typename Derived>
class AppWindow {
public:
    explicit AppWindow(const WindowConfig& cfg) : cfg_(cfg) {}

    template <typename F>
    void monitor(F&& is_alive) {
        using namespace std::chrono;
        auto deadline = steady_clock::now() + seconds(30);
        while (steady_clock::now() < deadline && !is_alive())
            std::this_thread::sleep_for(milliseconds(500));
        while (is_alive())
            std::this_thread::sleep_for(seconds(1));
    }

protected:
    WindowConfig cfg_;
};

// ============================================================
// macOS / Linux
// ============================================================
#if defined(__APPLE__) || defined(__linux__)

#include <sys/wait.h>
#include <unistd.h>

class UnixBrowserWindow : public AppWindow<UnixBrowserWindow> {
public:
    using AppWindow::AppWindow;

    bool open() {
        pid_ = fork();
        if (pid_ == 0) {
#ifdef __APPLE__
            execlp("open", "open", cfg_.url.c_str(), nullptr);
#else
            execlp("xdg-open", "xdg-open", cfg_.url.c_str(), nullptr);
#endif
            _exit(1);
        }
        return pid_ > 0;
    }

    void monitor() { if (pid_ > 0) waitpid(pid_, nullptr, 0); }
    void close() { if (pid_ > 0) { kill(pid_, SIGTERM); pid_ = 0; } }

private:
    pid_t pid_{-1};
};

using NativeWindow = UnixBrowserWindow;

#endif

// ============================================================
// Windows: Edge --app mode via .lnk shortcut (独立任务栏图标)
// ============================================================
#ifdef _WIN32

class WinEdgeWindow : public AppWindow<WinEdgeWindow> {
public:
    using AppWindow::AppWindow;

    bool open() {
        std::string exe = find_edge();
        if (exe.empty()) {
            LOG_ERROR("FATAL: Could not find msedge.exe. Is Edge installed?");
            return false;
        }
        LOG_DEBUG("AppWindow: {}", exe);

        auto data_dir = std::filesystem::temp_directory_path() / "KiCad_Forge_Edge";
        std::filesystem::create_directories(data_dir);

        std::string args = "--app=" + cfg_.url +
                           " --window-size=" + std::to_string(cfg_.width) + "," +
                           std::to_string(cfg_.height) +
                           " --user-data-dir=\"" + data_dir.string() + "\"";

        // .lnk with AppUserModelID — the only way Windows taskbar respects the ID
        auto lnk_path = std::filesystem::temp_directory_path() / "KiCad_Forge.lnk";

        CoInitialize(nullptr);
        IShellLinkW* psl = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_IShellLinkW, (void**)&psl))) {
            CoUninitialize();
            return false;
        }

        auto w_exe = std::wstring(exe.begin(), exe.end());
        auto w_args = std::wstring(args.begin(), args.end());
        psl->SetPath(w_exe.c_str());
        psl->SetArguments(w_args.c_str());

        // Set AppUserModelID
        IPropertyStore* pps = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_IPropertyStore, (void**)&pps))) {
            PROPVARIANT pv{};
            pv.vt = VT_LPWSTR;
            pv.pwszVal = (LPWSTR)L"Kicad_Forge.App";
            pps->SetValue(PKEY_AppUserModel_ID, pv);
            pps->Commit();
            pps->Release();
        }

        IPersistFile* ppf = nullptr;
        bool ok = false;
        if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
            ppf->Save(lnk_path.wstring().c_str(), TRUE);
            ppf->Release();
            ok = true;
        }
        psl->Release();

        if (!ok) { CoUninitialize(); return false; }

        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        auto lnk_wstr = lnk_path.wstring();
        sei.lpFile = lnk_wstr.c_str();
        sei.nShow = SW_SHOW;
        BOOL result = ShellExecuteExW(&sei);
        CoUninitialize();
        return result == TRUE;
    }

private:
    static std::string find_edge() {
        namespace fs = std::filesystem;

        for (auto* key : {R"(SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\msedge.exe)",
                          R"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\msedge.exe)"}) {
            char path[MAX_PATH];
            DWORD len = sizeof(path);
            if (ERROR_SUCCESS == RegGetValueA(HKEY_LOCAL_MACHINE, key, "", RRF_RT_REG_SZ,
                                              nullptr, path, &len))
                if (fs::exists(path)) return path;
            if (ERROR_SUCCESS == RegGetValueA(HKEY_CURRENT_USER, key, "", RRF_RT_REG_SZ,
                                              nullptr, path, &len))
                if (fs::exists(path)) return path;
        }

        for (auto* base : {R"(C:\Program Files (x86)\Microsoft\Edge\Application)",
                           R"(C:\Program Files\Microsoft\Edge\Application)"}) {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(base, ec)) {
                if (!entry.is_directory()) continue;
                auto exe = entry.path() / "msedge.exe";
                if (fs::exists(exe)) return exe.string();
            }
        }
        return "";
    }
};

using NativeWindow = WinEdgeWindow;

#endif

}  // namespace kforge::platform
