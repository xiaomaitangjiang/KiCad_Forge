#include "storage/repositories.h"

#include <random>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "core/type_registry.h"

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

static std::string component_type_to_string(core::ComponentType t) {
    int idx = static_cast<int>(t);
    return core::component_type_name(idx);
}

static core::ComponentType string_to_component_type(const std::string& s) {
    int idx = core::component_type_index(s);
    if (idx < 0) return core::ComponentType::Unknown;
    return static_cast<core::ComponentType>(idx);
}

static std::string package_type_to_string(core::PackageType t) {
    int idx = static_cast<int>(t);
    return core::package_type_name(idx);
}

static core::PackageType string_to_package_type(const std::string& s) {
    int idx = core::package_type_index(s);
    if (idx < 0) return core::PackageType::Unknown;
    return static_cast<core::PackageType>(idx);
}

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

LibraryRepository::LibraryRepository(sqlite3* db) : db_(db) {}

util::Result<core::LibraryMeta> LibraryRepository::insert(const core::LibraryMeta& lib) {
    // Check if library with this name already exists (dedup by name)
    auto all = find_all();
    if (all) for (auto& existing : *all) {
        if (existing.name == lib.name) return existing;
    }
    const char* sql = "INSERT OR IGNORE INTO libraries(id,name,file_path,component_library_id,description) VALUES(?1,?2,?3,?4,?5)";
    core::LibraryMeta l = lib;
    if (l.id.empty()) l.id = make_uuid();
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    bind_text(stmt, 1, l.id); bind_text(stmt, 2, l.name);
    bind_text(stmt, 3, l.file_path.string()); bind_text(stmt, 4, l.component_library_id);
    bind_text(stmt, 5, l.description);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    return l;
}

util::Result<std::vector<core::LibraryMeta>> LibraryRepository::find_all() {
    const char* sql = "SELECT id,name,file_path,component_library_id,description FROM libraries ORDER BY name";
    std::vector<core::LibraryMeta> result;
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        core::LibraryMeta m;
        m.id = col_text(stmt, 0); m.name = col_text(stmt, 1);
        m.file_path = col_text(stmt, 2); m.component_library_id = col_text(stmt, 3);
        m.description = col_text(stmt, 4);
        result.push_back(m);
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

SymbolRepository::SymbolRepository(sqlite3* db) : db_(db) {}

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
    sym.component_type = string_to_component_type(col_text(stmt, 12));
    sym.package_type   = string_to_package_type(col_text(stmt, 13));
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
    bind_text(stmt, 13, component_type_to_string(sym.component_type));
    bind_text(stmt, 14, package_type_to_string(sym.package_type));
    // Serialize pin definitions as JSON inside properties
    auto props = sym.properties();
    if (!sym.pins().empty()) {
        nlohmann::json pins_arr = nlohmann::json::array();
        for (auto& p : sym.pins()) {
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
    bind_text(stmt, 15, properties_to_json(props));
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
    bind_text(stmt, 10, component_type_to_string(sym.component_type));
    bind_text(stmt, 11, package_type_to_string(sym.package_type));
    bind_text(stmt, 12, properties_to_json(sym.properties()));
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

util::Result<std::vector<core::Symbol>> SymbolRepository::search(
    const std::string& keyword) {
    const char* sql =
        "SELECT id, library_id, name, lib_id, default_value, footprint_ref, "
        "datasheet, description, mpn, reference_prefix, is_power, pin_count, "
        "component_type, package_type, properties_json, Kicad_Forge_ID, Pre_Kicad_Forge_ID "
        "FROM symbols "
        "WHERE name LIKE ?1 OR description LIKE ?2 OR mpn LIKE ?3 "
        "ORDER BY name";

    std::vector<core::Symbol> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Search symbols prepare: " + std::string(sqlite3_errmsg(db_))));
    }

    std::string pattern = "%" + keyword + "%";
    bind_text(stmt, 1, pattern);
    bind_text(stmt, 2, pattern);
    bind_text(stmt, 3, pattern);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.push_back(row_to_symbol(stmt));
    }
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Search symbols step: " + std::string(sqlite3_errmsg(db_))));
    }
    return result;
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

FootprintRepository::FootprintRepository(sqlite3* db) : db_(db) {}

core::Footprint FootprintRepository::row_to_footprint(sqlite3_stmt* stmt) const {
    core::Footprint fp;
    // Column order:
    // 0:id  1:name  2:library_path  3:description  4:tags
    // 5:pad_count  6:courtyard_w  7:courtyard_h  8:package_type
    // 9:properties_json
    fp.set_id(col_text(stmt, 0));
    fp.set_name(col_text(stmt, 1));
    // library_path at index 2 — not stored on Footprint, skip
    fp.set_description(col_text(stmt, 3));
    fp.set_tags(col_text(stmt, 4));
    // pad_count at index 5 — derived from pads_ vector, not stored
    fp.courtyard_width  = col_double(stmt, 6);
    fp.courtyard_height = col_double(stmt, 7);
    fp.package_type = string_to_package_type(col_text(stmt, 8));
    for (auto& [k, v] : json_to_properties(col_text(stmt, 9))) fp.set_property(k, v);
    return fp;
}

util::Result<core::Footprint> FootprintRepository::insert(
    const core::Footprint& fp) {
    const char* sql =
        "INSERT INTO footprints (id, name, library_path, description, tags, "
        "pad_count, courtyard_w, courtyard_h, package_type, properties_json) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)";

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
    bind_text(stmt, 3, "");  // library_path — not used yet
    bind_text(stmt, 4, f.description());
    bind_text(stmt, 5, f.tags());
    sqlite3_bind_int(stmt, 6, f.pad_count());
    sqlite3_bind_double(stmt, 7, f.courtyard_width);
    sqlite3_bind_double(stmt, 8, f.courtyard_height);
    bind_text(stmt, 9, package_type_to_string(f.package_type));
    bind_text(stmt, 10, properties_to_json(f.properties()));

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(util::Error::make<Kind::DbError>(
            "Insert footprint step: " + std::string(sqlite3_errmsg(db_))));
    }
    return f;
}

