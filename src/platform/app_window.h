// Cross-platform app window — CRTP for compile-time dispatch.
// Windows: Edge --app mode | macOS: WKWebView | Linux: GTK WebKit
#pragma once

#include <memory>
#include <string>
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

// CRTP base — derived must implement: open(), close()
template <typename Derived>
class AppWindow {
public:
    explicit AppWindow(const WindowConfig& cfg) : cfg_(cfg) {}
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
        if (exe.empty()) return false;
        printf("AppWindow: %s\n", exe.c_str());

        std::string cmd = "\"" + exe + "\" --app=" + cfg_.url +
            " --window-size=" + std::to_string(cfg_.width) + "," + std::to_string(cfg_.height);

        STARTUPINFOA si{sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOW;
        PROCESS_INFORMATION pi{};

        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                            0, nullptr, nullptr, &si, &pi))
            return false;

        CloseHandle(pi.hThread);
        process_.reset(pi.hProcess);
        return true;
    }

    void close() { process_.reset(); }

    /// Block until the Edge window closes, or return immediately if no process.
    void monitor() {
        if (process_) WaitForSingleObject(process_.get(), INFINITE);
    }

private:
    static std::string find_edge() {
        const char* bases[] = {
            "C:\\Program Files (x86)\\Microsoft\\Edge\\Application",
            "C:\\Program Files\\Microsoft\\Edge\\Application",
            "C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application",
            "C:\\Program Files\\Microsoft\\EdgeWebView\\Application",
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
                // Try msedge.exe first (supports --app), fall back to msedgewebview2.exe
                for (auto* name : {"msedge.exe", "msedgewebview2.exe"}) {
                    std::string path = std::string(base) + "\\" + fd.cFileName + "\\" + name;
                    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        best = path;
                        break;
                    }
                }
                if (!best.empty()) break;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            if (!best.empty()) return best;
        }
        return "";
    }

    struct HandleDeleter { void operator()(HANDLE h) const { if (h) CloseHandle(h); } };
    std::unique_ptr<void, HandleDeleter> process_;
};

using NativeWindow = WinEdgeWindow;

#endif // _WIN32

}  // namespace kforge::platform
