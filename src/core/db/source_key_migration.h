// A03 迁移：为已存在的库/封装行回填来源键。
// 规则：只有唯一确定的规范路径才写入；同一路径对应多行、路径无效或归属不明的
// 行保留 UUID、外键和旧路径，source_key 留空 + 冲突清单，交用户显式重定位。
#pragma once

#include <string>
#include <vector>

struct sqlite3;

namespace kforge::storage
{

struct SourceKeyBackfill
{
    int libraries_keyed = 0;
    int footprints_keyed = 0;
    int libraries_unresolved = 0;  // 文件缺失/路径无效 → 保持 NULL
    int footprints_unresolved = 0;
    std::vector<std::string> conflicts;  // 冲突明细，供报告文件
};

/// 回填 source_key 与 footprint_lib_name（不删除、不合并任何旧行）
SourceKeyBackfill backfill_source_keys(sqlite3* db);

}  // namespace kforge::storage
