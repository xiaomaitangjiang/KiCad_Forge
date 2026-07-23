// WebView2 host — supports bundled Fixed Version runtime.
// Priority: webview2_runtime/ next to exe → system WebView2 → default browser.
#ifdef _WIN32

#include "platform/webview_win.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <cstdio>
#include <string>

namespace kforge::platform {

// ============================================================
// Minimal WebView2 COM definitions
// ============================================================

struct EventRegistrationToken { long long value{0}; };

static const IID IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler = {
    0x82cc526f, 0xc316, 0x4daf, {0xb7, 0x20, 0x8c, 0x4e, 0x4f, 0x3b, 0xc6, 0x30}};
static const IID IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler = {
    0x8ce53b6e, 0xf89d, 0x4c99, {0x9b, 0x68, 0x6b, 0xb1, 0xb0, 0xfd, 0x41, 0x5c}};

struct ICoreWebView2 : public IUnknown {
    virtual HRESULT Navigate(LPCWSTR uri) = 0;
    virtual HRESULT add_NavigationCompleted(void*, EventRegistrationToken*) = 0;
};
struct ICoreWebView2Controller : public IUnknown {
    virtual HRESULT put_IsVisible(BOOL v) = 0;
    virtual HRESULT put_Bounds(RECT b) = 0;
    virtual HRESULT get_CoreWebView2(ICoreWebView2** v) = 0;
    virtual HRESULT Close() = 0;
};
struct ICoreWebView2Environment : public IUnknown {
    virtual HRESULT CreateCoreWebView2Controller(HWND parent, void* handler) = 0;
};

using CreateEnvFn = HRESULT (*)(LPCWSTR browserDir, LPCWSTR userDataDir,
                                  void* options, void* handler);

// ============================================================
// Callbacks
// ============================================================

struct ControllerCB : IUnknown {
    HRESULT QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER; *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) {
            *ppv = static_cast<IUnknown*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++ref_; }
    ULONG Release() override { ULONG r = --ref_; if (r == 0) delete this; return r; }

    HRESULT Invoke(HRESULT ec, ICoreWebView2Controller* c) {
        if (FAILED(ec) || !c) return ec;
        c->AddRef();
        g_ctrl = c;
        c->get_CoreWebView2(&webview_);
        RECT r; GetClientRect(hwnd_, &r);
        c->put_Bounds(r);
        c->put_IsVisible(TRUE);
        if (webview_) {
            int n = MultiByteToWideChar(CP_UTF8, 0, url_, -1, nullptr, 0);
            auto* w = new wchar_t[n];
            MultiByteToWideChar(CP_UTF8, 0, url_, -1, w, n);
            webview_->Navigate(w);
            delete[] w;
        }
        return S_OK;
    }
    HWND hwnd_{};
    const char* url_{};
    ICoreWebView2* webview_{};
    static inline ICoreWebView2Controller* g_ctrl = nullptr;
    ULONG ref_{1};
};

struct EnvCB : IUnknown {
    HRESULT QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER; *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) {
            *ppv = static_cast<IUnknown*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++ref_; }
    ULONG Release() override { ULONG r = --ref_; if (r == 0) delete this; return r; }

    HRESULT Invoke(HRESULT ec, ICoreWebView2Environment* env) {
        if (FAILED(ec) || !env) return ec;
        auto* h = new ControllerCB();
        h->hwnd_ = hwnd_; h->url_ = url_;
        HRESULT hr = env->CreateCoreWebView2Controller(hwnd_, h);
        if (FAILED(hr)) delete h;
        return hr;
    }
    HWND hwnd_{};
    const char* url_{};
    ULONG ref_{1};
};

// ============================================================
// Window proc
// ============================================================

