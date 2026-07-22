#include "storage/database.h"

#include <sqlite3.h>

namespace kforge::storage {

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
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::db(
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
        return std::unexpected(util::Error::db("SQL: " + msg));
    }
    return {};
}

util::Result<void> Database::run_migrations() {
    // Embed all CREATE TABLE statements directly — no runtime file search needed.
    // This ensures tables always exist regardless of working directory.
    const char* schema_sql = R"SQL(
        CREATE TABLE IF NOT EXISTS libraries (
            id TEXT PRIMARY KEY, name TEXT NOT NULL, file_path TEXT NOT NULL,
            description TEXT DEFAULT '', created_at TEXT DEFAULT (datetime('now')),
            updated_at TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS symbols (
            id TEXT PRIMARY KEY, library_id TEXT NOT NULL REFERENCES libraries(id) ON DELETE CASCADE,
            name TEXT NOT NULL, lib_id TEXT NOT NULL, default_value TEXT DEFAULT '',
            footprint_ref TEXT DEFAULT '', datasheet TEXT DEFAULT '',
            description TEXT DEFAULT '', mpn TEXT DEFAULT '',
            reference_prefix TEXT DEFAULT 'U', is_power INTEGER DEFAULT 0,
            pin_count INTEGER DEFAULT 0, component_type TEXT DEFAULT 'Unknown',
            package_type TEXT DEFAULT 'Unknown', properties_json TEXT DEFAULT '{}',
            created_at TEXT DEFAULT (datetime('now')),
            UNIQUE(library_id, name)
        );
        CREATE INDEX IF NOT EXISTS idx_symbols_library ON symbols(library_id);
        CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(name);

        CREATE TABLE IF NOT EXISTS footprints (
            id TEXT PRIMARY KEY, name TEXT NOT NULL, library_path TEXT DEFAULT '',
            description TEXT DEFAULT '', tags TEXT DEFAULT '',
            pad_count INTEGER DEFAULT 0, courtyard_w REAL DEFAULT 0,
            courtyard_h REAL DEFAULT 0, package_type TEXT DEFAULT 'Unknown',
            properties_json TEXT DEFAULT '{}', created_at TEXT DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_footprints_name ON footprints(name);

        CREATE TABLE IF NOT EXISTS models_3d (
            id TEXT PRIMARY KEY, file_path TEXT NOT NULL UNIQUE,
            format TEXT DEFAULT 'step', description TEXT DEFAULT '',
            width REAL DEFAULT 0, height REAL DEFAULT 0, depth REAL DEFAULT 0,
            source TEXT DEFAULT 'unknown', created_at TEXT DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_models_3d_path ON models_3d(file_path);

        CREATE TABLE IF NOT EXISTS symbol_footprint_links (
            id TEXT PRIMARY KEY,
            symbol_id TEXT NOT NULL REFERENCES symbols(id) ON DELETE CASCADE,
            footprint_id TEXT NOT NULL REFERENCES footprints(id) ON DELETE CASCADE,
            link_type TEXT DEFAULT 'explicit', confidence REAL DEFAULT 1.0,
            created_at TEXT DEFAULT (datetime('now')),
            UNIQUE(symbol_id, footprint_id)
        );
        CREATE INDEX IF NOT EXISTS idx_sfl_symbol ON symbol_footprint_links(symbol_id);
        CREATE INDEX IF NOT EXISTS idx_sfl_footprint ON symbol_footprint_links(footprint_id);

        CREATE TABLE IF NOT EXISTS footprint_model_links (
            id TEXT PRIMARY KEY,
            footprint_id TEXT NOT NULL REFERENCES footprints(id) ON DELETE CASCADE,
            model_id TEXT NOT NULL REFERENCES models_3d(id) ON DELETE CASCADE,
            link_type TEXT DEFAULT 'explicit',
            created_at TEXT DEFAULT (datetime('now')),
            UNIQUE(footprint_id, model_id)
        );
        CREATE INDEX IF NOT EXISTS idx_fml_footprint ON footprint_model_links(footprint_id);
        CREATE INDEX IF NOT EXISTS idx_fml_model ON footprint_model_links(model_id);

        CREATE TABLE IF NOT EXISTS plugin_registry (
            id TEXT PRIMARY KEY, name TEXT NOT NULL, version TEXT NOT NULL,
            author TEXT DEFAULT '', description TEXT DEFAULT '',
            manifest_path TEXT NOT NULL, binary_path TEXT NOT NULL,
            enabled INTEGER DEFAULT 1, installed_at TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS classification_rules (
            id TEXT PRIMARY KEY, name TEXT NOT NULL, priority INTEGER DEFAULT 100,
            target_library TEXT NOT NULL, condition_json TEXT NOT NULL,
            confidence INTEGER DEFAULT 80, enabled INTEGER DEFAULT 1,
            is_builtin INTEGER DEFAULT 0, created_at TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS classification_history (
            id TEXT PRIMARY KEY, component_id TEXT NOT NULL, rule_id TEXT NOT NULL,
            from_library TEXT DEFAULT '', to_library TEXT NOT NULL,
            confidence INTEGER DEFAULT 0, auto_applied INTEGER DEFAULT 0,
            applied_at TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS correspondence_issues (
            id TEXT PRIMARY KEY, component_id TEXT NOT NULL,
            issue_type TEXT NOT NULL, severity TEXT DEFAULT 'Warning',
            message TEXT DEFAULT '', resolved INTEGER DEFAULT 0,
            resolved_at TEXT, created_at TEXT DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_ci_component ON correspondence_issues(component_id);
        CREATE INDEX IF NOT EXISTS idx_ci_resolved ON correspondence_issues(resolved);

        CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY, value TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS schema_migrations (
            version INTEGER PRIMARY KEY, name TEXT NOT NULL,
            applied_at TEXT DEFAULT (datetime('now'))
        );
    )SQL";

    char* err = nullptr;
    int rc = sqlite3_exec(db_, schema_sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg(err ? err : "unknown");
        sqlite3_free(err);
        return std::unexpected(util::Error::db("Schema init: " + msg));
    }

    // Insert default settings if missing
    sqlite3_exec(db_,
        "INSERT OR IGNORE INTO settings(key,value) VALUES('symbol_lib_path','');"
        "INSERT OR IGNORE INTO settings(key,value) VALUES('footprint_lib_path','');"
        "INSERT OR IGNORE INTO settings(key,value) VALUES('model_3d_path','');",
        nullptr, nullptr, nullptr);

    // Mark migrations as applied
    sqlite3_exec(db_,
        "INSERT OR IGNORE INTO schema_migrations(version,name) VALUES(1,'001_initial.sql');"
        "INSERT OR IGNORE INTO schema_migrations(version,name) VALUES(2,'002_plugin_registry.sql');"
        "INSERT OR IGNORE INTO schema_migrations(version,name) VALUES(3,'003_settings.sql');",
        nullptr, nullptr, nullptr);

    printf("DB: schema initialized successfully\n");
    return {};
}

}  // namespace kforge::storage
