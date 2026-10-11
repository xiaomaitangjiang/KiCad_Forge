#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

#include "core/db/db_session.h"
#include "core/db/source_key.h"
#include "core/model/types.h"
#include "core/model/symbol.h"
#include "core/model/footprint.h"
#include "core/model/model_3d.h"
#include "core/model/component.h"
#include "util/error.h"

namespace kforge::storage {

// ============================================================
// LibraryRepository — CRUD for libraries
// ============================================================
class LibraryRepository {
public:
    // 借用 session 的连接；仓储不得比 session 活得久
    explicit LibraryRepository(const DbSession& session) : db_(session.handle()) {}

    /// 身份是来源键（同名不同来源是两行）；虚拟库（来源键为空）按 UUID 落行
    util::Result<core::LibraryMeta> insert(const core::LibraryMeta& lib);
    util::Result<core::LibraryMeta> find_by_source_key(const std::string& source_key);
    /// 命中同一来源则保留 UUID 并刷新名称/路径/归属，否则插入
    util::Result<core::LibraryMeta> upsert_by_source_key(const core::LibraryMeta& lib);
    util::Result<std::vector<core::LibraryMeta>> find_all();
    util::Result<void> remove(const core::Uuid& id);
    [[nodiscard]] int count() const;

private:
    sqlite3* db_;
};

// ============================================================
// SymbolRepository — CRUD for symbols
// ============================================================
class SymbolRepository {
public:
    explicit SymbolRepository(const DbSession& session) : db_(session.handle()) {}

    util::Result<core::Symbol> insert(const core::Symbol& sym);
    util::Result<void> update(const core::Symbol& sym);
    util::Result<void> remove(const core::Uuid& id);
    util::Result<core::Symbol> find_by_id(const core::Uuid& id);
    util::Result<core::Symbol> find_by_name(const std::string& name);
    util::Result<std::vector<core::Symbol>> find_by_library(const core::Uuid& lib_id);
    util::Result<std::vector<core::Symbol>> find_all();
    util::Result<std::vector<core::Symbol>> search(const std::string& keyword);
    /// 引用某个元件类型 ID 的符号数（删除类型前的引用检查）
    int count_by_component_type(const std::string& type_id) const;
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
    explicit FootprintRepository(const DbSession& session) : db_(session.handle()) {}

    util::Result<core::Footprint> insert(const core::Footprint& fp);
    util::Result<void> update(const core::Footprint& fp);
    util::Result<void> remove(const core::Uuid& id);
    util::Result<core::Footprint> find_by_id(const core::Uuid& id);
    util::Result<core::Footprint> find_by_name(const std::string& name);
    /// 按来源规范路径定位（同名不同 .pretty 目录的封装靠它区分）
    util::Result<core::Footprint> find_by_source_key(const std::string& source_key);
    util::Result<std::vector<core::Footprint>> find_all();
    util::Result<std::vector<core::Footprint>> search(const std::string& keyword);
    /// 引用某个封装类型 ID 的封装数（删除类型前的引用检查）
    int count_by_package_type(const std::string& type_id) const;
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
    explicit Model3DRepository(const DbSession& session) : db_(session.handle()) {}

    util::Result<core::Model3D> insert(const core::Model3D& m);
    util::Result<void> update(const core::Model3D& m);
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
    explicit RelationshipRepository(const DbSession& session) : db_(session.handle()) {}

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

    // Bulk: all existing symbol→footprint links in one query (symbol_id → footprint_id)
    util::Result<std::unordered_map<std::string, std::string>> find_all_symbol_links();

    // Bulk: all footprint→model links (footprint_id → model ids)，避免按行点查
    util::Result<std::unordered_map<std::string, std::vector<std::string>>>
    find_all_model_links();

    // Bulk queries
    util::Result<std::vector<core::Uuid>> find_symbols_without_footprints();
    util::Result<std::vector<core::Uuid>> find_footprints_without_symbols();
    util::Result<std::vector<core::Uuid>> find_footprints_without_3d_models();
    util::Result<std::vector<core::Uuid>> find_orphan_models();

private:
    sqlite3* db_;
};

// ============================================================
// 元件库仓库 — 符号+封装+3D 路径的组合
// ============================================================
class ComponentLibraryRepository {
public:
    explicit ComponentLibraryRepository(const DbSession& session) : db_(session.handle()) {}

    struct ComponentLibrary {
        std::string id;
        std::string name;
        std::string symbol_path;
        std::string footprint_path;
        std::string model_3d_path;
        bool enabled{true};
        int sort_order{0};
        bool locked{false};        // user-set lock — prevents delete/modify
        bool is_protected{false};  // official KiCad lib — cannot be unlocked
    };

    util::Result<ComponentLibrary> insert(const ComponentLibrary& lib);
    util::Result<void> update(const ComponentLibrary& lib);
    util::Result<void> remove(const std::string& id);
    util::Result<std::vector<ComponentLibrary>> find_all();
    util::Result<std::vector<ComponentLibrary>> find_enabled();
    int count() const;

private:
    ComponentLibrary row_to_library(sqlite3_stmt* stmt) const;
    sqlite3* db_;
};

// ============================================================
// ImportedFilesRepository — per-file mtime for incremental import
// ============================================================
class ImportedFilesRepository {
public:
    explicit ImportedFilesRepository(const DbSession& session) : db_(session.handle()) {}

    // Returns the stored mtime (seconds since epoch) if the file was imported before
    util::Result<std::optional<int64_t>> find_mtime(const std::string& path);
    util::Result<void> upsert(const std::string& path, int64_t mtime);
    // Load all snapshots in one query — used to build an in-memory index
    util::Result<std::unordered_map<std::string, int64_t>> all();

private:
    sqlite3* db_;
};

}  // namespace kforge::storage
