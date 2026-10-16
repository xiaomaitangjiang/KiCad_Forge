#include "core/db/database.h"
#include "core/db/source_key_migration.h"
#include "platform/kicad_detect.h"
#include "util/logger.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>

namespace kforge::storage
{
using Kind = util::Error::Kind;

namespace
{
/// 冲突/未解析行写报告文件留证 —— 迁移不猜目标，交用户显式重定位
void write_conflict_report(const std::filesystem::path& db_path,
                           const SourceKeyBackfill& report)
{
    if (report.conflicts.empty() && report.libraries_unresolved == 0 &&
        report.footprints_unresolved == 0)
        return;
    std::ofstream out(db_path.string() + ".source-key-report.txt");
    if (!out)
        return;
    out << "# KiCad_Forge 来源身份迁移报告\n";
    out << "# libraries: " << report.libraries_keyed << " keyed, "
        << report.libraries_unresolved << " unresolved\n";
    out << "# footprints: " << report.footprints_keyed << " keyed, "
        << report.footprints_unresolved << " unresolved\n";
    for (const auto& line : report.conflicts)
        out << line << "\n";
}
}  // namespace

Database::~Database()
{
    std::lock_guard lock(mutex_);  // 等仍在跑的 session 结束再关连接
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

util::Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path& path)
{
    auto db = std::unique_ptr<Database>(new Database());
    db->path_ = path;
    std::filesystem::create_directories(path.parent_path());

    int rc = sqlite3_open(path.string().c_str(), &db->db_);
    if (rc == SQLITE_OK)
    {
        sqlite3_busy_timeout(db->db_, 5000);  // 5s timeout for concurrent access
    }
    else
    {
        return std::unexpected(util::Error::make<Kind::DbError>(std::string("sqlite3_open: ") +
                                                                sqlite3_errmsg(db->db_)));
    }

    sqlite3_exec(db->db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db->db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);

    auto mig = db->run_migrations();
    if (!mig)
        return std::unexpected(mig.error());

    return db;
}

util::Result<void> Database::execute(const std::string& sql)
{
    std::lock_guard lock(mutex_);
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        std::string msg(err ? err : "unknown");
        sqlite3_free(err);
        return std::unexpected(util::Error::make<Kind::DbError>("SQL: " + msg));
    }
    return {};
}

