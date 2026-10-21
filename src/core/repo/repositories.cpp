#include "core/repo/repositories.h"

#include <filesystem>
#include <random>

#include <nlohmann/json.hpp>
#include <sqlite3.h>


namespace kforge::storage {
using Kind = util::Error::Kind;

// ============================================================
// Internal helpers
// ============================================================

/// RAII wrapper for sqlite3_stmt that finalises on scope exit.
class ScopedStmt {
public:
    ScopedStmt() = default;
    ~ScopedStmt() { reset(); }

    ScopedStmt(const ScopedStmt&) = delete;
    ScopedStmt& operator=(const ScopedStmt&) = delete;
    ScopedStmt(ScopedStmt&& other) noexcept
        : stmt_(other.stmt_) { other.stmt_ = nullptr; }
    ScopedStmt& operator=(ScopedStmt&& other) noexcept {
        if (this != &other) { reset(); stmt_ = other.stmt_; other.stmt_ = nullptr; }
        return *this;
    }

    sqlite3_stmt* get() const { return stmt_; }
    sqlite3_stmt** ref() { reset(); return &stmt_; }

    operator sqlite3_stmt*() const { return stmt_; }
    explicit operator bool() const { return stmt_ != nullptr; }

private:
    void reset() { if (stmt_ != nullptr) { sqlite3_finalize(stmt_); stmt_ = nullptr; } }
    sqlite3_stmt* stmt_ = nullptr;
};

/// Safely read a TEXT column; NULL becomes empty string.
inline std::string col_text(sqlite3_stmt* stmt, int idx) {
    const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, idx));
    return txt ? std::string(txt) : std::string();
}

inline int col_int(sqlite3_stmt* stmt, int idx) {
    return sqlite3_column_int(stmt, idx);
}

/// 数据库里的路径文本是 UTF-8 契约：路径列一律按 UTF-8 存、按 UTF-8 读。
/// 窄串会被按 ANSI 代码页重新解释（中文路径直接变坏），必须经 char8_t 转换。
inline std::filesystem::path path_from_utf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const auto* bytes = reinterpret_cast<const char8_t*>(utf8.data());
    return std::filesystem::path(std::u8string_view(bytes, utf8.size()));
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
    auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline double col_double(sqlite3_stmt* stmt, int idx) {
    return sqlite3_column_double(stmt, idx);
}

/// Generate a random UUID v4 string: "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
static std::string make_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<int> dist(0, 15);
    static const char hex[] = "0123456789abcdef";

    std::string uuid;
    uuid.reserve(36);

    for (int i = 0; i < 8; ++i)  uuid += hex[dist(gen)];
    uuid += '-';
    for (int i = 0; i < 4; ++i)  uuid += hex[dist(gen)];
    uuid += '-';
    uuid += '4';  // version 4
    for (int i = 0; i < 3; ++i)  uuid += hex[dist(gen)];
    uuid += '-';
    uuid += hex[8 + dist(gen) % 4];  // variant (8/9/a/b)
    for (int i = 0; i < 3; ++i)  uuid += hex[dist(gen)];
    uuid += '-';
    for (int i = 0; i < 12; ++i) uuid += hex[dist(gen)];

    return uuid;
}

/// Bind a TEXT parameter; helper to avoid reinterpret_cast noise.
static inline void bind_text(sqlite3_stmt* stmt, int idx, const std::string& val) {
    sqlite3_bind_text(stmt, idx, val.c_str(), static_cast<int>(val.size()),
                      SQLITE_TRANSIENT);
}

static inline void bind_text_nullable(sqlite3_stmt* stmt, int idx,
                                       const std::string& val) {
    if (val.empty())
        sqlite3_bind_null(stmt, idx);
    else
        sqlite3_bind_text(stmt, idx, val.c_str(), static_cast<int>(val.size()),
                          SQLITE_TRANSIENT);
}

// ============================================================
// Enum ↔ string conversions
// ============================================================

/// Serialize a string→string properties map to JSON.
static std::string properties_to_json(
    const std::unordered_map<std::string, std::string>& props)
{
    if (props.empty()) return "{}";
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [key, value] : props) {
        j[key] = value;
    }
    return j.dump();
}

/// Serialize a symbol's properties + pin definitions to the properties_json column.
/// insert 与 update 共用同一份序列化：只有一处写 _pins_json，更新之后回读的
/// pins() 才不会凭空变空（update 曾只写 properties，pin 定义被丢在库里）。
/// pins() 为空时显式清掉 _pins_json：免得不存在的引脚从旧 JSON 里复活。
static std::string symbol_properties_json(const core::Symbol& sym)
{
    auto props = sym.properties();
    if (sym.pins().empty()) {
        props.erase("_pins_json");
    } else {
        nlohmann::json pins_arr = nlohmann::json::array();
        for (const auto& p : sym.pins()) {
            nlohmann::json pj;
            pj["name"] = p.name;
            pj["number"] = p.number;
            pj["electrical_type"] = p.electrical_type;
            pj["x"] = p.x;
            pj["y"] = p.y;
            pins_arr.push_back(pj);
        }
        props["_pins_json"] = pins_arr.dump();
    }
    return properties_to_json(props);
}

/// Deserialize JSON back into a properties map.
static std::unordered_map<std::string, std::string>
json_to_properties(const std::string& json)
{
    std::unordered_map<std::string, std::string> props;
    if (json.empty()) return props;
    try {
        auto j = nlohmann::json::parse(json);
        if (j.is_object()) {
            for (auto& [key, value] : j.items()) {
                props[key] = value.is_string() ? value.get<std::string>()
                                               : value.dump();
            }
        }
    } catch (const nlohmann::json::exception&) {
        // Corrupt JSON — leave properties empty
    }
    return props;
}

// ============================================================
// LibraryRepository
// ============================================================

util::Result<core::LibraryMeta> LibraryRepository::insert(const core::LibraryMeta& lib) {
    // 身份是来源路径，不是名字：同名不同来源的库必须落在不同行上。
    // 未落盘的虚拟库来源键为空，此时身份就是 UUID，不做去重。
    core::LibraryMeta l = lib;
    if (l.source_key.empty())
        l.source_key = existing_source_key(l.file_path);
    if (!l.source_key.empty()) {
        if (auto existing = find_by_source_key(l.source_key)) {
            return *existing;  // 同一来源已在册 —— 保留原 UUID
        }
    }

    const char* sql =
        "INSERT INTO libraries(id,name,file_path,component_library_id,description,source_key) "
        "VALUES(?1,?2,?3,?4,?5,?6)";
    if (l.id.empty()) l.id = make_uuid();
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    bind_text(stmt, 1, l.id); bind_text(stmt, 2, l.name);
    bind_text(stmt, 3, path_to_utf8(l.file_path)); bind_text_nullable(stmt, 4, l.component_library_id);
    bind_text(stmt, 5, l.description);
    bind_text_nullable(stmt, 6, l.source_key);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    return l;
}