static bool g_closed = false;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SIZE && ControllerCB::g_ctrl) {
        RECT r; GetClientRect(hwnd, &r);
        ControllerCB::g_ctrl->put_Bounds(r);
    } else if (msg == WM_CLOSE) {
        g_closed = true; DestroyWindow(hwnd);
    } else if (msg == WM_DESTROY) {
        PostQuitMessage(0);
    } else {
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

// ============================================================
// DLL loading — supports bundled runtime, system, app-local
// ============================================================

static HMODULE try_load_webview2_dll(const wchar_t* runtime_dir = nullptr) {
    // 1. Bundled runtime (webview2_runtime/)
    if (runtime_dir && runtime_dir[0]) {
        wchar_t dll_path[MAX_PATH];
        swprintf(dll_path, MAX_PATH, L"%s\\EBWebView\\x64\\EmbeddedBrowserWebView.dll", runtime_dir);
        HMODULE m = LoadLibraryW(dll_path);
        if (m) return m;
    }

    // 2. System WebView2 Runtime (via registry)
    HKEY hk;
    const wchar_t* keys[] = {
        L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
    };
    for (auto* k : keys) {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, k, 0, KEY_READ, &hk) == ERROR_SUCCESS ||
            RegOpenKeyExW(HKEY_CURRENT_USER, k, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            wchar_t ver[64] = {}; DWORD sz = sizeof(ver);
            if (RegQueryValueExW(hk, L"pv", nullptr, nullptr, (LPBYTE)ver, &sz) == ERROR_SUCCESS && ver[0]) {
                wchar_t path[MAX_PATH];
                swprintf(path, MAX_PATH,
                    L"C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application\\%s\\"
                    L"EBWebView\\x64\\EmbeddedBrowserWebView.dll", ver);
                HMODULE m = LoadLibraryW(path);
                if (m) { RegCloseKey(hk); return m; }
            }
            RegCloseKey(hk);
        }
    }

    // 3. App-local WebView2Loader.dll (the SDK loader, not the engine)
    wchar_t exe_dir[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(exe_dir, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat(exe_dir, L"WebView2Loader.dll");
    HMODULE m = LoadLibraryW(exe_dir);
    if (m) return m;

    // 4. System PATH (last resort)
    return LoadLibraryW(L"WebView2Loader.dll");
}

// ============================================================
// Find bundled WebView2 runtime (webview2_runtime/ next to exe)
// ============================================================

static bool find_bundled_runtime(wchar_t* out_path, size_t out_size) {
    wchar_t exe_dir[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = 0;

    swprintf(out_path, out_size, L"%s\\webview2_runtime", exe_dir);
    // Check for msedgewebview2.exe inside
    wchar_t check[MAX_PATH];
    swprintf(check, MAX_PATH, L"%s\\msedgewebview2.exe", out_path);
    return (GetFileAttributesW(check) != INVALID_FILE_ATTRIBUTES);
}

// ============================================================
// Public API
// ============================================================

bool show_webview_window(const char* url, const char* title,
                          int width, int height) {
    // Try WebView2 — skip if DLL can't be loaded cleanly
    HMODULE wv2 = nullptr;
    wchar_t bundled_path[MAX_PATH] = {};
    bool has_bundled = find_bundled_runtime(bundled_path, MAX_PATH);
    if (has_bundled) {
        wv2 = try_load_webview2_dll(bundled_path);
    }
    if (!wv2) wv2 = try_load_webview2_dll();

    if (!wv2 || !GetProcAddress(wv2, "CreateCoreWebView2EnvironmentWithOptions")) {
        printf("WebView2 runtime not found. Installing Evergreen Runtime...\n");
        // Launch the Microsoft bootstrapper (small download, ~2MB)
        // The bootstrapper is included with the app or downloaded on demand
        wchar_t setup_path[MAX_PATH];
        GetTempPathW(MAX_PATH, setup_path);
        wcscat(setup_path, L"MicrosoftEdgeWebview2Setup.exe");

        // Try local copy first, otherwise download
        if (GetFileAttributesW(setup_path) == INVALID_FILE_ATTRIBUTES) {
            if (wv2) FreeLibrary(wv2);
            // Fall back to browser — bootstrapper not available
            printf("Bootstrapper not found. Opening in default browser.\n"
                   "To enable built-in browser, install Edge WebView2 Runtime:\n"
                   "  https://go.microsoft.com/fwlink/p/?LinkId=2124703\n");
            ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOW);
            // Keep server alive with simple notification
            MessageBoxA(nullptr,
                "KiCad Forge is running at http://localhost:8080\n\n"
                "Click OK to stop the server and exit.",
                "KiCad Forge", MB_OK | MB_ICONINFORMATION);
            return true;
        }

        // Run the bootstrapper silently
        SHELLEXECUTEINFOW sei = {sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
        sei.lpFile = setup_path;
        sei.lpParameters = L"/install /silent";
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 60000);
            CloseHandle(sei.hProcess);
            // Retry loading
            wv2 = try_load_webview2_dll();
            if (wv2) printf("WebView2 Runtime installed successfully\n");
        }

        if (!wv2) {
            ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOW);
            MessageBoxA(nullptr,
                "KiCad Forge is running at http://localhost:8080\n\n"
                "Click OK to stop the server and exit.",
                "KiCad Forge", MB_OK | MB_ICONINFORMATION);
            return true;
        }
    }

    auto* createEnv = (CreateEnvFn)GetProcAddress(wv2, "CreateCoreWebView2EnvironmentWithOptions");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"KiCadForgeWebView";

    int tl = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
    auto* wtitle = new wchar_t[tl];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, tl);

    if (!RegisterClassExW(&wc)) {
        delete[] wtitle; CoUninitialize(); FreeLibrary(wv2); return false;
    }

    HWND hwnd = CreateWindowExW(0, L"KiCadForgeWebView", wtitle,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        width, height, nullptr, nullptr, wc.hInstance, nullptr);
    delete[] wtitle;

    if (!hwnd) { CoUninitialize(); FreeLibrary(wv2); return false; }

    // Create WebView2 environment — pass bundled path if available
    LPCWSTR browser_dir = has_bundled ? bundled_path : nullptr;
    auto* env_cb = new EnvCB();
    env_cb->hwnd_ = hwnd;
    env_cb->url_ = url;

    HRESULT hr = createEnv(browser_dir, nullptr, nullptr, env_cb);
    if (FAILED(hr)) {
        delete env_cb;
        ShellExecuteA(hwnd, "open", url, nullptr, nullptr, SW_SHOW);
        DestroyWindow(hwnd); CoUninitialize(); FreeLibrary(wv2); return true;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Message loop
    g_closed = false;
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0 && !g_closed) {
        TranslateMessage(&msg); DispatchMessage(&msg);
    }

    if (ControllerCB::g_ctrl) {
        ControllerCB::g_ctrl->Close();
        ControllerCB::g_ctrl->Release();
        ControllerCB::g_ctrl = nullptr;
    }
    FreeLibrary(wv2);
    CoUninitialize();
    return true;
}

}  // namespace kforge::platform
#endif  // _WIN32
