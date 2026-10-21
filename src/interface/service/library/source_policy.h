#pragma once

#include "core/db/source_key.h"
#include "core/io/source_mutation.h"
#include "core/repo/repositories.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace kforge::services
{
/// 规范路径 —— 实现见 core/db/source_key.h（与库/封装的来源键同一套规则）
inline std::filesystem::path normalized_source_path(const std::filesystem::path& path)
{
    return storage::source_key(path);
}

/// 目录归属按路径组件比较（C:/libs2 不属于 C:/libs）
inline bool source_belongs_to(const std::filesystem::path& file, const std::string& root)
{
    return storage::path_belongs_to(file, root);
}

/// 薄封装：判定实现只有一份（core/io/source_mutation.cpp），
/// 接口层不再复制第二套权限判断
inline util::Result<bool> source_is_locked(const storage::DbSession& session,
                                           const std::filesystem::path& path,
                                           const std::string& owner = "")
{
    return io::source_write_blocked(session, path, owner);
}
}
