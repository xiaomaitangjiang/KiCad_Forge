CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
INSERT OR IGNORE INTO settings(key,value) VALUES('symbol_lib_path','');
INSERT OR IGNORE INTO settings(key,value) VALUES('footprint_lib_path','');
INSERT OR IGNORE INTO settings(key,value) VALUES('model_3d_path','');
