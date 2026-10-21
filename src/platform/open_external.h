// SPDX-License-Identifier: MIT
// Open a directory or an allow-listed URL with the platform file manager/browser.
#pragma once

#include <string>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace kforge::platform
{

#ifdef _WIN32
inline std::wstring utf8_to_wide(const std::string& value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        wide.data(), size);
    return wide;
}

inline bool shell_open(const std::wstring& target)
{
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
}
#else
inline std::string shell_quote(const std::string& value)
{
    std::string quoted = "'";
    for (char c : value)
        quoted += (c == '\'') ? "'\\''" : std::string(1, c);
    quoted += "'";
    return quoted;
}
#endif

inline bool open_directory(const std::string& path)
{
#ifdef _WIN32
    return shell_open(utf8_to_wide(path));
#elif defined(__APPLE__)
    return std::system(("open " + shell_quote(path)).c_str()) == 0;
#else
    return std::system(("xdg-open " + shell_quote(path)).c_str()) == 0;
#endif
}

inline bool open_url(const std::string& url)
{
#ifdef _WIN32
    return shell_open(utf8_to_wide(url));
#elif defined(__APPLE__)
    return std::system(("open " + shell_quote(url)).c_str()) == 0;
#else
    return std::system(("xdg-open " + shell_quote(url)).c_str()) == 0;
#endif
}

}  // namespace kforge::platform
