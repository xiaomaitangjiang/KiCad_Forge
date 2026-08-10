// Cross-platform native folder picker — CRTP pattern, same style as app_window.h.
// Windows: IFileDialog COM | macOS: NSOpenPanel | Linux: zenity subprocess
#pragma once

#include <string>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace kforge::platform {

// CRTP base — platform implementations must provide pick_folder_impl()
template <typename Derived>
class FolderDialogBase {
public:
    static std::string pick_folder() { return Derived::pick_folder_impl(); }
};

// ============================================================
// macOS / Linux — subprocess
// ============================================================
#if defined(__APPLE__) || defined(__linux__)

class UnixFolderDialog : public FolderDialogBase<UnixFolderDialog> {
public:
    static std::string pick_folder_impl() {
#ifdef __APPLE__
        const char* script =
            "osascript -e 'POSIX path of (choose folder with prompt \"Select folder:\")' 2>/dev/null";
#else
        const char* script =
            "zenity --file-selection --directory --title=\"Select folder\" 2>/dev/null || "
            "kdialog --getexistingdirectory 2>/dev/null";
#endif
        FILE* pipe = popen(script, "r");
        if (!pipe) return "";
        char buf[4096];
        std::string result;
        while (fgets(buf, sizeof(buf), pipe)) result += buf;
        pclose(pipe);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
            result.pop_back();
        return result;
    }
};

using NativeFolderDialog = UnixFolderDialog;

#endif // __APPLE__ || __linux__

// ============================================================
// Windows — IFileDialog COM
// ============================================================
#ifdef _WIN32

class WinFolderDialog : public FolderDialogBase<WinFolderDialog> {
public:
    static std::string pick_folder_impl() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool init_com = SUCCEEDED(hr);

        std::string result;
        IFileOpenDialog* pfd = nullptr;
        hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&pfd));
        if (SUCCEEDED(hr)) {
            DWORD flags = 0;
            pfd->GetOptions(&flags);
            pfd->SetOptions(flags | FOS_PICKFOLDERS);

            if (SUCCEEDED(pfd->Show(nullptr))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(pfd->GetResult(&item))) {
                    wchar_t* path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, path, -1,
                                                      nullptr, 0, nullptr, nullptr);
                        if (len > 0) {
                            result.resize(len - 1);
                            WideCharToMultiByte(CP_UTF8, 0, path, -1,
                                                &result[0], len, nullptr, nullptr);
                        }
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            pfd->Release();
        }

        if (init_com) CoUninitialize();
        return result;
    }
};

using NativeFolderDialog = WinFolderDialog;

#endif // _WIN32

}  // namespace kforge::platform
