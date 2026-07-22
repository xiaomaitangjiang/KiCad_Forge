-- Initial schema: libraries, symbols, footprints, models, relationships

CREATE TABLE IF NOT EXISTS libraries (
    id          TEXT PRIMARY KEY,
    name        TEXT NOT NULL,
    file_path   TEXT NOT NULL,
    description TEXT DEFAULT '',
    created_at  TEXT DEFAULT (datetime('now')),
    updated_at  TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS symbols (
    id               TEXT PRIMARY KEY,
    library_id       TEXT NOT NULL REFERENCES libraries(id) ON DELETE CASCADE,
    name             TEXT NOT NULL,
    lib_id           TEXT NOT NULL,
    default_value    TEXT DEFAULT '',
    footprint_ref    TEXT DEFAULT '',
    datasheet        TEXT DEFAULT '',
    description      TEXT DEFAULT '',
    mpn              TEXT DEFAULT '',
    reference_prefix TEXT DEFAULT 'U',
    is_power         INTEGER DEFAULT 0,
    pin_count        INTEGER DEFAULT 0,
    component_type   TEXT DEFAULT 'Unknown',
    package_type     TEXT DEFAULT 'Unknown',
    properties_json  TEXT DEFAULT '{}',
    created_at       TEXT DEFAULT (datetime('now'))
);
CREATE INDEX idx_symbols_library ON symbols(library_id);
CREATE INDEX idx_symbols_name ON symbols(name);

CREATE TABLE IF NOT EXISTS footprints (
    id               TEXT PRIMARY KEY,
    name             TEXT NOT NULL,
    library_path     TEXT DEFAULT '',
    description      TEXT DEFAULT '',
    tags             TEXT DEFAULT '',
    pad_count        INTEGER DEFAULT 0,
    courtyard_w      REAL DEFAULT 0,
    courtyard_h      REAL DEFAULT 0,
    package_type     TEXT DEFAULT 'Unknown',
    properties_json  TEXT DEFAULT '{}',
    created_at       TEXT DEFAULT (datetime('now'))
);
CREATE INDEX idx_footprints_name ON footprints(name);

CREATE TABLE IF NOT EXISTS models_3d (
    id          TEXT PRIMARY KEY,
    file_path   TEXT NOT NULL UNIQUE,
    format      TEXT DEFAULT 'step',
    description TEXT DEFAULT '',
    width       REAL DEFAULT 0,
    height      REAL DEFAULT 0,
    depth       REAL DEFAULT 0,
    source      TEXT DEFAULT 'unknown',
    created_at  TEXT DEFAULT (datetime('now'))
);
CREATE INDEX idx_models_3d_path ON models_3d(file_path);

-- Symbol ↔ Footprint relationships
CREATE TABLE IF NOT EXISTS symbol_footprint_links (
    id          TEXT PRIMARY KEY,
    symbol_id   TEXT NOT NULL REFERENCES symbols(id) ON DELETE CASCADE,
    footprint_id TEXT NOT NULL REFERENCES footprints(id) ON DELETE CASCADE,
    link_type   TEXT DEFAULT 'explicit',  -- explicit, heuristic, manual
    confidence  REAL DEFAULT 1.0,
    created_at  TEXT DEFAULT (datetime('now')),
    UNIQUE(symbol_id, footprint_id)
);
CREATE INDEX idx_sfl_symbol ON symbol_footprint_links(symbol_id);
CREATE INDEX idx_sfl_footprint ON symbol_footprint_links(footprint_id);

-- Footprint ↔ 3D Model relationships
CREATE TABLE IF NOT EXISTS footprint_model_links (
    id           TEXT PRIMARY KEY,
    footprint_id TEXT NOT NULL REFERENCES footprints(id) ON DELETE CASCADE,
    model_id     TEXT NOT NULL REFERENCES models_3d(id) ON DELETE CASCADE,
    link_type    TEXT DEFAULT 'explicit',
    created_at   TEXT DEFAULT (datetime('now')),
    UNIQUE(footprint_id, model_id)
);
CREATE INDEX idx_fml_footprint ON footprint_model_links(footprint_id);
CREATE INDEX idx_fml_model ON footprint_model_links(model_id);