util::Result<void> FootprintRepository::update(const core::Footprint& fp) {
    const char* sql =
        "UPDATE footprints SET name=?1, description=?2, tags=?3, "
        "pad_count=?4, package_type=?5, properties_json=?6 WHERE id=?7";

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
    bind_text(stmt, 5, package_type_to_string(fp.package_type));
    bind_text(stmt, 6, properties_to_json(fp.properties()));
    bind_text(stmt, 7, fp.id());

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
    const char* sql =
        "SELECT id, name, library_path, description, tags, "
        "pad_count, courtyard_w, courtyard_h, package_type, properties_json "
        "FROM footprints WHERE id = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
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
    const char* sql =
        "SELECT id, name, library_path, description, tags, "
        "pad_count, courtyard_w, courtyard_h, package_type, properties_json "
        "FROM footprints WHERE name = ?1";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
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

util::Result<std::vector<core::Footprint>> FootprintRepository::find_all() {
    const char* sql =
        "SELECT id, name, library_path, description, tags, "
        "pad_count, courtyard_w, courtyard_h, package_type, properties_json "
        "FROM footprints ORDER BY name";

    std::vector<core::Footprint> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
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

util::Result<std::vector<core::Footprint>> FootprintRepository::search(
    const std::string& keyword) {
    const char* sql =
        "SELECT id, name, library_path, description, tags, "
        "pad_count, courtyard_w, courtyard_h, package_type, properties_json "
        "FROM footprints "
        "WHERE name LIKE ?1 OR description LIKE ?2 OR tags LIKE ?3 "
        "ORDER BY name";

    std::vector<core::Footprint> result;

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt.ref(), nullptr);
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

Model3DRepository::Model3DRepository(sqlite3* db) : db_(db) {}

core::Model3D Model3DRepository::row_to_model(sqlite3_stmt* stmt) const {
    core::Model3D m;
    // 0:id  1:file_path  2:format  3:description
    // 4:width  5:height  6:depth  7:source
    m.set_id(col_text(stmt, 0));
    m.set_file_path(std::filesystem::path(col_text(stmt, 1)));
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
    bind_text(stmt, 2, model.file_path().string());
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

RelationshipRepository::RelationshipRepository(sqlite3* db) : db_(db) {}

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

ComponentLibraryRepository::ComponentLibraryRepository(sqlite3* db) : db_(db) {}

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
    return lib;
}

util::Result<ComponentLibraryRepository::ComponentLibrary>
ComponentLibraryRepository::insert(const ComponentLibrary& lib) {
    const char* sql =
        "INSERT INTO component_libraries (id, name, symbol_path, footprint_path, "
        "model_3d_path, enabled, sort_order) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)";

    ComponentLibrary l = lib;
    if (l.id.empty()) l.id = make_uuid();

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
        "model_3d_path=?4, enabled=?5, sort_order=?6 WHERE id=?7";

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
        "enabled, sort_order FROM component_libraries ORDER BY sort_order, name";

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
// SettingsRepository
// ============================================================

SettingsRepository::SettingsRepository(sqlite3* db) : db_(db) {}

util::Result<std::string> SettingsRepository::get(const std::string& key) {
    sqlite3_exec(db_, "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT NOT NULL)",
                 nullptr, nullptr, nullptr);
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT value FROM settings WHERE key=?", -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    bind_text(stmt, 1, key);
    if (sqlite3_step(stmt) == SQLITE_ROW) return col_text(stmt, 0);
    return "";  // key not found → empty string
}

util::Result<void> SettingsRepository::set(const std::string& key, const std::string& value) {
    // Ensure the settings table exists
    sqlite3_exec(db_, "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT NOT NULL)",
                 nullptr, nullptr, nullptr);
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO settings(key,value) VALUES(?,?)", -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    bind_text(stmt, 1, key);
    bind_text(stmt, 2, value);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    return {};
}

util::Result<std::unordered_map<std::string, std::string>> SettingsRepository::all() {
    std::unordered_map<std::string, std::string> map;
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT key, value FROM settings", -1, stmt.ref(), nullptr);
    if (rc != SQLITE_OK) return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(db_)));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        map[col_text(stmt, 0)] = col_text(stmt, 1);
    }
    return map;
}

}  // namespace kforge::storage
