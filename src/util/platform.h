// SPDX-License-Identifier: MIT
// Cross-platform executable/data directory resolution — shared by
// api_server.cpp (webui, DB, logs) and plugin_manager.cpp (default search paths)
#pragma once

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h>
#endif

namespace kforge::util
{

/// Directory containing the running executable (no trailing slash).
inline std::filesystem::path get_exe_dir()
{
#ifdef _WIN32
    std::array<char, MAX_PATH> buf{};
    GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return std::filesystem::path(buf.data()).parent_path();
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
        return std::filesystem::path(buf).parent_path();
    return ".";
#else
    return std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
}

/// Portable: <exe_dir>/data if it exists, otherwise %APPDATA%/KiCad_Forge/data
/// (installer mode) or $HOME/.KiCad_Forge.
inline std::string get_data_dir()
{
    auto exe_dir = get_exe_dir();
    auto portable = exe_dir / "data";
    if (std::filesystem::exists(portable))
    {
        return portable.string();
    }
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    return (appdata != nullptr) ? std::string(appdata) + "/KiCad_Forge/data"
                                : (exe_dir / "data").string();
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.KiCad_Forge" : "./data";
#endif
}

}  // namespace kforge::util
