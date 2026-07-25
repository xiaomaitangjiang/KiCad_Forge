// Cross-platform app window — CRTP for compile-time dispatch.
// Windows: Edge --app mode | macOS: WKWebView | Linux: GTK WebKit
#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kforge::platform {

struct WindowConfig {
    std::string url;
    std::string title{"KiCad Forge"}; //useless
    int width{1280};
    int height{800};
};

// CRTP base — derived must implement: open()
template <typename Derived>
class AppWindow {
public:
    explicit AppWindow(const WindowConfig& cfg) : cfg_(cfg) {}

    /// Block until the heartbeat stops (frontend window closed).
    /// `is_alive` returns true while the frontend is still connected.
    template <typename F>
    void monitor(F&& is_alive) {
        using namespace std::chrono;
        // Wait up to 30s for the first heartbeat
        auto deadline = steady_clock::now() + seconds(30);
        while (steady_clock::now() < deadline && !is_alive()) {
            std::this_thread::sleep_for(milliseconds(500));
        }
        // Keep running while heartbeat is alive
        while (is_alive()) {
            std::this_thread::sleep_for(seconds(1));
        }
        // Heartbeat stopped — window closed, time to exit
    }

protected:
    WindowConfig cfg_;
};

// ============================================================
// Windows: Edge --app mode (Win11 built-in, zero dependency)
// ============================================================
// ============================================================
// macOS / Linux: fork+exec browser, monitor child process
// ============================================================
#if defined(__APPLE__) || defined(__linux__)

#include <unistd.h>
#include <sys/wait.h>

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

    void monitor() {
        if (pid_ > 0) waitpid(pid_, nullptr, 0);
    }

    void close() {
        if (pid_ > 0) { kill(pid_, SIGTERM); pid_ = 0; }
    }

private:
    pid_t pid_{-1};
};

using NativeWindow = UnixBrowserWindow;

#endif // __APPLE__ || __linux__

// ============================================================
// Windows: Edge --app mode (Win11 built-in, zero dependency)
// ============================================================
#ifdef _WIN32

class WinEdgeWindow : public AppWindow<WinEdgeWindow> {
public:
    using AppWindow::AppWindow;

    bool open() {
        std::string exe = find_edge();
        if (exe.empty()) {
            printf("FATAL: Could not find msedge.exe. Is Edge installed?\n");
            return false;
        }
        printf("AppWindow: %s\n", exe.c_str());
        printf("AppWindow: launching: %s\n", cfg_.url.c_str());

        std::string cmd = "\"" + exe + "\" --app=" + cfg_.url
            + " --window-size=" + std::to_string(cfg_.width) + "," + std::to_string(cfg_.height);

        // CreateProcessA may modify the command line — copy to mutable buffer
        std::vector<char> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back('\0');

        STARTUPINFOA si{sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOW;
        PROCESS_INFORMATION pi{};

        if (!CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr,
                            FALSE, 0, nullptr, nullptr, &si, &pi))
            return false;

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

private:
    static std::string find_edge() {
        // 1. Try registry (most reliable across installations)
        for (auto* key : {"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe",
                          "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe"}) {
            char path[MAX_PATH]; DWORD len = sizeof(path);
            if (ERROR_SUCCESS == RegGetValueA(HKEY_LOCAL_MACHINE, key, "", RRF_RT_REG_SZ,
                                              nullptr, path, &len))
                if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return path;
            if (ERROR_SUCCESS == RegGetValueA(HKEY_CURRENT_USER, key, "", RRF_RT_REG_SZ,
                                              nullptr, path, &len))
                if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return path;
        }

        // 2. Scan version folders under known install dirs
        const char* bases[] = {
            "C:\\Program Files (x86)\\Microsoft\\Edge\\Application",
            "C:\\Program Files\\Microsoft\\Edge\\Application",
        };
        for (auto* base : bases) {
            WIN32_FIND_DATAA fd{};
            std::string pattern = std::string(base) + "\\*";
            HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            std::string best;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == '.') continue;
                std::string path = std::string(base) + "\\" + fd.cFileName + "\\msedge.exe";
                if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    best = path; break;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            if (!best.empty()) return best;
        }
        return "";
    }

};

using NativeWindow = WinEdgeWindow;

#endif // _WIN32

}  // namespace kforge::platform
