// 已落盘来源的身份标识：规范绝对路径。
// 库名、封装名只是显示字段，跨目录同名（Device.kicad_sym / 自建 Device.kicad_sym）
// 必须落在不同行上，靠这个键区分；未落盘的虚拟记录保持空串，由 UUID 识别。
#pragma once

#include <algorithm>
#include <filesystem>
#include <string>

namespace kforge::storage
{

/// 规范路径。Windows 上按 ASCII 折叠大小写：逐字节对 UTF-8 调 tolower 会破坏
/// 非 ASCII 字节，这里只折 A-Z，其余字节原样保留。
/// 纯规范化，不判断存在性（可比较尚未创建的路径）。
inline std::string source_key(const std::filesystem::path& path)
{
    if (path.empty())
        return {};
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error)
        return {};
    auto text = canonical.generic_string();
#ifdef _WIN32
    for (char& c : text)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
#endif
    return text;
}

/// 已落盘来源的身份键；文件不存在则返回空串 —— 未落盘的虚拟记录保持 NULL，
/// 由 UUID 识别。
inline std::string existing_source_key(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return {};
    return source_key(path);
}

/// 目录归属判断，按路径组件边界比较：
/// C:/libs2 不属于 C:/libs（字符串前缀判断会误判为属于）。
/// 根自身（D:/、/）不能被裁成 D: —— 它的规范形式本来就以分隔符结尾，
/// 前缀命中的那一刻边界已经成立，不能再要求一个跟在后面的分隔符。
inline bool path_belongs_to(const std::filesystem::path& file,
                            const std::filesystem::path& root)
{
    if (root.empty() || file.empty())
        return false;
    auto base = source_key(root);
    auto child = source_key(file);
    if (base.empty() || child.empty())
        return false;
    if (child.size() < base.size())
        return false;
    if (child.compare(0, base.size(), base) != 0)
        return false;
    if (child.size() == base.size())
        return true;
    if (child[base.size()] == '/' || child[base.size()] == '\\')
        return true;
    // 根自带尾分隔符（D:/、/）：前缀命中即已跨过组件边界
    return base.back() == '/' || base.back() == '\\';
}

/// 封装引用里的 .pretty 库名："Pkg:Name" → "Pkg"，无前缀返回空串
inline std::string footprint_lib_name_of(const std::string& reference)
{
    auto colon = reference.find(':');
    if (colon == std::string::npos)
        return {};
    return reference.substr(0, colon);
}

}  // namespace kforge::storage