util::Result<core::LibraryMeta> LibraryRepository::find_by_source_key(const std::string& key) {
    if (key.empty()) {
        return std::unexpected(util::Error::make<Kind::NotFound>("Library has no source key"));
    }
    const char* sql =
        "SELECT id,name,file_path,component_library_id,description,source_key FROM libraries "
        "WHERE source_key = ?1";
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr) != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find library by source key prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        core::LibraryMeta m;
        m.id = col_text(stmt, 0); m.name = col_text(stmt, 1);
        m.file_path = path_from_utf8(col_text(stmt, 2)); m.component_library_id = col_text(stmt, 3);
        m.description = col_text(stmt, 4); m.source_key = col_text(stmt, 5);
        return m;
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find library by source key step: " + std::string(sqlite3_errmsg(db_))));
    }
    return std::unexpected(util::Error::make<Kind::NotFound>("Library not found: " + key));
}

util::Result<core::LibraryMeta> LibraryRepository::upsert_by_source_key(
    const core::LibraryMeta& lib) {
    core::LibraryMeta incoming = lib;
    if (incoming.source_key.empty())
        incoming.source_key = existing_source_key(incoming.file_path);
    if (incoming.source_key.empty())
        return insert(incoming);  // 虚拟库：按 UUID 落行

    auto existing = find_by_source_key(incoming.source_key);
    if (!existing)
        return insert(incoming);

    // 同一源文件被配置到两个不同组件库：报归属冲突，不暗中改 owner
    if (!existing->component_library_id.empty() && !incoming.component_library_id.empty() &&
        existing->component_library_id != incoming.component_library_id)
    {
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "Source belongs to component library " + existing->component_library_id +
            ", cannot reassign to " + incoming.component_library_id + ": " +
            incoming.source_key));
    }

    // 命中同一来源：保留 UUID，刷新显示名、路径与归属
    core::LibraryMeta updated = *existing;
    updated.name = incoming.name;
    updated.file_path = incoming.file_path;
    updated.description = incoming.description;
    if (!incoming.component_library_id.empty())
        updated.component_library_id = incoming.component_library_id;

    const char* sql =
        "UPDATE libraries SET name=?1, file_path=?2, component_library_id=?3, description=?4, "
        "updated_at=datetime('now') WHERE id=?5";
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr) != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Upsert library prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    bind_text(stmt, 1, updated.name);
    bind_text(stmt, 2, path_to_utf8(updated.file_path));
    bind_text_nullable(stmt, 3, updated.component_library_id);
    bind_text(stmt, 4, updated.description);
    bind_text(stmt, 5, updated.id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Upsert library step: " + std::string(sqlite3_errmsg(db_))));
    }
    return updated;
}

util::Result<std::vector<core::LibraryMeta>> LibraryRepository::find_all() {
    const char* sql = "SELECT id,name,file_path,component_library_id,description,source_key "
                      "FROM libraries ORDER BY name";
    std::vector<core::LibraryMeta> result;
    ScopedStmt stmt;
    // 读不出来必须报错：把查询失败当「一行都没有」会让按归属/锁做的写入判定
    // 以为目标库没登记过，直接放行
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all libraries prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        core::LibraryMeta m;
        m.id = col_text(stmt, 0); m.name = col_text(stmt, 1);
        m.file_path = path_from_utf8(col_text(stmt, 2)); m.component_library_id = col_text(stmt, 3);
        m.description = col_text(stmt, 4); m.source_key = col_text(stmt, 5);
        result.push_back(m);
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all libraries step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

int LibraryRepository::count() const {
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM libraries", -1, stmt.ref(), nullptr) == SQLITE_OK
        && sqlite3_step(stmt) == SQLITE_ROW) return sqlite3_column_int(stmt, 0);
    return 0;
}

util::Result<void> LibraryRepository::remove(const core::Uuid& id) {
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, "DELETE FROM libraries WHERE id = ?", -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE)
        return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    return {};
}

// ============================================================
// SymbolRepository
// ============================================================

core::Symbol SymbolRepository::row_to_symbol(sqlite3_stmt* stmt) const {
    core::Symbol sym;
    // Column order matches the SELECT list in all queries:
    // 0:id  1:library_id  2:name  3:lib_id  4:default_value
    // 5:footprint_ref  6:datasheet  7:description  8:mpn
    // 9:reference_prefix  10:is_power  11:pin_count
    // 12:component_type  13:package_type  14:properties_json
    // 15:Kicad_Forge_ID  16:Pre_Kicad_Forge_ID
    sym.set_id(col_text(stmt, 0));
    sym.set_library_id(col_text(stmt, 1));
    sym.set_name(col_text(stmt, 2));
    sym.set_lib_id(col_text(stmt, 3));
    sym.set_default_value(col_text(stmt, 4));
    sym.set_footprint(col_text(stmt, 5));
    sym.set_datasheet(col_text(stmt, 6));
    sym.set_description(col_text(stmt, 7));
    sym.set_mpn(col_text(stmt, 8));
    sym.set_reference_prefix(col_text(stmt, 9));
    sym.set_power(col_int(stmt, 10) != 0);
    int db_pin_count = col_int(stmt, 11);
    sym.component_type_id = col_text(stmt, 12);
    sym.package_type_id   = col_text(stmt, 13);
    auto props = json_to_properties(col_text(stmt, 14));
    // Restore pin definitions from properties JSON
    auto pins_json = props.find("_pins_json");
    if (pins_json != props.end()) {
        try {
            auto j = nlohmann::json::parse(pins_json->second);
            if (j.is_array()) {
                for (auto& pj : j) {
                    core::PinDefinition pin;
                    pin.name = pj.value("name", "");
                    pin.number = pj.value("number", "");
                    pin.electrical_type = pj.value("electrical_type", "");
                    pin.x = pj.value("x", 0.0);
                    pin.y = pj.value("y", 0.0);
                    sym.add_pin(std::move(pin));
                }
            }
        } catch (...) {}
        props.erase(pins_json);
    }
    for (auto& [k, v] : props) sym.set_property(k, v);
    sym.set_Kicad_Forge_ID(col_text(stmt, 15));
    sym.set_Pre_Kicad_Forge_ID(col_text(stmt, 16));
    // Preserve pin_count from DB even if pins_ deserialization failed
    if (sym.pin_count() == 0 && db_pin_count > 0) {
        sym.set_property("_pin_count", std::to_string(db_pin_count));
    }
    return sym;
}

void SymbolRepository::bind_symbol_params(sqlite3_stmt* stmt,
                                           const core::Symbol& sym) const {
    // Params: id(1), library_id(2), name(3), lib_id(4), default_value(5),
    // footprint_ref(6), datasheet(7), description(8), mpn(9),
    // reference_prefix(10), is_power(11), pin_count(12),
    // component_type(13), package_type(14), properties_json(15),
    // Kicad_Forge_ID(16), Pre_Kicad_Forge_ID(17)
    bind_text(stmt, 1,  sym.id());
    bind_text(stmt, 2,  sym.library_id());
    bind_text(stmt, 3,  sym.name());
    bind_text(stmt, 4,  sym.lib_id());
    bind_text(stmt, 5,  sym.default_value());
    bind_text(stmt, 6,  sym.footprint());
    bind_text(stmt, 7,  sym.datasheet());
    bind_text(stmt, 8,  sym.description());
    bind_text(stmt, 9,  sym.mpn());
    bind_text(stmt, 10, sym.reference_prefix());
    sqlite3_bind_int(stmt, 11, sym.is_power() ? 1 : 0);
    sqlite3_bind_int(stmt, 12, sym.pin_count());
    bind_text(stmt, 13, sym.component_type_id);
    bind_text(stmt, 14, sym.package_type_id);
    bind_text(stmt, 15, symbol_properties_json(sym));  // 含 _pins_json，与 update 同一份序列化
    bind_text(stmt, 16, sym.Kicad_Forge_ID());
    bind_text(stmt, 17, sym.Pre_Kicad_Forge_ID());
}