util::Result<void> Database::backup_to(const std::filesystem::path& path)
{
    std::lock_guard lock(mutex_);
    std::filesystem::create_directories(path.parent_path());

    sqlite3* dest = nullptr;
    if (sqlite3_open(path.string().c_str(), &dest) != SQLITE_OK)
    {
        std::string message = dest ? sqlite3_errmsg(dest) : "cannot open backup target";
        if (dest)
            sqlite3_close(dest);
        return std::unexpected(util::Error::make<Kind::IoError>("Backup open: " + message));
    }

    sqlite3_backup* backup = sqlite3_backup_init(dest, "main", db_, "main");
    if (backup == nullptr)
    {
        std::string message = sqlite3_errmsg(dest);
        sqlite3_close(dest);
        return std::unexpected(util::Error::make<Kind::DbError>("Backup init: " + message));
    }

    int rc = SQLITE_OK;
    do
    {
        rc = sqlite3_backup_step(backup, 256);
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
    sqlite3_backup_finish(backup);
    std::string message = sqlite3_errmsg(dest);
    sqlite3_close(dest);

    if (rc != SQLITE_DONE)
    {
        return std::unexpected(util::Error::make<Kind::DbError>("Backup step: " + message));
    }
    return {};
}

util::Result<void> Database::run_migrations()
{
    // Schema — one string per table, executed in dependency order
    const char* migrations[] = {
        // Core tables
        R"SQL(CREATE TABLE IF NOT EXISTS libraries (
            id TEXT PRIMARY KEY, name TEXT NOT NULL, file_path TEXT NOT NULL,
            component_library_id TEXT DEFAULT '' REFERENCES component_libraries(id) ON DELETE SET DEFAULT,
            description TEXT DEFAULT '', created_at TEXT DEFAULT (datetime('now')),
            updated_at TEXT DEFAULT (datetime('now'))
        ))SQL",

        R"SQL(CREATE TABLE IF NOT EXISTS component_libraries (
            id TEXT PRIMARY KEY, name TEXT NOT NULL,
            symbol_path TEXT DEFAULT '', footprint_path TEXT DEFAULT '',
            model_3d_path TEXT DEFAULT '', enabled INTEGER DEFAULT 1,
            sort_order INTEGER DEFAULT 0, created_at TEXT DEFAULT (datetime('now'))
        ))SQL",

        // Symbols (depends on libraries)
        R"SQL(CREATE TABLE IF NOT EXISTS symbols (
            id TEXT PRIMARY KEY, library_id TEXT NOT NULL REFERENCES libraries(id) ON DELETE CASCADE,
            name TEXT NOT NULL, lib_id TEXT NOT NULL, default_value TEXT DEFAULT '',
            footprint_ref TEXT DEFAULT '', datasheet TEXT DEFAULT '',
            description TEXT DEFAULT '', mpn TEXT DEFAULT '',
            reference_prefix TEXT DEFAULT 'U', is_power INTEGER DEFAULT 0,
            pin_count INTEGER DEFAULT 0, component_type TEXT DEFAULT 'Unknown',
            package_type TEXT DEFAULT 'Unknown', properties_json TEXT DEFAULT '{}',
            Kicad_Forge_ID TEXT DEFAULT '', Pre_Kicad_Forge_ID TEXT DEFAULT '',
            created_at TEXT DEFAULT (datetime('now')),
            UNIQUE(library_id, name)
        ))SQL",
        R"SQL(CREATE INDEX IF NOT EXISTS idx_symbols_library ON symbols(library_id))SQL",
        R"SQL(CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(name))SQL",

        // Footprints
        R"SQL(CREATE TABLE IF NOT EXISTS footprints (
            id TEXT PRIMARY KEY, name TEXT NOT NULL, library_path TEXT DEFAULT '',
            description TEXT DEFAULT '', tags TEXT DEFAULT '',
            pad_count INTEGER DEFAULT 0, courtyard_w REAL DEFAULT 0,
            courtyard_h REAL DEFAULT 0, package_type TEXT DEFAULT 'Unknown',
            properties_json TEXT DEFAULT '{}', created_at TEXT DEFAULT (datetime('now'))
        ))SQL",
        R"SQL(CREATE INDEX IF NOT EXISTS idx_footprints_name ON footprints(name))SQL",

        // 3D Models
        R"SQL(CREATE TABLE IF NOT EXISTS models_3d (
            id TEXT PRIMARY KEY, file_path TEXT NOT NULL UNIQUE,
            format TEXT DEFAULT 'step', description TEXT DEFAULT '',
            width REAL DEFAULT 0, height REAL DEFAULT 0, depth REAL DEFAULT 0,
            source TEXT DEFAULT 'unknown', created_at TEXT DEFAULT (datetime('now'))
        ))SQL",
        R"SQL(CREATE INDEX IF NOT EXISTS idx_models_3d_path ON models_3d(file_path))SQL",

        // Relationship tables
        R"SQL(CREATE TABLE IF NOT EXISTS symbol_footprint_links (
            id TEXT PRIMARY KEY,
            symbol_id TEXT NOT NULL REFERENCES symbols(id) ON DELETE CASCADE,
            footprint_id TEXT NOT NULL REFERENCES footprints(id) ON DELETE CASCADE,
            link_type TEXT DEFAULT 'explicit', confidence REAL DEFAULT 1.0,
            created_at TEXT DEFAULT (datetime('now')),
            UNIQUE(symbol_id, footprint_id)
        ))SQL",
        R"SQL(CREATE INDEX IF NOT EXISTS idx_sfl_symbol ON symbol_footprint_links(symbol_id))SQL",
        R"SQL(CREATE INDEX IF NOT EXISTS idx_sfl_footprint ON symbol_footprint_links(footprint_id))SQL",

        R"SQL(CREATE TABLE IF NOT EXISTS footprint_model_links (
            id TEXT PRIMARY KEY,
            footprint_id TEXT NOT NULL REFERENCES footprints(id) ON DELETE CASCADE,
            model_id TEXT NOT NULL REFERENCES models_3d(id) ON DELETE CASCADE,
            link_type TEXT DEFAULT 'explicit',
            created_at TEXT DEFAULT (datetime('now')),
            UNIQUE(footprint_id, model_id)
        ))SQL",
        R"SQL(CREATE INDEX IF NOT EXISTS idx_fml_footprint ON footprint_model_links(footprint_id))SQL",
        R"SQL(CREATE INDEX IF NOT EXISTS idx_fml_model ON footprint_model_links(model_id))SQL",

        // Plugin registry
        R"SQL(CREATE TABLE IF NOT EXISTS plugin_registry (
            id TEXT PRIMARY KEY, name TEXT NOT NULL, version TEXT NOT NULL,
            author TEXT DEFAULT '', description TEXT DEFAULT '',
            manifest_path TEXT NOT NULL, binary_path TEXT NOT NULL,
            enabled INTEGER DEFAULT 1, installed_at TEXT DEFAULT (datetime('now'))
        ))SQL",

        // Classification
        R"SQL(CREATE TABLE IF NOT EXISTS classification_rules (
            id TEXT PRIMARY KEY, name TEXT NOT NULL, priority INTEGER DEFAULT 100,
            target_library TEXT NOT NULL, condition_json TEXT NOT NULL,
            confidence INTEGER DEFAULT 80, enabled INTEGER DEFAULT 1,
            is_builtin INTEGER DEFAULT 0, created_at TEXT DEFAULT (datetime('now'))
        ))SQL",
        R"SQL(CREATE TABLE IF NOT EXISTS classification_history (
            id TEXT PRIMARY KEY, component_id TEXT NOT NULL, rule_id TEXT NOT NULL,
            from_library TEXT DEFAULT '', to_library TEXT NOT NULL,
            confidence INTEGER DEFAULT 0, auto_applied INTEGER DEFAULT 0,
            applied_at TEXT DEFAULT (datetime('now'))
        ))SQL",

        // Issues
        R"SQL(CREATE TABLE IF NOT EXISTS correspondence_issues (
            id TEXT PRIMARY KEY, component_id TEXT NOT NULL,
            issue_type TEXT NOT NULL, severity TEXT DEFAULT 'Warning',
            message TEXT DEFAULT '', resolved INTEGER DEFAULT 0,
            resolved_at TEXT, created_at TEXT DEFAULT (datetime('now'))
        ))SQL",
        R"SQL(CREATE INDEX IF NOT EXISTS idx_ci_component ON correspondence_issues(component_id))SQL",
        R"SQL(CREATE INDEX IF NOT EXISTS idx_ci_resolved ON correspondence_issues(resolved))SQL",

        // 导入来源映射（C02）：(目标库, provider, 外部编号) → 符号 UUID。
        // 同源重试/新版本要更新原行，异源同名要报冲突，都靠它判定
        R"SQL(CREATE TABLE IF NOT EXISTS import_sources (
            id TEXT PRIMARY KEY, target_library_id TEXT NOT NULL,
            provider TEXT NOT NULL, external_id TEXT NOT NULL,
            symbol_id TEXT NOT NULL, symbol_name TEXT NOT NULL,
            content_digest TEXT NOT NULL,
            updated_at TEXT DEFAULT (datetime('now')),
            UNIQUE(target_library_id, provider, external_id)
        ))SQL",

        // 操作幂等：同一个操作 ID + 同样的请求内容 → 返回原结果；
        // 同一 ID 换了内容 → 拒绝
        R"SQL(CREATE TABLE IF NOT EXISTS import_operations (
            id TEXT PRIMARY KEY, request_digest TEXT NOT NULL, result_json TEXT NOT NULL,
            created_at TEXT DEFAULT (datetime('now'))
        ))SQL",

        // 源文件变更恢复日志（A05）：prepared → applied 两态，
        // 业务变更与 applied 必须落在同一个事务里
        R"SQL(CREATE TABLE IF NOT EXISTS file_mutations (
            id TEXT PRIMARY KEY, state TEXT NOT NULL, plan_json TEXT NOT NULL,
            created_at TEXT DEFAULT (datetime('now'))
        ))SQL",

        // Settings are stored in JSON (util/config_store), not the DB.
        R"SQL(CREATE TABLE IF NOT EXISTS imported_files (
            file_path TEXT PRIMARY KEY,
            mtime INTEGER NOT NULL,
            imported_at TEXT DEFAULT (datetime('now'))
        ))SQL",

        // Dedup legacy duplicate link rows — link inserts were not idempotent
        R"SQL(DELETE FROM symbol_footprint_links WHERE id NOT IN (
            SELECT MIN(id) FROM symbol_footprint_links GROUP BY symbol_id, footprint_id))SQL",
        R"SQL(DELETE FROM footprint_model_links WHERE id NOT IN (
            SELECT MIN(id) FROM footprint_model_links GROUP BY footprint_id, model_id))SQL",
        R"SQL(CREATE TABLE IF NOT EXISTS schema_migrations (
            version INTEGER PRIMARY KEY, name TEXT NOT NULL,
            applied_at TEXT DEFAULT (datetime('now'))
        ))SQL",
    };

    for (auto* sql : migrations)
    {
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK)
        {
            std::string msg(err ? err : "unknown");
            sqlite3_free(err);
            return std::unexpected(util::Error::make<Kind::DbError>("Schema init: " + msg));
        }
    }

    // Migration: add Kicad_Forge_ID columns for existing databases
    // Use PRAGMA table_info to check before ALTER — avoids silent failures from
    // disk-full, corruption, or permission errors.
    {
        auto has_column = [](sqlite3* db, const char* table, const char* col) -> bool
        {
            char* sql = sqlite3_mprintf("PRAGMA table_info(%s)", table);
            sqlite3_stmt* stmt = nullptr;
            bool found = false;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK)
            {
                while (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    if (strcmp((const char*) sqlite3_column_text(stmt, 1), col) == 0)
                    {
                        found = true;
                        break;
                    }
                }
            }
            sqlite3_finalize(stmt);
            sqlite3_free(sql);
            return found;
        };
        // Target table is explicit — component_library_id belongs to
        // `libraries`, and hardcoding `symbols` left the real column missing.
        auto safe_alter = [&](const char* table, const char* col, const char* def)
        {
            if (has_column(db_, table, col))
                return;
            char* err = nullptr;
            char* sql =
                sqlite3_mprintf("ALTER TABLE %s ADD COLUMN %s TEXT DEFAULT %Q", table, col, def);
            int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
            if (rc != SQLITE_OK)
            {
                kforge::util::log_error{}("DB migration error (ALTER TABLE {} ADD {}): {}", table,
                                          col, err ? err : "unknown");
                kforge::util::log_warn{}("DB migration: ALTER TABLE {} ADD {} failed: {}", table,
                                         col, err ? err : "unknown");
                sqlite3_free(err);
            }
            sqlite3_free(sql);
        };
        safe_alter("symbols", "Kicad_Forge_ID", "");
        safe_alter("symbols", "Pre_Kicad_Forge_ID", "");
        safe_alter("libraries", "component_library_id", "");

        // Component library lock flags (official KiCad libraries are locked)
        auto safe_alter_int = [&](const char* table, const char* col, int def)
        {
            if (has_column(db_, table, col))
                return;
            char* err = nullptr;
            char* sql = sqlite3_mprintf(
                "ALTER TABLE %s ADD COLUMN %s INTEGER DEFAULT %d", table, col, def);
            int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
            if (rc != SQLITE_OK)
            {
                kforge::util::log_error{}("DB migration error (ALTER TABLE {} ADD {}): {}", table,
                                          col, err ? err : "unknown");
                kforge::util::log_warn{}("DB migration: ALTER TABLE ADD {} failed: {}", col,
                                         err ? err : "unknown");
                sqlite3_free(err);
            }
            sqlite3_free(sql);
        };
        safe_alter_int("component_libraries", "locked", 0);
        safe_alter_int("component_libraries", "protected", 0);

        // Mark existing official KiCad libraries as locked & protected
        sqlite3_exec(db_,
                     "UPDATE component_libraries SET protected=1, locked=1 WHERE "
                     "symbol_path LIKE '%share/kicad%' OR footprint_path LIKE '%share/kicad%' "
                     "OR model_3d_path LIKE '%share/kicad%'",
                     nullptr, nullptr, nullptr);

        // ---- A03：来源身份（source_key / footprint_lib_name）----
        // 同名不同来源的库/封装必须落在不同行上，身份取规范路径。
        // 迁移前用 SQLite backup API 留一致性快照（不能直接复制文件：
        // WAL 里可能还有未检查点的页）。
        if (!has_column(db_, "libraries", "source_key"))
        {
            std::filesystem::path snapshot(path_.string() + ".pre-a03.bak");
            if (auto backed = backup_to(snapshot); backed)
                kforge::util::log_info{}("DB: pre-migration snapshot -> {}",
                                         snapshot.string());
            else
                kforge::util::log_error{}("DB: pre-migration snapshot failed: {}",
                                          backed.error().format_message());

            const char* ddl[] = {
                "ALTER TABLE libraries ADD COLUMN source_key TEXT",
                "ALTER TABLE footprints ADD COLUMN source_key TEXT",
                "ALTER TABLE footprints ADD COLUMN footprint_lib_name TEXT DEFAULT ''",
                "CREATE UNIQUE INDEX IF NOT EXISTS uq_libraries_source_key "
                "ON libraries(source_key) WHERE source_key IS NOT NULL",
                "CREATE UNIQUE INDEX IF NOT EXISTS uq_footprints_source_key "
                "ON footprints(source_key) WHERE source_key IS NOT NULL",
                "CREATE INDEX IF NOT EXISTS ix_footprints_ref "
                "ON footprints(footprint_lib_name, name)",
            };
            bool migrated = sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) ==
                            SQLITE_OK;
            std::string failure;
            if (migrated)
            {
                for (const char* sql : ddl)
                {
                    char* err = nullptr;
                    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK)
                    {
                        failure = err ? err : "unknown";
                        sqlite3_free(err);
                        break;
                    }
                }
            }

            SourceKeyBackfill report;
            if (migrated && failure.empty())
            {
                report = backfill_source_keys(db_);
                sqlite3_exec(db_,
                             "INSERT OR IGNORE INTO schema_migrations(version,name) "
                             "VALUES(5,'005_source_key.sql')",
                             nullptr, nullptr, nullptr);
                if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
                {
                    failure = sqlite3_errmsg(db_);
                    migrated = false;
                }
            }
            else if (migrated)
            {
                sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                migrated = false;
            }

            if (!failure.empty())
            {
                kforge::util::log_error{}("DB: source_key migration failed: {}", failure);
            }
            else
            {
                kforge::util::log_info{}("DB: source_key backfill — libraries {} keyed / {} "
                                         "unresolved, footprints {} keyed / {} unresolved",
                                         report.libraries_keyed, report.libraries_unresolved,
                                         report.footprints_keyed, report.footprints_unresolved);
                write_conflict_report(path_, report);
            }
        }

        // 迁移后校验外键完整性
        {
            sqlite3_stmt* stmt = nullptr;
            int violations = 0;
            if (sqlite3_prepare_v2(db_, "PRAGMA foreign_key_check", -1, &stmt, nullptr) == SQLITE_OK)
            {
                while (sqlite3_step(stmt) == SQLITE_ROW)
                    violations++;
            }
            sqlite3_finalize(stmt);
            if (violations > 0)
                kforge::util::log_error{}("DB: foreign_key_check reported {} violations",
                                          violations);
        }
    }

    // Settings are stored in JSON (util/config_store), not the DB.

    // 插入默认元件库 — 跨平台检测 KiCad 安装路径 (CRTP)
    {
        auto share = kforge::platform::NativeKicadDetector::find_share_path();
        if (!share.empty())
            kforge::util::log_info{}("KiCad detected at {}", share);
        else
            kforge::util::log_info{}("KiCad not found — skipping auto-import");
        std::string sym_path = share.empty() ? "" : share + "/symbols";
        std::string fp_path = share.empty() ? "" : share + "/footprints";
        std::string m3d_path = share.empty() ? "" : share + "/3dmodels";

        // The default library keeps its stable id 'default'. Never DELETE it —
        // imported libraries reference component_library_id='default', and a
        // delete would orphan them (or trip foreign keys). Upsert instead:
        // refresh the paths each start so a moved KiCad install is picked up.
        if (!sym_path.empty())
        {
            char* sql = sqlite3_mprintf(
                "INSERT INTO component_libraries(id,name,symbol_path,footprint_path,"
                "model_3d_path,locked,protected) "
                "VALUES('default','KiCad Libraries',%Q,%Q,%Q,1,1) "
                "ON CONFLICT(id) DO UPDATE SET "
                "symbol_path=excluded.symbol_path, footprint_path=excluded.footprint_path, "
                "model_3d_path=excluded.model_3d_path",
                sym_path.c_str(), fp_path.c_str(), m3d_path.c_str());
            sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
            sqlite3_free(sql);
        }
        else
        {
            // KiCad not found: keep an empty default only when nothing exists
            sqlite3_exec(db_,
                         "INSERT OR IGNORE INTO component_libraries(id,name,locked,protected) "
                         "SELECT 'default','KiCad Libraries',1,1 "
                         "WHERE NOT EXISTS (SELECT 1 FROM component_libraries);",
                         nullptr, nullptr, nullptr);
        }

        if (!sym_path.empty())
            kforge::util::log_info{}("DB: default component library -> {}", share);
    }

    // Mark migrations as applied
    sqlite3_exec(
        db_,
        "INSERT OR IGNORE INTO schema_migrations(version,name) VALUES(1,'001_initial.sql');"
        "INSERT OR IGNORE INTO schema_migrations(version,name) VALUES(2,'002_plugin_registry.sql');"
        "INSERT OR IGNORE INTO schema_migrations(version,name) VALUES(3,'003_drop_settings.sql');"
        "INSERT OR IGNORE INTO schema_migrations(version,name) "
        "VALUES(4,'004_component_libraries.sql');",
        nullptr, nullptr, nullptr);

    kforge::util::log_info{}("DB: schema initialized successfully");
    return {};
}

}  // namespace kforge::storage