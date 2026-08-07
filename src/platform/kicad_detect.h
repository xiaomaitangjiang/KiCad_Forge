// Cross-platform KiCad installation detector — CRTP pattern.
// Windows: registry search  |  macOS: /Applications search  |  Linux: known paths
#pragma once

#include <filesystem>
#include <string>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kforge::platform {

// CRTP base
template <typename Derived>
class KicadDetectorBase
{
public:
    /// Returns the share/kicad directory path, or empty string if not found.
    static std::string find_share_path()
    {
        return Derived::find_share_path_impl();
    }
};

// ============================================================
// macOS / Linux — search known paths
// ============================================================
#if defined(__APPLE__) || defined(__linux__)

class UnixKicadDetector : public KicadDetectorBase<UnixKicadDetector>
{
public:
    static std::string find_share_path_impl()
    {
        const char* candidates[] = {
#ifdef __APPLE__
            "/Applications/KiCad/KiCad.app/Contents/SharedSupport",
            "/Library/Application Support/kicad",
#else
            "/usr/share/kicad",
            "/usr/local/share/kicad",
            "/opt/kicad/share/kicad",
#endif
        };
        for (auto* p : candidates)
        {
            if (std::filesystem::exists(p))
                return p;
        }
        return "";
    }
};

using NativeKicadDetector = UnixKicadDetector;

#endif // __APPLE__ || __linux__

// ============================================================
// Windows — registry search
// ============================================================
#ifdef _WIN32

class WinKicadDetector : public KicadDetectorBase<WinKicadDetector>
{
public:
    static std::string find_share_path_impl()
    {
        std::string best_install;
        const char* reg_paths[] = {
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
            "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"};

        for (auto* rp : reg_paths)
        {
            HKEY hk;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, rp, 0, KEY_READ, &hk) != ERROR_SUCCESS)
                continue;

            char subkey[256];
            DWORD idx = 0;
            while (RegEnumKeyA(hk, idx++, subkey, sizeof(subkey)) == ERROR_SUCCESS)
            {
                HKEY sk;
                if (RegOpenKeyExA(hk, subkey, 0, KEY_READ, &sk) != ERROR_SUCCESS)
                    continue;

                char display[256] = {};
                DWORD sz = sizeof(display);
                if (RegQueryValueExA(sk, "DisplayName", nullptr, nullptr,
                                     (LPBYTE) display, &sz) == ERROR_SUCCESS)
                {
                    std::string name(display);
                    if (name.rfind("KiCad ", 0) == 0)
                    {
                        char loc[512] = {};
                        sz = sizeof(loc);
                        if (RegQueryValueExA(sk, "InstallLocation", nullptr, nullptr,
                                             (LPBYTE) loc, &sz) == ERROR_SUCCESS)
                        {
                            std::string loc_str(loc);
                            if (loc_str > best_install)
                                best_install = loc_str;
                        }
                    }
                }
                RegCloseKey(sk);
            }
            RegCloseKey(hk);
        }

        if (best_install.empty())
            return "";

        // Normalize: ensure trailing separator before appending share/kicad
        if (best_install.back() != '\\' && best_install.back() != '/')
            best_install += '/';
        std::string share = best_install + "share/kicad";
        if (std::filesystem::exists(share))
            return share;

        return "";
    }
};

using NativeKicadDetector = WinKicadDetector;

#endif // _WIN32

}  // namespace kforge::platform
