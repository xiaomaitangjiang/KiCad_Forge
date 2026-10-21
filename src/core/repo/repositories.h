#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include <unordered_map>

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
    /// 只改 component_type 一列的比较并写（CAS）：当前值（NULL 视同空串）
    /// 必须仍等于 expected_type_id，且与新值不同才写。
    /// 返回 false = 0 行受影响：行已删除，或类型已被并发改写 ——
    /// 调用方应当跳过，而不是拿旧快照整行 update 覆盖其它列。
    util::Result<bool> update_component_type_if_current(const core::Uuid& id,
                                                       const std::string& expected_type_id,
                                                       const std::string& new_type_id);
    util::Result<void> remove(const core::Uuid& id);
    util::Result<core::Symbol> find_by_id(const core::Uuid& id);
    util::Result<bool> is_library_locked(const core::Uuid& id) const;
    util::Result<core::Symbol> find_by_name(const std::string& name);
    util::Result<std::vector<core::Symbol>> find_by_library(const core::Uuid& lib_id);
    util::Result<std::vector<core::Symbol>> find_all();
    /// 列表页只读显示字段：跳过 properties_json（80MB 级）与描述/KF ID，
    /// 其余字段与 find_all/search 完全一致（pin_count 由列回填）。
    util::Result<std::vector<core::Symbol>> find_summaries(const std::string& keyword,
                                                           const core::Uuid& library_id = "");
    /// 对应关系检查只读身份/名称/封装引用/电源标记，跳过属性 JSON 与引脚定义。
    util::Result<std::vector<core::Symbol>> find_all_for_check();
    /// 自动匹配候选：只读判分所需字段，并在 SQL 层截断。
    /// limit == 0 表示不截断；排序与 find_all 一致（name）。
    util::Result<std::vector<core::Symbol>> find_for_matching(size_t limit);
    /// 导入关联核对只需身份与引用，避免载入每个符号的属性 JSON。
    struct FootprintReferenceRow {
        std::string id;
        std::string name;
        std::string footprint_ref;
    };
    util::Result<std::vector<FootprintReferenceRow>> find_footprint_references();
    /// 空库没有分组行；调用方按 0 处理，不为计数读取完整符号。
    util::Result<std::unordered_map<std::string, int>> count_by_library() const;
    util::Result<std::vector<core::Symbol>> search(const std::string& keyword,
                                                 const core::Uuid& library_id = "");
    /// 引用某个元件类型 ID 的符号数（删除类型前的引用检查）。
    /// 查询失败返回错误 —— 绝不能当成 0 引用放行删除。
    util::Result<int> count_by_component_type(const std::string& type_id) const;
    /// 引用某个封装类型 ID 的符号数（symbols.package_type 也是包类型的引用）
    util::Result<int> count_by_package_type(const std::string& type_id) const;
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
    /// 旧协议回退：只给了 (库名, 封装名) 时唯一定位。
    /// 名字是显示字段、不是身份 —— 0 项 NotFound，多项 ambiguous（ValidationError），
    /// 绝不静默取第一个。library 为空时按名字全局匹配，同样要求唯一。
    util::Result<core::Footprint> resolve_legacy_reference(const std::string& library,
                                                           const std::string& name);
    util::Result<std::vector<core::Footprint>> find_all();
    /// 存在性判定只读身份/来源/引用名，跳过描述、属性 JSON 与焊盘定义。
    util::Result<std::vector<core::Footprint>> find_all_for_index();
    /// 自动匹配候选：只读判分所需字段，并在 SQL 层截断。
    /// limit == 0 表示不截断；排序与 find_all 一致（name）。
    util::Result<std::vector<core::Footprint>> find_for_matching(size_t limit);
    util::Result<std::vector<core::Footprint>> search(const std::string& keyword);
    /// 引用某个封装类型 ID 的封装数（删除类型前的引用检查）。
    /// 查询失败返回错误 —— 绝不能当成 0 引用放行删除。
    util::Result<int> count_by_package_type(const std::string& type_id) const;
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
    /// 存在性判定只需要 id + 文件路径。
    util::Result<std::vector<core::Model3D>> find_all_for_index();
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

    // 关系表的一行 —— 同一符号可能挂多条关联，用单值 map 读会丢行
    struct SymbolLinkRow
    {
        std::string symbol_id;
        std::string footprint_id;
        std::string link_type;
        double confidence = 1.0;
    };

    // Bulk: all existing symbol→footprint links in one query (symbol_id → footprint_id)
    util::Result<std::unordered_map<std::string, std::string>> find_all_symbol_links();

    // Bulk: 逐行读出全部 symbol→footprint 关联（含关联类型），不合并多行
    util::Result<std::vector<SymbolLinkRow>> find_all_symbol_link_rows();

    // 单个符号的全部关联行：no-op/替换判断必须看完整集合，不能只看首条
    util::Result<std::vector<SymbolLinkRow>> find_symbol_link_rows(const core::Uuid& sym_id);

    // 删除某个符号的全部封装关联（重算关联前的一步）
    util::Result<void> unlink_all_symbol_footprints(const core::Uuid& sym_id);

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
