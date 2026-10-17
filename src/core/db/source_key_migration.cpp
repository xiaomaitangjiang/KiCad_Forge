#include "core/db/source_key_migration.h"

#include "core/db/source_key.h"

#include <filesystem>
#include <sqlite3.h>
#include <unordered_map>

namespace kforge::storage
{

namespace
{

struct Row
{
    std::string id;
    std::string path;
};

std::vector<Row> read_rows(sqlite3* db, const char* sql)
{
    std::vector<Row> rows;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rows.push_back({id ? id : "", path ? path : ""});
    }
    sqlite3_finalize(stmt);
    return rows;
}

bool set_source_key(sqlite3* db, const char* sql, const std::string& key, const std::string& id)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

/// 同一来源键只允许一行落键；多行一律保持 NULL 并记录冲突。
void assign_unique_keys(sqlite3* db, const std::vector<Row>& rows, const char* update_sql,
                        const char* label, int& keyed, int& unresolved,
                        std::vector<std::string>& conflicts)
{
    std::unordered_map<std::string, std::vector<std::string>> by_key;
    for (const auto& row : rows)
    {
        auto key = existing_source_key(row.path);
        if (key.empty())
        {
            unresolved++;
            continue;
        }
        by_key[key].push_back(row.id);
    }

    for (const auto& [key, ids] : by_key)
    {
        if (ids.size() > 1)
        {
            conflicts.push_back(std::string(label) + " 同一来源 " + key + " 对应 " +
                                std::to_string(ids.size()) + " 行，全部保持未绑定");
            continue;
        }
        if (set_source_key(db, update_sql, key, ids.front()))
            keyed++;
        else
            unresolved++;
    }
}

}  // namespace

SourceKeyBackfill backfill_source_keys(sqlite3* db)
{
    // 事务由调用方持有：加列、建索引、回填、记录版本要在同一个迁移事务里
    SourceKeyBackfill report;

    assign_unique_keys(db,
                       read_rows(db, "SELECT id, file_path FROM libraries WHERE file_path <> ''"),
                       "UPDATE libraries SET source_key=?1 WHERE id=?2", "libraries",
                       report.libraries_keyed, report.libraries_unresolved, report.conflicts);

    assign_unique_keys(db,
                       read_rows(db,
                                 "SELECT id, library_path FROM footprints WHERE library_path <> ''"),
                       "UPDATE footprints SET source_key=?1 WHERE id=?2", "footprints",
                       report.footprints_keyed, report.footprints_unresolved, report.conflicts);

    // 封装库名（父目录名，如 "Pkg.pretty" → "Pkg"）——解析 "Pkg:Name" 引用要用
    for (const auto& row : read_rows(
             db, "SELECT id, library_path FROM footprints WHERE library_path <> '' AND "
                 "footprint_lib_name = ''"))
    {
        std::filesystem::path path(row.path);
        if (path.extension() != ".kicad_mod")
            continue;
        set_source_key(db, "UPDATE footprints SET footprint_lib_name=?1 WHERE id=?2",
                       path.parent_path().stem().string(), row.id);
    }

    return report;
}

}  // namespace kforge::storage