util::Result<core::Symbol> SymbolRepository::insert(const core::Symbol& sym) {
    // Ensure default library exists for foreign key
    sqlite3_exec(db_, "INSERT OR IGNORE INTO libraries(id,name,file_path) VALUES('default','Default Library','')",
                 nullptr, nullptr, nullptr);
    const char* sql =
        "INSERT OR IGNORE INTO symbols (id, library_id, name, lib_id, default_value, "
        "footprint_ref, datasheet, description, mpn, reference_prefix, "
        "is_power, pin_count, component_type, package_type, properties_json, "
        "Kicad_Forge_ID, Pre_Kicad_Forge_ID) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
        "?13, ?14, ?15, ?16, ?17)";

    core::Symbol s = sym;
    if (s.id().empty()) s.set_id(make_uuid());
    if (s.Kicad_Forge_ID().empty()) s.set_Kicad_Forge_ID(core::Symbol::compute_hash(s));

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Insert symbol prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_symbol_params(stmt, s);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Insert symbol step: " + std::string(sqlite3_errmsg(db_))));
    }
    return s;
}

util::Result<void> SymbolRepository::update(const core::Symbol& sym) {
    const char* sql =
        "UPDATE symbols SET name=?1, default_value=?2, footprint_ref=?3, "
        "datasheet=?4, description=?5, mpn=?6, reference_prefix=?7, "
        "is_power=?8, pin_count=?9, component_type=?10, package_type=?11, "
        "properties_json=?12, Kicad_Forge_ID=?13, Pre_Kicad_Forge_ID=?14 "
        "WHERE id=?15";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Update symbol prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1,  sym.name());
    bind_text(stmt, 2,  sym.default_value());
    bind_text(stmt, 3,  sym.footprint());
    bind_text(stmt, 4,  sym.datasheet());
    bind_text(stmt, 5,  sym.description());
    bind_text(stmt, 6,  sym.mpn());
    bind_text(stmt, 7,  sym.reference_prefix());
    sqlite3_bind_int(stmt, 8,  sym.is_power() ? 1 : 0);
    sqlite3_bind_int(stmt, 9,  sym.pin_count());
    bind_text(stmt, 10, sym.component_type_id);
    bind_text(stmt, 11, sym.package_type_id);
    bind_text(stmt, 12, symbol_properties_json(sym));
    bind_text(stmt, 13, sym.Kicad_Forge_ID());
    bind_text(stmt, 14, sym.Pre_Kicad_Forge_ID());
    bind_text(stmt, 15, sym.id());

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Update symbol step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<bool> SymbolRepository::update_component_type_if_current(
    const core::Uuid& id, const std::string& expected_type_id,
    const std::string& new_type_id) {
    // COALESCE 把 NULL 和空串当成同一件事（老行的 component_type 可能是 NULL）；
    // 值与目标相同不算变更，不让「没改」被计成 updated
    const char* sql =
        "UPDATE symbols SET component_type = ?3 "
        "WHERE id = ?1 AND COALESCE(component_type, '') = ?2 "
        "AND COALESCE(component_type, '') <> ?3";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Update symbol type prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, id);
    bind_text(stmt, 2, expected_type_id);
    bind_text(stmt, 3, new_type_id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Update symbol type step: " + std::string(sqlite3_errmsg(db_))));
    }
    return sqlite3_changes(db_) > 0;
}

util::Result<void> SymbolRepository::remove(const core::Uuid& id) {
    const char* sql = "DELETE FROM symbols WHERE id = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Delete symbol prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Delete symbol step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<core::Symbol> SymbolRepository::find_by_name(const std::string& name) {
    const char* sql =
        "SELECT id, library_id, name, lib_id, default_value, footprint_ref, "
        "datasheet, description, mpn, reference_prefix, is_power, pin_count, "
        "component_type, package_type, properties_json, Kicad_Forge_ID, Pre_Kicad_Forge_ID "
        "FROM symbols WHERE name = ?1 LIMIT 1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbol by name prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, name);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return row_to_symbol(stmt);
    }
    if (rc == SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::NotFound>("Symbol not found: " + name));
    }
    return std::unexpected(util::Error::make<Kind::DbError>(
        "Find symbol by name step: " + std::string(sqlite3_errmsg(db_))));
}

util::Result<core::Symbol> SymbolRepository::find_by_id(const core::Uuid& id) {
    const char* sql =
        "SELECT id, library_id, name, lib_id, default_value, footprint_ref, "
        "datasheet, description, mpn, reference_prefix, is_power, pin_count, "
        "component_type, package_type, properties_json, Kicad_Forge_ID, Pre_Kicad_Forge_ID "
        "FROM symbols WHERE id = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbol by id prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return row_to_symbol(stmt);
    }
    if (rc == SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::NotFound>("Symbol not found: " + id));
    }
    return std::unexpected(util::Error::make<Kind::DbError>(
        "Find symbol by id step: " + std::string(sqlite3_errmsg(db_))));
}

util::Result<std::vector<core::Symbol>> SymbolRepository::find_by_library(
    const core::Uuid& lib_id) {
    const char* sql =
        "SELECT id, library_id, name, lib_id, default_value, footprint_ref, "
        "datasheet, description, mpn, reference_prefix, is_power, pin_count, "
        "component_type, package_type, properties_json, Kicad_Forge_ID, Pre_Kicad_Forge_ID "
        "FROM symbols WHERE library_id = ?1 ORDER BY name";

    std::vector<core::Symbol> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbols by library prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, lib_id);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(row_to_symbol(stmt));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbols by library step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

namespace
{
/// COUNT(*) 引用查询：查询本身失败必须报错 —— 返回 0 会被当成"没人引用"，
/// 于是删类型时静默放行。
util::Result<int> count_rows_by_type(sqlite3* db, const char* sql, const std::string& type_id,
                                     const char* what)
{
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(db, sql, -1, stmt.ref(), nullptr) != SQLITE_OK)
    {
        return std::unexpected(util::Error::make<Kind::DbError>(
            std::string(what) + " prepare: " + std::string(sqlite3_errmsg(db))));
    }
    bind_text(stmt, 1, type_id);
    if (sqlite3_step(stmt) != SQLITE_ROW)
    {
        return std::unexpected(util::Error::make<Kind::DbError>(
            std::string(what) + " step: " + std::string(sqlite3_errmsg(db))));
    }
    return sqlite3_column_int(stmt, 0);
}
}  // namespace

util::Result<int> SymbolRepository::count_by_component_type(const std::string& type_id) const {
    return count_rows_by_type(db_, "SELECT COUNT(*) FROM symbols WHERE component_type = ?1",
                              type_id, "Count symbols by component type");
}

