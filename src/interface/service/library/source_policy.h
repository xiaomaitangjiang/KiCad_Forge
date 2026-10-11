#pragma once

#include "core/db/source_key.h"
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

inline util::Result<bool> source_is_locked(const storage::DbSession& session,
                                           const std::filesystem::path& path,
                                           const std::string& owner = "")
{
    auto normalized = normalized_source_path(path);
    bool share = false;
    for (const auto& component : normalized)
    {
        if (share && component == "kicad") return true;
        share = component == "share";
    }
    auto libraries = storage::ComponentLibraryRepository(session).find_all();
    if (!libraries) return std::unexpected(libraries.error());
    for (const auto& lib : *libraries)
    {
        if (!(lib.locked || lib.is_protected)) continue;
        if ((!owner.empty() && lib.id == owner) ||
            source_belongs_to(path, lib.symbol_path) ||
            source_belongs_to(path, lib.footprint_path) ||
            source_belongs_to(path, lib.model_3d_path)) return true;
    }
    return false;
}
}
