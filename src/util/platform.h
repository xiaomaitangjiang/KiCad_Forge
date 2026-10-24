// SPDX-License-Identifier: MIT
// Cross-platform executable/data directory resolution — shared by
// api_server.cpp (webui, DB, logs) and plugin_manager.cpp (default search paths)
#pragma once

#include <array>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
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
    std::wstring buf(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (length == 0 || length >= buf.size())
        throw std::runtime_error("Cannot resolve the application executable path");
    buf.resize(length);
    return std::filesystem::path(buf).parent_path();
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

inline bool is_macos_app_bundle(const std::filesystem::path& exe_dir)
{
    return exe_dir.filename() == "MacOS" && exe_dir.parent_path().filename() == "Contents" &&
           exe_dir.parent_path().parent_path().extension() == ".app";
}

/// Immutable assets live in Contents/Resources for an installed macOS app.
inline std::filesystem::path get_resource_dir(const std::filesystem::path& exe_dir = get_exe_dir())
{
    return is_macos_app_bundle(exe_dir) ? exe_dir.parent_path() / "Resources" : exe_dir;
}

/// A bundled app always uses user data; writing into the app would break
/// read-only installs and code signatures. Other layouts retain portable data.
inline std::string get_data_dir(const std::filesystem::path& exe_dir = get_exe_dir())
{
    if (is_macos_app_bundle(exe_dir))
    {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0')
            throw std::runtime_error("HOME is required to locate macOS application data");
        return (std::filesystem::path(home) / "Library/Application Support/KiCad_Forge/data").string();
    }
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

/// Resolve the private interpreter before fork, in the parent process.
inline std::filesystem::path get_python_executable(const std::filesystem::path& exe_dir = get_exe_dir())
{
    if (is_macos_app_bundle(exe_dir))
        return exe_dir.parent_path() / "Resources/python/bin/python3";
#ifdef _WIN32
    if (std::filesystem::exists(exe_dir / "kforge-distribution.json") ||
        std::filesystem::is_directory(exe_dir / "runtime/python"))
        return exe_dir / "runtime/python/python.exe";
    return "python";
#else
    if (std::filesystem::exists(exe_dir / "kforge-distribution.json") ||
        std::filesystem::is_directory(exe_dir / "runtime/python"))
        return exe_dir / "runtime/python/bin/python3";
    return "python3";
#endif
}

}  // namespace kforge::util