util::Result<int> SymbolRepository::count_by_package_type(const std::string& type_id) const {
    return count_rows_by_type(db_, "SELECT COUNT(*) FROM symbols WHERE package_type = ?1",
                              type_id, "Count symbols by package type");
}

util::Result<std::vector<core::Symbol>> SymbolRepository::find_all() {
    const char* sql =
        "SELECT id, library_id, name, lib_id, default_value, footprint_ref, "
        "datasheet, description, mpn, reference_prefix, is_power, pin_count, "
        "component_type, package_type, properties_json, Kicad_Forge_ID, Pre_Kicad_Forge_ID "
        "FROM symbols ORDER BY name";

    std::vector<core::Symbol> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all symbols prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(row_to_symbol(stmt));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all symbols step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<core::Symbol>> SymbolRepository::find_summaries(
    const std::string& keyword, const core::Uuid& library_id) {
    // 列顺序固定：0:id 1:library_id 2:name 3:default_value 4:footprint_ref
    // 5:pin_count 6:mpn 7:component_type
    std::string sql =
        "SELECT id, library_id, name, default_value, footprint_ref, pin_count, mpn, "
        "component_type FROM symbols";
    const bool has_keyword = !keyword.empty();
    if (has_keyword) {
        sql += library_id.empty() ? " WHERE " : " WHERE library_id = ?4 AND ";
        sql += "(name LIKE ?1 OR description LIKE ?2 OR mpn LIKE ?3)";
    } else if (!library_id.empty()) {
        sql += " WHERE library_id = ?1";
    }
    sql += " ORDER BY name";

    std::vector<core::Symbol> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbol summaries prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    if (has_keyword) {
        std::string pattern = "%" + keyword + "%";
        bind_text(stmt, 1, pattern);
        bind_text(stmt, 2, pattern);
        bind_text(stmt, 3, pattern);
        if (!library_id.empty()) bind_text(stmt, 4, library_id);
    } else if (!library_id.empty()) {
        bind_text(stmt, 1, library_id);
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        core::Symbol sym;
        sym.set_id(col_text(stmt, 0));
        sym.set_library_id(col_text(stmt, 1));
        sym.set_name(col_text(stmt, 2));
        sym.set_default_value(col_text(stmt, 3));
        sym.set_footprint(col_text(stmt, 4));
        const int pins = col_int(stmt, 5);
        if (pins > 0) sym.set_property("_pin_count", std::to_string(pins));
        sym.set_mpn(col_text(stmt, 6));
        sym.component_type_id = col_text(stmt, 7);
        result.push_back(std::move(sym));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbol summaries step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<core::Symbol>> SymbolRepository::find_all_for_check() {
    const char* sql =
        "SELECT id, name, footprint_ref, is_power FROM symbols ORDER BY name";

    std::vector<core::Symbol> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all symbols for check prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        core::Symbol sym;
        sym.set_id(col_text(stmt, 0));
        sym.set_name(col_text(stmt, 1));
        sym.set_footprint(col_text(stmt, 2));
        sym.set_power(col_int(stmt, 3) != 0);
        result.push_back(std::move(sym));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all symbols for check step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<core::Symbol>> SymbolRepository::find_for_matching(size_t limit) {
    // 判分只用 name / footprint / 电源标记 / 引脚数；属性 JSON 与引脚定义都不读。
    std::string sql =
        "SELECT id, name, footprint_ref, is_power, pin_count FROM symbols ORDER BY name";
    if (limit > 0) sql += " LIMIT ?1";

    std::vector<core::Symbol> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbols for matching prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    if (limit > 0) sqlite3_bind_int(stmt, 1, static_cast<int>(limit));

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        core::Symbol sym;
        sym.set_id(col_text(stmt, 0));
        sym.set_name(col_text(stmt, 1));
        sym.set_footprint(col_text(stmt, 2));
        sym.set_power(col_int(stmt, 3) != 0);
        const int pins = col_int(stmt, 4);
        if (pins > 0) sym.set_property("_pin_count", std::to_string(pins));
        result.push_back(std::move(sym));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbols for matching step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<SymbolRepository::FootprintReferenceRow>>
SymbolRepository::find_footprint_references() {
    std::vector<FootprintReferenceRow> rows;
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, "SELECT id, name, footprint_ref FROM symbols",
                                -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbol footprint references prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        rows.push_back({col_text(stmt, 0), col_text(stmt, 1), col_text(stmt, 2)});
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbol footprint references step: " + std::string(sqlite3_errmsg(db_))));
    }
    return rows;
}

util::Result<std::unordered_map<std::string, int>> SymbolRepository::count_by_library() const {
    std::unordered_map<std::string, int> counts;
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, "SELECT library_id, COUNT(*) FROM symbols GROUP BY library_id",
                                -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Count symbols by library prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        counts.emplace(col_text(stmt, 0), col_int(stmt, 1));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Count symbols by library step: " + std::string(sqlite3_errmsg(db_))));
    }
    return counts;
}

util::Result<std::vector<core::Symbol>> SymbolRepository::search(
    const std::string& keyword, const core::Uuid& library_id) {
    const std::string sql = std::string(
        "SELECT id, library_id, name, lib_id, default_value, footprint_ref, "
        "datasheet, description, mpn, reference_prefix, is_power, pin_count, "
        "component_type, package_type, properties_json, Kicad_Forge_ID, Pre_Kicad_Forge_ID "
        "FROM symbols WHERE ") +
        (library_id.empty() ? "" : "library_id = ?4 AND ") +
        "(name LIKE ?1 OR description LIKE ?2 OR mpn LIKE ?3) "
        "ORDER BY name";

    std::vector<core::Symbol> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Search symbols prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    std::string pattern = "%" + keyword + "%";
    bind_text(stmt, 1, pattern);
    bind_text(stmt, 2, pattern);
    bind_text(stmt, 3, pattern);
    if (!library_id.empty()) bind_text(stmt, 4, library_id);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(row_to_symbol(stmt));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Search symbols step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<bool> SymbolRepository::is_library_locked(const core::Uuid& id) const {
    const char* sql = "SELECT COALESCE(c.locked, 0) FROM symbols s "
                      "LEFT JOIN libraries l ON l.id = s.library_id "
                      "LEFT JOIN component_libraries c ON c.id = l.component_library_id "
                      "WHERE s.id = ?1";
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr) != SQLITE_OK)
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Read symbol lock state: " + std::string(sqlite3_errmsg(db_))));
    bind_text(stmt, 1, id);
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) return col_int(stmt, 0) != 0;
    if (rc == SQLITE_DONE)
        return std::unexpected(util::Error::make<Kind::NotFound>("Symbol not found: " + id));
    return std::unexpected(util::Error::make<Kind::DbError>(
        "Read symbol lock state: " + std::string(sqlite3_errmsg(db_))));
}

int SymbolRepository::count() const {
    const char* sql = "SELECT COUNT(*) FROM symbols";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return 0;

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) return sqlite3_column_int(stmt, 0);
    return 0;
}

// ============================================================
// FootprintRepository
// ============================================================

// 所有封装查询共用的列顺序（row_to_footprint 按下标读取）
static constexpr char kFootprintColumns[] =
    "id, name, library_path, description, tags, pad_count, courtyard_w, courtyard_h, "
    "package_type, properties_json, source_key, footprint_lib_name";

