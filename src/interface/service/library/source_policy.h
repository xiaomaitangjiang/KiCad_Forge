#pragma once

#include "core/repo/repositories.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace kforge::services
{
inline std::filesystem::path normalized_source_path(const std::filesystem::path& path)
{
    auto text = std::filesystem::weakly_canonical(path).generic_string();
#ifdef _WIN32
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return std::tolower(c); });
#endif
    return text;
}

inline bool source_belongs_to(const std::filesystem::path& file, const std::string& root)
{
    if (root.empty()) return false;
    auto base = normalized_source_path(root);
    auto child = normalized_source_path(file);
    auto [end, unused] = std::mismatch(base.begin(), base.end(), child.begin(), child.end());
    return end == base.end();
}

inline util::Result<bool> source_is_locked(sqlite3* db, const std::filesystem::path& path,
                                           const std::string& owner = "")
{
    auto normalized = normalized_source_path(path);
    bool share = false;
    for (const auto& component : normalized)
    {
        if (share && component == "kicad") return true;
        share = component == "share";
    }
    auto libraries = storage::ComponentLibraryRepository(db).find_all();
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
