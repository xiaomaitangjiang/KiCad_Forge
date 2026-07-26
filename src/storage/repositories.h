#pragma once

#include <vector>
#include <optional>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

#include "core/types.h"
#include "core/symbol.h"
#include "core/footprint.h"
#include "core/model_3d.h"
#include "core/component.h"
#include "util/result.h"

namespace kforge::storage {

// ============================================================
// LibraryRepository — CRUD for libraries
// ============================================================
class LibraryRepository {
public:
    explicit LibraryRepository(sqlite3* db);

    util::Result<core::LibraryMeta> insert(const core::LibraryMeta& lib);
    util::Result<std::vector<core::LibraryMeta>> find_all();
    util::Result<void> remove(const core::Uuid& id);
    int count() const;

private:
    sqlite3* db_;
};

// ============================================================
// SymbolRepository — CRUD for symbols
// ============================================================
class SymbolRepository {
public:
    explicit SymbolRepository(sqlite3* db);

    util::Result<core::Symbol> insert(const core::Symbol& sym);
    util::Result<void> update(const core::Symbol& sym);
    util::Result<void> remove(const core::Uuid& id);
    util::Result<core::Symbol> find_by_id(const core::Uuid& id);
    util::Result<core::Symbol> find_by_name(const std::string& name);
    util::Result<std::vector<core::Symbol>> find_by_library(const core::Uuid& lib_id);
    util::Result<std::vector<core::Symbol>> find_all();
    util::Result<std::vector<core::Symbol>> search(const std::string& keyword);
    int count() const;

private:
    core::Symbol row_to_symbol(sqlite3_stmt* stmt) const;
    void bind_symbol_params(sqlite3_stmt* stmt, const core::Symbol& sym) const;
    sqlite3* db_;
};

// ============================================================
// FootprintRepository — CRUD for footprints
// ============================================================
class FootprintRepository {
public:
    explicit FootprintRepository(sqlite3* db);

    util::Result<core::Footprint> insert(const core::Footprint& fp);
    util::Result<void> update(const core::Footprint& fp);
    util::Result<void> remove(const core::Uuid& id);
    util::Result<core::Footprint> find_by_id(const core::Uuid& id);
    util::Result<core::Footprint> find_by_name(const std::string& name);
    util::Result<std::vector<core::Footprint>> find_all();
    util::Result<std::vector<core::Footprint>> search(const std::string& keyword);
    int count() const;

private:
    core::Footprint row_to_footprint(sqlite3_stmt* stmt) const;
    sqlite3* db_;
};

// ============================================================
// Model3DRepository — CRUD for 3D models
// ============================================================
class Model3DRepository {
public:
    explicit Model3DRepository(sqlite3* db);

    util::Result<core::Model3D> insert(const core::Model3D& m);
    util::Result<void> remove(const core::Uuid& id);
    util::Result<core::Model3D> find_by_id(const core::Uuid& id);
    util::Result<core::Model3D> find_by_path(const std::string& path);
    util::Result<std::vector<core::Model3D>> find_all();
    util::Result<std::vector<core::Model3D>> find_orphans();
    int count() const;

private:
    core::Model3D row_to_model(sqlite3_stmt* stmt) const;
    sqlite3* db_;
};

// ============================================================
// RelationshipRepository — symbol↔footprint↔3D links
// ============================================================
class RelationshipRepository {
public:
    explicit RelationshipRepository(sqlite3* db);

    // Symbol ↔ Footprint
    util::Result<void> link_symbol_to_footprint(
        const core::Uuid& sym_id, const core::Uuid& fp_id,
        const std::string& link_type = "explicit", double confidence = 1.0);
    util::Result<void> unlink_symbol_footprint(const core::Uuid& sym_id,
                                                const core::Uuid& fp_id);
    util::Result<std::optional<core::Uuid>> find_footprint_for_symbol(
        const core::Uuid& sym_id);
    util::Result<std::vector<core::Uuid>> find_symbols_for_footprint(
        const core::Uuid& fp_id);

    // Footprint ↔ 3D Model
    util::Result<void> link_footprint_to_model(
        const core::Uuid& fp_id, const core::Uuid& model_id,
        const std::string& link_type = "explicit");
    util::Result<void> unlink_footprint_model(const core::Uuid& fp_id,
                                               const core::Uuid& model_id);
    util::Result<std::vector<core::Uuid>> find_models_for_footprint(
        const core::Uuid& fp_id);

    // Bulk queries
    util::Result<std::vector<core::Uuid>> find_symbols_without_footprints();
    util::Result<std::vector<core::Uuid>> find_footprints_without_symbols();
    util::Result<std::vector<core::Uuid>> find_footprints_without_3d_models();
    util::Result<std::vector<core::Uuid>> find_orphan_models();

private:
    sqlite3* db_;
};

// ============================================================
// SettingsRepository — key-value settings
// ============================================================
class SettingsRepository {
public:
    explicit SettingsRepository(sqlite3* db);

    util::Result<std::string> get(const std::string& key);
    util::Result<void> set(const std::string& key, const std::string& value);
    util::Result<std::unordered_map<std::string, std::string>> all();

private:
    sqlite3* db_;
};

}  // namespace kforge::storage