core::Footprint FootprintRepository::row_to_footprint(sqlite3_stmt* stmt) const {
    core::Footprint fp;
    // Column order:
    // 0:id  1:name  2:library_path  3:description  4:tags
    // 5:pad_count  6:courtyard_w  7:courtyard_h  8:package_type
    // 9:properties_json
    fp.set_id(col_text(stmt, 0));
    fp.set_name(col_text(stmt, 1));
    fp.set_library_path(col_text(stmt, 2));
    fp.set_description(col_text(stmt, 3));
    fp.set_tags(col_text(stmt, 4));
    // pad_count at index 5：pads_ 不从这里重建，但计数不能因此回读成 0
    int db_pad_count = col_int(stmt, 5);
    fp.courtyard_width  = col_double(stmt, 6);
    fp.courtyard_height = col_double(stmt, 7);
    fp.package_type_id = col_text(stmt, 8);
    for (auto& [k, v] : json_to_properties(col_text(stmt, 9))) fp.set_property(k, v);
    // 与 Symbol 的 _pin_count 同一套回退：只有 pads_ 没被还原时才用它
    if (fp.pad_count() == 0 && db_pad_count > 0)
        fp.set_property("_pad_count", std::to_string(db_pad_count));
    fp.set_source_key(col_text(stmt, 10));
    fp.set_footprint_lib_name(col_text(stmt, 11));
    return fp;
}

util::Result<core::Footprint> FootprintRepository::insert(
    const core::Footprint& fp) {
    const char* sql =
        "INSERT OR IGNORE INTO footprints (id, name, library_path, description, tags, "
        "pad_count, courtyard_w, courtyard_h, package_type, properties_json, "
        "source_key, footprint_lib_name) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)";

    core::Footprint f = fp;
    if (f.id().empty()) f.set_id(make_uuid());

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Insert footprint prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, f.id());
    bind_text(stmt, 2, f.name());
    bind_text(stmt, 3, f.library_path());
    bind_text(stmt, 4, f.description());
    bind_text(stmt, 5, f.tags());
    sqlite3_bind_int(stmt, 6, f.pad_count());
    sqlite3_bind_double(stmt, 7, f.courtyard_width);
    sqlite3_bind_double(stmt, 8, f.courtyard_height);
    bind_text(stmt, 9, f.package_type_id);
    bind_text(stmt, 10, properties_to_json(f.properties()));
    // 未落盘的封装保持 NULL，由 UUID 识别（部分唯一索引只约束已落盘来源）
    bind_text_nullable(stmt, 11, f.source_key());
    bind_text(stmt, 12, f.footprint_lib_name());

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Insert footprint step: " + std::string(sqlite3_errmsg(db_))));
    }
    return f;
}

util::Result<core::Footprint> FootprintRepository::find_by_source_key(
    const std::string& key) {
    if (key.empty()) {
        return std::unexpected(util::Error::make<Kind::NotFound>("Footprint has no source key"));
    }
    std::string sql =
        std::string("SELECT ") + kFootprintColumns + " FROM footprints WHERE source_key = ?1";

    ScopedStmt stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt.ref(), nullptr) != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find footprint by source key prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return row_to_footprint(stmt);
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find footprint by source key step: " + std::string(sqlite3_errmsg(db_))));
    }
    return std::unexpected(util::Error::make<Kind::NotFound>("Footprint not found: " + key));
}

util::Result<core::Footprint> FootprintRepository::resolve_legacy_reference(
    const std::string& library, const std::string& name) {
    if (name.empty()) {
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "Footprint reference needs a name"));
    }
    auto rows = find_all();
    if (!rows) return std::unexpected(rows.error());

    std::vector<core::Footprint> matches;
    for (const auto& fp : *rows) {
        if (fp.name() != name) continue;
        if (!library.empty()) {
            // 路径文本是 UTF-8：窄串转换会把中文目录名变成乱码，永远匹配不上
            auto parent = path_to_utf8(path_from_utf8(fp.library_path()).parent_path().stem());
            if (parent != library) continue;
        }
        matches.push_back(fp);
    }
    if (matches.empty()) {
        return std::unexpected(util::Error::make<Kind::NotFound>(
            "No registered footprint matches " + (library.empty() ? name : library + ":" + name)));
    }
    if (matches.size() > 1) {
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "Footprint reference is ambiguous (" + std::to_string(matches.size()) +
            " matches for " + (library.empty() ? name : library + ":" + name) +
            "); pass an explicit footprint id"));
    }
    return matches.front();
}

util::Result<void> FootprintRepository::update(const core::Footprint& fp) {
    const char* sql =
        "UPDATE footprints SET name=?1, description=?2, tags=?3, "
        "pad_count=?4, package_type=?5, properties_json=?6, library_path=?8, "
        "source_key=?9, footprint_lib_name=?10 WHERE id=?7";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Update footprint prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, fp.name());
    bind_text(stmt, 2, fp.description());
    bind_text(stmt, 3, fp.tags());
    sqlite3_bind_int(stmt, 4, fp.pad_count());
    bind_text(stmt, 5, fp.package_type_id);
    bind_text(stmt, 6, properties_to_json(fp.properties()));
    bind_text(stmt, 7, fp.id());
    bind_text(stmt, 8, fp.library_path());
    bind_text_nullable(stmt, 9, fp.source_key());
    bind_text(stmt, 10, fp.footprint_lib_name());

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Update footprint step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<void> FootprintRepository::remove(const core::Uuid& id) {
    const char* sql = "DELETE FROM footprints WHERE id = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Delete footprint prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Delete footprint step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<core::Footprint> FootprintRepository::find_by_id(
    const core::Uuid& id) {
    std::string sql = std::string("SELECT ") + kFootprintColumns + " FROM footprints WHERE id = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find footprint by id prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return row_to_footprint(stmt);
    }
    if (rc == SQLITE_DONE) {
        return std::unexpected(
            util::Error::make<Kind::NotFound>("Footprint not found: " + id));
    }
    return std::unexpected(util::Error::make<Kind::DbError>(
        "Find footprint by id step: " + std::string(sqlite3_errmsg(db_))));
}

util::Result<core::Footprint> FootprintRepository::find_by_name(
    const std::string& name) {
    std::string sql =
        std::string("SELECT ") + kFootprintColumns + " FROM footprints WHERE name = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find footprint by name prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, name);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return row_to_footprint(stmt);
    }
    if (rc == SQLITE_DONE) {
        return std::unexpected(
            util::Error::make<Kind::NotFound>("Footprint not found: " + name));
    }
    return std::unexpected(util::Error::make<Kind::DbError>(
        "Find footprint by name step: " + std::string(sqlite3_errmsg(db_))));
}

util::Result<int> FootprintRepository::count_by_package_type(const std::string& type_id) const {
    return count_rows_by_type(db_, "SELECT COUNT(*) FROM footprints WHERE package_type = ?1",
                              type_id, "Count footprints by package type");
}

