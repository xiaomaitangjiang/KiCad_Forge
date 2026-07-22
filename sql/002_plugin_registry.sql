-- Plugin registry and classification results

CREATE TABLE IF NOT EXISTS plugin_registry (
    id          TEXT PRIMARY KEY,
    name        TEXT NOT NULL,
    version     TEXT NOT NULL,
    author      TEXT DEFAULT '',
    description TEXT DEFAULT '',
    manifest_path TEXT NOT NULL,
    binary_path  TEXT NOT NULL,
    enabled     INTEGER DEFAULT 1,
    installed_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS classification_rules (
    id          TEXT PRIMARY KEY,
    name        TEXT NOT NULL,
    priority    INTEGER DEFAULT 100,
    target_library TEXT NOT NULL,
    condition_json TEXT NOT NULL,  -- JSON-serialized condition AST
    confidence  INTEGER DEFAULT 80,
    enabled     INTEGER DEFAULT 1,
    is_builtin  INTEGER DEFAULT 0,
    created_at  TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS classification_history (
    id            TEXT PRIMARY KEY,
    component_id  TEXT NOT NULL,
    rule_id       TEXT NOT NULL,
    from_library  TEXT DEFAULT '',
    to_library    TEXT NOT NULL,
    confidence    INTEGER DEFAULT 0,
    auto_applied  INTEGER DEFAULT 0,
    applied_at    TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS correspondence_issues (
    id          TEXT PRIMARY KEY,
    component_id TEXT NOT NULL,
    issue_type  TEXT NOT NULL,  -- MissingFootprint, MissingModel3D, etc.
    severity    TEXT DEFAULT 'Warning',  -- Error, Warning, Info
    message     TEXT DEFAULT '',
    resolved    INTEGER DEFAULT 0,
    resolved_at TEXT,
    created_at  TEXT DEFAULT (datetime('now'))
);
CREATE INDEX idx_ci_component ON correspondence_issues(component_id);
CREATE INDEX idx_ci_resolved ON correspondence_issues(resolved);

-- Schema version tracking
CREATE TABLE IF NOT EXISTS schema_migrations (
    version     INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    applied_at  TEXT DEFAULT (datetime('now'))
);
