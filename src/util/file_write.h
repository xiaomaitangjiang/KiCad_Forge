#pragma once

#include <filesystem>
#include <fstream>
#include <string_view>

namespace kforge::util
{
// 调用方负责同一目标文件的串行写入。失败不截断原文件。
inline void replace_file(const std::filesystem::path& path, std::string_view content)
{
    auto temporary = path;
    temporary += ".tmp";
    if (std::filesystem::symlink_status(temporary).type() != std::filesystem::file_type::not_found)
        throw std::filesystem::filesystem_error("Temporary file already exists", temporary,
            std::make_error_code(std::errc::file_exists));
    std::ofstream out;
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(temporary, std::ios::binary | std::ios::trunc);
    try
    {
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        std::filesystem::rename(temporary, path);
    }
    catch (...)
    {
        out.exceptions(std::ios::goodbit);
        if (out.is_open()) out.close();
        std::error_code error;
        std::filesystem::remove(temporary, error);
        throw;
    }
}
}