util::Result<std::vector<core::Footprint>> FootprintRepository::find_all() {
    std::string sql =
        std::string("SELECT ") + kFootprintColumns + " FROM footprints ORDER BY name";

    std::vector<core::Footprint> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all footprints prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(row_to_footprint(stmt));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all footprints step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<core::Footprint>> FootprintRepository::find_all_for_index() {
    const char* sql =
        "SELECT id, name, library_path, source_key, footprint_lib_name "
        "FROM footprints ORDER BY name";

    std::vector<core::Footprint> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all footprints for index prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        core::Footprint fp;
        fp.set_id(col_text(stmt, 0));
        fp.set_name(col_text(stmt, 1));
        fp.set_library_path(col_text(stmt, 2));
        fp.set_source_key(col_text(stmt, 3));
        fp.set_footprint_lib_name(col_text(stmt, 4));
        result.push_back(std::move(fp));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all footprints for index step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<core::Footprint>> FootprintRepository::find_for_matching(size_t limit) {
    // 判分只用 name / 焊盘数；描述、属性 JSON 与焊盘定义都不读。
    std::string sql = "SELECT id, name, pad_count FROM footprints ORDER BY name";
    if (limit > 0) sql += " LIMIT ?1";

    std::vector<core::Footprint> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find footprints for matching prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    if (limit > 0) sqlite3_bind_int(stmt, 1, static_cast<int>(limit));

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        core::Footprint fp;
        fp.set_id(col_text(stmt, 0));
        fp.set_name(col_text(stmt, 1));
        const int pads = col_int(stmt, 2);
        if (pads > 0) fp.set_property("_pad_count", std::to_string(pads));
        result.push_back(std::move(fp));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find footprints for matching step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<core::Footprint>> FootprintRepository::search(
    const std::string& keyword) {
    std::string sql = std::string("SELECT ") + kFootprintColumns +
                      " FROM footprints "
                      "WHERE name LIKE ?1 OR description LIKE ?2 OR tags LIKE ?3 "
                      "ORDER BY name";

    std::vector<core::Footprint> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Search footprints prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    std::string pattern = "%" + keyword + "%";
    bind_text(stmt, 1, pattern);
    bind_text(stmt, 2, pattern);
    bind_text(stmt, 3, pattern);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(row_to_footprint(stmt));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Search footprints step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

int FootprintRepository::count() const {
    const char* sql = "SELECT COUNT(*) FROM footprints";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return 0;

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) return sqlite3_column_int(stmt, 0);
    return 0;
}

// ============================================================
// Model3DRepository
// ============================================================

core::Model3D Model3DRepository::row_to_model(sqlite3_stmt* stmt) const {
    core::Model3D m;
    // 0:id  1:file_path  2:format  3:description
    // 4:width  5:height  6:depth  7:source
    m.set_id(col_text(stmt, 0));
    m.set_file_path(path_from_utf8(col_text(stmt, 1)));
    m.set_format(col_text(stmt, 2));
    m.set_description(col_text(stmt, 3));
    m.width  = col_double(stmt, 4);
    m.height = col_double(stmt, 5);
    m.depth  = col_double(stmt, 6);
    m.set_source(col_text(stmt, 7));
    return m;
}

util::Result<core::Model3D> Model3DRepository::insert(const core::Model3D& m) {
    const char* sql =
        "INSERT INTO models_3d (id, file_path, format, description, "
        "width, height, depth, source) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)";

    core::Model3D model = m;
    if (model.id().empty()) model.set_id(make_uuid());

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Insert 3D model prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, model.id());
    bind_text(stmt, 2, path_to_utf8(model.file_path()));
    bind_text(stmt, 3, model.format());
    bind_text(stmt, 4, model.description());
    sqlite3_bind_double(stmt, 5, model.width);
    sqlite3_bind_double(stmt, 6, model.height);
    sqlite3_bind_double(stmt, 7, model.depth);
    bind_text(stmt, 8, model.source());

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Insert 3D model step: " + std::string(sqlite3_errmsg(db_))));
    }
    return model;
}

util::Result<void> Model3DRepository::update(const core::Model3D& m) {
    const char* sql =
        "UPDATE models_3d SET file_path=?1, format=?2, description=?3, "
        "width=?4, height=?5, depth=?6, source=?7 WHERE id=?8";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Update 3D model prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, path_to_utf8(m.file_path()));
    bind_text(stmt, 2, m.format());
    bind_text(stmt, 3, m.description());
    sqlite3_bind_double(stmt, 4, m.width);
    sqlite3_bind_double(stmt, 5, m.height);
    sqlite3_bind_double(stmt, 6, m.depth);
    bind_text(stmt, 7, m.source());
    bind_text(stmt, 8, m.id());

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Update 3D model step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<void> Model3DRepository::remove(const core::Uuid& id) {
    const char* sql = "DELETE FROM models_3d WHERE id = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Delete 3D model prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Delete 3D model step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<core::Model3D> Model3DRepository::find_by_id(
    const core::Uuid& id) {
    const char* sql =
        "SELECT id, file_path, format, description, width, height, depth, source "
        "FROM models_3d WHERE id = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find 3D model by id prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return row_to_model(stmt);
    }
    if (rc == SQLITE_DONE) {
        return std::unexpected(
            util::Error::make<Kind::NotFound>("3D model not found: " + id));
    }
    return std::unexpected(util::Error::make<Kind::DbError>(
        "Find 3D model by id step: " + std::string(sqlite3_errmsg(db_))));
}

util::Result<core::Model3D> Model3DRepository::find_by_path(
    const std::string& path) {
    const char* sql =
        "SELECT id, file_path, format, description, width, height, depth, source "
        "FROM models_3d WHERE file_path = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find 3D model by path prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, path);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return row_to_model(stmt);
    }
    if (rc == SQLITE_DONE) {
        return std::unexpected(
            util::Error::make<Kind::NotFound>("3D model not found: " + path));
    }
    return std::unexpected(util::Error::make<Kind::DbError>(
        "Find 3D model by path step: " + std::string(sqlite3_errmsg(db_))));
}

