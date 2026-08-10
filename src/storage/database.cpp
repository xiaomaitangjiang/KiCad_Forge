#include "storage/database.h"

#include <sqlite3.h>
#include <cstring>

#include "platform/kicad_detect.h"
#include "util/logger.h"

namespace kforge::storage {
using Kind = util::Error::Kind;

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

util::Result<std::unique_ptr<Database>> Database::open(
    const std::filesystem::path& path) {
    auto db = std::unique_ptr<Database>(new Database());
    db->path_ = path;
    std::filesystem::create_directories(path.parent_path());

    int rc = sqlite3_open(path.string().c_str(), &db->db_);
    if (rc == SQLITE_OK) {
        sqlite3_busy_timeout(db->db_, 5000);  // 5s timeout for concurrent access
    } else {
        return std::unexpected(util::Error::make<Kind::DbError>(
            std::string("sqlite3_open: ") + sqlite3_errmsg(db->db_)));
    }

    sqlite3_exec(db->db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db->db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);

    auto mig = db->run_migrations();
    if (!mig) return std::unexpected(mig.error());

    return db;
}

util::Result<void> Database::execute(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg(err ? err : "unknown");
        sqlite3_free(err);
        return std::unexpected(util::Error::make<Kind::DbError>("SQL: " + msg));
    }
    return {};
}

util::Result<void> Database::run_migrations() {
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

        // Settings + migrations
        R"SQL(CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY, value TEXT NOT NULL
        ))SQL",
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
        auto has_column = [](sqlite3* db, const char* table, const char* col) -> bool {
            char* sql = sqlite3_mprintf("PRAGMA table_info(%s)", table);
            sqlite3_stmt* stmt = nullptr;
            bool found = false;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (strcmp((const char*)sqlite3_column_text(stmt, 1), col) == 0)
                        { found = true; break; }
                }
            }
            sqlite3_finalize(stmt);
            sqlite3_free(sql);
            return found;
        };
        auto safe_alter = [&](const char* col, const char* def) {
            if (has_column(db_, "symbols", col)) return;
            char* err = nullptr;
            char* sql = sqlite3_mprintf("ALTER TABLE symbols ADD COLUMN %s TEXT DEFAULT %Q", col, def);
            int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
            if (rc != SQLITE_OK) {
                LOG_ERROR("DB migration error (ALTER TABLE symbols ADD {}): {}",
                          col, err ? err : "unknown");
                spdlog::warn("DB migration: ALTER TABLE ADD {} failed: {}",
                             col, err ? err : "unknown");
                sqlite3_free(err);
            }
            sqlite3_free(sql);
        };
        safe_alter("Kicad_Forge_ID", "");
        safe_alter("Pre_Kicad_Forge_ID", "");
        safe_alter("component_library_id", "");
    }

    // Insert default settings if missing
    sqlite3_exec(db_,
        "INSERT OR IGNORE INTO settings(key,value) VALUES('symbol_lib_path','');"
        "INSERT OR IGNORE INTO settings(key,value) VALUES('footprint_lib_path','');"
        "INSERT OR IGNORE INTO settings(key,value) VALUES('model_3d_path','');",
        nullptr, nullptr, nullptr);

    // 插入默认元件库 — 跨平台检测 KiCad 安装路径 (CRTP)
    {
        auto share = kforge::platform::NativeKicadDetector::find_share_path();
        if (!share.empty())
            LOG_INFO("KiCad detected at {}", share);
        else
            LOG_INFO("KiCad not found — skipping auto-import");
        std::string sym_path = share.empty() ? "" : share + "/symbols";
        std::string fp_path  = share.empty() ? "" : share + "/footprints";
        std::string m3d_path = share.empty() ? "" : share + "/3dmodels";

        // Remove stale default row (old migration may have symbol_path but empty fp/m3d)
        sqlite3_exec(db_, "DELETE FROM component_libraries WHERE id='default'", nullptr, nullptr, nullptr);

        char* sql = sqlite3_mprintf(
            "INSERT OR IGNORE INTO component_libraries(id,name,symbol_path,footprint_path,model_3d_path) "
            "SELECT 'default','KiCad Libraries',%Q,%Q,%Q "
            "WHERE NOT EXISTS (SELECT 1 FROM component_libraries);",
            sym_path.c_str(), fp_path.c_str(), m3d_path.c_str());
        sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        sqlite3_free(sql);

        if (!sym_path.empty()) LOG_INFO("DB: default component library -> {}", share);
    }

    // Mark migrations as applied
    sqlite3_exec(db_,
        "INSERT OR IGNORE INTO schema_migrations(version,name) VALUES(1,'001_initial.sql');"
        "INSERT OR IGNORE INTO schema_migrations(version,name) VALUES(2,'002_plugin_registry.sql');"
        "INSERT OR IGNORE INTO schema_migrations(version,name) VALUES(3,'003_settings.sql');"
        "INSERT OR IGNORE INTO schema_migrations(version,name) VALUES(4,'004_component_libraries.sql');",
        nullptr, nullptr, nullptr);

    LOG_INFO("DB: schema initialized successfully");
    return {};
}

}  // namespace kforge::storage