util::Result<std::vector<core::Model3D>> Model3DRepository::find_all() {
    const char* sql =
        "SELECT id, file_path, format, description, width, height, depth, source "
        "FROM models_3d ORDER BY file_path";

    std::vector<core::Model3D> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all 3D models prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(row_to_model(stmt));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all 3D models step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<core::Model3D>> Model3DRepository::find_all_for_index() {
    const char* sql = "SELECT id, file_path FROM models_3d";

    std::vector<core::Model3D> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all 3D models for index prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        core::Model3D model;
        model.set_id(col_text(stmt, 0));
        model.set_file_path(path_from_utf8(col_text(stmt, 1)));
        result.push_back(std::move(model));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all 3D models for index step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<core::Model3D>> Model3DRepository::find_orphans() {
    const char* sql =
        "SELECT m.id, m.file_path, m.format, m.description, "
        "m.width, m.height, m.depth, m.source "
        "FROM models_3d m "
        "LEFT JOIN footprint_model_links fml ON m.id = fml.model_id "
        "WHERE fml.id IS NULL";

    std::vector<core::Model3D> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find orphan 3D models prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(row_to_model(stmt));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find orphan 3D models step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

int Model3DRepository::count() const {
    const char* sql = "SELECT COUNT(*) FROM models_3d";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return 0;

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) return sqlite3_column_int(stmt, 0);
    return 0;
}

// ============================================================
// RelationshipRepository
// ============================================================

// --- Symbol ↔ Footprint ---

util::Result<void> RelationshipRepository::link_symbol_to_footprint(
    const core::Uuid& sym_id, const core::Uuid& fp_id,
    const std::string& link_type, double confidence) {
    const char* sql =
        "INSERT OR REPLACE INTO symbol_footprint_links "
        "(id, symbol_id, footprint_id, link_type, confidence) "
        "VALUES (?1, ?2, ?3, ?4, ?5)";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Link symbol-footprint prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, make_uuid());
    bind_text(stmt, 2, sym_id);
    bind_text(stmt, 3, fp_id);
    bind_text(stmt, 4, link_type);
    sqlite3_bind_double(stmt, 5, confidence);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Link symbol-footprint step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<void> RelationshipRepository::unlink_symbol_footprint(
    const core::Uuid& sym_id, const core::Uuid& fp_id) {
    const char* sql =
        "DELETE FROM symbol_footprint_links "
        "WHERE symbol_id = ?1 AND footprint_id = ?2";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Unlink symbol-footprint prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, sym_id);
    bind_text(stmt, 2, fp_id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Unlink symbol-footprint step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<std::optional<core::Uuid>>
RelationshipRepository::find_footprint_for_symbol(const core::Uuid& sym_id) {
    const char* sql =
        "SELECT footprint_id FROM symbol_footprint_links WHERE symbol_id = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find footprint for symbol prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, sym_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        return col_text(stmt, 0);
    }
    if (rc == SQLITE_DONE) {
        return std::nullopt;
    }
    return std::unexpected(util::Error::make<Kind::DbError>(
        "Find footprint for symbol step: " + std::string(sqlite3_errmsg(db_))));
}

util::Result<std::vector<core::Uuid>>
RelationshipRepository::find_symbols_for_footprint(const core::Uuid& fp_id) {
    const char* sql =
        "SELECT symbol_id FROM symbol_footprint_links WHERE footprint_id = ?1";

    std::vector<core::Uuid> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbols for footprint prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, fp_id);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(col_text(stmt, 0));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbols for footprint step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

// --- Footprint ↔ 3D Model ---

util::Result<void> RelationshipRepository::link_footprint_to_model(
    const core::Uuid& fp_id, const core::Uuid& model_id,
    const std::string& link_type) {
    const char* sql =
        "INSERT OR REPLACE INTO footprint_model_links "
        "(id, footprint_id, model_id, link_type) VALUES (?1, ?2, ?3, ?4)";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Link footprint-model prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, make_uuid());
    bind_text(stmt, 2, fp_id);
    bind_text(stmt, 3, model_id);
    bind_text(stmt, 4, link_type);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Link footprint-model step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<std::unordered_map<std::string, std::string>>
RelationshipRepository::find_all_symbol_links() {
    std::unordered_map<std::string, std::string> links;
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT symbol_id, footprint_id FROM symbol_footprint_links",
        -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all symbol links prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    // 只有以 SQLITE_DONE 收尾才算读全：半截清单会让调用方以为「某些符号没有关联」
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        links[col_text(stmt, 0)] = col_text(stmt, 1);
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all symbol links step: " + std::string(sqlite3_errmsg(db_))));
    }
    return links;
}

util::Result<std::vector<RelationshipRepository::SymbolLinkRow>>
RelationshipRepository::find_all_symbol_link_rows() {
    std::vector<SymbolLinkRow> rows;
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT symbol_id, footprint_id, link_type, confidence FROM symbol_footprint_links",
        -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all symbol link rows prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    // 只有以 SQLITE_DONE 收尾才算读全：中途的 step 错误不能当成「表就这些行」，
    // 否则调用方会拿半截清单去重算关联（人工绑定的行可能还没读到）
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        rows.push_back(SymbolLinkRow{col_text(stmt, 0), col_text(stmt, 1), col_text(stmt, 2),
                                    col_double(stmt, 3)});
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all symbol link rows step: " + std::string(sqlite3_errmsg(db_))));
    }
    return rows;
}

util::Result<std::vector<RelationshipRepository::SymbolLinkRow>>
RelationshipRepository::find_symbol_link_rows(const core::Uuid& sym_id) {
    std::vector<SymbolLinkRow> rows;
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT symbol_id, footprint_id, link_type, confidence FROM symbol_footprint_links "
        "WHERE symbol_id = ?1",
        -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbol link rows prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    bind_text(stmt, 1, sym_id);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        rows.push_back(SymbolLinkRow{col_text(stmt, 0), col_text(stmt, 1), col_text(stmt, 2),
                                    col_double(stmt, 3)});
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbol link rows step: " + std::string(sqlite3_errmsg(db_))));
    }
    return rows;
}

util::Result<void> RelationshipRepository::unlink_all_symbol_footprints(    const core::Uuid& sym_id) {
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "DELETE FROM symbol_footprint_links WHERE symbol_id = ?1",
        -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Unlink all symbol footprints prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    bind_text(stmt, 1, sym_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Unlink all symbol footprints step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<std::unordered_map<std::string, std::vector<std::string>>>
RelationshipRepository::find_all_model_links() {
    std::unordered_map<std::string, std::vector<std::string>> links;
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT footprint_id, model_id FROM footprint_model_links",
        -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all model links prepare: " + std::string(sqlite3_errmsg(db_))));
    }
    // 最终 step 必须是 SQLITE_DONE：中途读错当成「模型表就这些行」，
    // 会让存在性判定把有模型的封装报成 unassigned
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        links[col_text(stmt, 0)].push_back(col_text(stmt, 1));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all model links step: " + std::string(sqlite3_errmsg(db_))));
    }
    return links;
}

util::Result<void> RelationshipRepository::unlink_footprint_model(
    const core::Uuid& fp_id, const core::Uuid& model_id) {
    const char* sql =
        "DELETE FROM footprint_model_links "
        "WHERE footprint_id = ?1 AND model_id = ?2";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Unlink footprint-model prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, fp_id);
    bind_text(stmt, 2, model_id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Unlink footprint-model step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<std::vector<core::Uuid>>
RelationshipRepository::find_models_for_footprint(const core::Uuid& fp_id) {
    const char* sql =
        "SELECT model_id FROM footprint_model_links WHERE footprint_id = ?1";

    std::vector<core::Uuid> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find models for footprint prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, fp_id);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(col_text(stmt, 0));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find models for footprint step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

// --- Bulk queries ---

util::Result<std::vector<core::Uuid>>
RelationshipRepository::find_symbols_without_footprints() {
    const char* sql =
        "SELECT s.id FROM symbols s "
        "LEFT JOIN symbol_footprint_links sfl ON s.id = sfl.symbol_id "
        "WHERE sfl.id IS NULL AND s.is_power = 0";

    std::vector<core::Uuid> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbols without footprints prepare: "
            + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(col_text(stmt, 0));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find symbols without footprints step: "
            + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<core::Uuid>>
RelationshipRepository::find_footprints_without_symbols() {
    const char* sql =
        "SELECT f.id FROM footprints f "
        "LEFT JOIN symbol_footprint_links sfl ON f.id = sfl.footprint_id "
        "WHERE sfl.id IS NULL";

    std::vector<core::Uuid> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find footprints without symbols prepare: "
            + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(col_text(stmt, 0));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find footprints without symbols step: "
            + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<core::Uuid>>
RelationshipRepository::find_footprints_without_3d_models() {
    const char* sql =
        "SELECT f.id FROM footprints f "
        "LEFT JOIN footprint_model_links fml ON f.id = fml.footprint_id "
        "WHERE fml.id IS NULL";

    std::vector<core::Uuid> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find footprints without 3D models prepare: "
            + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(col_text(stmt, 0));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find footprints without 3D models step: "
            + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<core::Uuid>>
RelationshipRepository::find_orphan_models() {
    const char* sql =
        "SELECT m.id FROM models_3d m "
        "LEFT JOIN footprint_model_links fml ON m.id = fml.model_id "
        "WHERE fml.id IS NULL";

    std::vector<core::Uuid> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find orphan models prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(col_text(stmt, 0));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find orphan models step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

// ============================================================
// ComponentLibraryRepository
// ============================================================

namespace
{
// KiCad official libraries live under <install>/share/kicad
bool is_kicad_official(const std::string& p)
{
    auto lower = p;
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c)
                           {
                               return std::tolower(c);
                           });
    return lower.find("share/kicad") != std::string::npos ||
           lower.find("share\\kicad") != std::string::npos;
}
}  // namespace

ComponentLibraryRepository::ComponentLibrary
ComponentLibraryRepository::row_to_library(sqlite3_stmt* stmt) const {
    ComponentLibrary lib;
    lib.id = col_text(stmt, 0);
    lib.name = col_text(stmt, 1);
    lib.symbol_path = col_text(stmt, 2);
    lib.footprint_path = col_text(stmt, 3);
    lib.model_3d_path = col_text(stmt, 4);
    lib.enabled = col_int(stmt, 5) != 0;
    lib.sort_order = col_int(stmt, 6);
    lib.locked = col_int(stmt, 7) != 0;
    lib.is_protected = col_int(stmt, 8) != 0;
    return lib;
}

util::Result<ComponentLibraryRepository::ComponentLibrary>
ComponentLibraryRepository::insert(const ComponentLibrary& lib) {
    const char* sql =
        "INSERT INTO component_libraries (id, name, symbol_path, footprint_path, "
        "model_3d_path, enabled, sort_order, locked, protected) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)";

    ComponentLibrary l = lib;
    if (l.id.empty()) l.id = make_uuid();
    l.is_protected = l.is_protected || is_kicad_official(l.symbol_path) ||
                     is_kicad_official(l.footprint_path) || is_kicad_official(l.model_3d_path);
    if (l.is_protected) l.locked = true;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Insert component library prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, l.id);
    bind_text(stmt, 2, l.name);
    bind_text(stmt, 3, l.symbol_path);
    bind_text(stmt, 4, l.footprint_path);
    bind_text(stmt, 5, l.model_3d_path);
    sqlite3_bind_int(stmt, 6, l.enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 7, l.sort_order);
    sqlite3_bind_int(stmt, 8, l.locked ? 1 : 0);
    sqlite3_bind_int(stmt, 9, l.is_protected ? 1 : 0);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Insert component library step: " + std::string(sqlite3_errmsg(db_))));
    }
    return l;
}

util::Result<void> ComponentLibraryRepository::update(const ComponentLibrary& lib) {
    const char* sql =
        "UPDATE component_libraries SET name=?1, symbol_path=?2, footprint_path=?3, "
        "model_3d_path=?4, enabled=?5, sort_order=?6, locked=?8, protected=?9 WHERE id=?7";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Update component library prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, lib.name);
    bind_text(stmt, 2, lib.symbol_path);
    bind_text(stmt, 3, lib.footprint_path);
    bind_text(stmt, 4, lib.model_3d_path);
    sqlite3_bind_int(stmt, 5, lib.enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 6, lib.sort_order);
    bind_text(stmt, 7, lib.id);
    sqlite3_bind_int(stmt, 8, lib.locked ? 1 : 0);
    sqlite3_bind_int(stmt, 9, lib.is_protected ? 1 : 0);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Update component library step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<void> ComponentLibraryRepository::remove(const std::string& id) {
    const char* sql = "DELETE FROM component_libraries WHERE id = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Delete component library prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    bind_text(stmt, 1, id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Delete component library step: " + std::string(sqlite3_errmsg(db_))));
    }
    return {};
}

util::Result<std::vector<ComponentLibraryRepository::ComponentLibrary>>
ComponentLibraryRepository::find_all() {
    const char* sql =
        "SELECT id, name, symbol_path, footprint_path, model_3d_path, "
        "enabled, sort_order, locked, protected FROM component_libraries "
        "ORDER BY sort_order, name";

    std::vector<ComponentLibrary> result;
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all component libraries prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(row_to_library(stmt));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find all component libraries step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

util::Result<std::vector<ComponentLibraryRepository::ComponentLibrary>>
ComponentLibraryRepository::find_enabled() {
    const char* sql =
        "SELECT id, name, symbol_path, footprint_path, model_3d_path, "
        "enabled, sort_order FROM component_libraries "
        "WHERE enabled = 1 ORDER BY sort_order, name";

    std::vector<ComponentLibrary> result;
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find enabled component libraries prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(row_to_library(stmt));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Find enabled component libraries step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
}

int ComponentLibraryRepository::count() const {
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT COUNT(*) FROM component_libraries", -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return 0;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) return sqlite3_column_int(stmt, 0);
    return 0;
}

// ============================================================
// ImportedFilesRepository
// ============================================================

static constexpr char kImportedFilesSchema[] =
    "CREATE TABLE IF NOT EXISTS imported_files("
    "file_path TEXT PRIMARY KEY, mtime INTEGER NOT NULL,"
    "imported_at TEXT DEFAULT (datetime('now')))";

util::Result<std::optional<int64_t>> ImportedFilesRepository::find_mtime(
    const std::string& path)
{
    sqlite3_exec(db_, kImportedFilesSchema, nullptr, nullptr, nullptr);
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT mtime FROM imported_files WHERE file_path=?", -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    bind_text(stmt, 1, path);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        return std::optional<int64_t>{sqlite3_column_int64(stmt, 0)};
    }
    return std::optional<int64_t>{};
}

util::Result<void> ImportedFilesRepository::upsert(const std::string& path, int64_t mtime)
{
    sqlite3_exec(db_, kImportedFilesSchema, nullptr, nullptr, nullptr);
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "INSERT INTO imported_files(file_path, mtime) VALUES(?,?) "
        "ON CONFLICT(file_path) DO UPDATE SET mtime=excluded.mtime",
        -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    bind_text(stmt, 1, path);
    sqlite3_bind_int64(stmt, 2, mtime);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    return {};
}

util::Result<std::unordered_map<std::string, int64_t>> ImportedFilesRepository::all()
{
    sqlite3_exec(db_, kImportedFilesSchema, nullptr, nullptr, nullptr);
    std::unordered_map<std::string, int64_t> map;
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT file_path, mtime FROM imported_files", -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        map[col_text(stmt, 0)] = sqlite3_column_int64(stmt, 1);
    }
    return map;
}

}  // namespace kforge::storage
