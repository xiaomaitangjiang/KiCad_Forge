#include <doctest/doctest.h>
#include "support/test_environment.h"
#include "classifier/rule_engine.h"
#include "classifier/rule_loader.h"
#include "classifier/type_matching.h"
#include "core/db/database.h"
#include "core/db/source_key.h"
#include "core/db/source_key_migration.h"
#include "core/io/source_mutation.h"
#include "core/model/type_registry.h"
#include "core/repo/repositories.h"
#include "core/io/parser/symbol_lib_parser.h"
#include "interface/api/import_manager.h"
#include "interface/api/import_orchestrator.h"
#include "interface/api/staged_import.h"
#include "interface/api/type_matching.h"
#include "plugin/plugin_manager.h"
#include "interface/service/library/binding.h"
#include "interface/service/library/footprint_resolver.h"
#include "interface/api/footprint_status_json.h"
#include "interface/service/library/service.h"
#include "interface/service/classification/service.h"
#include "interface/service/library/source_policy.h"
#include "core/io/sexpr/text_util.h"
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sqlite3.h>
#ifdef _WIN32
// LCSC01-N 用官方 API 构造悬空链接：MinGW libstdc++ 的 create_symlink 在 Windows
// 上未实现，只能落到 Win32。NOMINMAX/LEAN_AND_MEAN 防止污染标准库与工程头。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#endif

namespace
{
namespace fs = std::filesystem;
using namespace kforge;

std::string read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path& path, std::string_view text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    REQUIRE(out.good());
}
constexpr std::string_view source = R"((kicad_symbol_lib (version 20231120)
  (symbol "R" (property "Reference" "R") (property "Footprint" "Pkg:Old"))))";

/// 定义在本文件末尾的 U04-A 段落（同一个匿名命名空间，无名命名空间按 TU 唯一）：
/// 用内存 JSON 装载全部内置元件类型，不读用户数据目录、不调用 TypeRegistry::build。
void u04a_load_full_registry();
int audit_scalar_int(storage::Database& db, const std::string& sql);

struct RegistrySnapshot
{
    std::vector<core::TypeEntry> components = core::TypeRegistry::instance().component_types();
    std::vector<core::TypeEntry> packages = core::TypeRegistry::instance().package_types();
    std::string component_path = core::TypeRegistry::instance().component_types_path();
    std::string package_path = core::TypeRegistry::instance().package_types_path();
    ~RegistrySnapshot()
    {
        auto dump = [](const std::vector<core::TypeEntry>& entries)
        {
            nlohmann::json array = nlohmann::json::array();
            for (const auto& entry : entries)
            {
                nlohmann::json item;
                item["id"] = entry.id;
                item["name"] = entry.name;
                item["icon"] = entry.icon;
                item["color"] = entry.color;
                item["category"] = entry.category;
                array.push_back(std::move(item));
            }
            nlohmann::json root;
            root["types"] = std::move(array);
            return root.dump();
        };
        auto& registry = core::TypeRegistry::instance();
        registry.load_component_types_json(dump(components));
        registry.load_package_types_json(dump(packages));
        registry.bind_component_types_path(component_path);
        registry.bind_package_types_path(package_path);
    }
};

struct LibraryFixture
{
    test_support::TempDirectory temp;
    test_support::RestoreDefaultLogger restore;
    RegistrySnapshot registry_restore;
    classifier::TypeMatchingStore matching_store;
    fs::path matching_config = temp.path() / "component_type_matching.json";
    std::unique_ptr<storage::Database> db;
    fs::path file = temp.path() / "symbols" / "test.kicad_sym";
    LibraryFixture()
    {
        spdlog::set_default_logger(std::make_shared<spdlog::logger>(
            "data-test", std::make_shared<spdlog::sinks::null_sink_mt>()));
        // 完整内置元件表显式来自内存：默认种子引用的类型必须在案，
        // 不依赖前序用例或用户数据目录里的注册表状态（RegistrySnapshot 负责还原）。
        u04a_load_full_registry();
        write(matching_config, classifier::default_matching_config_json());
        REQUIRE(matching_store.load(matching_config.string()).has_value());
        REQUIRE(matching_store.acquire_business_snapshot().has_value());
        auto opened = storage::Database::open(temp.path() / "test.db");
        REQUIRE(opened.has_value());
        db = std::move(*opened);
        REQUIRE(db->execute("DELETE FROM component_libraries").has_value());
        write(file, source);
    }
    std::string group(bool locked = false, bool official = false)
    {
        storage::ComponentLibraryRepository::ComponentLibrary lib;
        lib.name = "Test";
        lib.symbol_path = file.parent_path().string();
        lib.locked = locked;
        lib.is_protected = official;
        auto session = db->session();
        auto result = storage::ComponentLibraryRepository(session).insert(lib);
        REQUIRE(result.has_value());
        return result->id;
    }
    core::Symbol symbol(const std::string& owner = "")
    {
        core::LibraryMeta meta;
        meta.name = "test";
        meta.file_path = file;
        meta.component_library_id = owner;
        auto session = db->session();
        auto lib = storage::LibraryRepository(session).insert(meta);
        REQUIRE(lib.has_value());
        core::Symbol sym;
        sym.set_name("R");
        sym.set_library_id(lib->id);
        sym.set_footprint("Pkg:Old");
        auto result = storage::SymbolRepository(session).insert(sym);
        REQUIRE(result.has_value());
        return *result;
    }
    core::Footprint footprint(std::string name, std::string model = "")
    {
        auto path = temp.path() / "Pkg.pretty" / (name + ".kicad_mod");
        auto text = "(footprint \"" + name + "\"";
        if (!model.empty())
        {
            write(path.parent_path() / model, "test model");
            text += " (model \"" + model + "\")";
        }
        write(path, text + ")");
        core::Footprint fp;
        fp.set_name(name);
        fp.set_library_path(path.string());
        auto session = db->session();
        auto result = storage::FootprintRepository(session).insert(fp);
        REQUIRE(result.has_value());
        return *result;
    }
    auto import(bool write_ids = false, std::string owner = "")
    {
        services::ImportPipeline pipe(*db, nullptr, write_ids, &matching_store);
        return pipe | services::symbols_from{file.parent_path().string(), owner} | services::execute;
    }

    // ---- FPSTATUS 辅助：真实文件 + 显式库名/来源键，覆盖「旧行/新行」两种落库形态 ----

    /// 登记一个封装：目录名给 .pretty 库名，写不写真实文件由调用方决定
    core::Footprint footprint_in(const std::string& library_dir, const std::string& name,
                                 bool write_file = true, bool with_source_key = false,
                                 const std::string& schema_name = "")
    {
        auto path = temp.path() / library_dir / (name + ".kicad_mod");
        if (write_file)
            write(path, "(footprint \"" + name + "\" (layer \"F.Cu\"))");
        core::Footprint fp;
        fp.set_name(name);
        fp.set_library_path(path.string());
        fp.set_footprint_lib_name(schema_name.empty() ? fs::path(library_dir).stem().string()
                                                      : schema_name);
        if (with_source_key)
            fp.set_source_key(storage::existing_source_key(path));
        auto session = db->session();
        auto result = storage::FootprintRepository(session).insert(fp);
        REQUIRE(result.has_value());
        return *result;
    }

    /// 直接登记一条封装行（路径/来源键完全由调用方给，用来造陈旧来源）
    core::Footprint footprint_row(const std::string& name, const fs::path& path,
                                  const std::string& source_key = "")
    {
        core::Footprint fp;
        fp.set_name(name);
        fp.set_library_path(path.string());
        fp.set_footprint_lib_name(path.parent_path().stem().string());
        fp.set_source_key(source_key);
        auto session = db->session();
        auto result = storage::FootprintRepository(session).insert(fp);
        REQUIRE(result.has_value());
        return *result;
    }

    core::Model3D model_file(const std::string& name, bool write_file = true)
    {
        auto path = temp.path() / "models" / name;
        if (write_file)
            write(path, "solid fpstatus");
        core::Model3D model;
        model.set_file_path(path);
        model.set_format("step");
        auto session = db->session();
        auto result = storage::Model3DRepository(session).insert(model);
        REQUIRE(result.has_value());
        return *result;
    }

    void link_footprint(const std::string& sym_id, const std::string& fp_id,
                        const std::string& link_type = "manual")
    {
        auto session = db->session();
        auto linked =
            storage::RelationshipRepository(session).link_symbol_to_footprint(sym_id, fp_id, link_type);
        REQUIRE(linked.has_value());
    }

    void link_model(const std::string& fp_id, const std::string& model_id)
    {
        auto session = db->session();
        auto linked =
            storage::RelationshipRepository(session).link_footprint_to_model(fp_id, model_id, "explicit");
        REQUIRE(linked.has_value());
    }

    void set_reference(const std::string& sym_id, const std::string& reference)
    {
        auto session = db->session();
        storage::SymbolRepository repo(session);
        auto sym = repo.find_by_id(sym_id);
        if (!sym)
        {
            MESSAGE("set_reference lookup failed for [", sym_id, "]: ", sym.error().format_message());
        }
        REQUIRE(sym.has_value());        sym->set_footprint(reference);
        REQUIRE(repo.update(*sym).has_value());
    }

    /// FPSTATUS：造第 2..n 个符号。symbols 表有 UNIQUE(library_id,name)，
    /// 同库同名的第二次 INSERT OR IGNORE 会被静默丢弃并返回一个库里不存在的 id，
    /// 所以这里每次用唯一名字，并且回查确认行真的写进去了。
    core::Symbol extra_symbol(const std::string& name, const std::string& footprint_ref)
    {
        core::LibraryMeta meta;
        meta.name = "test";
        meta.file_path = file;
        auto session = db->session();
        auto lib = storage::LibraryRepository(session).insert(meta);
        REQUIRE(lib.has_value());
        core::Symbol sym;
        sym.set_name(name);
        sym.set_library_id(lib->id);
        sym.set_footprint(footprint_ref);
        auto result = storage::SymbolRepository(session).insert(sym);
        REQUIRE(result.has_value());
        auto stored = storage::SymbolRepository(session).find_by_id(result->id());
        REQUIRE(stored.has_value());
        return *stored;
    }

    /// 与读接口同路：同一 session 取符号 + 快照，释放会话后解析
    services::Resolution resolve(const std::string& sym_id)
    {
        core::Symbol sym;
        services::Snapshot snapshot;
        {
            auto session = db->session();
            auto row = storage::SymbolRepository(session).find_by_id(sym_id);
            REQUIRE(row.has_value());
            sym = *row;
            auto loaded = services::load_snapshot(session);
            REQUIRE(loaded.has_value());
            snapshot = std::move(*loaded);
        }
        services::FileCheckContext files;
        return services::resolve_symbol(sym, snapshot, files);
    }
};
}

TEST_CASE_FIXTURE(LibraryFixture, "binding updates source DB reference and relationship together")
{
    auto sym = symbol();
    auto fp = footprint("New", "new.step");
    services::SymbolBindingManager manager(*db);
    auto assigned = manager.assign_footprint(sym.id(), fp.id());
    INFO((assigned ? "success" : assigned.error().format_message()));
    REQUIRE(assigned.has_value());
    auto parsed = parser::SymbolLibParser::parse(file);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 1);
    CHECK(parsed->items.front().footprint() == "Pkg:New");
    {
        auto session = db->session();
        auto updated = storage::SymbolRepository(session).find_by_id(sym.id());
        REQUIRE(updated.has_value());
        CHECK(updated->footprint() == "Pkg:New");
    }
    auto binding = manager.get(sym.id());
    REQUIRE(binding.has_value());
    CHECK(binding->footprint_id == fp.id());
    CHECK(binding->model_name == "new.step");
    auto no_model = footprint("Bare");
    REQUIRE(manager.assign_footprint(sym.id(), no_model.id()).has_value());
    CHECK(manager.get(sym.id())->model_id.empty());
}

TEST_CASE_FIXTURE(LibraryFixture, "footprint repository preserves source path on insert and update")
{
    auto fp = footprint("New");
    auto session = db->session();
    storage::FootprintRepository repo(session);
    auto saved = repo.find_by_id(fp.id());
    REQUIRE(saved.has_value());
    CHECK(saved->library_path() == fp.library_path());
    fp.set_library_path((temp.path() / "Other.pretty" / "New.kicad_mod").string());
    REQUIRE(repo.update(fp).has_value());
    saved = repo.find_by_id(fp.id());
    REQUIRE(saved.has_value());
    CHECK(saved->library_path() == fp.library_path());
}

TEST_CASE_FIXTURE(LibraryFixture, "footprint reimport preserves path and reloads changed model references")
{
    auto directory = temp.path() / "Import.pretty";
    auto path = directory / "Part.kicad_mod";
    write(directory / "first.step", "first");
    write(directory / "second.step", "second");
    write(path, R"((footprint "Part" (model "first.step")))");
    auto run = [&] {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        return pipe | services::footprints_from{directory.string()} | services::execute;
    };
    run();
    std::string fp_id;
    {
        auto session = db->session();
        auto fp = storage::FootprintRepository(session).find_by_name("Part");
        REQUIRE(fp.has_value());
        CHECK(fp->library_path() == path.string());
        fp_id = fp->id();
    }
    auto old_time = fs::last_write_time(path);
    write(path, R"((footprint "Part" (model "second.step")))");
    fs::last_write_time(path, old_time + std::chrono::seconds(2));
    run();
    auto session = db->session();
    auto links = storage::RelationshipRepository(session).find_models_for_footprint(fp_id);
    REQUIRE(links.has_value());
    REQUIRE(links->size() == 1);
    auto model = storage::Model3DRepository(session).find_by_id(links->front());
    REQUIRE(model.has_value());
    CHECK(model->file_path().filename() == "second.step");
}

TEST_CASE_FIXTURE(LibraryFixture, "binding failure preserves old link and source")
{
#ifdef _WIN32
    const auto permission_path = file;
#else
    const auto permission_path = file.parent_path();
#endif
    struct RestorePermissions
    {
        fs::path path;
        fs::perms original;
        ~RestorePermissions()
        {
            std::error_code error;
            fs::permissions(path, original, fs::perm_options::replace, error);
        }
    } restore_permissions{permission_path, fs::status(permission_path).permissions()};
    auto sym = symbol();
    auto old = footprint("Old");
    auto next = footprint("New", "new.step");
    {
        auto session = db->session();
        storage::RelationshipRepository relations(session);
        REQUIRE(relations.link_symbol_to_footprint(sym.id(), old.id()).has_value());
    }
    bool missing = false;
    SUBCASE("unknown footprint") { next.set_id("missing"); }
    SUBCASE("missing source") { fs::remove(file); missing = true; }
    SUBCASE("write failure") {
        // Windows 的只读目标阻止替换；POSIX 要阻止父目录写入，
        // 单独把目标文件设为只读仍可能允许原子替换目录项。
#ifdef _WIN32
        fs::permissions(permission_path, fs::perms::owner_read, fs::perm_options::replace);
#else
        fs::permissions(permission_path, fs::perms::owner_read | fs::perms::owner_exec,
                        fs::perm_options::replace);
#endif
    }
    SUBCASE("DB failure") {
        REQUIRE(db->execute("CREATE TRIGGER reject_link BEFORE INSERT ON symbol_footprint_links BEGIN SELECT RAISE(ABORT,'test failure'); END").has_value());
    }
    SUBCASE("commit failure") {
        // 钩子挂在连接上，设置完即释放 session —— 之后每次 COMMIT 都会失败
        auto session = db->session();
        sqlite3_commit_hook(session.handle(), [](void*) { return 1; }, nullptr);
    }
    SUBCASE("missing footprint source") { fs::remove(next.library_path()); }
    SUBCASE("malformed footprint source") { write(next.library_path(), "("); }
    services::SymbolBindingManager manager(*db);
    CHECK_FALSE(manager.assign_footprint(sym.id(), next.id()).has_value());
    CHECK(manager.get(sym.id())->footprint_id == old.id());
    if (!missing) CHECK(read(file) == source);
    auto session = db->session();
    auto current = storage::SymbolRepository(session).find_by_id(sym.id());
    REQUIRE(current.has_value());
    CHECK(current->footprint() == "Pkg:Old");
    CHECK(storage::Model3DRepository(session).count() == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "binding service blocks source writes to locked libraries")
{
    auto sym = symbol(group(true));
    auto fp = footprint("New");
    services::SymbolBindingManager manager(*db);
    CHECK_FALSE(manager.assign_footprint(sym.id(), fp.id()).has_value());
    CHECK(read(file) == source);
}

TEST_CASE_FIXTURE(LibraryFixture, "incremental import reports skipped files and preserves symbol identity")
{
    import();
    std::string before_id;
    {
        auto session = db->session();
        auto before = storage::SymbolRepository(session).find_all();
        REQUIRE(before.has_value());
        REQUIRE(before->size() == 1);
        before_id = before->front().id();
    }
    auto result = import();
    CHECK(result.symbols == 0);
    CHECK(result.skipped == 1);
    auto session = db->session();
    auto after = storage::SymbolRepository(session).find_all();
    REQUIRE(after.has_value());
    REQUIRE(after->size() == 1);
    CHECK(after->front().id() == before_id);
    CHECK(read(file) == source);
}

TEST_CASE_FIXTURE(LibraryFixture, "manual force import reloads unchanged files after cancellation")
{
    group();
    util::ConfigStore config(temp.path() / "config.json");
    api::ImportManager manager(*db, &config, &matching_store);
    manager.import_all({});
    std::string changed_id;
    {
        auto session = db->session();
        auto syms = storage::SymbolRepository(session).find_all();
        REQUIRE(syms.has_value());
        REQUIRE(syms->size() == 1);
        auto changed = syms->front();
        changed.set_footprint("DB-only");
        changed_id = changed.id();
        REQUIRE(storage::SymbolRepository(session).update(changed).has_value());
    }
    manager.stop_async();
    manager.import_all({.force = true});
    auto session = db->session();
    auto refreshed = storage::SymbolRepository(session).find_by_id(changed_id);
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->footprint() == "Pkg:Old");
}

TEST_CASE_FIXTURE(LibraryFixture, "KF ID opt-in writes user libraries but preserves locked and official sources")
{
    bool allowed = false;
    bool enabled = true;
    bool locked = false;
    bool official = false;
    bool official_path = false;
    SUBCASE("default off") { enabled = false; }
    SUBCASE("user enabled") { allowed = true; }
    SUBCASE("locked") { locked = true; }
    SUBCASE("protected") { official = true; }
    SUBCASE("official directory without owner") {
        official_path = true;
        file = temp.path() / "share" / "kicad" / "symbols" / "test.kicad_sym";
        write(file, source);
    }
    auto owner = !official_path ? group(locked, official) : std::string{};
    import(enabled, owner);
    auto session = db->session();
    auto rows = storage::SymbolRepository(session).find_all();
    REQUIRE(rows.has_value());
    REQUIRE(rows->size() == 1);
    CHECK_FALSE(rows->front().Kicad_Forge_ID().empty());
    if (allowed) {
        CHECK(read(file).find("Kicad_Forge_ID") != std::string::npos);
        auto parsed = parser::SymbolLibParser::parse(file);
        REQUIRE(parsed.has_value());
        CHECK(parsed->items.front().Kicad_Forge_ID() == rows->front().Kicad_Forge_ID());
    } else CHECK(read(file) == source);
}

TEST_CASE_FIXTURE(LibraryFixture, "classification updates locked-library metadata without changing its source")
{
    auto sym = symbol(group(true));
    // 夹具按名字 "R" 建行，reference_prefix 停在字段默认值 "U"。
    // 分类只认真实 Reference 属性：这里明确写入位号 R —— 产品不该为了迁就一个
    // 名字叫 R、位号却是 U 的夹具去按型号名猜类型（前缀列只作缓存，不是来源）。
    {
        auto session = db->session();
        auto stored = storage::SymbolRepository(session).find_by_id(sym.id());
        REQUIRE(stored.has_value());
        stored->set_reference_prefix("R");
        stored->set_property("reference", "R");
        REQUIRE(storage::SymbolRepository(session).update(*stored).has_value());
        INFO("stored reference_prefix=", stored->reference_prefix(),
             " reference=", stored->property("reference").value_or("<missing>"));
        CHECK(stored->reference_prefix() == "R");
        REQUIRE(stored->property("reference").has_value());
        CHECK(*stored->property("reference") == "R");
    }

    services::ClassificationService service(db.get(), &matching_store);
    auto result = service.classify_all();
    REQUIRE(result.has_value());
    CHECK(result->type_updated == 1);
    {
        auto session = db->session();
        auto after = storage::SymbolRepository(session).find_by_id(sym.id());
        REQUIRE(after.has_value());
        CHECK(after->component_type_id == "Resistor");   // 依据是 Reference R，不是名字
    }
    CHECK(read(file) == source);
}

TEST_CASE_FIXTURE(LibraryFixture, "U08PIPE footprint file root fails before writes or linking")
{
    const auto owner = group();
    std::vector<std::string> phases;
    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    pipe | services::symbols_from{file.parent_path().string(), owner}
         | services::footprints_from{file.string()}
         | services::progress{[&](const std::string& phase, int, int) { phases.push_back(phase); }};
    const auto result = pipe | services::execute;
    CHECK_FALSE(result.ok());
    CHECK(result.error_count == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols") == 0);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM imported_files") == 0);
    CHECK(std::find(phases.begin(), phases.end(), "linking") == phases.end());
}

TEST_CASE_FIXTURE(LibraryFixture, "U08PIPE single symbol file imports only selected file")
{
    write(file.parent_path() / "Other.kicad_sym",
          "(kicad_symbol_lib (version 20231120) (symbol \"Other\" (property \"Reference\" \"U\")))");
    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    const auto result = pipe | services::symbols_from{file.string(), ""} | services::execute;
    CHECK(result.ok());
    CHECK(result.symbols == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'R'") == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'Other'") == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "U08PIPE linking reads only symbol identity and footprint reference")
{
    symbol();
    sqlite3* handle = nullptr;
    struct RemoveAuthorizer
    {
        sqlite3*& handle;
        ~RemoveAuthorizer() { if (handle) sqlite3_set_authorizer(handle, nullptr, nullptr); }
    } cleanup{handle};
    {
        auto session = db->session();
        handle = session.handle();
        REQUIRE(sqlite3_set_authorizer(handle,
            [](void*, int action, const char* table, const char* column, const char*, const char*)
            {
                return action == SQLITE_READ && table && column
                           && std::string_view(table) == "symbols"
                           && std::string_view(column) == "properties_json" ? SQLITE_DENY : SQLITE_OK;
            }, nullptr) == SQLITE_OK);
    }
    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    const auto result = pipe | services::execute;
    sqlite3_set_authorizer(handle, nullptr, nullptr);
    CHECK(result.ok());
    CHECK(result.error_count == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "U08PIPE unchanged correct imported relationship is not rewritten")
{
    const auto sym = symbol();
    const auto fp = footprint_in("Pkg.pretty", "Old", true, true);
    {
        auto session = db->session();
        REQUIRE(storage::RelationshipRepository(session)
                    .link_symbol_to_footprint(sym.id(), fp.id(), "imported", 1.0).has_value());
    }
    REQUIRE(db->execute("CREATE TRIGGER u08_no_link_delete BEFORE DELETE ON symbol_footprint_links "
                        "BEGIN SELECT RAISE(ABORT, 'correct link must stay'); END").has_value());
    REQUIRE(db->execute("CREATE TRIGGER u08_no_link_insert BEFORE INSERT ON symbol_footprint_links "
                        "BEGIN SELECT RAISE(ABORT, 'correct link must stay'); END").has_value());
    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    const auto result = pipe | services::execute;
    CHECK(result.ok());
    CHECK(result.error_count == 0);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbol_footprint_links") == 1);
}

TEST_CASE_FIXTURE(LibraryFixture, "U08PIPE cancelled footprint parse does not start every source")
{
    const auto directory = temp.path() / "Bounded.pretty";
    constexpr int file_count = 64;
    for (int i = 0; i < file_count; ++i)
        write(directory / ("Part" + std::to_string(i) + ".kicad_mod"),
              "(footprint \"Part" + std::to_string(i) + "\" (layer \"F.Cu\"))");
    std::atomic<bool> cancel{false};
    int parsed = 0;
    services::ImportPipeline pipe(*db, &cancel, false, &matching_store);
    pipe | services::footprints_from{directory.string()}
         | services::progress{[&](const std::string& phase, int completed, int total)
           {
               if (phase != "footprints_parse") return;
               CHECK(total == file_count);
               CHECK(completed >= parsed);
               parsed = completed;
               if (completed > 0) cancel.store(true);
           }};
    const auto result = pipe | services::execute;
    CHECK(parsed >= 1);
    CHECK(parsed <= 16);
    CHECK(result.skipped == 0);
    CHECK(result.footprints == 0);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM footprints") == 0);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM imported_files") == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "async import can restart after its previous thread finished")
{
    group();
    api::ImportManager manager(*db, nullptr, &matching_store);
    for (int run = 1; run <= 2; ++run)
    {
        manager.start_async();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (manager.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE_FALSE(manager.is_running());
        CHECK(manager.orchestrator()->import_seq() == run);
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "manual import honors KF ID setting")
{
    group();
    util::ConfigStore config(temp.path() / "config.json");
    config.set_bool("write_kf_id_to_file", true);
    api::ImportManager manager(*db, &config, &matching_store);
    manager.import_all({});
    CHECK(read(file).find("Kicad_Forge_ID") != std::string::npos);
}

TEST_CASE_FIXTURE(LibraryFixture, "cancelled pipeline does not write sources or consume import snapshots")
{
    std::atomic<bool> cancel = true;
    services::ImportPipeline cancelled(*db, &cancel, true, &matching_store);
    auto result = cancelled | services::symbols_from{file.parent_path().string(), ""} | services::execute;
    CHECK(result.symbols == 0);
    CHECK(read(file) == source);
    {
        auto session = db->session();
        auto snapshot = storage::ImportedFilesRepository(session).find_mtime(file.string());
        REQUIRE(snapshot.has_value());
        CHECK_FALSE(snapshot->has_value());
    }
    cancel = false;
    services::ImportPipeline resumed(*db, &cancel, false, &matching_store);
    result = resumed | services::symbols_from{file.parent_path().string(), ""} | services::execute;
    CHECK(result.symbols == 1);
}

TEST_CASE_FIXTURE(LibraryFixture, "source protection checks complete path components")
{
    group(true);
    auto session = db->session();
    auto locked = services::source_is_locked(session, file);
    REQUIRE(locked.has_value());
    CHECK(*locked);
    auto sibling = temp.path() / "symbols-extra" / "other.kicad_sym";
    auto unlocked = services::source_is_locked(session, sibling);
    REQUIRE(unlocked.has_value());
    CHECK_FALSE(*unlocked);
    CHECK_FALSE(services::source_belongs_to(temp.path() / "other", file.parent_path().string()));
}

TEST_CASE_FIXTURE(LibraryFixture, "same-name libraries from different sources stay separate")
{
    auto other = temp.path() / "symbols2" / "test.kicad_sym";
    write(other, source);

    auto session = db->session();
    storage::LibraryRepository lr(session);
    core::LibraryMeta first;
    first.name = "test";
    first.file_path = file;
    core::LibraryMeta second;
    second.name = "test";  // 显示名相同
    second.file_path = other;

    auto a = lr.upsert_by_source_key(first);
    auto b = lr.upsert_by_source_key(second);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->id != b->id);                  // 不同来源 → 不同行
    CHECK(a->source_key != b->source_key);
    CHECK_FALSE(a->source_key.empty());

    // 同一来源重复导入 → 保留原 UUID
    auto again = lr.upsert_by_source_key(first);
    REQUIRE(again.has_value());
    CHECK(again->id == a->id);
}

TEST_CASE_FIXTURE(LibraryFixture, "same source under two component libraries reports ownership conflict")
{
    auto owner_a = group();
    auto owner_b = group();  // 另一个组件库，指向同一目录
    REQUIRE(owner_a != owner_b);

    auto session = db->session();
    storage::LibraryRepository lr(session);
    core::LibraryMeta meta;
    meta.name = "test";
    meta.file_path = file;
    meta.component_library_id = owner_a;
    auto first = lr.upsert_by_source_key(meta);
    REQUIRE(first.has_value());

    meta.component_library_id = owner_b;
    auto conflict = lr.upsert_by_source_key(meta);
    CHECK_FALSE(conflict.has_value());  // 不暗中改归属

    auto stored = lr.find_by_source_key(first->source_key);
    REQUIRE(stored.has_value());
    CHECK(stored->component_library_id == owner_a);
}

TEST_CASE_FIXTURE(LibraryFixture, "same-name footprints in different libraries stay separate")
{
    auto dir_a = temp.path() / "PkgA.pretty";
    auto dir_b = temp.path() / "PkgB.pretty";
    write(dir_a / "X.kicad_mod", "(footprint \"X\")");
    write(dir_b / "X.kicad_mod", "(footprint \"X\")");

    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    pipe | services::footprints_from{dir_a.string()};
    pipe | services::footprints_from{dir_b.string()};
    auto result = pipe | services::execute;
    CHECK(result.footprints == 2);

    auto session = db->session();
    auto all = storage::FootprintRepository(session).find_all();
    REQUIRE(all.has_value());
    REQUIRE(all->size() == 2);
    CHECK((*all)[0].source_key() != (*all)[1].source_key());
    auto lib_names = {(*all)[0].footprint_lib_name(), (*all)[1].footprint_lib_name()};
    CHECK(std::set<std::string>(lib_names.begin(), lib_names.end()) ==
          std::set<std::string>{"PkgA", "PkgB"});
}

TEST_CASE_FIXTURE(LibraryFixture, "path ownership respects component boundaries")
{
    auto root = temp.path() / "libs";
    auto inside = root / "x.kicad_sym";
    auto sibling = temp.path() / "libs2" / "x.kicad_sym";
    write(inside, "x");
    write(sibling, "x");

    CHECK(storage::path_belongs_to(inside, root));
    CHECK(storage::path_belongs_to(root, root));
    CHECK_FALSE(storage::path_belongs_to(sibling, root));  // C:/libs2 不属于 C:/libs
    CHECK_FALSE(storage::path_belongs_to(root / ".." / "libs3", root));
}

TEST_CASE_FIXTURE(LibraryFixture, "source key backfill keys unique rows and leaves conflicts unbound")
{
    auto unique_file = temp.path() / "symbols3" / "unique.kicad_sym";
    write(unique_file, source);
    auto missing_file = temp.path() / "gone.kicad_sym";

    auto session = db->session();
    auto run = [&](const std::string& sql) {
        REQUIRE(sqlite3_exec(session.handle(), sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    };
    // 模拟旧库：没有来源键的同名两行 + 唯一行 + 文件已丢失的行
    // （component_library_id 显式 NULL：空串会撞外键，仓储走的是同一个约定）
    run("INSERT INTO libraries(id,name,file_path,component_library_id,source_key) "
        "VALUES('old1','dup','" + file.string() + "',NULL,NULL)");
    run("INSERT INTO libraries(id,name,file_path,component_library_id,source_key) "
        "VALUES('old2','dup','" + file.string() + "',NULL,NULL)");
    run("INSERT INTO libraries(id,name,file_path,component_library_id,source_key) "
        "VALUES('old3','unique','" + unique_file.string() + "',NULL,NULL)");
    run("INSERT INTO libraries(id,name,file_path,component_library_id,source_key) "
        "VALUES('old4','missing','" + missing_file.string() + "',NULL,NULL)");

    auto report = storage::backfill_source_keys(session.handle());
    CHECK(report.libraries_keyed == 1);       // 只有唯一的 old3 落键
    CHECK(report.libraries_unresolved == 1);  // 文件缺失的 old4
    REQUIRE(report.conflicts.size() == 1);    // 同一来源两行

    storage::LibraryRepository lr(session);
    auto keyed = lr.find_by_source_key(storage::existing_source_key(unique_file));
    REQUIRE(keyed.has_value());
    CHECK(keyed->id == "old3");
    // 冲突行保留 UUID 与旧路径，只把来源键留空
    auto all = lr.find_all();
    REQUIRE(all.has_value());
    int unbound = 0;
    for (auto& lib : *all)
    {
        if (lib.source_key.empty())
            unbound++;
    }
    CHECK(unbound == 3);  // dup ×2 + missing ×1
}

TEST_CASE_FIXTURE(LibraryFixture, "stored type ids survive registry reordering")
{
    RegistrySnapshot restore;
    auto sym = symbol();
    {
        auto session = db->session();
        storage::SymbolRepository repo(session);
        auto row = repo.find_by_id(sym.id());
        REQUIRE(row.has_value());
        row->component_type_id = "Capacitor";
        REQUIRE(repo.update(*row).has_value());
    }

    // 换一份顺序不同的注册表：ID 才是身份，顺序变了值不能跟着变
    core::TypeRegistry::instance().load_component_types_json(
        R"({"types":[{"name":"Inductor"},{"name":"Capacitor"},{"name":"Resistor"},{"name":"Unknown"}]})");

    auto session = db->session();
    auto after = storage::SymbolRepository(session).find_by_id(sym.id());
    REQUIRE(after.has_value());
    CHECK(after->component_type_id == "Capacitor");
    CHECK(core::TypeRegistry::instance().component_display("Capacitor") == "Capacitor");
}

TEST_CASE_FIXTURE(LibraryFixture, "unrecognized type ids survive update and reimport")
{
    RegistrySnapshot restore;
    auto sym = symbol();
    {
        auto session = db->session();
        storage::SymbolRepository repo(session);
        auto row = repo.find_by_id(sym.id());
        REQUIRE(row.has_value());
        row->component_type_id = "custom:not-registered";
        row->package_type_id = "custom:pkg";
        REQUIRE(repo.update(*row).has_value());
    }
    // 展示层不认识这个 ID，就原样显示，不悄悄改成 Unknown
    CHECK(core::TypeRegistry::instance().component_display("custom:not-registered") ==
          "custom:not-registered");

    // 让文件"变过"再导入：普通更新必须保留认不出的原始 ID
    auto old_time = fs::last_write_time(file);
    write(file, std::string(source) + "\n");
    fs::last_write_time(file, old_time + std::chrono::seconds(2));
    import();

    auto session = db->session();
    auto after = storage::SymbolRepository(session).find_by_id(sym.id());
    REQUIRE(after.has_value());
    CHECK(after->component_type_id == "custom:not-registered");
    CHECK(after->package_type_id == "custom:pkg");
}

TEST_CASE_FIXTURE(LibraryFixture, "type deletion refuses built-ins and referenced custom types")
{
    RegistrySnapshot restore;
    auto& reg = core::TypeRegistry::instance();
    CHECK_FALSE(reg.remove_component_type("Capacitor"));  // 内置不可删

    core::TypeEntry entry;
    entry.name = "MyCustomType";
    auto added = reg.add_component_type(entry);
    REQUIRE(added.has_value());
    CHECK(added->starts_with("custom:"));

    auto sym = symbol();
    {
        auto session = db->session();
        storage::SymbolRepository repo(session);
        auto row = repo.find_by_id(sym.id());
        REQUIRE(row.has_value());
        row->component_type_id = *added;
        REQUIRE(repo.update(*row).has_value());
        auto references = repo.count_by_component_type(*added);
        REQUIRE(references.has_value());
        CHECK(*references == 1);
    }
    CHECK(reg.remove_component_type(*added));  // 未被引用时可以删
}

TEST_CASE_FIXTURE(LibraryFixture, "legacy type files get ids on first save")
{
    RegistrySnapshot restore;
    auto path = temp.path() / "component_types.json";
    write(path, R"({"types":[{"name":"Resistor"},{"name":"MyLegacy","icon":"x"}]})");

    auto& reg = core::TypeRegistry::instance();
    REQUIRE(reg.load_component_types(path.string()));
    auto loaded = reg.find_component("MyLegacy");  // 旧文件的 name 原样作为 legacy ID
    REQUIRE(loaded.has_value());
    CHECK(loaded->name == "MyLegacy");

    REQUIRE(reg.save_component_types());
    auto saved = read(path);
    CHECK(saved.find("\"id\"") != std::string::npos);
    CHECK(saved.find("\"MyLegacy\"") != std::string::npos);
}

namespace
{
/// 直接数一张表的行数（测试里查恢复日志的残渣）
int count_rows(storage::Database& db, const std::string& table)
{
    auto session = db.session();
    int count = -1;
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "SELECT COUNT(*) FROM " + table;
    if (sqlite3_prepare_v2(session.handle(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
    {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

void insert_mutation_log(storage::Database& db, const std::string& id, const std::string& state,
                         const std::string& plan)
{
    auto session = db.session();
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(session.handle(),
                               "INSERT INTO file_mutations(id,state,plan_json) VALUES(?1,?2,?3)",
                               -1, &stmt, nullptr) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, plan.c_str(), -1, SQLITE_TRANSIENT);
    REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "source mutation rolls the file back when the business step fails")
{
    auto target = temp.path() / "symbols" / "mutable.kicad_sym";
    write(target, "original");
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "mutable";
        meta.file_path = target;
        REQUIRE(storage::LibraryRepository(session).insert(meta).has_value());
    }

    io::SourceMutationService mutations(*db);
    auto failing = [](storage::DbSession&) -> util::Result<void>
    {
        return std::unexpected(util::Error::make<util::Error::Kind::DbError>("boom"));
    };
    auto result = mutations.apply(
        {io::FileChange{.path = target,
                        .content = "changed",
                        .expected_digest = io::content_digest("original")}},
        failing);
    CHECK_FALSE(result.has_value());
    CHECK(read(target) == "original");           // 文件回到原状
    CHECK(count_rows(*db, "file_mutations") == 0);  // 日志不留残渣

    // 业务成功：文件替换 + 日志转 applied
    auto ok = [](storage::DbSession&) -> util::Result<void> { return {}; };
    REQUIRE(mutations.apply({io::FileChange{.path = target,
                                            .content = "changed",
                                            .expected_digest = io::content_digest("original")}},
                            ok)
                .has_value());
    CHECK(read(target) == "changed");
    CHECK(count_rows(*db, "file_mutations") == 1);
    // 备份/临时文件都清理干净
    CHECK_FALSE(fs::exists(fs::path(target.string() + ".bak")));
}

TEST_CASE_FIXTURE(LibraryFixture, "source mutation refuses to overwrite an externally edited file")
{
    auto target = temp.path() / "symbols" / "edited.kicad_sym";
    write(target, "original");
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "edited";
        meta.file_path = target;
        REQUIRE(storage::LibraryRepository(session).insert(meta).has_value());
    }
    write(target, "external edit");  // 读完之后被外部改过

    io::SourceMutationService mutations(*db);
    auto ok = [](storage::DbSession&) -> util::Result<void> { return {}; };
    auto result = mutations.apply({io::FileChange{.path = target,
                                                  .content = "mine",
                                                  .expected_digest = io::content_digest("original")}},
                                  ok);
    CHECK_FALSE(result.has_value());
    CHECK(read(target) == "external edit");  // 外部修改不被覆盖
}

TEST_CASE_FIXTURE(LibraryFixture, "startup recovery restores prepared mutations and clears applied ones")
{
    auto restored = temp.path() / "symbols" / "restored.kicad_sym";
    auto finalized = temp.path() / "symbols" / "finalized.kicad_sym";
    auto conflicted = temp.path() / "symbols" / "conflicted.kicad_sym";
    write(restored, "half-written");
    write(fs::path(restored.string() + ".bak"), "original");
    write(finalized, "final");
    write(fs::path(finalized.string() + ".bak"), "before");
    write(conflicted, "someone else wrote this");
    write(fs::path(conflicted.string() + ".bak"), "before");

    // old_digest 必须是真实旧内容的摘要 —— 空摘要不是合法计划，
    // 恢复器无从判断备份是否可信
    auto plan = [](const std::string& path, const std::string& old_content,
                   const std::string& new_content)
    {
        nlohmann::json file;
        file["path"] = path;
        file["temp"] = path + ".tmp";
        file["backup"] = path + ".bak";
        file["existed"] = true;
        file["remove"] = false;
        file["old_digest"] = io::content_digest(old_content);
        file["new_digest"] = io::content_digest(new_content);
        nlohmann::json root;
        root["operation"] = "test";
        root["files"] = nlohmann::json::array({file});
        return root.dump();
    };
    insert_mutation_log(*db, "m-prepared", "prepared",
                        plan(restored.string(), "original", "half-written"));
    insert_mutation_log(*db, "m-applied", "applied",
                        plan(finalized.string(), "before", "final"));
    insert_mutation_log(*db, "m-conflict", "applied",
                        plan(conflicted.string(), "before", "our-result"));

    io::SourceMutationService mutations(*db);
    auto report = mutations.recover();
    CHECK(report.restored == 1);
    CHECK(report.finalized == 1);
    CHECK(report.conflicts == 1);
    CHECK(read(restored) == "original");        // prepared 还原
    CHECK(read(finalized) == "final");          // applied 保持结果
    CHECK(read(conflicted) == "someone else wrote this");  // 冲突不覆盖
    CHECK(fs::exists(fs::path(conflicted.string() + ".bak")));  // 证据保留
    CHECK_FALSE(fs::exists(fs::path(restored.string() + ".bak")));
    // 冲突的那条必须保留未决记录与证据，只有处理干净的两条被清掉
    CHECK(count_rows(*db, "file_mutations") == 1);
}

TEST_CASE_FIXTURE(LibraryFixture, "library delete removes source file and rows in one mutation")
{
    auto target = temp.path() / "symbols" / "gone.kicad_sym";
    write(target, source);
    std::string lib_id;
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "gone";
        meta.file_path = target;
        auto inserted = storage::LibraryRepository(session).insert(meta);
        REQUIRE(inserted.has_value());
        lib_id = inserted->id;
        core::Symbol sym;
        sym.set_name("R");
        sym.set_library_id(lib_id);
        REQUIRE(storage::SymbolRepository(session).insert(sym).has_value());
    }

    services::LibraryService libraries(db.get(), &matching_store);
    auto removed = libraries.delete_library(lib_id);
    REQUIRE(removed.has_value());
    CHECK(removed->symbols_removed == 1);
    CHECK_FALSE(fs::exists(target));  // 文件与行一起消失
    {
        auto session = db->session();
        CHECK_FALSE(storage::LibraryRepository(session).find_by_source_key(
                        storage::existing_source_key(target))
                        .has_value());
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "malformed candidate content never reaches the source file")
{
    auto target = temp.path() / "symbols" / "guard.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120))");
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "guard";
        meta.file_path = target;
        REQUIRE(storage::LibraryRepository(session).insert(meta).has_value());
    }

    io::SourceMutationService mutations(*db);
    auto ok = [](storage::DbSession&) -> util::Result<void> { return {}; };
    auto result = mutations.apply({io::FileChange{.path = target,
                                                  .content = "(kicad_symbol_lib (broken",
                                                  .expected_digest =
                                                      io::content_digest(
                                                          "(kicad_symbol_lib (version 20231120))")}},
                                  ok);
    CHECK_FALSE(result.has_value());
    CHECK(read(target) == "(kicad_symbol_lib (version 20231120))");
    CHECK(count_rows(*db, "file_mutations") == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "data version advances only on committed mutations")
{
    auto target = temp.path() / "symbols" / "versioned.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120))");
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "versioned";
        meta.file_path = target;
        REQUIRE(storage::LibraryRepository(session).insert(meta).has_value());
    }

    auto before = db->data_version();
    io::SourceMutationService mutations(*db);
    auto failing = [](storage::DbSession&) -> util::Result<void>
    {
        return std::unexpected(util::Error::make<util::Error::Kind::DbError>("boom"));
    };
    CHECK_FALSE(mutations.apply({io::FileChange{.path = target,
                                                .content = "(kicad_symbol_lib (version 20231120) )",
                                                .expected_digest =
                                                    io::content_digest(
                                                        "(kicad_symbol_lib (version 20231120))")}},
                                failing)
                    .has_value());
    CHECK(db->data_version() == before);  // 失败不算数据变化

    auto ok = [](storage::DbSession&) -> util::Result<void> { return {}; };
    REQUIRE(mutations.apply({io::FileChange{.path = target,
                                            .content = "(kicad_symbol_lib (version 20231120) )",
                                            .expected_digest =
                                                io::content_digest(
                                                    "(kicad_symbol_lib (version 20231120))")}},
                            ok)
                .has_value());
    CHECK(db->data_version() == before + 1);
}

namespace
{
// 最小 staged-import-v1 插件：只往暂存目录写文件。
// 符号名带零件号（不同零件不撞名），Stamp 用暂存目录名 ——
// 同一个零件重跑时内容必然不同，用来验证「同源更新是替换不是追加」。
const char* STUB_PLUGIN_PY = R"PY(
import json, os, sys
action = sys.argv[1] if len(sys.argv) > 1 else ""
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
lcsc = args.get("lcsc_id", "")
# 尾缀 X 只是换个来源编号，转换出的符号名相同 —— 用来构造「异源同名」
base = lcsc[:-1] if lcsc.endswith("X") else lcsc
name = "STUB_" + base
stamp = os.path.basename(staging.rstrip("/\\"))
symbol = ('(symbol "%s" (property "Reference" "U") (property "Footprint" "old:fp")'
          ' (property "Stamp" "%s"))') % (name, stamp)
with open(os.path.join(staging, name + ".kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator stub)\n" + symbol + "\n)\n")
mode = (args.get("footprint_choice") or {}).get("mode", "new")
entry = None
if mode != "reuse":
    with open(os.path.join(staging, "STUBFP.kicad_mod"), "w", encoding="utf-8") as f:
        f.write('(footprint "STUBFP" (layer "F.Cu"))')
    entry = {"name": "STUBFP", "file": "STUBFP.kicad_mod"}
manifest = {"protocol": "staged-import-v1", "lcsc_id": lcsc,
            "symbols": [{"name": name, "file": name + ".kicad_sym"}], "footprint": entry}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY";

int count_occurrences(const std::string& text, const std::string& needle)
{
    int count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        count++;
    return count;
}

const char* STUB_PLUGIN_MANIFEST = R"JSON({
  "id": "stub.lcsc",
  "name": "Stub LCSC",
  "version": "1.0.0",
  "actions": [ { "id": "import", "name": "Import", "protocol": "staged-import-v1" } ]
})JSON";
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "staged import commits host-validated files and is idempotent")
{
    if (std::system("python -c \"\" >NUL 2>&1") != 0)
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "MyLib.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"EXISTING\" (property \"Reference\" \"U\"))\n)\n");

    std::string library_id;
    {
        auto session = db->session();
        storage::ComponentLibraryRepository::ComponentLibrary group;
        group.name = "User";
        group.symbol_path = sym_dir.string();
        group.footprint_path = fp_dir.string();
        auto inserted_group = storage::ComponentLibraryRepository(session).insert(group);
        REQUIRE(inserted_group.has_value());

        core::LibraryMeta meta;
        meta.name = "MyLib";
        meta.file_path = target;
        meta.component_library_id = inserted_group->id;
        auto inserted = storage::LibraryRepository(session).insert(meta);
        REQUIRE(inserted.has_value());
        library_id = inserted->id;
    }

    auto scan = temp.path() / "plugins";
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", STUB_PLUGIN_PY);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "stub.lcsc";
    request.external_id = "C123";
    request.target_library_id = library_id;
    request.footprint_mode = "new";
    request.operation_id = "op-1";

    auto result = importer.run(request);
    if (!result)
        MESSAGE("staged import failed: ", result.error().format_message());
    REQUIRE(result.has_value());
    CHECK(result->value("ok", false));
    CHECK(result->value("footprint_reference", "") == "MyLib:STUBFP");

    // 目标库保留原有符号，新符号原样并入，Footprint 引用由宿主决定
    auto merged = read(target);
    CHECK(merged.find("EXISTING") != std::string::npos);
    CHECK(merged.find("STUB_C123") != std::string::npos);
    CHECK(merged.find("\"MyLib:STUBFP\"") != std::string::npos);
    CHECK(merged.find("old:fp") == std::string::npos);

    // 同源更新（内容变了、操作 ID 不同）：符号节点必须被替换而不是追加
    api::StagedImporter::Request update = request;
    update.operation_id = "op-update";
    auto updated_run = importer.run(update);
    REQUIRE(updated_run.has_value());
    auto after_update = read(target);
    CHECK(count_occurrences(after_update, "(symbol \"STUB_C123\"") == 1);
    CHECK(after_update.find("EXISTING") != std::string::npos);

    // 封装落在宿主决定的路径上
    CHECK(fs::exists(fp_dir / "MyLib.pretty" / "STUBFP.kicad_mod"));

    // 数据库：符号行 + 关联 + 来源映射
    {
        auto session = db->session();
        auto found = storage::SymbolRepository(session).find_by_name("STUB_C123");
        REQUIRE(found.has_value());
        CHECK(found->footprint() == "MyLib:STUBFP");
        storage::RelationshipRepository links(session);
        auto linked = links.find_footprint_for_symbol(found->id());
        REQUIRE(linked.has_value());
        CHECK(linked->has_value());
    }

    // 同一操作 ID + 同一请求 → 回放原结果，不重复写入
    auto replay = importer.run(request);
    REQUIRE(replay.has_value());
    CHECK(replay->value("replayed", false));
    CHECK(replay->value("symbol_id", std::string("")) == result->value("symbol_id", std::string("")));

    // 同一操作 ID 换了内容 → 拒绝
    request.external_id = "C999";
    CHECK_FALSE(importer.run(request).has_value());

    // 同名符号来自另一个来源（C123X 转换出的名字与 C123 相同）→ 冲突，不悄悄改写
    request.external_id = "C123X";
    request.operation_id = "op-2";
    auto conflict = importer.run(request);
    CHECK_FALSE(conflict.has_value());
    if (!conflict)
        CHECK(conflict.error().message().find("different source") != std::string::npos);

    // 来源映射记在库里：同一 (目标库, 来源, 编号) 只认一个符号
    {
        auto session = db->session();
        int mappings = -1;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(session.handle(), "SELECT COUNT(*) FROM import_sources", -1, &stmt,
                               nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        {
            mappings = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        CHECK(mappings == 1);
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "staged import rejects plugin output that escapes the staging dir")
{
    if (std::system("python -c \"\" >NUL 2>&1") != 0)
    {
        WARN("python not available — staged import test skipped");
        return;
    }
    auto root = temp.path() / "userlib2";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Evil.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (symbol \"KEEP\"))\n");
    std::string library_id;
    {
        auto session = db->session();
        storage::ComponentLibraryRepository::ComponentLibrary group;
        group.name = "User2";
        group.symbol_path = sym_dir.string();
        group.footprint_path = fp_dir.string();
        auto inserted_group = storage::ComponentLibraryRepository(session).insert(group);
        REQUIRE(inserted_group.has_value());
        core::LibraryMeta meta;
        meta.name = "Evil";
        meta.file_path = target;
        meta.component_library_id = inserted_group->id;
        auto inserted = storage::LibraryRepository(session).insert(meta);
        REQUIRE(inserted.has_value());
        library_id = inserted->id;
    }

    auto scan = temp.path() / "plugins2";
    write(scan / "evil" / "manifest.json",
          R"({"id":"evil","name":"Evil","version":"1.0.0","actions":[{"id":"import","name":"I","protocol":"staged-import-v1"}]})");
    write(scan / "evil" / "plugin.py", R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
manifest = {"protocol": "staged-import-v1",
            "symbols": [{"name": "ESCAPE", "file": "../../escaped.kicad_sym"}]}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY");
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "evil";
    request.external_id = "C1";
    request.target_library_id = library_id;
    request.footprint_mode = "new";

    auto result = importer.run(request);
    CHECK_FALSE(result.has_value());
    CHECK(read(target) == "(kicad_symbol_lib (version 20231120) (symbol \"KEEP\"))\n");
    CHECK(count_rows(*db, "file_mutations") == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "KF ID patch is idempotent and does not replace an existing ID")
{
    CHECK(sexpr::patch_kf_id_to_file(file, {{"R", "KF123"}}) == 1);
    auto first = read(file);
    CHECK(sexpr::patch_kf_id_to_file(file, {{"R", "OTHER"}}) == 0);
    CHECK(read(file) == first);
    auto parsed = parser::SymbolLibParser::parse(file);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 1);
    CHECK(parsed->items.front().Kicad_Forge_ID() == "KF123");
}

// ============================================================
// LOCAL_AUDIT —— 本地行为边界审查（只追加，不改动既有用例）
//
// 约束：
//   * 所有读写路径都在 fixture 的 TempDirectory 内，不使用绝对路径，
//     不触碰用户库、安装目录或其它工作区；不访问公网。
//   * 断言的是「应有的正确行为」。当前实现满足不了时用例失败 ——
//     失败本身就是审查产出，不为了变绿去改断言或改产品代码。
// ============================================================
// 本段追加需要复核宿主落盘的 .kicad_mod，用仓库既有的 FootprintParser（头文件自带 include guard）
#include "core/io/parser/footprint_parser.h"

namespace
{
/// 读取单个标量（断言/诊断 DB 列）；失败返回 -1 让断言直接暴露
int audit_scalar_int(storage::Database& db, const std::string& sql)
{
    auto session = db.session();
    sqlite3_stmt* stmt = nullptr;
    int value = -1;
    if (sqlite3_prepare_v2(session.handle(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
    {
        value = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

/// 读取单个文本标量（断言未暴露给仓储的列，如 properties_json）；失败返回空串
std::string audit_scalar_text(storage::Database& db, const std::string& sql)
{
    auto session = db.session();
    sqlite3_stmt* stmt = nullptr;
    std::string value;
    if (sqlite3_prepare_v2(session.handle(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (const unsigned char* text = sqlite3_column_text(stmt, 0))
            value.assign(reinterpret_cast<const char*>(text));
    }
    sqlite3_finalize(stmt);
    return value;
}

/// 符号当前挂着的全部封装关联（含关联类型）—— 直接看关系表，
/// 不用 find_footprint_for_symbol（多行时它返回哪一行是不确定的）
struct AuditLink
{
    std::string footprint_id;
    std::string link_type;
};

std::vector<AuditLink> audit_symbol_links(storage::Database& db, const std::string& symbol_id)
{
    auto session = db.session();
    std::vector<AuditLink> links;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(session.handle(),
                           "SELECT footprint_id, link_type FROM symbol_footprint_links "
                           "WHERE symbol_id = ?1",
                           -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text(stmt, 1, symbol_id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            links.push_back({id ? id : "", type ? type : ""});
        }
    }
    sqlite3_finalize(stmt);
    return links;
}

/// 在临时根里登记一个可写的用户库（元件库 + 库行），返回库 UUID
std::string audit_register_user_library(storage::Database& db, const fs::path& target,
                                        const fs::path& symbol_dir, const fs::path& footprint_dir)
{
    auto session = db.session();
    storage::ComponentLibraryRepository::ComponentLibrary group;
    group.name = "AuditUser";
    group.symbol_path = symbol_dir.string();
    group.footprint_path = footprint_dir.string();
    auto inserted_group = storage::ComponentLibraryRepository(session).insert(group);
    REQUIRE(inserted_group.has_value());
    core::LibraryMeta meta;
    meta.name = target.stem().string();
    meta.file_path = target;
    meta.component_library_id = inserted_group->id;
    auto inserted = storage::LibraryRepository(session).insert(meta);
    REQUIRE(inserted.has_value());
    return inserted->id;
}

bool audit_python_available()
{
    return std::system("python -c \"\" >NUL 2>&1") == 0;
}

// 桩产物带完整元数据：Value / Description / MPN、2 个 pin，封装 2 个 pad。
// 符号按 KiCad 8 的正常结构写：顶层 symbol + 单元 symbol，pin 用
// (pin <电气类型> line (at ...) (length ...) (name "..." (effects ...)) (number "..." (effects ...)))，
// 也就是 SymbolLibParser 必须能读回来的真实格式。
// 主机必须把这些解析结果落进数据库，而不是只留名字。
const char* AUDIT_META_PLUGIN_PY = R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
fx = '(effects (font (size 1.27 1.27)))'
symbol = ('(symbol "U_LM358"\n'
          '  (pin_names (offset 0.127))\n'
          '  (exclude_from_sim no)\n'
          '  (in_bom yes)\n'
          '  (on_board yes)\n'
          '  (property "Reference" "U" (at -5.08 5.08 0) ' + fx + ')\n'
          '  (property "Value" "LM358-DR" (at 5.08 5.08 0) ' + fx + ')\n'
          '  (property "Footprint" "old:fp" (at 0 0 0) ' + fx + ' (hide yes))\n'
          '  (property "Datasheet" "https://example.invalid/lm358.pdf" (at 0 0 0) ' + fx + ' (hide yes))\n'
          '  (property "Description" "Dual op-amp, SOIC-8" (at 0 0 0) ' + fx + ' (hide yes))\n'
          '  (property "MPN" "LM358DR" (at 0 0 0) ' + fx + ' (hide yes))\n'
          '  (symbol "U_LM358_1_1"\n'
          '    (pin input line (at -7.62 -2.54 0) (length 2.54)\n'
          '      (name "IN+" ' + fx + ')\n'
          '      (number "1" ' + fx + '))\n'
          '    (pin output line (at 7.62 -5.08 180) (length 2.54)\n'
          '      (name "OUT" ' + fx + ')\n'
          '      (number "2" ' + fx + '))))')
with open(os.path.join(staging, "U_LM358.kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator \"kicad_symbol_editor\")\n" + symbol + "\n)\n")
with open(os.path.join(staging, "AUDITFP2.kicad_mod"), "w", encoding="utf-8") as f:
    f.write('(footprint "AUDITFP2" (layer "F.Cu")\n'
            '  (pad "1" smd rect (at 0 0) (size 1 1))\n'
            '  (pad "2" smd rect (at 1 0) (size 1 1)))\n')
manifest = {"protocol": "staged-import-v1", "lcsc_id": args.get("lcsc_id", ""),
            "symbols": [{"name": "U_LM358", "file": "U_LM358.kicad_sym"}],
            "footprint": {"name": "AUDITFP2", "file": "AUDITFP2.kicad_mod"}}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY";

// 复用封装的桩：两次调用的产物逐字节相同（不掺 staging 目录名/时间戳）——
// 这样「换了复用封装」的唯一差别就只是请求本身，而不是插件内容。
const char* AUDIT_REUSE_PLUGIN_PY = R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
fx = '(effects (font (size 1.27 1.27)))'
symbol = ('(symbol "AUDIT_REUSE"\n'
          '  (property "Reference" "U" (at -5.08 5.08 0) ' + fx + ')\n'
          '  (property "Value" "AUDIT_REUSE" (at 5.08 5.08 0) ' + fx + ')\n'
          '  (property "Footprint" "old:fp" (at 0 0 0) ' + fx + ')\n'
          '  (symbol "AUDIT_REUSE_1_1"\n'
          '    (pin passive line (at 0 0 0) (length 2.54)\n'
          '      (name "P1" ' + fx + ')\n'
          '      (number "1" ' + fx + '))))')
with open(os.path.join(staging, "AUDIT_REUSE.kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator \"kicad_symbol_editor\")\n" + symbol + "\n)\n")
manifest = {"protocol": "staged-import-v1", "lcsc_id": args.get("lcsc_id", ""),
            "symbols": [{"name": "AUDIT_REUSE", "file": "AUDIT_REUSE.kicad_sym"}]}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY";

const char* AUDIT_META_MANIFEST = R"JSON({
  "id": "audit.meta",
  "name": "Audit Metadata Stub",
  "version": "1.0.0",
  "actions": [ { "id": "import", "name": "Import", "protocol": "staged-import-v1" } ]
})JSON";

const char* AUDIT_REUSE_MANIFEST = R"JSON({
  "id": "audit.reuse",
  "name": "Audit Reuse Stub",
  "version": "1.0.0",
  "actions": [ { "id": "import", "name": "Import", "protocol": "staged-import-v1" } ]
})JSON";
}  // namespace

// ------------------------------------------------------------
// 1. 暂存目录必须被限制在专用暂存根内
// 操作 ID 是外部输入，直接拼进文件系统路径再 remove_all，
// 就能借「..」把专用暂存根之外的兄弟目录整棵删掉。
// 受害者目录建在 fixture 自己的临时根里，只验证边界，不碰任何真实路径。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "LOCAL_AUDIT staged import keeps the operation id inside its staging root")
{
    auto root = temp.path() / "userlib_audit1";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Audit.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"KEEP\" (property \"Reference\" \"U\"))\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);

    auto scan = temp.path() / "plugins_audit1";
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", STUB_PLUGIN_PY);
    plugin::PluginManager manager({scan});
    manager.discover();

    // 位于临时根内、但与 staging 平级的目录：正确的实现绝不会碰它
    auto victim = temp.path() / "victim";
    auto sentinel = victim / "sentinel.txt";
    write(sentinel, "keep me");
    write(victim / "nested" / "also.txt", "keep me too");

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "stub.lcsc";
    request.external_id = "C700";
    request.target_library_id = library_id;
    request.footprint_mode = "new";
    request.operation_id = "../victim";  // 借此逃出 <db目录>/staging

    auto result = importer.run(request);
    if (result)
        MESSAGE("operation id was accepted and executed: ", result->dump());
    CHECK_FALSE(result.has_value());  // 带父路径分量的操作 ID 必须被拒绝
    CHECK(fs::exists(victim));        // 暂存根之外的目录不能被 remove_all 掉
    CHECK(read(sentinel) == "keep me");
    CHECK(fs::exists(victim / "nested" / "also.txt"));
}

// ------------------------------------------------------------
// 2. 暂存导入保留解析出来的元数据
// 产物里写明了 Value/Description/MPN、2 个 pin 和 2 个 pad；
// 主机必须按产物落库，而不是建一个只有名字的空壳。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "LOCAL_AUDIT staged import keeps parsed symbol and footprint metadata")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib_audit2";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Audit.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"EXISTING\" (property \"Reference\" \"U\"))\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);

    auto scan = temp.path() / "plugins_audit2";
    write(scan / "meta" / "manifest.json", AUDIT_META_MANIFEST);
    write(scan / "meta" / "plugin.py", AUDIT_META_PLUGIN_PY);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "audit.meta";
    request.external_id = "C701";
    request.target_library_id = library_id;
    request.footprint_mode = "new";
    request.operation_id = "audit-meta-1";

    auto result = importer.run(request);
    if (!result)
        MESSAGE("staged import failed: ", result.error().format_message());
    REQUIRE(result.has_value());
    CHECK(result->value("ok", false));

    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_name("U_LM358");
        REQUIRE(sym.has_value());
        INFO("pin_count=", sym->pin_count(), " value=", sym->default_value(),
             " description=", sym->description(), " mpn=", sym->mpn(),
             " footprint=", sym->footprint());
        CHECK(sym->pin_count() == 2);                       // 产物里 2 个 pin
        CHECK(sym->default_value() == "LM358-DR");           // property Value
        CHECK(sym->description() == "Dual op-amp, SOIC-8");  // property Description
        CHECK(sym->mpn() == "LM358DR");                      // property MPN
        CHECK(sym->reference_prefix() == "U");               // property Reference
        CHECK(sym->footprint() == "Audit:AUDITFP2");         // 宿主改写的封装引用
    }
    // row_to_footprint 不还原 pads，直接查列：产物里 2 个 pad
    CHECK(audit_scalar_int(*db, "SELECT pad_count FROM footprints WHERE name = 'AUDITFP2'") == 2);
    // 符号文件与封装文件都真的落盘了
    CHECK(fs::exists(fp_dir / "Audit.pretty" / "AUDITFP2.kicad_mod"));
    CHECK(read(target).find("U_LM358") != std::string::npos);

    // 再读宿主已落盘的文件复核：产物必须是 parser 认得的真实 KiCad 格式。
    // 上面「入库丢字段」的失败因此只能归因于主机，不能拿夹具格式当借口。
    {
        auto written = parser::SymbolLibParser::parse(target);
        if (!written)
            MESSAGE("reparse of written library failed: ", written.error().format_message());
        REQUIRE(written.has_value());
        const core::Symbol* node = nullptr;
        for (const auto& item : written->items)
            if (item.name() == "U_LM358")
                node = &item;
        REQUIRE(node != nullptr);
        INFO("written file: value=", node->default_value(), " mpn=", node->mpn(),
             " description=", node->description(), " pins=", node->pin_count());
        CHECK(node->default_value() == "LM358-DR");           // property Value
        CHECK(node->mpn() == "LM358DR");                      // property MPN
        CHECK(node->description() == "Dual op-amp, SOIC-8");  // property Description
        CHECK(node->pin_count() == 2);
    }
    {
        auto written_fp =
            parser::FootprintParser::parse(fp_dir / "Audit.pretty" / "AUDITFP2.kicad_mod");
        if (!written_fp)
            MESSAGE("reparse of written footprint failed: ",
                    written_fp.error().format_message());
        REQUIRE(written_fp.has_value());
        CHECK(written_fp->pad_count() == 2);
    }
}

// ------------------------------------------------------------
// 3. 同一零件重新选择复用封装必须生效
// 两次调用只有 footprint_id 不同、插件产物逐字节相同：
// 第二次是新的用户意图，不能被「同源同内容」当成 no-op 丢掉。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "LOCAL_AUDIT reusing a different footprint is not a silent no-op")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib_audit3";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Reuse.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"EXISTING\" (property \"Reference\" \"U\"))\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);

    // 临时根里登记两个可复用封装：Pkg:A 与 Pkg:B
    auto fp_a = footprint("A");
    auto fp_b = footprint("B");
    REQUIRE(fp_a.id() != fp_b.id());

    auto scan = temp.path() / "plugins_audit3";
    write(scan / "reuse" / "manifest.json", AUDIT_REUSE_MANIFEST);
    write(scan / "reuse" / "plugin.py", AUDIT_REUSE_PLUGIN_PY);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request first;
    first.plugin_id = "audit.reuse";
    first.external_id = "C702";
    first.target_library_id = library_id;
    first.footprint_mode = "reuse";
    first.footprint_id = fp_a.id();
    first.operation_id = "audit-reuse-1";

    auto run1 = importer.run(first);
    if (!run1)
        MESSAGE("first reuse import failed: ", run1.error().format_message());
    REQUIRE(run1.has_value());
    CHECK(run1->value("footprint_reference", std::string("")) == "Pkg:A");
    std::string symbol_id = run1->value("symbol_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());

    auto second = first;
    second.footprint_id = fp_b.id();
    second.operation_id = "audit-reuse-2";  // 新操作 ID：不是幂等回放
    auto run2 = importer.run(second);
    if (!run2)
        MESSAGE("second reuse import failed: ", run2.error().format_message());
    REQUIRE(run2.has_value());
    if (run2->value("noop", false))
        MESSAGE("second run was treated as a no-op: ", run2->dump());
    CHECK_FALSE(run2->value("noop", false));  // 换了复用封装就不是重复导入

    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        CHECK(row->footprint() == "Pkg:B");
        auto linked = storage::RelationshipRepository(session).find_footprint_for_symbol(symbol_id);
        REQUIRE(linked.has_value());
        REQUIRE(linked->has_value());
        CHECK(**linked == fp_b.id());
    }
    CHECK(read(target).find("\"Pkg:B\"") != std::string::npos);
}

// ------------------------------------------------------------
// 4. 自动分类保留已确定的有效类型
// LM358、Reference=U、8 脚，库里已经是 OpAmp；
// 名字首字母 L 只是猜类型的兜底，不能覆盖已确定的分类结果。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "LOCAL_AUDIT classification keeps an already determined component type")
{
    // symbols.library_id 有外键约束：先按正常路径登记一个真实库，再挂符号上去
    auto root = temp.path() / "userlib_audit4";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "AuditClassify.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, root / "footprints");

    std::string symbol_id;
    {
        auto session = db->session();
        core::Symbol part;
        part.set_name("LM358");
        part.set_reference_prefix("U");
        part.set_library_id(library_id);
        part.component_type_id = "OpAmp";
        for (int i = 1; i <= 8; ++i)
        {
            core::PinDefinition pin;
            pin.number = std::to_string(i);
            pin.name = "P" + std::to_string(i);
            part.add_pin(std::move(pin));
        }
        auto inserted = storage::SymbolRepository(session).insert(part);
        if (!inserted)
            MESSAGE("symbol insert failed: ", inserted.error().format_message());
        REQUIRE(inserted.has_value());
        symbol_id = inserted->id();
    }

    // 前提核验：落库读回的 Reference / 已定类型 / 引脚数就是用例描述的那一只
    {
        auto session = db->session();
        auto stored = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(stored.has_value());
        INFO("stored: reference_prefix=", stored->reference_prefix(),
             " component_type_id=", stored->component_type_id,
             " pin_count=", stored->pin_count());
        CHECK(stored->reference_prefix() == "U");
        CHECK(stored->component_type_id == "OpAmp");
        CHECK(stored->pin_count() == 8);
    }

    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    REQUIRE(summary.has_value());
    MESSAGE("classification: total=", summary->total, " matched=", summary->matched,
            " type_updated=", summary->type_updated);

    {
        auto session = db->session();
        auto after = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(after.has_value());
        INFO("component_type_id=", after->component_type_id);
        CHECK(after->component_type_id != "Inductor");  // 不能只因首字母 L 就改判
        CHECK(after->component_type_id == "OpAmp");     // 已确定的有效类型原样保留
    }
}

// ------------------------------------------------------------
// 5. 单条写入失败后不能把源文件记成「已导入」
// Good 与 Blocked 同在一个库文件里，Blocked 的 INSERT 被触发器拒绝；
// 第一次导入没写全，就不能留下 mtime 快照挡住重试。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "LOCAL_AUDIT partial insert failure does not consume the file snapshot")
{
    auto dir = temp.path() / "sym_audit5";
    auto target = dir / "Retry.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120)\n"
                  "  (symbol \"AuditGood\" (property \"Reference\" \"R\") "
                  "(property \"Footprint\" \"Pkg:Old\"))\n"
                  "  (symbol \"AuditBlocked\" (property \"Reference\" \"R\") "
                  "(property \"Footprint\" \"Pkg:Old\"))\n)\n");

    // 只拒绝 AuditBlocked 落库：模拟单条写入失败（触发器只建在临时数据库上）
    REQUIRE(db->execute("CREATE TRIGGER local_audit_reject_blocked BEFORE INSERT ON symbols "
                        "WHEN NEW.name = 'AuditBlocked' "
                        "BEGIN SELECT RAISE(ABORT, 'local audit: rejected'); END")
                .has_value());

    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        return pipe | services::symbols_from{dir.string(), ""} | services::execute;
    };
    auto first = run();
    MESSAGE("first run: symbols=", first.symbols, " skipped=", first.skipped);

    int good = audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'AuditGood'");
    int blocked = audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'AuditBlocked'");
    MESSAGE("rows after first run: AuditGood=", good, " AuditBlocked=", blocked);
    // good 只作诊断：整库失败就不该留下半截 Good（未来做对事务回滚时 Good 理应为 0），
    // 把 good==1 写成期望等于固化「允许部分提交」
    CHECK(blocked == 0);  // 前提：这条确实没写进去（触发器对两次导入都拒绝它）

    {
        auto session = db->session();
        auto snapshot = storage::ImportedFilesRepository(session).find_mtime(target.string());
        REQUIRE(snapshot.has_value());
        CHECK_FALSE(snapshot->has_value());  // 没写全的文件不能被记成已导入
    }

    // 撤销触发器后原样重导（源文件一个字节都不改）
    REQUIRE(db->execute("DROP TRIGGER local_audit_reject_blocked").has_value());
    auto second = run();
    MESSAGE("second run: symbols=", second.symbols, " skipped=", second.skipped);
    // 源文件一个字节没改，这次必须把两条都补齐 —— 半途而废不能把文件挡在门外
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'AuditBlocked'") == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'AuditGood'") == 1);
}

// ------------------------------------------------------------
// 6. 外部改过 Footprint 引用后重导，属性与实际关联必须一致
// 文件从 Pkg:A 改成 Pkg:B，mtime 前进两秒再导入；
// 初始关联是导入自动生成的（link_type='imported'），必须跟着引用走。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "LOCAL_AUDIT reimport follows an externally changed footprint reference")
{
    auto sym_dir = temp.path() / "sym_audit6";
    auto sym_file = sym_dir / "Relink.kicad_sym";
    write(sym_file, "(kicad_symbol_lib (version 20231120)\n"
                    "  (symbol \"AuditRelink\" (property \"Reference\" \"R\") "
                    "(property \"Footprint\" \"Pkg:A\"))\n)\n");
    auto pretty = temp.path() / "Pkg.pretty";
    write(pretty / "A.kicad_mod", "(footprint \"A\" (layer \"F.Cu\"))");
    write(pretty / "B.kicad_mod", "(footprint \"B\" (layer \"F.Cu\"))");

    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        pipe | services::symbols_from{sym_dir.string(), ""} |
            services::footprints_from{pretty.string()};
        return pipe | services::execute;
    };
    run();

    std::string symbol_id;
    std::string fp_a;
    std::string fp_b;
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_name("AuditRelink");
        REQUIRE(sym.has_value());
        CHECK(sym->footprint() == "Pkg:A");
        symbol_id = sym->id();
        auto a = storage::FootprintRepository(session).find_by_name("A");
        REQUIRE(a.has_value());
        fp_a = a->id();
        auto b = storage::FootprintRepository(session).find_by_name("B");
        REQUIRE(b.has_value());
        fp_b = b->id();
    }
    REQUIRE(fp_a != fp_b);

    auto before = audit_symbol_links(*db, symbol_id);
    REQUIRE(before.size() == 1);
    CHECK(before[0].footprint_id == fp_a);
    CHECK(before[0].link_type == "imported");  // 前提：自动关联，而不是人工绑定

    // 外部把引用改成 Pkg:B，并让文件看起来变过
    auto old_time = fs::last_write_time(sym_file);
    write(sym_file, "(kicad_symbol_lib (version 20231120)\n"
                    "  (symbol \"AuditRelink\" (property \"Reference\" \"R\") "
                    "(property \"Footprint\" \"Pkg:B\"))\n)\n");
    fs::last_write_time(sym_file, old_time + std::chrono::seconds(2));

    auto second = run();
    MESSAGE("reimport: symbols=", second.symbols, " footprints=", second.footprints);

    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(sym.has_value());
        INFO("footprint_ref=", sym->footprint());
        CHECK(sym->footprint() == "Pkg:B");  // 属性跟着文件走
    }
    auto after = audit_symbol_links(*db, symbol_id);
    for (const auto& link : after)
        MESSAGE("link after reimport: ", link.footprint_id, " (", link.link_type, ")");
    REQUIRE(after.size() == 1);              // 旧关联必须被替换，不能新旧并存
    CHECK(after[0].footprint_id == fp_b);    // 实际关联也必须是 B
}

// ------------------------------------------------------------
// 7. prepared 状态的删除恢复不能覆盖外部重建的内容
// 日志格式沿用既有的 file_mutations 恢复用例；目标文件已被外部重建，
// 恢复时必须报冲突并保留外部内容、备份与未决日志，而不是拿旧备份盖回去。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "LOCAL_AUDIT prepared delete recovery keeps externally rebuilt content")
{
    io::SourceMutationService mutations(*db);
    auto target = temp.path() / "symbols" / "rebuilt.kicad_sym";
    auto backup = fs::path(target.string() + ".bak");

    // remove=true 的计划项没有 temp/new_digest —— 与 apply() 落盘的格式一致
    auto plan = [](const std::string& path)
    {
        nlohmann::json file;
        file["path"] = path;
        file["temp"] = path + ".tmp";
        file["backup"] = path + ".bak";
        file["existed"] = true;
        file["remove"] = true;
        file["old_digest"] = io::content_digest("original");
        file["new_digest"] = "";
        nlohmann::json root;
        root["operation"] = "test";
        root["files"] = nlohmann::json::array({file});
        return root.dump();
    };

    SUBCASE("file was rebuilt externally while the delete was pending")
    {
        write(target, "external rebuild");
        write(backup, "original");
        insert_mutation_log(*db, "m-audit-delete", "prepared", plan(target.string()));

        auto report = mutations.recover();
        for (const auto& message : report.messages)
            MESSAGE("recover: ", message);
        MESSAGE("recover: restored=", report.restored, " finalized=", report.finalized,
                " conflicts=", report.conflicts);
        CHECK(report.conflicts == 1);                    // 摘要对不上 → 报冲突
        CHECK(read(target) == "external rebuild");       // 外部重建的内容不被备份覆盖
        CHECK(fs::exists(backup));                       // 备份作为证据保留
        CHECK(read(backup) == "original");
        CHECK(count_rows(*db, "file_mutations") == 1);   // 未决变更的日志不能被悄悄清掉
    }

    SUBCASE("file is still missing — restore it from the backup")
    {
        write(backup, "original");
        insert_mutation_log(*db, "m-audit-delete", "prepared", plan(target.string()));
        REQUIRE_FALSE(fs::exists(target));

        auto report = mutations.recover();
        MESSAGE("recover: restored=", report.restored, " conflicts=", report.conflicts);
        CHECK(report.conflicts == 0);
        CHECK(report.restored == 1);                     // 该还原的仍然要还原
        CHECK(read(target) == "original");
        CHECK_FALSE(fs::exists(backup));
        CHECK(count_rows(*db, "file_mutations") == 0);
    }
}

// ------------------------------------------------------------
// 8. 标准 KiCad 引脚与 Reference 的解析
// 夹具是 KiCad 8 编辑器实际写出的结构：顶层 symbol + 单元 symbol，引脚形如
//   (pin input line (at ...) (length ...)
//     (name "IN+" (effects (font (size ...)))) (number "1" (effects (font (size ...)))))
// 正确行为：编号取自 (number ...)，名字取自 (name ...)，电气类型取自 pin 的
// 第二个位置，reference_prefix 来自 property Reference "U"。
// 当前实现把电气类型当成编号、(number ...) 整个丢掉，又用型号首字母覆盖 Reference，
// 所以这些断言现在会失败 —— 失败即审查产出，不改成迁就现状的期望。
// 只用夹具临时根里的文件，不碰插件缓存与用户元件库。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "LOCAL_AUDIT parses a standard KiCad pin and Reference")
{
    auto dir = temp.path() / "sym_audit8";
    auto path = dir / "LM358.kicad_sym";
    write(path, R"KICAD((kicad_symbol_lib
  (version 20231120)
  (generator "kicad_symbol_editor")
  (generator_version "8.0")
  (symbol "LM358"
    (pin_names (offset 0.127))
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (property "Reference" "U"
      (at -5.08 5.08 0)
      (effects (font (size 1.27 1.27)))
    )
    (property "Value" "LM358"
      (at 5.08 5.08 0)
      (effects (font (size 1.27 1.27)))
    )
    (property "Footprint" ""
      (at 0 0 0)
      (effects (font (size 1.27 1.27)) (hide yes))
    )
    (property "Datasheet" "https://www.ti.com/lit/ds/symlink/lm358.pdf"
      (at 0 0 0)
      (effects (font (size 1.27 1.27)) (hide yes))
    )
    (symbol "LM358_1_1"
      (pin input line
        (at -7.62 2.54 0)
        (length 2.54)
        (name "IN+" (effects (font (size 1.27 1.27))))
        (number "1" (effects (font (size 1.27 1.27))))
      )
      (pin output line
        (at 7.62 0 180)
        (length 2.54)
        (name "OUT" (effects (font (size 1.27 1.27))))
        (number "2" (effects (font (size 1.27 1.27))))
      )
    )
  )
)
)KICAD");

    auto parsed = parser::SymbolLibParser::parse(path);
    if (!parsed)
        MESSAGE("standard symbol parse failed: ", parsed.error().format_message());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 1);

    const core::Symbol& sym = parsed->items.front();
    INFO("name=", sym.name(), " reference_prefix=", sym.reference_prefix(),
         " pins=", sym.pin_count());
    CHECK(sym.name() == "LM358");
    CHECK(sym.reference_prefix() == "U");  // 由 property Reference 决定，不是型号首字母 "LM"
    CHECK(sym.pin_count() == 2);
    REQUIRE(sym.pins().size() == 2);
    CHECK(sym.pins()[0].number == "1");                // (number "1" ...)
    CHECK(sym.pins()[1].number == "2");
    CHECK(sym.pins()[0].name == "IN+");                // (name "IN+" ...)
    CHECK(sym.pins()[1].name == "OUT");
    CHECK(sym.pins()[0].electrical_type == "input");   // (pin input line ...)
    CHECK(sym.pins()[1].electrical_type == "output");
}

// ============================================================
// FIX01 —— 恢复、导入原子性、源引用关系一致性
// 全部只在 fixture 临时目录与临时数据库内操作
// ============================================================

namespace
{
/// FIX01 用的文件变更计划（与 apply() 落盘的字段一致）
std::string fix01_plan(const std::string& path, bool existed, bool remove,
                       const std::string& old_content, const std::string& new_content)
{
    nlohmann::json file;
    file["path"] = path;
    file["temp"] = path + ".tmp";
    file["backup"] = path + ".bak";
    file["existed"] = existed;
    file["remove"] = remove;
    file["old_digest"] = old_content.empty() ? "" : io::content_digest(old_content);
    file["new_digest"] = new_content.empty() ? "" : io::content_digest(new_content);
    nlohmann::json root;
    root["operation"] = "fix01";
    root["files"] = nlohmann::json::array({file});
    return root.dump();
}
}  // namespace

// ------------------------------------------------------------
// FIX01-L01-a：applied 计划要求目标确实还是记录里的结果
// 目标缺失（或内容对不上）都不是「已完成」，必须报冲突并保留证据。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 applied recovery refuses a missing or changed result")
{
    io::SourceMutationService mutations(*db);

    SUBCASE("target is missing although the mutation was recorded as applied")
    {
        auto target = temp.path() / "symbols" / "applied_gone.kicad_sym";
        auto backup = fs::path(target.string() + ".bak");
        write(backup, "before");
        insert_mutation_log(*db, "m-fix01-applied-gone", "applied",
                            fix01_plan(target.string(), true, false, "before", "after"));

        auto report = mutations.recover();
        for (const auto& message : report.messages)
            MESSAGE("recover: ", message);
        CHECK(report.conflicts == 1);
        CHECK(report.finalized == 0);                    // 缺文件不算已完成
        CHECK_FALSE(fs::exists(target));
        CHECK(fs::exists(backup));                       // 证据保留
        CHECK(count_rows(*db, "file_mutations") == 1);   // 未决记录保留

        auto again = mutations.recover();                // 连续恢复保持同一结论
        CHECK(again.conflicts == 1);
        CHECK(again.finalized == 0);
        CHECK(count_rows(*db, "file_mutations") == 1);
    }

    SUBCASE("recorded delete must not be resurrected by a later recovery")
    {
        auto target = temp.path() / "symbols" / "deleted_then_rebuilt.kicad_sym";
        auto backup = fs::path(target.string() + ".bak");
        write(target, "rebuilt by someone else");
        write(backup, "original");
        insert_mutation_log(*db, "m-fix01-applied-remove", "applied",
                            fix01_plan(target.string(), true, true, "original", ""));

        auto report = mutations.recover();
        CHECK(report.conflicts == 1);                    // 删除后又出现 → 冲突
        CHECK(read(target) == "rebuilt by someone else");  // 外部内容不被动
        CHECK(fs::exists(backup));
        CHECK(count_rows(*db, "file_mutations") == 1);
    }
}

// ------------------------------------------------------------
// FIX01-L01-b：prepared 新建只回收摘要等于 new 的半成品
// 外部重建的内容既不能删也不能覆盖；没有半成品时日志可以正常结清。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 prepared new-file recovery only reclaims its own bytes")
{
    io::SourceMutationService mutations(*db);
    auto target = temp.path() / "symbols" / "new_file.kicad_sym";
    auto backup = fs::path(target.string() + ".bak");

    SUBCASE("externally rebuilt content is a conflict and keeps the whole group")
    {
        write(target, "someone else's bytes");
        write(target.string() + ".tmp", "our staged bytes");
        write(target.string() + ".bak", "evidence backup");
        insert_mutation_log(*db, "m-fix01-new-keep", "prepared",
                            fix01_plan(target.string(), false, false, "", "our staged bytes"));

        auto report = mutations.recover();
        for (const auto& message : report.messages)
            MESSAGE("recover: ", message);
        // 未知内容不能被当成「无事结清」：恢复没做完就必须留下冲突与全部证据
        CHECK(report.conflicts == 1);
        CHECK(report.restored == 0);
        CHECK(read(target) == "someone else's bytes");    // 外部内容一个字节不动
        CHECK(fs::exists(target.string() + ".tmp"));      // temp 证据保留
        CHECK(read(target.string() + ".tmp") == "our staged bytes");
        CHECK(fs::exists(target.string() + ".bak"));      // backup 证据保留
        CHECK(read(target.string() + ".bak") == "evidence backup");
        CHECK(count_rows(*db, "file_mutations") == 1);    // 未决日志保留，人工可查
    }

    SUBCASE("half-written new file is removed")
    {
        write(target, "our staged bytes");
        insert_mutation_log(*db, "m-fix01-new-remove", "prepared",
                            fix01_plan(target.string(), false, false, "", "our staged bytes"));

        auto report = mutations.recover();
        CHECK(report.conflicts == 0);
        CHECK(report.restored == 1);
        CHECK_FALSE(fs::exists(target));
        CHECK_FALSE(fs::exists(backup));
        CHECK(count_rows(*db, "file_mutations") == 0);
    }

    SUBCASE("file was never created — nothing to do")
    {
        insert_mutation_log(*db, "m-fix01-new-missing", "prepared",
                            fix01_plan(target.string(), false, false, "", "our staged bytes"));
        auto report = mutations.recover();
        CHECK(report.conflicts == 0);
        CHECK(count_rows(*db, "file_mutations") == 0);
    }
}

// ------------------------------------------------------------
// FIX01-L01-c：一组变更要么全部预检通过，要么整组保留
// 第二个文件冲突时，第一个文件也不能被清理 —— 备份、临时文件、日志、
// 目标内容全部保持原样。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 a conflicting file keeps its whole mutation group intact")
{
    io::SourceMutationService mutations(*db);
    auto ok_target = temp.path() / "symbols" / "group_ok.kicad_sym";
    auto bad_target = temp.path() / "symbols" / "group_bad.kicad_sym";
    auto ok_backup = fs::path(ok_target.string() + ".bak");
    auto bad_backup = fs::path(bad_target.string() + ".bak");

    // 第一项：新建的半成品（本可回收）；第二项：目标被外部改过（冲突）
    write(ok_target, "staged bytes");
    write(ok_target.string() + ".tmp", "staged bytes");
    write(bad_target, "external edit");
    write(bad_backup, "original");

    nlohmann::json first;
    first["path"] = ok_target.string();
    first["temp"] = ok_target.string() + ".tmp";
    first["backup"] = ok_target.string() + ".bak";
    first["existed"] = false;
    first["remove"] = false;
    first["old_digest"] = "";
    first["new_digest"] = io::content_digest("staged bytes");

    nlohmann::json second;
    second["path"] = bad_target.string();
    second["temp"] = bad_target.string() + ".tmp";
    second["backup"] = bad_target.string() + ".bak";
    second["existed"] = true;
    second["remove"] = true;
    second["old_digest"] = io::content_digest("original");
    second["new_digest"] = "";

    nlohmann::json root;
    root["operation"] = "fix01";
    root["files"] = nlohmann::json::array({first, second});
    insert_mutation_log(*db, "m-fix01-group", "prepared", root.dump());

    auto report = mutations.recover();
    for (const auto& message : report.messages)
        MESSAGE("recover: ", message);
    CHECK(report.conflicts == 1);
    CHECK(report.restored == 0);                      // 冲突前不做任何部分恢复
    CHECK(read(ok_target) == "staged bytes");         // 第一项原样保留
    CHECK(fs::exists(ok_target.string() + ".tmp"));
    CHECK(read(bad_target) == "external edit");
    CHECK(read(bad_backup) == "original");
    CHECK(count_rows(*db, "file_mutations") == 1);
}

// ------------------------------------------------------------
// FIX01-L01-d：目标已经是旧内容时无需改动，且恢复可重复
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 recovery is idempotent for an already intact target")
{
    io::SourceMutationService mutations(*db);
    auto target = temp.path() / "symbols" / "intact.kicad_sym";
    auto backup = fs::path(target.string() + ".bak");
    write(target, "original");   // 替换从未发生
    write(backup, "original");
    insert_mutation_log(*db, "m-fix01-intact", "prepared",
                        fix01_plan(target.string(), true, false, "original", "after"));

    auto report = mutations.recover();
    for (const auto& message : report.messages)
        MESSAGE("recover: ", message);
    CHECK(report.conflicts == 0);
    CHECK(report.restored == 0);                      // 已经等于旧内容 → 无需改动
    CHECK(read(target) == "original");
    CHECK_FALSE(fs::exists(backup));                  // 结清后才清理备份
    CHECK(count_rows(*db, "file_mutations") == 0);

    auto again = mutations.recover();
    CHECK(again.conflicts == 0);
    CHECK(again.restored == 0);
    CHECK(read(target) == "original");
}

// ------------------------------------------------------------
// FIX01-L01-e：计划里字段类型写坏（path 是数字、remove 是字符串）时，
// recover 只能按行报冲突 —— 不能让 nlohmann 的 type_error 逃出去（启动期
// 恢复抛出异常等于启动失败），目标、temp/backup 证据与日志必须原样保留。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 recovery rejects typed-malformed plans without raising")
{
    io::SourceMutationService mutations(*db);
    auto target = temp.path() / "symbols" / "typed_bad.kicad_sym";
    // 计划本身是「本次新建、目标摘要正好等于 new」—— 字段没坏时它会回收掉
    // target 并结清日志；正因为如此，坏字段一旦被强行当成默认值处理，
    // 这个用例就会立刻看到文件被删、日志被清
    write(target, "current bytes");
    write(target.string() + ".tmp", "current bytes");
    write(target.string() + ".bak", "backup bytes");

    auto plan = nlohmann::json::parse(
        fix01_plan(target.string(), false, false, "", "current bytes"));
    SUBCASE("path is a number") { plan["files"][0]["path"] = 123; }
    SUBCASE("remove is a string") { plan["files"][0]["remove"] = "yes"; }
    insert_mutation_log(*db, "m-fix01-typed", "prepared", plan.dump());

    auto report = mutations.recover();   // 坏字段不能把异常抛到恢复流程之外
    for (const auto& message : report.messages)
        MESSAGE("recover: ", message);
    CHECK(report.conflicts == 1);
    CHECK(report.restored == 0);                     // 字段类型不对 → 什么都不做
    CHECK(report.finalized == 0);
    CHECK(fs::exists(target));                       // 目标不动
    CHECK(read(target) == "current bytes");
    CHECK(fs::exists(target.string() + ".tmp"));     // 整组证据保留
    CHECK(read(target.string() + ".tmp") == "current bytes");
    CHECK(fs::exists(target.string() + ".bak"));
    CHECK(read(target.string() + ".bak") == "backup bytes");
    CHECK(count_rows(*db, "file_mutations") == 1);   // 坏计划记录保留
}

// ------------------------------------------------------------
// FIX01-L01-g：existed 是语义关键字段 —— 缺失、null 或与摘要自相矛盾的计划，
// 都只能报冲突，绝不能替日志推断出默认的 false。
// 现场就是一次正常的覆盖写：target 已经是 new、backup 是 old、temp 也在
// （与 apply() 落盘的正常 overwrite 计划完全一致）。一旦把缺字段的 existed
// 读成 false，恢复器会把它当成本次新建的半成品：删掉已经落地的 target，
// 再结清日志、清掉 backup 与 temp —— 这正是恢复最不该做的事。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 recovery refuses a missing, null or contradictory existed flag")
{
    io::SourceMutationService mutations(*db);
    auto target = temp.path() / "symbols" / "existed_bad.kicad_sym";
    auto backup = fs::path(target.string() + ".bak");
    auto temp_file = fs::path(target.string() + ".tmp");

    write(target, "new bytes");        // 替换已经发生：目标就是计划里的结果
    write(temp_file, "new bytes");     // 半成品/证据
    write(backup, "old bytes");        // 可验证的旧内容备份

    auto plan = nlohmann::json::parse(
        fix01_plan(target.string(), true, false, "old bytes", "new bytes"));
    REQUIRE(plan["files"].size() == 1);
    SUBCASE("existed field is absent")
    {
        CHECK(plan["files"][0].erase(std::string("existed")) == 1);
    }
    SUBCASE("existed is null")
    {
        plan["files"][0]["existed"] = nullptr;
    }
    SUBCASE("existed=false contradicts the recorded old_digest")
    {
        plan["files"][0]["existed"] = false;   // old_digest 仍是非空摘要
    }
    SUBCASE("remove=true contradicts the recorded new_digest")
    {
        plan["files"][0]["remove"] = true;     // new_digest 仍是非空摘要
    }
    SUBCASE("remove field is absent")
    {
        CHECK(plan["files"][0].erase(std::string("remove")) == 1);
    }
    insert_mutation_log(*db, "m-fix01-existed", "prepared", plan.dump());

    auto report = mutations.recover();
    for (const auto& message : report.messages)
        MESSAGE("recover: ", message);
    CHECK(report.conflicts == 1);                    // 坏计划只能冲突
    CHECK(report.restored == 0);                     // 不能回收/还原任何文件
    CHECK(report.finalized == 0);
    CHECK(read(target) == "new bytes");              // 目标一个字节不动
    CHECK(read(backup) == "old bytes");              // 备份证据原样保留
    CHECK(read(temp_file) == "new bytes");           // 临时文件证据原样保留
    CHECK(count_rows(*db, "file_mutations") == 1);   // 未决记录保留，人工可查

    auto again = mutations.recover();                // 重复恢复保持同一结论
    CHECK(again.conflicts == 1);
    CHECK(again.restored == 0);
    CHECK(read(target) == "new bytes");
    CHECK(read(backup) == "old bytes");
    CHECK(read(temp_file) == "new bytes");
    CHECK(count_rows(*db, "file_mutations") == 1);
}

// ------------------------------------------------------------
// FIX01-L01-f：变更日志读不完整时，恢复不能拿半截清单动手
// 注入 step 故障（progress_handler 中途打断 SELECT）：报冲突返回，
// 目标、备份与未决日志一个都不许动。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 recovery refuses to act on a partially read mutation log")
{
    io::SourceMutationService mutations(*db);
    auto target = temp.path() / "symbols" / "log_read.kicad_sym";
    auto backup = fs::path(target.string() + ".bak");
    write(target, "original");
    write(backup, "original");
    insert_mutation_log(*db, "m-fix01-log-read", "prepared",
                        fix01_plan(target.string(), true, true, "original", ""));

    static int progress_calls = 0;
    progress_calls = 0;
    {
        auto session = db->session();
        sqlite3_progress_handler(session.handle(), 1,
                                 [](void* data) {
                                     ++*static_cast<int*>(data);
                                     return 1;  // 非零 → 打断当前语句
                                 },
                                 &progress_calls);
    }
    auto report = mutations.recover();
    {
        auto session = db->session();
        sqlite3_progress_handler(session.handle(), 0, nullptr, nullptr);  // 摘掉故障
    }
    CHECK(progress_calls > 0);
    for (const auto& message : report.messages)
        MESSAGE("recover: ", message);
    CHECK(report.conflicts == 1);          // 读不完整 → 冲突，不能当「没事可做」
    CHECK(report.restored == 0);
    CHECK(report.finalized == 0);
    CHECK(read(target) == "original");     // 半截清单不能触发任何删除或还原
    CHECK(fs::exists(backup));
    CHECK(count_rows(*db, "file_mutations") == 1);
}

TEST_CASE_FIXTURE(LibraryFixture, "CI_PORTABILITY import snapshots retain exact Unix seconds")
{
    constexpr int64_t unix_seconds = 1704067200;
    const auto dir = temp.path() / "fixed_mtime";
    const auto target = dir / "FixedTime.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) "
                  "(symbol \"FixedTime\" (property \"Reference\" \"R\") "
                  "(property \"Footprint\" \"Pkg:Old\")))");
    const auto fixed_time = std::chrono::file_clock::from_sys(
        std::chrono::sys_seconds{std::chrono::seconds{unix_seconds}});
    fs::last_write_time(target, fixed_time);

    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        return pipe | services::symbols_from{dir.string(), ""} | services::execute;
    };
    auto snapshot = [&]
    {
        auto session = db->session();
        auto value = storage::ImportedFilesRepository(session).find_mtime(target.string());
        REQUIRE(value.has_value());
        REQUIRE(value->has_value());
        return **value;
    };

    CHECK(run().symbols == 1);
    CHECK(snapshot() == unix_seconds);
    CHECK(run().symbols == 0);
    CHECK(snapshot() == unix_seconds);

    write(target, "(kicad_symbol_lib (version 20231120) "
                  "(symbol \"FixedTime\" (property \"Reference\" \"R\") "
                  "(property \"Footprint\" \"Pkg:New\")))");
    fs::last_write_time(target, fixed_time + std::chrono::seconds(2));
    CHECK(run().symbols == 1);
    CHECK(snapshot() == unix_seconds + 2);
}

// ------------------------------------------------------------
// FIX01-L04-a：整库导入是一个事务 —— 任一条 UPDATE 被拒绝就整体回滚，
// 不留下半更新的行，也不消费 mtime 快照；去掉故障后原文件原 mtime 能补齐。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 a rejected symbol update rolls back and keeps the import snapshot")
{
    auto dir = temp.path() / "sym_fix01_update";
    auto target = dir / "Update.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120)\n"
                  "  (symbol \"AuditUpdate\" (property \"Reference\" \"R\") "
                  "(property \"Footprint\" \"Pkg:Old\"))\n)\n");

    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        return pipe | services::symbols_from{dir.string(), ""} | services::execute;
    };
    auto first = run();
    CHECK(first.symbols == 1);

    std::string symbol_id;
    int64_t imported_mtime = 0;
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_name("AuditUpdate");
        REQUIRE(sym.has_value());
        symbol_id = sym->id();
        CHECK(sym->footprint() == "Pkg:Old");
        auto snapshot = storage::ImportedFilesRepository(session).find_mtime(target.string());
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->has_value());
        imported_mtime = **snapshot;
    }

    // 外部改写源文件（引用换成 Pkg:New）并让 mtime 前进
    auto old_time = fs::last_write_time(target);
    write(target, "(kicad_symbol_lib (version 20231120)\n"
                  "  (symbol \"AuditUpdate\" (property \"Reference\" \"R\") "
                  "(property \"Footprint\" \"Pkg:New\"))\n)\n");
    fs::last_write_time(target, old_time + std::chrono::seconds(2));

    REQUIRE(db->execute("CREATE TRIGGER fix01_reject_update BEFORE UPDATE ON symbols "
                        "WHEN NEW.name = 'AuditUpdate' "
                        "BEGIN SELECT RAISE(ABORT, 'fix01: rejected'); END")
                .has_value());
    auto rejected = run();
    MESSAGE("rejected run: symbols=", rejected.symbols, " skipped=", rejected.skipped);

    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(sym.has_value());
        CHECK(sym->footprint() == "Pkg:Old");   // 半截更新不能留下
        auto snapshot = storage::ImportedFilesRepository(session).find_mtime(target.string());
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->has_value());
        CHECK(**snapshot == imported_mtime);    // 失败的导入不能消费快照
    }
    CHECK(rejected.symbols == 0);               // 失败不计为导入成功

    // 去掉故障后原样重导（源文件与 mtime 都不再改动）必须补齐
    REQUIRE(db->execute("DROP TRIGGER fix01_reject_update").has_value());
    auto retry = run();
    CHECK(retry.symbols == 1);
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(sym.has_value());
        CHECK(sym->footprint() == "Pkg:New");
        auto snapshot = storage::ImportedFilesRepository(session).find_mtime(target.string());
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->has_value());
        CHECK(**snapshot > imported_mtime);
    }
}

// ------------------------------------------------------------
// FIX01-L04-b：快照写入被拒绝时，符号行不能留下半截
// 触发器只拒绝 imported_files 的写入；整库导入必须整体回滚，之后
// 同一份未改动的文件能重新导入成功。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 a rejected snapshot write rolls back the whole library")
{
    auto dir = temp.path() / "sym_fix01_snapshot";
    auto target = dir / "Snapshot.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120)\n"
                  "  (symbol \"AuditSnapA\" (property \"Reference\" \"R\") "
                  "(property \"Footprint\" \"\"))\n"
                  "  (symbol \"AuditSnapB\" (property \"Reference\" \"R\") "
                  "(property \"Footprint\" \"\"))\n)\n");

    // imported_files 是按需建的表：先触发一次建表，触发器才能挂上去
    {
        auto session = db->session();
        REQUIRE(storage::ImportedFilesRepository(session).all().has_value());
    }
    REQUIRE(db->execute("CREATE TRIGGER fix01_reject_snapshot BEFORE INSERT ON imported_files "
                        "WHEN NEW.file_path = '" + target.string() + "' "
                        "BEGIN SELECT RAISE(ABORT, 'fix01: snapshot rejected'); END")
                .has_value());

    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        return pipe | services::symbols_from{dir.string(), ""} | services::execute;
    };
    auto first = run();
    MESSAGE("rejected snapshot run: symbols=", first.symbols, " skipped=", first.skipped);

    // 整个库文件一行都不能落库
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'AuditSnapA'") == 0);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'AuditSnapB'") == 0);
    {
        auto session = db->session();
        auto snapshot = storage::ImportedFilesRepository(session).find_mtime(target.string());
        REQUIRE(snapshot.has_value());
        CHECK_FALSE(snapshot->has_value());   // 没有「已导入」记录挡住重试
    }
    CHECK(first.symbols == 0);

    // 去掉故障：源文件一字未改也能补齐
    REQUIRE(db->execute("DROP TRIGGER fix01_reject_snapshot").has_value());
    auto retry = run();
    CHECK(retry.symbols == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'AuditSnapA'") == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'AuditSnapB'") == 1);
}

// ------------------------------------------------------------
// FIX01-L04-c：COMMIT 失败同样不能留下半截库
// 用 sqlite3_commit_hook 让提交失败（钩子只挂在 fixture 连接上）：
// 符号行与快照都不得生效，摘掉钩子后同一份文件必须能补齐。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 a failed commit leaves no partial library rows")
{
    auto dir = temp.path() / "sym_fix01_commit";
    auto target = dir / "Commit.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120)\n"
                  "  (symbol \"AuditCommitA\" (property \"Reference\" \"R\") "
                  "(property \"Footprint\" \"\"))\n"
                  "  (symbol \"AuditCommitB\" (property \"Reference\" \"R\") "
                  "(property \"Footprint\" \"\"))\n)\n");

    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        return pipe | services::symbols_from{dir.string(), ""} | services::execute;
    };

    {
        auto session = db->session();
        sqlite3_commit_hook(session.handle(), [](void*) { return 1; }, nullptr);
    }
    auto failed = run();
    MESSAGE("commit-failure run: symbols=", failed.symbols, " skipped=", failed.skipped);
    CHECK(failed.symbols == 0);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name LIKE 'AuditCommit%'") == 0);
    {
        auto session = db->session();
        auto snapshot = storage::ImportedFilesRepository(session).find_mtime(target.string());
        REQUIRE(snapshot.has_value());
        CHECK_FALSE(snapshot->has_value());   // 提交失败的文件不能被记成已导入
    }

    {
        auto session = db->session();
        sqlite3_commit_hook(session.handle(), nullptr, nullptr);  // 摘掉故障
    }
    auto retry = run();
    CHECK(retry.symbols == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name LIKE 'AuditCommit%'") == 2);
}

namespace
{
/// 把符号文件的 Footprint 引用改写成 reference，并把 mtime 单调前推两秒。
/// 先读旧 mtime 再写：同一秒里的连续改写如果按「写入时刻 + 2 秒」取戳，
/// 两次改写会得到同一个 mtime，导入快照就会把后一次当成没变过。
void fix01_rewrite_reference(const fs::path& file, const std::string& symbol_name,
                             const std::string& reference)
{
    std::error_code ec;
    auto previous = fs::last_write_time(file, ec);
    write(file, "(kicad_symbol_lib (version 20231120)\n"
                "  (symbol \"" + symbol_name + "\" (property \"Reference\" \"R\") "
                "(property \"Footprint\" \"" + reference + "\"))\n)\n");
    auto stamp = fs::last_write_time(file);
    if (!ec)
    {
        auto advanced = previous + std::chrono::seconds(2);
        if (advanced > stamp)
            stamp = advanced;  // 旧戳 +2 秒，保证每次改写都严格前进
    }
    fs::last_write_time(file, stamp);
}
}  // namespace

// ------------------------------------------------------------
// FIX01-L06-a：新引用为空 / 不存在时不能残留旧关联；重复导入不增长关系
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 reimport clears links that no longer resolve")
{
    auto sym_dir = temp.path() / "sym_fix01_relink";
    auto sym_file = sym_dir / "Relink.kicad_sym";
    auto pretty = temp.path() / "AuditPkg.pretty";
    write(pretty / "A.kicad_mod", "(footprint \"A\" (layer \"F.Cu\"))");

    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        pipe | services::symbols_from{sym_dir.string(), ""} |
            services::footprints_from{pretty.string()};
        return pipe | services::execute;
    };

    write(sym_file, "(kicad_symbol_lib (version 20231120)\n"
                    "  (symbol \"AuditRelink3\" (property \"Reference\" \"R\") "
                    "(property \"Footprint\" \"AuditPkg:A\"))\n)\n");
    run();

    std::string symbol_id;
    std::string fp_a;
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_name("AuditRelink3");
        REQUIRE(sym.has_value());
        symbol_id = sym->id();
        auto a = storage::FootprintRepository(session).find_by_name("A");
        REQUIRE(a.has_value());
        fp_a = a->id();
    }
    auto links = audit_symbol_links(*db, symbol_id);
    REQUIRE(links.size() == 1);
    CHECK(links[0].footprint_id == fp_a);
    CHECK(links[0].link_type == "imported");

    // 同一引用重复导入（文件未变、mtime 未变）不增长关系
    fix01_rewrite_reference(sym_file, "AuditRelink3", "AuditPkg:A");
    run();
    links = audit_symbol_links(*db, symbol_id);
    for (const auto& link : links)
        MESSAGE("link after reimport: ", link.footprint_id, " (", link.link_type, ")");
    REQUIRE(links.size() == 1);
    CHECK(links[0].footprint_id == fp_a);

    // 引用清空 → 旧关联不能残留
    fix01_rewrite_reference(sym_file, "AuditRelink3", "");
    run();
    CHECK(audit_symbol_links(*db, symbol_id).empty());

    // 引用指向不存在的封装 → 不猜、不残留
    fix01_rewrite_reference(sym_file, "AuditRelink3", "AuditPkg:Missing");
    run();
    CHECK(audit_symbol_links(*db, symbol_id).empty());

    // 引用回到 A → 重新唯一解析
    fix01_rewrite_reference(sym_file, "AuditRelink3", "AuditPkg:A");
    run();
    links = audit_symbol_links(*db, symbol_id);
    REQUIRE(links.size() == 1);
    CHECK(links[0].footprint_id == fp_a);

    // 带库前缀但库里没有这个库名：裸名字 A 在库里存在也不能拿来顶替 ——
    // 不建立链接，并且把已经失效的旧关联清掉
    fix01_rewrite_reference(sym_file, "AuditRelink3", "NoSuchLib:A");
    run();
    links = audit_symbol_links(*db, symbol_id);
    for (const auto& link : links)
        MESSAGE("link after unknown-lib reference: ", link.footprint_id, " (", link.link_type,
                ")");
    CHECK(links.empty());
}

// ------------------------------------------------------------
// FIX01-L06-b：两个来源同库同名 → 歧义，不能取最后一个；
// 人工绑定的精确 UUID 不因重导被错绑或删除。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 ambiguous duplicates are never auto-linked and manual links survive")
{
    // 两个不同父目录下的同名 .pretty：库名都是 Dup —— 同名候选必然多于一个
    auto left = temp.path() / "left" / "Dup.pretty";
    auto right = temp.path() / "right" / "Dup.pretty";
    write(left / "Same.kicad_mod", "(footprint \"Same\" (layer \"F.Cu\"))");
    write(right / "Same.kicad_mod", "(footprint \"Same\" (layer \"F.Cu\"))");
    write(left / "Solo.kicad_mod", "(footprint \"Solo\" (layer \"F.Cu\"))");

    auto sym_dir = temp.path() / "sym_fix01_ambig";
    auto sym_file = sym_dir / "Ambig.kicad_sym";
    write(sym_file, "(kicad_symbol_lib (version 20231120)\n"
                    "  (symbol \"AuditAmbig\" (property \"Reference\" \"R\") "
                    "(property \"Footprint\" \"Dup:Same\"))\n)\n");

    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        pipe | services::symbols_from{sym_dir.string(), ""} |
            services::footprints_from{left.string()} |
            services::footprints_from{right.string()};
        return pipe | services::execute;
    };
    run();

    std::string symbol_id;
    std::string same_left;
    std::string same_right;
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_name("AuditAmbig");
        REQUIRE(sym.has_value());
        symbol_id = sym->id();
        storage::FootprintRepository repo(session);
        auto a = repo.find_by_source_key(storage::existing_source_key(left / "Same.kicad_mod"));
        auto b = repo.find_by_source_key(storage::existing_source_key(right / "Same.kicad_mod"));
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        same_left = a->id();
        same_right = b->id();
    }
    REQUIRE(same_left != same_right);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM footprints WHERE name = 'Same'") == 2);
    for (const auto& link : audit_symbol_links(*db, symbol_id))
        MESSAGE("ambiguous auto link: ", link.footprint_id, " (", link.link_type, ")");
    CHECK(audit_symbol_links(*db, symbol_id).empty());  // 歧义不自动关联

    // 人工绑定到右库那一个（精确 UUID）
    {
        auto session = db->session();
        auto linked = storage::RelationshipRepository(session).link_symbol_to_footprint(
            symbol_id, same_right, "explicit", 1.0);
        REQUIRE(linked.has_value());
    }

    // 重导（引用没变，只是文件又变过一次）：人工关系必须原样保留
    fix01_rewrite_reference(sym_file, "AuditAmbig", "Dup:Same");
    run();
    auto links = audit_symbol_links(*db, symbol_id);
    for (const auto& link : links)
        MESSAGE("link after reimport: ", link.footprint_id, " (", link.link_type, ")");
    REQUIRE(links.size() == 1);
    CHECK(links[0].footprint_id == same_right);   // 没有被同名候选顶掉
    CHECK(links[0].link_type == "explicit");

    // 引用改成唯一可解析的另一个封装：失效的人工关系清掉，按新引用重算
    fix01_rewrite_reference(sym_file, "AuditAmbig", "Dup:Solo");
    run();
    std::string solo_id;
    {
        auto session = db->session();
        auto solo = storage::FootprintRepository(session).find_by_name("Solo");
        REQUIRE(solo.has_value());
        solo_id = solo->id();
        // 符号与封装的 UUID 都不能因为重算而变
        CHECK(storage::SymbolRepository(session).find_by_id(symbol_id).has_value());
        CHECK(storage::FootprintRepository(session).find_by_id(same_right).has_value());
    }
    REQUIRE(solo_id != same_right);
    links = audit_symbol_links(*db, symbol_id);
    REQUIRE(links.size() == 1);
    CHECK(links[0].footprint_id == solo_id);
    CHECK(links[0].link_type == "imported");
}

// ------------------------------------------------------------
// FIX01-L06-c：关系写入失败时回滚，旧关联不被破坏
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 a failed relationship write keeps the previous link")
{
    auto sym_dir = temp.path() / "sym_fix01_linkfail";
    auto sym_file = sym_dir / "LinkFail.kicad_sym";
    auto pretty = temp.path() / "AuditPkg.pretty";
    write(pretty / "A.kicad_mod", "(footprint \"A\" (layer \"F.Cu\"))");
    write(pretty / "B.kicad_mod", "(footprint \"B\" (layer \"F.Cu\"))");

    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        pipe | services::symbols_from{sym_dir.string(), ""} |
            services::footprints_from{pretty.string()};
        return pipe | services::execute;
    };

    write(sym_file, "(kicad_symbol_lib (version 20231120)\n"
                    "  (symbol \"AuditLinkFail\" (property \"Reference\" \"R\") "
                    "(property \"Footprint\" \"AuditPkg:A\"))\n)\n");
    run();

    std::string symbol_id;
    std::string fp_a;
    std::string fp_b;
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_name("AuditLinkFail");
        REQUIRE(sym.has_value());
        symbol_id = sym->id();
        auto a = storage::FootprintRepository(session).find_by_name("A");
        auto b = storage::FootprintRepository(session).find_by_name("B");
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        fp_a = a->id();
        fp_b = b->id();
    }
    auto links = audit_symbol_links(*db, symbol_id);
    REQUIRE(links.size() == 1);
    CHECK(links[0].footprint_id == fp_a);

    // 引用改成 B，但 B 的关系写入被触发器拒绝
    fix01_rewrite_reference(sym_file, "AuditLinkFail", "AuditPkg:B");
    REQUIRE(db->execute("CREATE TRIGGER fix01_reject_link BEFORE INSERT ON symbol_footprint_links "
                        "WHEN NEW.footprint_id = '" + fp_b + "' "
                        "BEGIN SELECT RAISE(ABORT, 'fix01: link rejected'); END")
                .has_value());
    run();

    links = audit_symbol_links(*db, symbol_id);
    for (const auto& link : links)
        MESSAGE("link after failed recompute: ", link.footprint_id, " (", link.link_type, ")");
    REQUIRE(links.size() == 1);                  // 删除也被回滚，旧关联原样保留
    CHECK(links[0].footprint_id == fp_a);

    // 失败发生在关系阶段，文件本身已经导入过：库里引用已是 B，关系和它不一致
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(sym.has_value());
        CHECK(sym->footprint() == "AuditPkg:B");
    }

    // 去掉故障后原样重导（源文件与 mtime 都不再改动）：未取消的 execute
    // 必须重新核对关系把这一条补上，不能因为文件快照已消费就永远跳过
    REQUIRE(db->execute("DROP TRIGGER fix01_reject_link").has_value());
    run();
    links = audit_symbol_links(*db, symbol_id);
    REQUIRE(links.size() == 1);
    CHECK(links[0].footprint_id == fp_b);
    CHECK(links[0].link_type == "imported");
}

// ------------------------------------------------------------
// FIX01-L06-d：整表读取关系时 step 中途失败 → 只能是错误，不能是空集合
// progress_handler 在语句执行中途打断（只挂在 fixture 连接上）：
// 「被打断」和「表就读完了」必须区分开。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 relationship query surfaces a step failure as an error")
{
    auto sym = symbol();
    auto fp = footprint("AuditQueryFp");
    {
        auto session = db->session();
        REQUIRE(storage::RelationshipRepository(session)
                    .link_symbol_to_footprint(sym.id(), fp.id(), "explicit", 1.0)
                    .has_value());
    }
    {
        auto session = db->session();
        auto rows = storage::RelationshipRepository(session).find_all_symbol_link_rows();
        REQUIRE(rows.has_value());
        CHECK(rows->size() == 1);   // 前提：正常路径读得全
    }

    static int progress_calls = 0;
    progress_calls = 0;
    {
        auto session = db->session();
        sqlite3_progress_handler(session.handle(), 1,
                                 [](void* data)
                                 {
                                     ++*static_cast<int*>(data);
                                     return 1;  // 非零 → 打断当前语句
                                 },
                                 &progress_calls);
    }
    {
        auto session = db->session();
        auto rows = storage::RelationshipRepository(session).find_all_symbol_link_rows();
        CHECK_FALSE(rows.has_value());   // 中断不能当成读完了
    }
    {
        auto session = db->session();
        sqlite3_progress_handler(session.handle(), 0, nullptr, nullptr);  // 摘掉故障
    }
    CHECK(progress_calls > 0);
    {
        auto session = db->session();
        auto rows = storage::RelationshipRepository(session).find_all_symbol_link_rows();
        REQUIRE(rows.has_value());
        CHECK(rows->size() == 1);
    }
}

// ------------------------------------------------------------
// FIX01-L06-e：关系查询失败必须立刻停止关联重算
// 人工绑定（explicit，精确 UUID）挂在 B，源引用仍是 AuditPkg:A。
// 关系查询被拒绝时若拿空集合继续，就会把人工关系当成不存在而另加一条；
// 正确行为是原地停下：关系一个都不动。摘掉故障后才按源引用重算。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX01 a failed relationship query stops the link phase")
{
    auto sym_dir = temp.path() / "sym_fix01_queryfail";
    auto sym_file = sym_dir / "QueryFail.kicad_sym";
    auto pretty = temp.path() / "AuditPkg.pretty";
    write(pretty / "A.kicad_mod", "(footprint \"A\" (layer \"F.Cu\"))");
    write(pretty / "B.kicad_mod", "(footprint \"B\" (layer \"F.Cu\"))");
    write(sym_file, "(kicad_symbol_lib (version 20231120)\n"
                    "  (symbol \"AuditQueryFail\" (property \"Reference\" \"R\") "
                    "(property \"Footprint\" \"AuditPkg:A\"))\n)\n");

    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        pipe | services::symbols_from{sym_dir.string(), ""} |
            services::footprints_from{pretty.string()};
        return pipe | services::execute;
    };
    run();

    std::string symbol_id;
    std::string fp_a;
    std::string fp_b;
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_name("AuditQueryFail");
        REQUIRE(sym.has_value());
        symbol_id = sym->id();
        auto a = storage::FootprintRepository(session).find_by_name("A");
        auto b = storage::FootprintRepository(session).find_by_name("B");
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        fp_a = a->id();
        fp_b = b->id();
    }
    // 人工把关系绑到 B（精确 UUID）
    {
        auto session = db->session();
        storage::RelationshipRepository rr(session);
        REQUIRE(rr.unlink_all_symbol_footprints(symbol_id).has_value());
        REQUIRE(rr.link_symbol_to_footprint(symbol_id, fp_b, "explicit", 1.0).has_value());
    }

    // 注入故障：读取关系表被拒绝（只挂在 fixture 连接上）
    {
        auto session = db->session();
        sqlite3_set_authorizer(
            session.handle(),
            [](void*, int action, const char* arg1, const char*, const char*, const char*) -> int
            {
                if (action == SQLITE_READ && arg1 &&
                    std::string(arg1) == "symbol_footprint_links")
                    return SQLITE_DENY;
                return SQLITE_OK;
            },
            nullptr);
    }
    auto blocked = run();
    MESSAGE("import with denied relationship query: symbols=", blocked.symbols,
            " footprints=", blocked.footprints);
    {
        auto session = db->session();
        sqlite3_set_authorizer(session.handle(), nullptr, nullptr);  // 摘掉故障
    }

    // 查询失败的这一轮：人工关系原样保留，没有新增、没有被替换
    auto links = audit_symbol_links(*db, symbol_id);
    for (const auto& link : links)
        MESSAGE("link after failed query: ", link.footprint_id, " (", link.link_type, ")");
    REQUIRE(links.size() == 1);
    CHECK(links[0].footprint_id == fp_b);
    CHECK(links[0].link_type == "explicit");

    // 摘掉故障后再核对：引用还是 AuditPkg:A → 失效的人工关系清掉，按引用重算
    run();
    links = audit_symbol_links(*db, symbol_id);
    for (const auto& link : links)
        MESSAGE("link after recovery: ", link.footprint_id, " (", link.link_type, ")");
    REQUIRE(links.size() == 1);
    CHECK(links[0].footprint_id == fp_a);
    CHECK(links[0].link_type == "imported");
}

// ============================================================
// FIX02 —— 真实 KiCad 解析、分类保留与部分列写入
//
// 约束同 LOCAL_AUDIT：只用 fixture 的临时目录与临时数据库，
// 不碰用户元件库、插件缓存或其它工作区；断言的是应有的行为。
// ============================================================
// DOM 直接构造：验证派生类型名的生命周期（源文本切片另论）
#include "core/io/sexpr/dom/dom.h"
#include "core/io/sexpr/dom/dom_builder.h"

namespace
{
/// 在临时库里落一个符号行（symbols.library_id 有外键，库行必须先存在）
std::string fix02_insert_symbol(storage::Database& db, const std::string& library_id,
                                const core::Symbol& part)
{
    core::Symbol row = part;
    row.set_library_id(library_id);
    if (row.Kicad_Forge_ID().empty())
        row.set_Kicad_Forge_ID(core::Symbol::compute_hash(row));
    auto session = db.session();
    auto inserted = storage::SymbolRepository(session).insert(row);
    if (!inserted)
        MESSAGE("symbol insert failed: ", inserted.error().format_message());
    REQUIRE(inserted.has_value());
    return inserted->id();
}

/// 给符号补 n 个引脚（分类规则会看引脚数）
void fix02_add_pins(core::Symbol& part, int count)
{
    for (int i = 1; i <= count; ++i)
    {
        core::PinDefinition pin;
        pin.number = std::to_string(i);
        pin.name = "P" + std::to_string(i);
        part.add_pin(std::move(pin));
    }
}

/// 读回符号当前的 component_type 列
std::string fix02_stored_type(storage::Database& db, const std::string& symbol_id)
{
    auto session = db.session();
    auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
    if (!row)
        MESSAGE("symbol read failed: ", row.error().format_message());
    REQUIRE(row.has_value());
    return row->component_type_id;
}
}  // namespace

// ------------------------------------------------------------
// FIX02-01：标准 KiCad 符号的 8 脚 / 嵌套单元 / extends 都要解析正确
// 引脚形如 (pin <电气类型> line (at ...) (length ...) (name "..." (effects ...))
// (number "..." (effects ...)))：编号取 (number ...)、名字取 (name ...)、
// 电气类型取第一个 atom；Reference 由 property 决定，派生符号继承基符号。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX02 parses standard eight-pin units and extends")
{
    auto dir = temp.path() / "fix02_parse";
    auto path = dir / "DualOpAmp.kicad_sym";
    write(path, R"KICAD((kicad_symbol_lib
  (version 20231120)
  (generator "kicad_symbol_editor")
  (generator_version "8.0")
  (symbol "DualOpAmp"
    (pin_names (offset 0.127))
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (property "Reference" "U"
      (at -5.08 5.08 0)
      (effects (font (size 1.27 1.27)))
    )
    (property "Value" "DualOpAmp"
      (at 5.08 5.08 0)
      (effects (font (size 1.27 1.27)))
    )
    (property "Footprint" "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"
      (at 0 0 0)
      (effects (font (size 1.27 1.27)) (hide yes))
    )
    (property "Datasheet" "https://example.invalid/dual.pdf"
      (at 0 0 0)
      (effects (font (size 1.27 1.27)) (hide yes))
    )
    (symbol "DualOpAmp_1_1"
      (pin input line
        (at -7.62 2.54 0)
        (length 2.54)
        (name "IN1+" (effects (font (size 1.27 1.27))))
        (number "1" (effects (font (size 1.27 1.27))))
      )
      (pin input line
        (at -7.62 -2.54 0)
        (length 2.54)
        (name "IN1-" (effects (font (size 1.27 1.27))))
        (number "2" (effects (font (size 1.27 1.27))))
      )
      (pin output line
        (at 7.62 0 180)
        (length 2.54)
        (name "OUT1" (effects (font (size 1.27 1.27))))
        (number "3" (effects (font (size 1.27 1.27))))
      )
      (pin power_in line
        (at 0 -7.62 90)
        (length 2.54)
        (name "V-" (effects (font (size 1.27 1.27))))
        (number "4" (effects (font (size 1.27 1.27))))
      )
    )
    (symbol "DualOpAmp_2_1"
      (pin input line
        (at -7.62 2.54 0)
        (length 2.54)
        (name "IN2+" (effects (font (size 1.27 1.27))))
        (number "5" (effects (font (size 1.27 1.27))))
      )
      (pin input line
        (at -7.62 -2.54 0)
        (length 2.54)
        (name "IN2-" (effects (font (size 1.27 1.27))))
        (number "6" (effects (font (size 1.27 1.27))))
      )
      (pin output line
        (at 7.62 0 180)
        (length 2.54)
        (name "OUT2" (effects (font (size 1.27 1.27))))
        (number "7" (effects (font (size 1.27 1.27))))
      )
    )
    (symbol "DualOpAmp_3_1"
      (pin power_in line
        (at 0 7.62 270)
        (length 2.54)
        (name "V+" (effects (font (size 1.27 1.27))))
        (number "8" (effects (font (size 1.27 1.27))))
      )
    )
  )
  (symbol "LM358_SOIC"
    (extends "DualOpAmp")
    (property "Value" "LM358_SOIC"
      (at 5.08 5.08 0)
      (effects (font (size 1.27 1.27)))
    )
    (property "Footprint" "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"
      (at 0 0 0)
      (effects (font (size 1.27 1.27)) (hide yes))
    )
    (property "MPN" "LM358DR"
      (at 0 0 0)
      (effects (font (size 1.27 1.27)) (hide yes))
    )
  )
)
)KICAD");

    auto parsed = parser::SymbolLibParser::parse(path);
    if (!parsed)
        MESSAGE("standard symbol parse failed: ", parsed.error().format_message());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 2);

    const core::Symbol& base = parsed->items[0];
    INFO("base: name=", base.name(), " reference_prefix=", base.reference_prefix(),
         " pins=", base.pin_count());
    CHECK(base.name() == "DualOpAmp");
    CHECK(base.reference_prefix() == "U");  // 由 property Reference 决定，不是名字里的 "DualOpAmp"
    CHECK(base.default_value() == "DualOpAmp");
    CHECK(base.footprint() == "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm");
    CHECK(base.datasheet() == "https://example.invalid/dual.pdf");
    CHECK(base.pin_count() == 8);
    REQUIRE(base.pins().size() == 8);

    // 嵌套单元里的引脚按文档顺序收集，编号/名字/电气类型各自来自正确的槽位
    const char* expected_names[] = {"IN1+", "IN1-", "OUT1", "V-",
                                    "IN2+", "IN2-", "OUT2", "V+"};
    const char* expected_types[] = {"input", "input", "output", "power_in",
                                    "input", "input", "output", "power_in"};
    for (std::size_t i = 0; i < base.pins().size(); ++i)
    {
        INFO("pin ", i, ": number=", base.pins()[i].number, " name=", base.pins()[i].name,
             " electrical_type=", base.pins()[i].electrical_type);
        CHECK(base.pins()[i].number == std::to_string(i + 1));   // (number "...")，不是 "input"
        CHECK(base.pins()[i].name == expected_names[i]);         // (name "...")，含 effects 也不丢
        CHECK(base.pins()[i].electrical_type == expected_types[i]);  // (pin <电气类型> ...)
    }

    const core::Symbol& derived = parsed->items[1];
    INFO("derived: name=", derived.name(), " reference_prefix=", derived.reference_prefix(),
         " pins=", derived.pin_count(), " mpn=", derived.mpn());
    CHECK(derived.name() == "LM358_SOIC");
    CHECK(derived.reference_prefix() == "U");   // 自己没写 Reference → 继承基符号，不是型号首字母 "LM"
    CHECK(derived.mpn() == "LM358DR");
    CHECK(derived.footprint() == "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm");
    CHECK(derived.pin_count() == 8);            // extends：8 个引脚全部继承
    REQUIRE(derived.pins().size() == 8);
    CHECK(derived.pins()[0].number == "1");
    CHECK(derived.pins()[0].name == "IN1+");
    CHECK(derived.pins()[0].electrical_type == "input");
    CHECK(derived.pins()[7].number == "8");
}

// ------------------------------------------------------------
// FIX02-02：DOM 派生类型的文本由节点自己持有
// "line:at"、"长键:child" 这类类型名是拼出来的，源文本里并不存在；
// 只存 string_view 的话，拼接用的临时 std::string 一析构就悬垂。
// 堆扰动（分配/释放一批与派生类型长度相近的字符串）后名字必须原样。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX02 derived DOM type text survives heap churn")
{
    // 派生类型 = 「待配对的键 : 子节点类型」。长的那条必须是同级节点里的
    // 一个 atom 键（wrapper_node 里的 derived_key_with_a_long_name），
    // 后面跟一个子节点 (child node) —— 这样 builder 才会真的拼出
    // "derived_key_with_a_long_name:child" 这个源文本里不存在的类型名。
    std::string source_text =
        "(kicad_symbol_lib (version 20231120)\n"
        "  (symbol \"A\"\n"
        "    (pin input line\n"
        "      (at 0 0 0)\n"
        "      (name \"N\" (effects (font (size 1 1))))\n"
        "      (number \"1\" (effects (font (size 1 1)))))\n"
        "    (wrapper_node derived_key_with_a_long_name (child node))\n"
        "))\n";

    sexpr::DomBuilder builder;
    auto parsed = builder.parse_one(source_text);
    REQUIRE(parsed.has_value());
    sexpr::DomNode* root = parsed->get();
    REQUIRE(root != nullptr);

    sexpr::DomNode* symbol = root->find_child("symbol");
    REQUIRE(symbol != nullptr);
    sexpr::DomNode* pin = symbol->find_child("pin");
    REQUIRE(pin != nullptr);
    REQUIRE(pin->children().size() >= 1);
    sexpr::DomNode* short_derived = pin->children()[0].get();
    CHECK(short_derived->type() == "line:at");
    sexpr::DomNode* wrapper = symbol->find_child("wrapper_node");
    REQUIRE(wrapper != nullptr);
    sexpr::DomNode* long_derived = wrapper->find_child("derived_key_with_a_long_name:child");
    if (long_derived == nullptr)
        MESSAGE("derived type lost: ", short_derived->type());
    REQUIRE(long_derived != nullptr);

    {
        std::vector<std::string> churn;
        churn.reserve(256);
        for (int i = 0; i < 256; ++i)
            churn.emplace_back(static_cast<std::size_t>(8 + i % 48), char('a' + i % 26));
        for (auto& text : churn)
            text.assign(text.size(), 'Z');   // 覆盖刚释放的堆块
    }

    CHECK(short_derived->type() == "line:at");
    CHECK(long_derived->type() == "derived_key_with_a_long_name:child");
    // 源文本切片照旧零拷贝，解析结果不受影响
    CHECK(pin->is_atom());
    CHECK(pin->atom_value() == "input");
    sexpr::DomNode* pin_name = pin->find_child("name");
    REQUIRE(pin_name != nullptr);
    CHECK(pin_name->is_atom());
    CHECK(pin_name->atom_value() == "N");
}

// ------------------------------------------------------------
// FIX02-03：已定类型一律保留 —— 内置、custom:、未注册的历史原始 ID
// 分类只给「还没定类型」的符号补类型；把 custom/未注册 ID 先转成枚举
// Unknown 再按名字猜，等于用错判覆盖人工结果。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX02 classification keeps builtin custom and unregistered type ids")
{
    auto root = temp.path() / "fix02_keep";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "Keep.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, root / "footprints");

    auto insert_typed = [&](const std::string& name, const std::string& type_id)
    {
        core::Symbol part;
        part.set_name(name);
        part.set_reference_prefix("U");   // 通用 IC：光看 Reference 推不出具体类型
        part.component_type_id = type_id;
        fix02_add_pins(part, 8);
        return fix02_insert_symbol(*db, library_id, part);
    };

    const std::string builtin = insert_typed("Fix02Builtin", "OpAmp");
    const std::string custom = insert_typed("Fix02Custom", "custom:local-only");
    const std::string unregistered = insert_typed("Fix02Raw", "LegacyRawId");

    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());
    REQUIRE(summary.has_value());
    MESSAGE("classification: total=", summary->total, " matched=", summary->matched,
            " type_updated=", summary->type_updated);
    CHECK(summary->type_updated == 0);   // 三条都已定类型 → 一条都不写

    CHECK(fix02_stored_type(*db, builtin) == "OpAmp");
    CHECK(fix02_stored_type(*db, custom) == "custom:local-only");
    CHECK(fix02_stored_type(*db, unregistered) == "LegacyRawId");

    // 单符号分类的结果也要反映已有类型身份，custom 原值不能被抹成 Unknown
    auto single = service.classify_symbol(custom);
    if (!single)
        MESSAGE("classify_symbol failed: ", single.error().format_message());
    REQUIRE(single.has_value());
    REQUIRE(single->size() >= 1);
    for (const auto& result : *single)
        MESSAGE("single result: type_id=", result.type_id, " type=", result.type_name);
    CHECK(single->front().type_id == "custom:local-only");
}

// ------------------------------------------------------------
// FIX02-04：按真实 Reference 推断 —— R 能推断，U 只是通用 IC
// LM358 + Reference U 既不能被型号首字母判成电感，
// 也不能被当成 Microcontroller；推不出就保持未分类。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX02 classification infers from the real reference and never from the model name")
{
    auto root = temp.path() / "fix02_infer";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "Infer.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, root / "footprints");

    core::Symbol resistor;
    resistor.set_name("Fix02Resistor");
    // 本用例的意图是「真实位号 R」：前缀列只是缓存，分类依据是 Reference 属性
    resistor.set_reference_prefix("R");
    resistor.set_property("reference", "R");
    fix02_add_pins(resistor, 2);
    const std::string resistor_id = fix02_insert_symbol(*db, library_id, resistor);

    core::Symbol chip;
    chip.set_name("LM358");
    chip.set_reference_prefix("U");
    chip.set_property("reference", "U");
    fix02_add_pins(chip, 8);
    const std::string chip_id = fix02_insert_symbol(*db, library_id, chip);

    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());
    REQUIRE(summary.has_value());
    MESSAGE("classification: total=", summary->total, " matched=", summary->matched,
            " type_updated=", summary->type_updated);
    CHECK(summary->type_updated == 1);   // 只有 R 那一条真的补了类型

    CHECK(fix02_stored_type(*db, resistor_id) == "Resistor");   // 真实 Reference R
    const std::string chip_type = fix02_stored_type(*db, chip_id);
    INFO("LM358 with Reference U got type '", chip_type, "'");
    CHECK(chip_type != "Inductor");         // 型号首字母 L 不是分类依据
    CHECK(chip_type != "Microcontroller");  // U 只是通用 IC
    CHECK(chip_type.empty());               // 推不出就不写，保持未分类待人工确认

    auto single = service.classify_symbol(resistor_id);
    REQUIRE(single.has_value());
    REQUIRE(single->size() >= 1);
    CHECK(single->front().type_id == "Resistor");
}

// ------------------------------------------------------------
// FIX02-04b：Reference 是唯一依据 —— 不许退回型号名/名字前缀
// U 是通用 IC，名字（R123 / LEDdriver / CD4011）都不是位号；
// Reference 自己写成型号名（LEDdriver）也不算位号。
// 落库的原始值只有空串和显式 "Unknown" 算「待分类」，其余一律保留。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX02 classification never falls back to the model name")
{
    auto root = temp.path() / "fix02_nofallback";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "NoFallback.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, root / "footprints");

    auto insert_symbol = [&](const std::string& name, const std::string& reference,
                             const std::string& raw_type, int pins)
    {
        core::Symbol part;
        part.set_name(name);
        part.set_reference_prefix(reference);
        // 本用例的 reference 参数就是「真实写入文件的 Reference 原文」：
        // 前缀列只是它的缓存，分类只能读属性
        part.set_property("reference", reference);
        part.component_type_id = raw_type;
        fix02_add_pins(part, pins);
        return fix02_insert_symbol(*db, library_id, part);
    };

    // Reference U + 型号名：一个都不能被猜成 R / LED / 单片机
    const std::string empty_r123 = insert_symbol("R123", "U", "", 2);
    const std::string empty_leddriver = insert_symbol("LEDdriver", "U", "", 8);
    const std::string unknown_r123 = insert_symbol("R123Unknown", "U", "Unknown", 2);
    const std::string unknown_leddriver = insert_symbol("LEDdriverUnknown", "U", "Unknown", 8);
    const std::string unknown_cd4011 = insert_symbol("CD4011", "U", "Unknown", 14);
    // Reference 整个文本不是位号（多一位都不是）：不猜
    const std::string leddriver_ref = insert_symbol("LEDdriverRef", "LEDdriver", "", 2);
    // 真位号才推断：空串与显式 "Unknown" 一样都是待分类
    const std::string resistor = insert_symbol("Fix02RealR", "R", "Unknown", 2);
    const std::string led = insert_symbol("Fix02RealLED", "LED", "", 2);
    // 已有非空类型（custom / 未注册原始 ID）一律保留 —— 哪怕 Reference 认得出，
    // 也不许拿推断值覆盖人工结果
    const std::string custom_kept = insert_symbol("Fix02CustomR", "R", "custom:local-only", 2);
    const std::string raw_kept = insert_symbol("Fix02RawR", "R", "LegacyRawId", 2);

    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());
    REQUIRE(summary.has_value());
    MESSAGE("classification: total=", summary->total, " matched=", summary->matched,
            " type_updated=", summary->type_updated);
    CHECK(summary->type_updated == 2);   // 只有 Reference R / LED 两条真的补了类型

    // 型号名一个都没被当成依据：原始值原样（空串还是空串，"Unknown" 还是 "Unknown"）
    CHECK(fix02_stored_type(*db, empty_r123).empty());
    CHECK(fix02_stored_type(*db, empty_leddriver).empty());
    CHECK(fix02_stored_type(*db, leddriver_ref).empty());
    CHECK(fix02_stored_type(*db, unknown_r123) == "Unknown");
    CHECK(fix02_stored_type(*db, unknown_leddriver) == "Unknown");
    CHECK(fix02_stored_type(*db, unknown_cd4011) == "Unknown");
    for (const auto& id : {empty_r123, empty_leddriver, unknown_r123, unknown_leddriver,
                           unknown_cd4011, leddriver_ref})
    {
        const std::string type = fix02_stored_type(*db, id);
        INFO("symbol ", id, " got type '", type, "'");
        CHECK(type != "Microcontroller");
        CHECK(type != "Inductor");
        CHECK(type != "Resistor");
        CHECK(type != "LED");
    }

    // 真位号：显式 "Unknown" 与空串一样按 Reference 补类型
    CHECK(fix02_stored_type(*db, resistor) == "Resistor");
    CHECK(fix02_stored_type(*db, led) == "LED");
    // 已定类型不动：Reference 是 R 也不许把 custom / 未注册 ID 改成 Resistor
    CHECK(fix02_stored_type(*db, custom_kept) == "custom:local-only");
    CHECK(fix02_stored_type(*db, raw_kept) == "LegacyRawId");

    // 单符号结果与批量分类走同一套判据
    auto single_cd = service.classify_symbol(unknown_cd4011);
    if (!single_cd)
        MESSAGE("classify_symbol failed: ", single_cd.error().format_message());
    REQUIRE(single_cd.has_value());
    REQUIRE(single_cd->size() >= 1);
    for (const auto& result : *single_cd)
    {
        MESSAGE("CD4011 result: type_id=", result.type_id, " type=", result.type_name);
        CHECK(result.type_id != "Microcontroller");
        CHECK(result.type_id != "Inductor");
    }
    auto single_r = service.classify_symbol(resistor);
    REQUIRE(single_r.has_value());
    REQUIRE(single_r->size() >= 1);
    CHECK(single_r->front().type_id == "Resistor");   // 显式 "Unknown" 也能补上
}

// ------------------------------------------------------------
// FIX02-04c：初次导入与分类服务用同一个推断助手
// 新行的类型在导入那一刻就按真实 Reference 落定；LM358/U 这种落 "Unknown"
// （待分类占位），随后 classify_all 也不会把它猜成别的。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX02 initial import and classification share the same inference")
{
    write(file, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "ImportedR" (property "Reference" "R") (property "Value" "10k"))
  (symbol "LM358" (property "Reference" "U") (property "Value" "LM358"))
  (symbol "LEDdriver" (property "Reference" "LEDdriver") (property "Value" "driver"))))KICAD");

    auto imported = import();
    MESSAGE("import: symbols=", imported.symbols, " skipped=", imported.skipped);

    auto type_of = [&](const std::string& name) -> std::string
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_name(name);
        if (!row)
        {
            MESSAGE("symbol ", name, " missing: ", row.error().format_message());
            return "<missing>";
        }
        return row->component_type_id;
    };
    CHECK(type_of("ImportedR") == "Resistor");   // 真实 Reference R
    CHECK(type_of("LM358") == "Unknown");        // U 只是通用 IC，型号名不作数
    CHECK(type_of("LEDdriver") == "Unknown");    // "LEDdriver" 不是位号

    // 分类服务对同一批数据不会改判：已定的 Resistor 保留，Unknown 也推不出新类型
    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());
    REQUIRE(summary.has_value());
    CHECK(summary->type_updated == 0);
    CHECK(type_of("ImportedR") == "Resistor");
    CHECK(type_of("LM358") == "Unknown");
    CHECK(type_of("LEDdriver") == "Unknown");
}

// ------------------------------------------------------------
// FIX02-05：分类写回只动 component_type 一列，且带原值比较（CAS）
// 快照之后行被人改过：类型不许覆盖，footprint / MPN / ForgeID
// 也不许被旧快照整行写回冲掉。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX02 classification writes one column and never clobbers a concurrent change")
{
    auto root = temp.path() / "fix02_cas";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "Cas.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, root / "footprints");

    core::Symbol part;
    part.set_name("Fix02Cas");
    part.set_reference_prefix("R");
    part.set_property("reference", "R");   // 真实位号：分类的唯一依据
    part.set_footprint("Pkg:Old");
    part.set_mpn("MPN-OLD");
    part.set_Kicad_Forge_ID("KF-OLD");
    fix02_add_pins(part, 2);
    const std::string symbol_id = fix02_insert_symbol(*db, library_id, part);

    // 正常路径：补上类型，其余列一个都不动
    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());
    REQUIRE(summary.has_value());
    CHECK(summary->type_updated == 1);
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        INFO("type=", row->component_type_id, " footprint=", row->footprint(),
             " mpn=", row->mpn(), " kf_id=", row->Kicad_Forge_ID());
        CHECK(row->component_type_id == "Resistor");
        CHECK(row->footprint() == "Pkg:Old");
        CHECK(row->mpn() == "MPN-OLD");
        CHECK(row->Kicad_Forge_ID() == "KF-OLD");
    }

    // 并发场景：分类时读到的原值是 Resistor，之后行被改成 Inductor 且元数据全换新
    REQUIRE(db->execute("UPDATE symbols SET component_type = 'Inductor', "
                        "footprint_ref = 'Pkg:New', mpn = 'MPN-NEW', "
                        "Kicad_Forge_ID = 'KF-NEW' WHERE id = '" + symbol_id + "'")
                .has_value());
    {
        auto session = db->session();
        auto changed = storage::SymbolRepository(session).update_component_type_if_current(
            symbol_id, "Resistor", "Capacitor");            // 拿旧快照的原值去写
        if (!changed)
            MESSAGE("CAS failed: ", changed.error().format_message());
        REQUIRE(changed.has_value());
        CHECK_FALSE(*changed);                              // 原值对不上 → 0 行
    }
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        CHECK(row->component_type_id == "Inductor");        // 并发改的分类结果保留
        CHECK(row->footprint() == "Pkg:New");               // 旧快照的 footprint 没有写回
        CHECK(row->mpn() == "MPN-NEW");
        CHECK(row->Kicad_Forge_ID() == "KF-NEW");
    }

    // 目标值与原值相同不算变更
    {
        auto session = db->session();
        auto changed = storage::SymbolRepository(session).update_component_type_if_current(
            symbol_id, "Inductor", "Inductor");
        REQUIRE(changed.has_value());
        CHECK_FALSE(*changed);
    }

    // 原值确实没变时：只改这一列
    {
        auto session = db->session();
        auto changed = storage::SymbolRepository(session).update_component_type_if_current(
            symbol_id, "Inductor", "Capacitor");
        REQUIRE(changed.has_value());
        CHECK(*changed);
    }
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        CHECK(row->component_type_id == "Capacitor");
        CHECK(row->footprint() == "Pkg:New");
        CHECK(row->mpn() == "MPN-NEW");
        CHECK(row->Kicad_Forge_ID() == "KF-NEW");
    }

    // 行已删除：0 行，不是错误，也不算 updated
    REQUIRE(storage::SymbolRepository(db->session()).remove(symbol_id).has_value());
    {
        auto session = db->session();
        auto changed = storage::SymbolRepository(session).update_component_type_if_current(
            symbol_id, "Capacitor", "Resistor");
        REQUIRE(changed.has_value());
        CHECK_FALSE(*changed);
    }
}

// ------------------------------------------------------------
// FIX02-06：分类批次里任一步失败 → 报错并回滚当前批，不假成功
// 第二条 UPDATE 被触发器拒绝：同一批里先写成功的那条也必须回滚；
// 去掉故障后同一份数据能补齐。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX02 a rejected classification update rolls the batch back")
{
    auto root = temp.path() / "fix02_reject";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "Reject.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, root / "footprints");

    auto insert_reference = [&](const std::string& name)
    {
        core::Symbol part;
        part.set_name(name);
        part.set_reference_prefix("R");
        part.set_property("reference", "R");   // 真实位号：都推断得出 Resistor
        fix02_add_pins(part, 2);
        return fix02_insert_symbol(*db, library_id, part);
    };
    // 按名字排序时 A 在前、Z 在后：先写成功的那条必须一起回滚
    insert_reference("Fix02AOk");
    insert_reference("Fix02ZBlocked");

    REQUIRE(db->execute("CREATE TRIGGER fix02_reject_update BEFORE UPDATE ON symbols "
                        "WHEN NEW.name = 'Fix02ZBlocked' "
                        "BEGIN SELECT RAISE(ABORT, 'fix02: rejected'); END")
                .has_value());

    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (summary)
        MESSAGE("unexpected success: type_updated=", summary->type_updated);
    else
        MESSAGE("expected failure: ", summary.error().format_message());
    CHECK_FALSE(summary.has_value());   // 有一步失败就必须报错
    if (!summary)
    {
        const std::string message = summary.error().format_message();
        CHECK(message.find("classification stopped after 0 committed type updates") !=
              std::string::npos);   // 已提交数量写在错误里
    }
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE component_type = 'Resistor'") ==
          0);   // 批内先成功的那条也被回滚

    // 去掉故障：同样的数据能补齐，没有半截状态挡路
    REQUIRE(db->execute("DROP TRIGGER fix02_reject_update").has_value());
    auto retry = service.classify_all();
    if (!retry)
        MESSAGE("retry failed: ", retry.error().format_message());
    REQUIRE(retry.has_value());
    CHECK(retry->type_updated == 2);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE component_type = 'Resistor'") ==
          2);
}

// ------------------------------------------------------------
// FIX02-07：错误信息要说清此前已提交多少条
// 500 行是每批的事务边界：第 501 行失败时前 500 行已经落库，
// 「整批报错」不能把这些已提交的变更说成没发生，也不能把失败的当成功。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX02 classification reports the already committed updates on failure")
{
    auto root = temp.path() / "fix02_partial";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "Partial.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, root / "footprints");

    // 501 行按名字排序：前 500 行进第一批，最后一条单独一批。
    // symbols.library_id 有外键：每行都要挂到真实登记过的库上，
    // 不能指望 repo 兜底建的那个 default 库把空 library_id 补上。
    {
        auto session = db->session();
        REQUIRE(session.begin().has_value());
        storage::SymbolRepository repo(session);
        for (int i = 0; i < 500; ++i)
        {
            core::Symbol part;
            std::string index = std::to_string(i);
            while (index.size() < 3)
                index.insert(index.begin(), '0');
            part.set_name("Fix02Batch" + index);
            part.set_reference_prefix("R");
            part.set_property("reference", "R");   // 真实位号（前缀列只是缓存）
            part.set_library_id(library_id);
            auto inserted = repo.insert(part);
            if (!inserted)
                MESSAGE("batch insert failed at row ", i, ": ",
                        inserted.error().format_message());
            REQUIRE(inserted.has_value());
        }
        REQUIRE(session.commit().has_value());
    }
    {
        core::Symbol blocked;
        blocked.set_name("Fix02ZZFail");
        blocked.set_reference_prefix("R");
        blocked.set_property("reference", "R");
        fix02_insert_symbol(*db, library_id, blocked);
    }

    REQUIRE(db->execute("CREATE TRIGGER fix02_reject_last BEFORE UPDATE ON symbols "
                        "WHEN NEW.name = 'Fix02ZZFail' "
                        "BEGIN SELECT RAISE(ABORT, 'fix02: rejected'); END")
                .has_value());

    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (summary)
        MESSAGE("unexpected success: type_updated=", summary->type_updated);
    else
        MESSAGE("expected failure: ", summary.error().format_message());
    CHECK_FALSE(summary.has_value());
    if (!summary)
    {
        const std::string message = summary.error().format_message();
        CHECK(message.find("classification stopped after 500 committed type updates") !=
              std::string::npos);
    }
    // 第一批确实提交了：500 行已改，失败的那条没改
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE component_type = 'Resistor'") ==
          500);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'Fix02ZZFail' "
                                "AND COALESCE(component_type, '') = ''") == 1);

    REQUIRE(db->execute("DROP TRIGGER fix02_reject_last").has_value());
    auto retry = service.classify_all();
    REQUIRE(retry.has_value());
    CHECK(retry->type_updated == 1);   // 只剩没写成功的那一条
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE component_type = 'Resistor'") ==
          501);
}

// ------------------------------------------------------------
// FIX02-08：读不到符号只能是错误，不能当「没有可分类的符号」
// progress_handler 在 SELECT 执行中途打断（只挂在 fixture 连接上）：
// 被中断和读完了必须区分开。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX02 classification surfaces a failed read instead of an empty summary")
{
    static int progress_calls = 0;
    progress_calls = 0;
    {
        auto session = db->session();
        sqlite3_progress_handler(session.handle(), 1,
                                 [](void* data)
                                 {
                                     ++*static_cast<int*>(data);
                                     return 1;  // 非零 → 打断当前语句
                                 },
                                 &progress_calls);
    }

    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    {
        auto session = db->session();
        sqlite3_progress_handler(session.handle(), 0, nullptr, nullptr);  // 摘掉故障
    }
    if (summary)
        MESSAGE("unexpected success: total=", summary->total);
    else
        MESSAGE("expected failure: ", summary.error().format_message());
    CHECK(progress_calls > 0);
    CHECK_FALSE(summary.has_value());   // 读失败 ≠ 空摘要

    // 摘掉故障后同一个服务能正常返回
    auto recovered = service.classify_all();
    REQUIRE(recovered.has_value());
}

// ============================================================
// FIX03 —— 暂存导入的真实元数据 / 复用选择 / 宿主路径与 UUID
// 约束同 LOCAL_AUDIT：只用 fixture 的临时目录与临时数据库，不联网。
// ============================================================
namespace
{

/// FIX03 桩：3 pin 的符号 + 3 pad 的封装，属性值由模板参数给出。
/// 文件名走插件约定（固定编号），封装名带 ".." 字串用来验证宿主
/// 不会用「字符串包含 ..」把合法名字误杀。
const char* FIX03_META_TEMPLATE = R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
fx = '(effects (font (size 1.27 1.27)))'
symbol = ('(symbol "FIX03_PART"\n'
          '  (property "Reference" "U" (at 0 5.08 0) ' + fx + ')\n'
          '  (property "Value" "@VALUE@" (at 0 2.54 0) ' + fx + ')\n'
          '  (property "Footprint" "old:fp" (at 0 0 0) ' + fx + ')\n'
          '  (property "Datasheet" "https://example.invalid/d.pdf" (at 0 0 0) ' + fx + ')\n'
          '  (property "Description" "Fixture @VALUE@ part" (at 0 0 0) ' + fx + ')\n'
          '  (property "MPN" "@MPN@" (at 0 0 0) ' + fx + ')\n'
          '  (symbol "FIX03_PART_1_1"\n'
          '    (pin passive line (at -5.08 0 0) (length 2.54) (name "A" ' + fx + ') (number "1" ' + fx + '))\n'
          '    (pin passive line (at 0 0 0) (length 2.54) (name "B" ' + fx + ') (number "2" ' + fx + '))\n'
          '    (pin passive line (at 5.08 0 0) (length 2.54) (name "C" ' + fx + ') (number "3" ' + fx + '))))')
with open(os.path.join(staging, "symbol-0.kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator \"kicad_symbol_editor\")\n"
            + symbol + "\n)\n")
with open(os.path.join(staging, "fp..v1.kicad_mod"), "w", encoding="utf-8") as f:
    f.write('(footprint "FIX03_FP" (layer "F.Cu")\n'
            '  (pad "1" smd rect (at 0 0) (size 1 1))\n'
            '  (pad "2" smd rect (at 1 0) (size 1 1))\n'
            '  (pad "3" smd rect (at 2 0) (size 1 1)))\n')
manifest = {"protocol": "staged-import-v1", "lcsc_id": args.get("lcsc_id", ""),
            "symbols": [{"name": "FIX03_PART", "file": "symbol-0.kicad_sym"}],
            "footprint": {"name": "FIX03_FP", "file": "fp..v1.kicad_mod"}}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY";

std::string fix03_meta_plugin(const std::string& value, const std::string& mpn)
{
    std::string py = FIX03_META_TEMPLATE;
    const std::pair<std::string, std::string> replacements[2] = {{"@VALUE@", value}, {"@MPN@", mpn}};
    for (const auto& [marker, replacement] : replacements)
    {
        for (auto at = py.find(marker); at != std::string::npos; at = py.find(marker, at))
            py.replace(at, marker.size(), replacement);
    }
    return py;
}

/// FIX03 桩：额外的来源属性块与 1 号引脚定义都由参数决定（按 KF 桩的写法直接
/// 嵌 Python 表达式）。用来验证 V1 → V2 之间「删掉的源属性必须跟着消失」
/// 和「改过的引脚必须逐字段落库」，而不是只剩一个计数。
const char* FIX03_META_EXTRA_TEMPLATE = R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
fx = '(effects (font (size 1.27 1.27)))'
symbol = ('(symbol "FIX03_PART"\n'
          '  (property "Reference" "U" (at 0 5.08 0) ' + fx + ')\n'
          '  (property "Value" "@VALUE@" (at 0 2.54 0) ' + fx + ')\n'
          '  (property "Footprint" "old:fp" (at 0 0 0) ' + fx + ')\n'
          '  (property "Datasheet" "https://example.invalid/d.pdf" (at 0 0 0) ' + fx + ')\n'
          '  (property "Description" "Fixture @VALUE@ part" (at 0 0 0) ' + fx + ')\n'
          '  (property "MPN" "@MPN@" (at 0 0 0) ' + fx + ')\n'
          + @EXTRA_LINES@
          '  (symbol "FIX03_PART_1_1"\n'
          + @PIN1_LINE@
          '    (pin passive line (at 0 0 0) (length 2.54) (name "B" ' + fx + ') (number "2" ' + fx + '))\n'
          '    (pin passive line (at 5.08 0 0) (length 2.54) (name "C" ' + fx + ') (number "3" ' + fx + '))))')
with open(os.path.join(staging, "symbol-0.kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator \"kicad_symbol_editor\")\n"
            + symbol + "\n)\n")
with open(os.path.join(staging, "fp-extra.kicad_mod"), "w", encoding="utf-8") as f:
    f.write('(footprint "FIX03_FP" (layer "F.Cu")\n'
            '  (pad "1" smd rect (at 0 0) (size 1 1))\n'
            '  (pad "2" smd rect (at 1 0) (size 1 1))\n'
            '  (pad "3" smd rect (at 2 0) (size 1 1)))\n')
manifest = {"protocol": "staged-import-v1", "lcsc_id": args.get("lcsc_id", ""),
            "symbols": [{"name": "FIX03_PART", "file": "symbol-0.kicad_sym"}],
            "footprint": {"name": "FIX03_FP", "file": "fp-extra.kicad_mod"}}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY";

std::string fix03_meta_extra_plugin(const std::string& value, const std::string& mpn,
                                    const std::string& extra_lines, const std::string& pin_one)
{
    std::string py = FIX03_META_EXTRA_TEMPLATE;
    const std::pair<std::string, std::string> replacements[4] = {
        {"@VALUE@", value}, {"@MPN@", mpn}, {"@EXTRA_LINES@", extra_lines}, {"@PIN1_LINE@", pin_one}};
    for (const auto& [marker, replacement] : replacements)
    {
        for (auto at = py.find(marker); at != std::string::npos; at = py.find(marker, at))
            py.replace(at, marker.size(), replacement);
    }
    return py;
}

/// V1 的来源属性：vendor_tag / Manufacturer / ki_keywords。
/// 解析器把 Manufacturer 归一成 manufacturer、ki_keywords 归一成 keywords。
const char* FIX03_EXTRA_V1 = R"PY('  (property "vendor_tag" "V1-TAG" (at 0 0 0) ' + fx + ')\n'
          + '  (property "Manufacturer" "V1-MFR" (at 0 0 0) ' + fx + ')\n'
          + '  (property "ki_keywords" "v1 keywords" (at 0 0 0) ' + fx + ')\n')PY";

/// V2 把这三个来源属性整个删掉
const char* FIX03_EXTRA_V2 = "''";

const char* FIX03_PIN1_V1 =
    R"PY('    (pin passive line (at -5.08 0 0) (length 2.54) (name "A" ' + fx + ') (number "1" ' + fx + '))\n')PY";

/// V2 改的是同一个引脚：number/name/electrical_type 与位置 (at ...) 全都不一样
const char* FIX03_PIN1_V2 =
    R"PY('    (pin input line (at -7.62 2.54 0) (length 2.54) (name "AAA" ' + fx + ') (number "11" ' + fx + '))\n')PY";

/// 已登记符号库行上的真实组件库 owner。
/// audit_register_user_library 返回的是**符号库 UUID**，不是组件库 id ——
/// 授权判定与文件变更要的是后者，拿符号库 UUID 当 owner 必然对不上。
std::string audit_component_owner(storage::Database& db, const std::string& library_id)
{
    auto session = db.session();
    auto libs = storage::LibraryRepository(session).find_all();
    REQUIRE(libs.has_value());
    std::string owner;
    for (const auto& lib : *libs)
        if (lib.id == library_id)
            owner = lib.component_library_id;
    if (owner.empty())
        MESSAGE("no component library owner registered for library ", library_id);
    REQUIRE_FALSE(owner.empty());
    return owner;
}

/// 建一个组件库分组（root 由调用方决定），返回组件库 UUID
std::string audit_make_group(storage::Database& db, const std::string& name,
                             const fs::path& symbol_root, const fs::path& footprint_root,
                             bool locked = false)
{
    auto session = db.session();
    storage::ComponentLibraryRepository::ComponentLibrary group;
    group.name = name;
    group.symbol_path = symbol_root.string();
    group.footprint_path = footprint_root.string();
    group.locked = locked;
    auto inserted = storage::ComponentLibraryRepository(session).insert(group);
    REQUIRE(inserted.has_value());
    return inserted->id;
}

/// 把一个符号库文件登记到 owner 上，并放一个同名符号行；返回符号 UUID
std::string audit_register_symbol_in(storage::Database& db, const fs::path& file,
                                     const std::string& owner, const std::string& symbol_name)
{
    auto session = db.session();
    core::LibraryMeta meta;
    meta.name = file.stem().string();
    meta.file_path = file;
    meta.component_library_id = owner;
    auto library = storage::LibraryRepository(session).insert(meta);
    REQUIRE(library.has_value());
    core::Symbol sym;
    sym.set_name(symbol_name);
    sym.set_library_id(library->id);
    auto inserted = storage::SymbolRepository(session).insert(sym);
    REQUIRE(inserted.has_value());
    return inserted->id();  // core::Symbol::id() 是成员函数，不是字段
}

std::string audit_symbol_file_text(const std::string& name)
{
    return "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n  (symbol \"" + name +
           "\" (property \"Reference\" \"U\"))\n)\n";
}

/// 同一个扫描目录里放多个桩插件：id 必须各不相同（PluginManager 按 id 索引）
std::string audit_stub_manifest(const std::string& id)
{
    return "{\"id\":\"" + id + "\",\"name\":\"Stub " + id +
           "\",\"version\":\"1.0.0\",\"actions\":[{\"id\":\"import\",\"name\":\"I\","
           "\"protocol\":\"staged-import-v1\"}]}";
}

/// FIX03 桩：清单里的封装名、文件根节点名、是否带 Footprint 属性都由参数决定。
/// 用来验证「清单名必须等于文件根节点名」，以及缺属性的符号会被宿主安全补上。
const char* FIX03_STRICT_PLUGIN_PY = R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
fx = '(effects (font (size 1.27 1.27)))'
symbol = ('(symbol "FIX03_STRICT"\n'
          '  (property "Reference" "U" (at 0 5.08 0) ' + fx + ')\n'
          '  (property "Value" "STRICT" (at 0 2.54 0) ' + fx + ')\n'
          + @FP_PROPERTY@
          '  (symbol "FIX03_STRICT_1_1"\n'
          '    (pin passive line (at 0 0 0) (length 2.54) (name "A" ' + fx + ') (number "1" ' + fx + '))))')
with open(os.path.join(staging, "symbol-0.kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator \"kicad_symbol_editor\")\n" + symbol + "\n)\n")
with open(os.path.join(staging, "footprint-0.kicad_mod"), "w", encoding="utf-8") as f:
    f.write('(footprint "@FP_ROOT@" (layer "F.Cu")\n  (pad "1" smd rect (at 0 0) (size 1 1)))\n')
manifest = {"protocol": "staged-import-v1", "lcsc_id": args.get("lcsc_id", ""),
            "symbols": [{"name": "FIX03_STRICT", "file": "symbol-0.kicad_sym"}],
            "footprint": {"name": "@FP_NAME@", "file": "footprint-0.kicad_mod"}}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY";

std::string fix03_strict_plugin(const std::string& manifest_name, const std::string& root_name,
                                bool with_footprint_property)
{
    std::string py = FIX03_STRICT_PLUGIN_PY;
    const std::pair<std::string, std::string> replacements[3] = {
        {"@FP_NAME@", manifest_name},
        {"@FP_ROOT@", root_name},
        {"@FP_PROPERTY@",
         with_footprint_property
             ? std::string("'  (property \"Footprint\" \"old:fp\" (at 0 0 0) ' + fx + ')\\n'")
             : std::string("''")}};
    for (const auto& [marker, replacement] : replacements)
    {
        for (auto at = py.find(marker); at != std::string::npos; at = py.find(marker, at))
            py.replace(at, marker.size(), replacement);
    }
    return py;
}

/// FIX03 桩：显式 Kicad_Forge_ID / Pre_Kicad_Forge_ID 可选（空 = 文件里不带 KF 属性）
const char* FIX03_KF_PLUGIN_PY = R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
fx = '(effects (font (size 1.27 1.27)))'
symbol = ('(symbol "FIX03_KF"\n'
          '  (property "Reference" "U" (at 0 5.08 0) ' + fx + ')\n'
          '  (property "Value" "@VALUE@" (at 0 2.54 0) ' + fx + ')\n'
          '  (property "Footprint" "old:fp" (at 0 0 0) ' + fx + ')\n'
          + @KF_LINES@
          '  (symbol "FIX03_KF_1_1"\n'
          '    (pin passive line (at 0 0 0) (length 2.54) (name "A" ' + fx + ') (number "1" ' + fx + '))))')
with open(os.path.join(staging, "symbol-0.kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator \"kicad_symbol_editor\")\n" + symbol + "\n)\n")
with open(os.path.join(staging, "footprint-0.kicad_mod"), "w", encoding="utf-8") as f:
    f.write('(footprint "FIX03_KF_FP" (layer "F.Cu")\n  (pad "1" smd rect (at 0 0) (size 1 1)))\n')
manifest = {"protocol": "staged-import-v1", "lcsc_id": args.get("lcsc_id", ""),
            "symbols": [{"name": "FIX03_KF", "file": "symbol-0.kicad_sym"}],
            "footprint": {"name": "FIX03_KF_FP", "file": "footprint-0.kicad_mod"}}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY";

std::string fix03_kf_plugin(bool explicit_kf)
{
    std::string py = FIX03_KF_PLUGIN_PY;
    std::string kf_lines =
        explicit_kf
            ? "'  (property \"Kicad_Forge_ID\" \"KF-EXPLICIT-1\" (at 0 0 0) ' + fx + ')\\n'\n"
              "          + '  (property \"Pre_Kicad_Forge_ID\" \"KF-EXPLICIT-0\" (at 0 0 0) ' + fx + ')\\n'"
            : "''";
    const std::pair<std::string, std::string> replacements[2] = {
        {"@KF_LINES@", kf_lines}, {"@VALUE@", explicit_kf ? "KF-ON" : "KF-OFF"}};
    for (const auto& [marker, replacement] : replacements)
    {
        for (auto at = py.find(marker); at != std::string::npos; at = py.find(marker, at))
            py.replace(at, marker.size(), replacement);
    }
    return py;
}

const char* FIX03_META_V1_MANIFEST = R"JSON({
  "id": "fix03.meta1",
  "name": "FIX03 Metadata V1",
  "version": "1.0.0",
  "actions": [ { "id": "import", "name": "Import", "protocol": "staged-import-v1" } ]
})JSON";

const char* FIX03_META_V2_MANIFEST = R"JSON({
  "id": "fix03.meta2",
  "name": "FIX03 Metadata V2",
  "version": "1.0.0",
  "actions": [ { "id": "import", "name": "Import", "protocol": "staged-import-v1" } ]
})JSON";

/// 清单里两个符号：宿主必须整体拒绝，且不做任何文件/数据库改动
const char* FIX03_MULTI_SYMBOL_PLUGIN_PY = R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
fx = '(effects (font (size 1.27 1.27)))'
entries = []
for index in range(2):
    name = "FIX03_MULTI_%d" % index
    symbol = ('(symbol "' + name + '"\n'
              '  (property "Reference" "U" (at 0 5.08 0) ' + fx + ')\n'
              '  (property "Value" "' + name + '" (at 0 2.54 0) ' + fx + '))\n')
    file_name = "symbol-%d.kicad_sym" % index
    with open(os.path.join(staging, file_name), "w", encoding="utf-8") as f:
        f.write("(kicad_symbol_lib (version 20231120) (generator \"kicad_symbol_editor\")\n"
                + symbol + "\n)\n")
    entries.append({"name": name, "file": file_name})
manifest = {"protocol": "staged-import-v1", "lcsc_id": args.get("lcsc_id", ""),
            "symbols": entries}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY";

/// 封装名越界：名字里带父目录分量，磁盘路径必须仍然只在暂存目录里生成
const char* FIX03_EVIL_NAME_PLUGIN_PY = R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
fx = '(effects (font (size 1.27 1.27)))'
symbol = ('(symbol "FIX03_EVIL"\n'
          '  (property "Reference" "U" (at 0 5.08 0) ' + fx + ')\n'
          '  (property "Value" "EVIL" (at 0 2.54 0) ' + fx + '))\n')
with open(os.path.join(staging, "symbol-0.kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator \"kicad_symbol_editor\")\n"
            + symbol + "\n)\n")
with open(os.path.join(staging, "evil.kicad_mod"), "w", encoding="utf-8") as f:
    f.write('(footprint "EVIL" (layer "F.Cu") (pad "1" smd rect (at 0 0) (size 1 1)))\n')
manifest = {"protocol": "staged-import-v1", "lcsc_id": args.get("lcsc_id", ""),
            "symbols": [{"name": "FIX03_EVIL", "file": "symbol-0.kicad_sym"}],
            "footprint": {"name": "../ESCAPED_FP", "file": "evil.kicad_mod"}}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY";

}  // namespace

// ------------------------------------------------------------
// FIX03-A：根路径的归属语义（D:/ 与 Unix /）
// 前缀匹配之后又要求「下一个字符是分隔符」会把根自己的子文件全判成不属于。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 path ownership keeps the root semantics for drive and unix roots")
{
    auto root = temp.path().root_path();  // Windows 上是 "C:/"，Unix 上是 "/"
    CHECK(storage::path_belongs_to(temp.path() / "sub" / "a.txt", root));  // 根的子文件
    CHECK(storage::path_belongs_to(root, root));                           // 根自身
    CHECK(storage::path_belongs_to(temp.path(), root));

    auto libs = temp.path() / "libs";
    fs::create_directories(libs);
    CHECK(storage::path_belongs_to(libs / "a.kicad_sym", libs));
    CHECK(storage::path_belongs_to(libs / "a.kicad_sym", libs.string() + "/"));  // 尾 slash
    CHECK_FALSE(storage::path_belongs_to(temp.path() / "libs2" / "a.kicad_sym", libs));
    CHECK_FALSE(storage::path_belongs_to(libs, temp.path() / "libs2"));
}

// ------------------------------------------------------------
// FIX03-B：清单里的封装名是「名字」不是「路径」
// 带父目录分量的名字必须被拒绝，且暂存目录之外不能出现任何产物。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 staged import rejects a footprint name that escapes the library")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib_fix03b";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Fix03b.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"EXISTING\" (property \"Reference\" \"U\"))\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);
    std::string before = read(target);
    int symbols_before = count_rows(*db, "symbols");
    int operations_before = count_rows(*db, "import_operations");

    auto scan = temp.path() / "plugins_fix03b";
    write(scan / "evil" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "evil" / "plugin.py", FIX03_EVIL_NAME_PLUGIN_PY);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "stub.lcsc";  // 桩清单里的 id
    request.external_id = "C703";
    request.target_library_id = library_id;
    request.footprint_mode = "new";
    request.operation_id = "fix03-evil-name";

    auto result = importer.run(request);
    if (result)
        MESSAGE("escaping footprint name was accepted: ", result->dump());
    CHECK_FALSE(result.has_value());
    // 零副作用：目标库文件、库行数、操作记录都没变
    CHECK(read(target) == before);
    CHECK(count_rows(*db, "symbols") == symbols_before);
    CHECK(count_rows(*db, "import_operations") == operations_before);
    CHECK_FALSE(fs::exists(root / "ESCAPED_FP.kicad_mod"));
    CHECK_FALSE(fs::exists(sym_dir / "ESCAPED_FP.kicad_mod"));
    CHECK_FALSE(fs::exists(temp.path() / "ESCAPED_FP.kicad_mod"));
}

// ------------------------------------------------------------
// FIX03-C：一个 provider/external_id 只支持一个符号
// 清单里两项时整体 ValidationError，绝不能只取第一个。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 a multi-symbol manifest is rejected with zero side effects")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib_fix03c";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Fix03c.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"EXISTING\" (property \"Reference\" \"U\"))\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);
    std::string before = read(target);
    int symbols_before = count_rows(*db, "symbols");

    auto scan = temp.path() / "plugins_fix03c";
    write(scan / "multi" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "multi" / "plugin.py", FIX03_MULTI_SYMBOL_PLUGIN_PY);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "stub.lcsc";
    request.external_id = "C704";
    request.target_library_id = library_id;
    request.footprint_mode = "reuse";
    request.footprint_id = footprint("FIX03_REUSE").id();  // 复用：不产出封装
    request.operation_id = "fix03-multi";

    auto result = importer.run(request);
    if (result)
        MESSAGE("multi-symbol manifest was accepted: ", result->dump());
    CHECK_FALSE(result.has_value());
    CHECK(read(target) == before);                    // 没有把第一个符号写进去
    CHECK(count_rows(*db, "symbols") == symbols_before);
    CHECK(read(target).find("FIX03_MULTI_0") == std::string::npos);
}

// ------------------------------------------------------------
// FIX03-D：同源更新复用 UUID，源文件刷新属性，库里的非源信息保留
// 新导入 3 pin / 3 pad；第二次同源导入属性变了 —— 符号与封装 UUID 都不能换，
// DB 与宿主落盘文件必须一致（都用仓库既有解析器复核）。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 same-source import keeps UUIDs and refreshes source metadata")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib_fix03d";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Fix03d.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"EXISTING\" (property \"Reference\" \"U\"))\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);

    auto scan = temp.path() / "plugins_fix03d";
    write(scan / "v1" / "manifest.json", FIX03_META_V1_MANIFEST);
    write(scan / "v2" / "manifest.json", FIX03_META_V2_MANIFEST);
    write(scan / "v1" / "plugin.py", fix03_meta_plugin("FIX03-V1", "MPN-V1"));
    write(scan / "v2" / "plugin.py", fix03_meta_plugin("FIX03-V2", "MPN-V2"));
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "fix03.meta1";
    request.provider = "LCSC";
    request.external_id = "C705";
    request.target_library_id = library_id;
    request.footprint_mode = "new";
    request.operation_id = "fix03-meta-1";

    auto first = importer.run(request);
    if (!first)
        MESSAGE("first import failed: ", first.error().format_message());
    REQUIRE(first.has_value());
    std::string symbol_id = first->value("symbol_id", std::string(""));
    std::string footprint_id = first->value("footprint_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());
    REQUIRE_FALSE(footprint_id.empty());
    // 封装名带 ".." 字串是合法名字，不能被「字符串包含 ..」误杀
    CHECK(first->value("footprint_reference", std::string("")) == "Fix03d:FIX03_FP");
    CHECK(fs::exists(fp_dir / "Fix03d.pretty" / "FIX03_FP.kicad_mod"));

    {
        // 直接看 SQLite：3 个 pin、3 个 pad，属性来自产物
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(sym.has_value());
        CHECK(sym->pin_count() == 3);
        CHECK(sym->default_value() == "FIX03-V1");
        CHECK(sym->mpn() == "MPN-V1");
        CHECK(sym->description() == "Fixture FIX03-V1 part");
        CHECK(sym->datasheet() == "https://example.invalid/d.pdf");
        // 仓储回读的封装不能把 pad_count 变回 0
        auto fp = storage::FootprintRepository(session).find_by_id(footprint_id);
        REQUIRE(fp.has_value());
        CHECK(fp->pad_count() == 3);
        CHECK(fp->source_key() == storage::existing_source_key(fp->library_path()));
        CHECK_FALSE(fp->source_key().empty());
    }
    CHECK(audit_scalar_int(*db, "SELECT pad_count FROM footprints WHERE id = '" + footprint_id +
                                    "'") == 3);

    // 库里的非源信息（已确定类型、自定义范围内属性）在下次同源更新时必须保留
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(sym.has_value());
        sym->component_type_id = "Resistor";
        sym->set_property("value_range", "1k-10k");
        REQUIRE(storage::SymbolRepository(session).update(*sym).has_value());
    }

    // 第二次：同一个 external_id、同一份产物 → 最终宿主输出一致 = no-op，
    // 并且 no-op 也要给出与成功相同的字段
    api::StagedImporter::Request repeat = request;
    repeat.operation_id = "fix03-meta-2";
    auto again = importer.run(repeat);
    if (!again)
        MESSAGE("repeat import failed: ", again.error().format_message());
    REQUIRE(again.has_value());
    CHECK(again->value("noop", false));
    CHECK(again->value("mode", std::string("")) == "new");
    CHECK(again->value("symbol_id", std::string("")) == symbol_id);
    CHECK(again->value("footprint_id", std::string("")) == footprint_id);
    CHECK(again->value("footprint_reference", std::string("")) == "Fix03d:FIX03_FP");
    CHECK(again->value("operation_id", std::string("")) == "fix03-meta-2");

    // 第三次：换 V2 产物（新插件 = 新来源内容），UUID 必须复用、属性必须刷新
    api::StagedImporter::Request third = request;
    third.plugin_id = "fix03.meta2";
    third.operation_id = "fix03-meta-3";
    auto updated = importer.run(third);
    if (!updated)
        MESSAGE("attribute update failed: ", updated.error().format_message());
    REQUIRE(updated.has_value());
    CHECK_FALSE(updated->value("noop", false));  // 内容变了：不是重复导入
    CHECK(updated->value("symbol_id", std::string("")) == symbol_id);        // UUID 复用
    CHECK(updated->value("footprint_id", std::string("")) == footprint_id);  // 封装 UUID 复用

    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(sym.has_value());
        INFO("value=", sym->default_value(), " mpn=", sym->mpn(),
             " type=", sym->component_type_id, " pins=", sym->pin_count());
        CHECK(sym->default_value() == "FIX03-V2");           // 源文件属性已更新
        CHECK(sym->mpn() == "MPN-V2");
        CHECK(sym->description() == "Fixture FIX03-V2 part");
        CHECK(sym->component_type_id == "Resistor");         // 库里的类型 ID 保留
        CHECK(sym->property("value_range") == std::optional<std::string>("1k-10k"));
        CHECK(sym->pin_count() == 3);
        auto fp = storage::FootprintRepository(session).find_by_id(footprint_id);
        REQUIRE(fp.has_value());
        CHECK(fp->pad_count() == 3);                         // 回读不能又变成 0
    }
    CHECK(read(target).find("FIX03-V2") != std::string::npos);

    // 宿主落盘的文件与数据库说的是同一件事（用仓库既有的解析器复核）
    {
        auto written = parser::SymbolLibParser::parse(target);
        REQUIRE(written.has_value());
        const core::Symbol* node = nullptr;
        for (const auto& item : written->items)
            if (item.name() == "FIX03_PART")
                node = &item;
        REQUIRE(node != nullptr);
        CHECK(node->pin_count() == 3);
        CHECK(node->default_value() == "FIX03-V2");
        CHECK(node->mpn() == "MPN-V2");
        CHECK(node->footprint() == "Fix03d:FIX03_FP");
    }
    {
        auto written_fp =
            parser::FootprintParser::parse(fp_dir / "Fix03d.pretty" / "FIX03_FP.kicad_mod");
        REQUIRE(written_fp.has_value());
        CHECK(written_fp->pad_count() == 3);
    }

    // 同源同输出的 B→B 才是 no-op；记录可回放：同一 operation_id 再来一次同结果
    api::StagedImporter::Request replay = third;
    replay.operation_id = "fix03-meta-4";
    auto same = importer.run(replay);
    REQUIRE(same.has_value());
    CHECK(same->value("noop", false));
    CHECK(same->value("mode", std::string("")) == "new");
    CHECK(same->value("footprint_id", std::string("")) == footprint_id);
    CHECK(same->value("symbol_id", std::string("")) == symbol_id);
    auto replay_again = importer.run(replay);
    REQUIRE(replay_again.has_value());
    CHECK(replay_again->value("replayed", false));
    CHECK(replay_again->value("footprint_id", std::string("")) == footprint_id);
}

// ------------------------------------------------------------
// FIX03-N：同源更新要连「删掉的源属性」和「改过的引脚」一起落库
// 从旧行起手逐项叠加会留下 V1 的 vendor_tag/manufacturer/ki_keywords；
// 更新路径只写 properties 又会把引脚定义丢在 _pins_json 之外 ——
// 回读只剩一个 pin_count 计数，引脚本身全没了。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 same-source update drops removed properties and rewrites pins")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib_fix03n";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Fix03n.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"EXISTING\" (property \"Reference\" \"U\"))\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);

    auto scan = temp.path() / "plugins_fix03n";
    write(scan / "v1" / "manifest.json", FIX03_META_V1_MANIFEST);
    write(scan / "v2" / "manifest.json", FIX03_META_V2_MANIFEST);
    write(scan / "v1" / "plugin.py",
          fix03_meta_extra_plugin("FIX03-N1", "MPN-N1", FIX03_EXTRA_V1, FIX03_PIN1_V1));
    write(scan / "v2" / "plugin.py",
          fix03_meta_extra_plugin("FIX03-N2", "MPN-N2", FIX03_EXTRA_V2, FIX03_PIN1_V2));
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "fix03.meta1";
    request.provider = "LCSC";
    request.external_id = "C706";
    request.target_library_id = library_id;
    request.footprint_mode = "new";
    request.operation_id = "fix03-n-1";

    auto first = importer.run(request);
    if (!first)
        MESSAGE("first import failed: ", first.error().format_message());
    REQUIRE(first.has_value());
    std::string symbol_id = first->value("symbol_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());

    // V1：三个来源属性真的进了库（否则下面「V2 之后消失」就是空断言）
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(sym.has_value());
        CHECK(sym->property("vendor_tag") == std::optional<std::string>("V1-TAG"));
        CHECK(sym->property("manufacturer") == std::optional<std::string>("V1-MFR"));
        CHECK(sym->property("keywords") == std::optional<std::string>("v1 keywords"));
        REQUIRE(sym->pins().size() == 3);
    }

    // 库里的非源信息（手动类型、宿主 value_range property）在更新时必须保留
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(sym.has_value());
        sym->component_type_id = "Resistor";
        sym->set_property("value_range", "10k-100k");
        REQUIRE(storage::SymbolRepository(session).update(*sym).has_value());
    }

    api::StagedImporter::Request second = request;
    second.plugin_id = "fix03.meta2";
    second.operation_id = "fix03-n-2";
    auto updated = importer.run(second);
    if (!updated)
        MESSAGE("update failed: ", updated.error().format_message());
    REQUIRE(updated.has_value());
    CHECK(updated->value("symbol_id", std::string("")) == symbol_id);  // UUID 复用

    // 最终来源的引脚模型：库回读必须与它逐字段一致，而不是只剩一个计数
    core::Symbol final_source;
    {
        auto written = parser::SymbolLibParser::parse(target);
        REQUIRE(written.has_value());
        for (const auto& item : written->items)
            if (item.name() == "FIX03_PART")
                final_source = item;
        REQUIRE(final_source.name() == "FIX03_PART");
        REQUIRE(final_source.pins().size() == 3);
        // V2 改过的那个引脚在落盘文件里就是新值
        CHECK(final_source.pins()[0].number == "11");
        CHECK(final_source.pins()[0].name == "AAA");
        CHECK(final_source.pins()[0].electrical_type == "input");
        CHECK(read(target).find("(at -7.62 2.54 0)") != std::string::npos);
    }

    std::string raw_properties;
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(sym.has_value());
        // 源里删掉的键一个都不能留在行上
        CHECK_FALSE(sym->property("vendor_tag").has_value());
        CHECK_FALSE(sym->property("manufacturer").has_value());
        CHECK_FALSE(sym->property("keywords").has_value());
        // 宿主字段、手动类型与行身份照旧
        CHECK(sym->id() == symbol_id);
        CHECK(sym->component_type_id == "Resistor");
        CHECK(sym->property("value_range") == std::optional<std::string>("10k-100k"));
        CHECK(sym->default_value() == "FIX03-N2");
        CHECK(sym->mpn() == "MPN-N2");
        // 引脚：数量与每个字段都精确等于最终源解析结果。
        // （解析器本身不读引脚位置，(at ...) 不体现在模型里；这里比对的是
        //  「库回读 == 最终文件解析」这一最严格的一致关系，位置字段同样逐项比对）
        REQUIRE(sym->pins().size() == final_source.pins().size());
        for (std::size_t i = 0; i < sym->pins().size(); ++i)
        {
            INFO("pin ", i, " stored=", sym->pins()[i].number, "/", sym->pins()[i].name, "/",
                 sym->pins()[i].electrical_type, " source=", final_source.pins()[i].number, "/",
                 final_source.pins()[i].name, "/", final_source.pins()[i].electrical_type);
            CHECK(sym->pins()[i].number == final_source.pins()[i].number);
            CHECK(sym->pins()[i].name == final_source.pins()[i].name);
            CHECK(sym->pins()[i].electrical_type == final_source.pins()[i].electrical_type);
            CHECK(sym->pins()[i].x == final_source.pins()[i].x);
            CHECK(sym->pins()[i].y == final_source.pins()[i].y);
        }
    }
    // SQLite 的 _pins_json 说的是同一件事（仓储回读会把它从 properties 里摘掉，
    // 所以只能直接读列）。audit_scalar_text 自己会开一个 session，必须放在上面
    // 这个 session 作用域完全结束之后（timed_mutex 不可重入，否则自死锁）
    raw_properties = audit_scalar_text(
        *db, "SELECT properties_json FROM symbols WHERE id = '" + symbol_id + "'");
    // properties_json 里的 _pins_json 是「字符串里再套一层 JSON」（与 row_to_symbol
    // 的反序列化方式一致），要解两层才是引脚数组
    auto stored_properties = nlohmann::json::parse(raw_properties);
    auto stored_pins_it = stored_properties.find("_pins_json");
    REQUIRE(stored_pins_it != stored_properties.end());
    auto stored_pins = (*stored_pins_it).is_string()
                           ? nlohmann::json::parse((*stored_pins_it).get<std::string>())
                           : *stored_pins_it;
    REQUIRE(stored_pins.is_array());
    REQUIRE(stored_pins.size() == final_source.pins().size());
    for (std::size_t i = 0; i < stored_pins.size(); ++i)
    {
        CHECK(stored_pins[i].value("number", std::string("")) == final_source.pins()[i].number);
        CHECK(stored_pins[i].value("name", std::string("")) == final_source.pins()[i].name);
        CHECK(stored_pins[i].value("electrical_type", std::string("")) ==
              final_source.pins()[i].electrical_type);
        CHECK(stored_pins[i].value("x", -1.0) == final_source.pins()[i].x);
        CHECK(stored_pins[i].value("y", -1.0) == final_source.pins()[i].y);
    }
}

// ------------------------------------------------------------
// FIX03-E：授权判定只看登记与 root，allow_missing 不能豁免
// 前缀混淆（libs2 不是 libs）、未配置 root、保护根、以及插件转换之后
// 才发生的锁状态变更，都必须拦住写入。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 source mutation re-checks owner roots and lock state")
{
    auto base = temp.path() / "fix03_roots";
    auto sym_dir = base / "symbols";
    auto root_dir = base / "libs";  // 组件库的封装 root
    auto target = sym_dir / "Fix03e.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");

    io::SourceMutationService mutations(*db);
    auto ok = [](storage::DbSession&) -> util::Result<void> { return {}; };

    // 还没有任何组件库：不能向任意路径写
    auto no_root_target = temp.path() / "fix03_noroot" / "X.kicad_mod";
    auto no_root = mutations.apply({io::FileChange{.path = no_root_target,
                                                   .content = "(footprint \"X\")",
                                                   .allow_missing = true}},
                                   ok);
    CHECK_FALSE(no_root.has_value());
    CHECK_FALSE(fs::exists(no_root_target));

    // 真实 owner 是组件库 id：登记返回的是符号库 UUID，必须再读一次归属
    std::string owner =
        audit_component_owner(*db, audit_register_user_library(*db, target, sym_dir, root_dir));
    REQUIRE_FALSE(owner.empty());

    // 正常路径：登记过的 root 内新文件可以写
    auto good_target = root_dir / "New.pretty" / "NEW.kicad_mod";
    REQUIRE(mutations.apply({io::FileChange{.path = good_target,
                                            .content = "(footprint \"NEW\")",
                                            .allow_missing = true,
                                            .owner = owner}},
                            ok)
                .has_value());
    CHECK(read(good_target) == "(footprint \"NEW\")");

    // 根前缀混淆：libs2 不属于 libs
    auto confused = base / "libs2" / "Trap.pretty" / "X.kicad_mod";
    auto confused_result = mutations.apply({io::FileChange{.path = confused,
                                                           .content = "(footprint \"X\")",
                                                           .allow_missing = true,
                                                           .owner = owner}},
                                           ok);
    CHECK_FALSE(confused_result.has_value());
    CHECK_FALSE(fs::exists(confused));

    // 完全在 root 之外的新文件
    auto outside = temp.path() / "fix03_outside" / "X.kicad_mod";
    auto outside_result = mutations.apply({io::FileChange{.path = outside,
                                                          .content = "(footprint \"X\")",
                                                          .allow_missing = true,
                                                          .owner = owner}},
                                          ok);
    CHECK_FALSE(outside_result.has_value());
    CHECK_FALSE(fs::exists(outside));

    // 归属对不上：拿别人的 owner 也写不进这个 root
    auto other = audit_component_owner(
        *db, audit_register_user_library(*db, sym_dir / "Other.kicad_sym",
                                         temp.path() / "fix03_other" / "symbols",
                                         temp.path() / "fix03_other" / "footprints"));
    CHECK_FALSE(other.empty());
    CHECK(other != owner);
    auto wrong_owner = mutations.apply({io::FileChange{.path = root_dir / "New.pretty" / "OTHER.kicad_mod",
                                                       .content = "(footprint \"OTHER\")",
                                                       .allow_missing = true,
                                                       .owner = other}},
                                       ok);
    CHECK_FALSE(wrong_owner.has_value());

    // 保护根：即使 owner 正确也不能写
    {
        auto session = db->session();
        auto groups = storage::ComponentLibraryRepository(session).find_all();
        REQUIRE(groups.has_value());
        for (auto& group : *groups)
        {
            if (group.id != owner) continue;
            group.is_protected = true;
            REQUIRE(storage::ComponentLibraryRepository(session).update(group).has_value());
        }
    }
    auto protected_result = mutations.apply(
        {io::FileChange{.path = root_dir / "New.pretty" / "AGAIN.kicad_mod",
                        .content = "(footprint \"AGAIN\")",
                        .allow_missing = true,
                        .owner = owner}},
        ok);
    CHECK_FALSE(protected_result.has_value());
    CHECK_FALSE(fs::exists(root_dir / "New.pretty" / "AGAIN.kicad_mod"));
}

// ------------------------------------------------------------
// FIX03-F：旧参数 (库名, 封装名) 只有在唯一匹配时才是身份
// 同名多来源必须报错，明确给第二个 UUID 才成功。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 legacy footprint references resolve only when unique")
{
    auto only = footprint("ONLY");
    auto dup_a = footprint("DUP");
    auto dup_b = footprint("DUP");  // 同一个 .pretty、同一个名字，两个 UUID
    REQUIRE(dup_a.id() != dup_b.id());

    {
        auto session = db->session();
        storage::FootprintRepository footprints(session);
        auto unique = footprints.resolve_legacy_reference("Pkg", "ONLY");
        REQUIRE(unique.has_value());
        CHECK(unique->id() == only.id());

        // 唯一旧参数（只给名字、不给库名）仍然成功
        auto by_name_only = footprints.resolve_legacy_reference("", "ONLY");
        REQUIRE(by_name_only.has_value());
        CHECK(by_name_only->id() == only.id());

        // 同名多来源：ambiguous，绝不静默取第一个
        auto ambiguous = footprints.resolve_legacy_reference("Pkg", "DUP");
        CHECK_FALSE(ambiguous.has_value());
        CHECK(ambiguous.error().kind() == util::Error::Kind::ValidationError);

        // 0 项：NotFound
        auto missing = footprints.resolve_legacy_reference("Pkg", "NOPE");
        CHECK_FALSE(missing.has_value());
        CHECK(missing.error().kind() == util::Error::Kind::NotFound);

        // 明确给第二个 UUID：能取到，且与第一个可区分
        auto second = footprints.find_by_id(dup_b.id());
        REQUIRE(second.has_value());
        CHECK(second->id() == dup_b.id());
        CHECK(second->id() != dup_a.id());
    }
}

// ------------------------------------------------------------
// FIX03-G：精确已登记来源也要过真实 owner 的锁与 root
// 「source_key 命中就放行」会让锁定的组件库、空 root、归属失效或已经把文件
// 移出 root 的来源继续可写。这里走真实的删除符号路径（调用方不给 owner）。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 delete_symbol re-checks the registered owner lock and root")
{
    auto base = temp.path() / "fix03g";
    services::LibraryService libraries(db.get(), &matching_store);

    // 正常：owner 存在、未锁定、root 真的包含这个文件 → 允许删除
    {
        auto dir = base / "ok" / "symbols";
        auto file = dir / "Ok.kicad_sym";
        write(file, audit_symbol_file_text("OWNED"));
        auto owner = audit_make_group(*db, "OkGroup", dir, base / "ok" / "footprints");
        auto symbol_id = audit_register_symbol_in(*db, file, owner, "OWNED");

        auto removed = libraries.delete_symbol(symbol_id);
        if (!removed)
            MESSAGE("normal delete failed: ", removed.error().format_message());
        REQUIRE(removed.has_value());
        CHECK(read(file).find("OWNED") == std::string::npos);
    }

    // 锁定的 owner + 空 root：精确登记也不能绕过硬锁
    {
        auto dir = base / "locked" / "symbols";
        auto file = dir / "Locked.kicad_sym";
        write(file, audit_symbol_file_text("OWNED"));
        auto owner = audit_make_group(*db, "LockedGroup", "", "", true);
        auto symbol_id = audit_register_symbol_in(*db, file, owner, "OWNED");
        std::string before = read(file);

        auto removed = libraries.delete_symbol(symbol_id);
        CHECK_FALSE(removed.has_value());
        CHECK(read(file) == before);  // 文件不能动
        auto session = db->session();
        CHECK(storage::SymbolRepository(session).find_by_id(symbol_id).has_value());
    }

    // 非空 root，但文件被移出了 root（归属还在，位置不再成立）
    {
        auto dir = base / "moved" / "symbols";
        auto file = dir / "Moved.kicad_sym";
        write(file, audit_symbol_file_text("OWNED"));
        auto owner = audit_make_group(*db, "MovedGroup", base / "moved" / "elsewhere", "");
        auto symbol_id = audit_register_symbol_in(*db, file, owner, "OWNED");
        std::string before = read(file);

        auto removed = libraries.delete_symbol(symbol_id);
        CHECK_FALSE(removed.has_value());
        CHECK(read(file) == before);
        auto session = db->session();
        CHECK(storage::SymbolRepository(session).find_by_id(symbol_id).has_value());
    }

    // 归属失效：行上记的组件库已经不存在（旧库/外部工具留下的孤儿归属）
    {
        auto dir = base / "stale" / "symbols";
        auto file = dir / "Stale.kicad_sym";
        write(file, audit_symbol_file_text("OWNED"));
        // 外键会把不存在的归属挡在建表这一步：按旧库的样子直接写行
        REQUIRE(db->execute("PRAGMA foreign_keys=OFF").has_value());
        REQUIRE(db->execute("INSERT INTO libraries(id,name,file_path,component_library_id,source_key) "
                            "VALUES('fix03g-stale','Stale','" +
                            file.string() + "','fix03g-ghost','" + storage::source_key(file) + "')")
                    .has_value());
        REQUIRE(db->execute("PRAGMA foreign_keys=ON").has_value());
        std::string symbol_id;
        {
            auto session = db->session();
            core::Symbol sym;
            sym.set_name("OWNED");
            sym.set_library_id("fix03g-stale");
            auto inserted = storage::SymbolRepository(session).insert(sym);
            REQUIRE(inserted.has_value());
            symbol_id = inserted->id();
        }
        std::string before = read(file);

        auto removed = libraries.delete_symbol(symbol_id);
        CHECK_FALSE(removed.has_value());
        CHECK(read(file) == before);
    }
}

// ------------------------------------------------------------
// FIX03-O：已登记库行上的 owner 必须自证，不能因为「登记归属为空」放行
// 旧行（component_library_id 为空）配上任意 ghost owner 曾被无条件放行；
// 真实未锁 owner 但没配 root 时，已登记的精确文件又曾被误拒。
// 精确登记是「已存在来源」的兼容依据，不是「同目录新文件」的通行证。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 registered library ownership must be proven by the caller owner")
{
    auto base = temp.path() / "fix03o";
    io::SourceMutationService mutations(*db);
    auto ok = [](storage::DbSession&) -> util::Result<void> { return {}; };

    // --- 旧登记行没有归属：显式 ghost owner 不能冒充归属 ---
    auto legacy_dir = base / "legacy" / "symbols";
    auto legacy_file = legacy_dir / "Legacy.kicad_sym";
    write(legacy_file, audit_symbol_file_text("LEGACY"));
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "LegacyNoOwner";
        meta.file_path = legacy_file;  // component_library_id 故意留空
        auto inserted = storage::LibraryRepository(session).insert(meta);
        REQUIRE(inserted.has_value());
        CHECK(inserted->component_library_id.empty());
    }
    std::string legacy_before = read(legacy_file);
    auto ghost = mutations.apply({io::FileChange{.path = legacy_file,
                                                 .content = audit_symbol_file_text("GHOST"),
                                                 .owner = "fix03o-ghost-owner"}},
                                 ok);
    CHECK_FALSE(ghost.has_value());
    CHECK(read(legacy_file) == legacy_before);

    // 省略 owner 时：已登记的这个精确文件仍然兼容（旧来源不倒退）
    REQUIRE(mutations.apply({io::FileChange{.path = legacy_file,
                                            .content = audit_symbol_file_text("COMPAT")}},
                            ok)
                .has_value());
    CHECK(read(legacy_file).find("COMPAT") != std::string::npos);

    // 同目录里未登记的新文件不能搭这趟车
    auto legacy_new = legacy_dir / "New.kicad_sym";
    auto unregistered = mutations.apply({io::FileChange{.path = legacy_new,
                                                        .content = audit_symbol_file_text("NEW"),
                                                        .allow_missing = true}},
                                        ok);
    CHECK_FALSE(unregistered.has_value());
    CHECK_FALSE(fs::exists(legacy_new));

    // --- 真实 owner、未锁定，但没配 root：已登记的精确文件要兼容 ---
    auto owned_dir = base / "owned" / "symbols";
    auto owned_file = owned_dir / "Owned.kicad_sym";
    write(owned_file, audit_symbol_file_text("OWNED"));
    auto owner = audit_make_group(*db, "NoRootGroup", "", "");  // 存在、未锁定、无 root
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "OwnedNoRoot";
        meta.file_path = owned_file;
        meta.component_library_id = owner;
        REQUIRE(storage::LibraryRepository(session).insert(meta).has_value());
    }
    auto refreshed = mutations.apply({io::FileChange{.path = owned_file,
                                                     .content = audit_symbol_file_text("REFRESHED"),
                                                     .owner = owner}},
                                     ok);
    if (!refreshed)
        MESSAGE("registered exact file with a rootless owner was rejected: ",
                refreshed.error().format_message());
    CHECK(refreshed.has_value());
    CHECK(read(owned_file).find("REFRESHED") != std::string::npos);

    // 同一目录的新文件仍然必须落在某个可写 root 里：没有 root 就不能写
    auto owned_new = owned_dir / "New.kicad_sym";
    auto new_result = mutations.apply({io::FileChange{.path = owned_new,
                                                      .content = audit_symbol_file_text("NEW"),
                                                      .allow_missing = true,
                                                      .owner = owner}},
                                      ok);
    CHECK_FALSE(new_result.has_value());
    CHECK_FALSE(fs::exists(owned_new));

    // --- 锁定的 owner 即使登记精确、root 为空也不能绕过硬锁 ---
    auto locked_dir = base / "locked" / "symbols";
    auto locked_file = locked_dir / "Locked.kicad_sym";
    write(locked_file, audit_symbol_file_text("LOCKED"));
    auto locked_owner = audit_make_group(*db, "LockedNoRoot", "", "", true);
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "LockedNoRoot";
        meta.file_path = locked_file;
        meta.component_library_id = locked_owner;
        REQUIRE(storage::LibraryRepository(session).insert(meta).has_value());
    }
    std::string locked_before = read(locked_file);
    auto locked_result = mutations.apply({io::FileChange{.path = locked_file,
                                                         .content = audit_symbol_file_text("HACKED"),
                                                         .owner = locked_owner}},
                                         ok);
    CHECK_FALSE(locked_result.has_value());
    CHECK(read(locked_file) == locked_before);
}

// ------------------------------------------------------------
// FIX03-H：新文件的种类决定它该落在哪个 root
// 未登记的 .kicad_mod 不能借符号 root 混进去，.kicad_sym 也不能借封装 root；
// 精确登记的封装不接受调用方伪 owner 假冒归属。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 new source files match their own root kind and owner")
{
    auto base = temp.path() / "fix03h";
    auto sym_dir = base / "symbols";
    auto fp_dir = base / "footprints";
    auto target = sym_dir / "Kind.kicad_sym";
    write(target, audit_symbol_file_text("KEEP"));
    auto owner = audit_make_group(*db, "KindGroup", sym_dir, fp_dir);
    auto other = audit_make_group(*db, "OtherGroup", base / "other" / "symbols",
                                  base / "other" / "footprints");

    io::SourceMutationService mutations(*db);
    auto ok = [](storage::DbSession&) -> util::Result<void> { return {}; };

    // 对应种类：新封装进封装 root 可以
    auto good = fp_dir / "Good.pretty" / "GOOD.kicad_mod";
    REQUIRE(mutations.apply({io::FileChange{.path = good,
                                            .content = "(footprint \"GOOD\")",
                                            .allow_missing = true,
                                            .owner = owner}},
                            ok)
                .has_value());
    CHECK(read(good) == "(footprint \"GOOD\")");

    // .kicad_mod 落进符号 root：拒绝
    auto mod_in_sym = sym_dir / "Trap.kicad_mod";
    auto mod_result = mutations.apply({io::FileChange{.path = mod_in_sym,
                                                      .content = "(footprint \"T\")",
                                                      .allow_missing = true,
                                                      .owner = owner}},
                                      ok);
    CHECK_FALSE(mod_result.has_value());
    CHECK_FALSE(fs::exists(mod_in_sym));

    // .kicad_sym 落进封装 root：拒绝
    auto sym_in_fp = fp_dir / "Trap.kicad_sym";
    auto sym_result = mutations.apply({io::FileChange{.path = sym_in_fp,
                                                      .content = audit_symbol_file_text("T"),
                                                      .allow_missing = true,
                                                      .owner = owner}},
                                      ok);
    CHECK_FALSE(sym_result.has_value());
    CHECK_FALSE(fs::exists(sym_in_fp));

    // 精确登记的封装：伪 owner（另一个组件库）不能假冒归属
    {
        auto session = db->session();
        core::Footprint fp;
        fp.set_name("GOOD");
        fp.set_library_path(good.string());
        fp.set_source_key(storage::existing_source_key(good));
        REQUIRE_FALSE(fp.source_key().empty());
        REQUIRE(storage::FootprintRepository(session).insert(fp).has_value());
    }
    std::string before = read(good);
    auto fake_owner = mutations.apply({io::FileChange{.path = good,
                                                      .content = "(footprint \"GOOD\" (layer \"F.Cu\"))",
                                                      .owner = other}},
                                      ok);
    CHECK_FALSE(fake_owner.has_value());
    CHECK(read(good) == before);

    // 省略 owner：唯一 root 归属时兼容
    auto no_owner = mutations.apply(
        {io::FileChange{.path = good, .content = "(footprint \"GOOD\" (layer \"F.Cu\"))"}}, ok);
    REQUIRE(no_owner.has_value());
    CHECK(read(good).find("F.Cu") != std::string::npos);
}

// ------------------------------------------------------------
// FIX03-L：expected_missing 是「必须不存在」的强断言
// 只写 allow_missing 时，空摘要（= 不校验）会把取锁前别人新建的同名文件
// 当成可覆盖的新文件；断言 expected_missing 的调用方必须拿到冲突。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 expected_missing refuses a target created after the read")
{
    auto base = temp.path() / "fix03l";
    auto sym_dir = base / "symbols";
    auto fp_dir = base / "footprints";
    auto owner = audit_make_group(*db, "MissingGroup", sym_dir, fp_dir);

    io::SourceMutationService mutations(*db);
    auto ok = [](storage::DbSession&) -> util::Result<void> { return {}; };

    // 目标确实不在：断言 expected_missing 的新建照常成功
    auto fresh = sym_dir / "Fresh.kicad_sym";
    REQUIRE(mutations.apply({io::FileChange{.path = fresh,
                                            .content = audit_symbol_file_text("FRESH"),
                                            .allow_missing = true,
                                            .expected_missing = true,
                                            .owner = owner}},
                            ok)
                .has_value());
    CHECK(read(fresh).find("FRESH") != std::string::npos);

    // 目标已被（本操作之外的）内容占住：必须报冲突，且一个字节都不能改
    auto taken = sym_dir / "Taken.kicad_sym";
    write(taken, audit_symbol_file_text("THEIRS"));
    std::string before = read(taken);
    auto refused = mutations.apply({io::FileChange{.path = taken,
                                                   .content = audit_symbol_file_text("MINE"),
                                                   .allow_missing = true,
                                                   .expected_missing = true,
                                                   .owner = owner}},
                                   ok);
    CHECK_FALSE(refused.has_value());
    CHECK(read(taken) == before);

    // 旧语义不受影响：只给 allow_missing + 摘要，存在即按摘要覆盖
    auto legacy = mutations.apply({io::FileChange{.path = taken,
                                                  .content = audit_symbol_file_text("MINE"),
                                                  .expected_digest = io::content_digest(before),
                                                  .allow_missing = true,
                                                  .owner = owner}},
                                  ok);
    REQUIRE(legacy.has_value());
    CHECK(read(taken).find("MINE") != std::string::npos);
}

// ------------------------------------------------------------
// FIX03-I：no-op 的边界是「最终宿主输出还在」
// 同库无关符号被外部改写不影响本源；目标符号 / 新封装被外部改写或删除则必须重跑。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 staged no-op requires the final host output to be intact")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib_fix03i";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Fix03i.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"EXISTING\" (property \"Reference\" \"U\"))\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);

    auto scan = temp.path() / "plugins_fix03i";
    write(scan / "v1" / "manifest.json", FIX03_META_V1_MANIFEST);
    write(scan / "v1" / "plugin.py", fix03_meta_plugin("FIX03-V1", "MPN-V1"));
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "fix03.meta1";
    request.external_id = "C706";
    request.target_library_id = library_id;
    request.footprint_mode = "new";

    auto first = importer.run(request);
    if (!first)
        MESSAGE("first import failed: ", first.error().format_message());
    REQUIRE(first.has_value());
    std::string symbol_id = first->value("symbol_id", std::string(""));
    std::string footprint_id = first->value("footprint_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());
    REQUIRE_FALSE(footprint_id.empty());
    auto fp_path = fp_dir / "Fix03i.pretty" / "FIX03_FP.kicad_mod";
    REQUIRE(fs::exists(fp_path));

    // 1) 同库无关符号被外部改写：本源内容没动 → 仍应 no-op，且不能覆盖别人的改动
    {
        std::string text = read(target);
        auto close = text.find_last_of(')');
        REQUIRE(close != std::string::npos);
        text.insert(close, "  (symbol \"UNRELATED\" (property \"Reference\" \"R\"))\n");
        write(target, text);
    }
    auto still = importer.run(request);
    if (!still)
        MESSAGE("unrelated-edit import failed: ", still.error().format_message());
    REQUIRE(still.has_value());
    CHECK(still->value("noop", false));
    CHECK(read(target).find("UNRELATED") != std::string::npos);
    CHECK(still->value("symbol_id", std::string("")) == symbol_id);
    CHECK(still->value("footprint_id", std::string("")) == footprint_id);

    // 2) 目标符号被外部改写：不能误判 no-op，重跑后恢复成宿主输出
    {
        std::string text = read(target);
        auto at = text.find("FIX03-V1");
        REQUIRE(at != std::string::npos);
        text.replace(at, std::string("FIX03-V1").size(), "TAMPERED-1");
        write(target, text);
    }
    auto restored = importer.run(request);
    if (!restored)
        MESSAGE("restore import failed: ", restored.error().format_message());
    REQUIRE(restored.has_value());
    CHECK_FALSE(restored->value("noop", false));
    CHECK(read(target).find("TAMPERED-1") == std::string::npos);
    CHECK(read(target).find("FIX03-V1") != std::string::npos);
    CHECK(restored->value("symbol_id", std::string("")) == symbol_id);

    // 3) 新封装文件被外部删除：不能 no-op，重跑后重新落盘并复用 UUID
    fs::remove(fp_path);
    auto remade = importer.run(request);
    if (!remade)
        MESSAGE("re-create footprint failed: ", remade.error().format_message());
    REQUIRE(remade.has_value());
    CHECK_FALSE(remade->value("noop", false));
    CHECK(fs::exists(fp_path));
    CHECK(remade->value("footprint_id", std::string("")) == footprint_id);

    // 4) 新封装被外部改写：不能 no-op，重跑后内容回到宿主输出
    write(fp_path, "(footprint \"FIX03_FP\" (layer \"F.Cu\"))\n");
    auto refixed = importer.run(request);
    if (!refixed)
        MESSAGE("re-fix footprint failed: ", refixed.error().format_message());
    REQUIRE(refixed.has_value());
    CHECK_FALSE(refixed->value("noop", false));
    CHECK(read(fp_path).find("(pad ") != std::string::npos);
    CHECK(refixed->value("footprint_id", std::string("")) == footprint_id);
}

// ------------------------------------------------------------
// FIX03-J：清单里的封装名必须等于文件根节点名；符号缺 Footprint 属性时宿主
// 必须安全补上（文件里没有引用、数据库里却有绑定 = 数据不一致）
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 staged footprint name must match its file root name")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib_fix03j";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Fix03j.kicad_sym";
    write(target, audit_symbol_file_text("EXISTING"));
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);
    std::string before = read(target);
    int symbols_before = count_rows(*db, "symbols");

    auto scan = temp.path() / "plugins_fix03j";
    write(scan / "mismatch" / "manifest.json", audit_stub_manifest("fix03j.mismatch"));
    write(scan / "mismatch" / "plugin.py", fix03_strict_plugin("MANIFEST_NAME", "ROOT_NAME", true));
    write(scan / "consistent" / "manifest.json", audit_stub_manifest("fix03j.consistent"));
    write(scan / "consistent" / "plugin.py", fix03_strict_plugin("ROOT_NAME", "ROOT_NAME", false));
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "fix03j.mismatch";
    request.external_id = "C707";
    request.target_library_id = library_id;
    request.footprint_mode = "new";
    request.operation_id = "fix03j-mismatch";

    auto mismatched = importer.run(request);
    if (mismatched)
        MESSAGE("mismatched footprint name was accepted: ", mismatched->dump());
    CHECK_FALSE(mismatched.has_value());
    if (!mismatched)
        CHECK(mismatched.error().kind() == util::Error::Kind::ValidationError);
    // 零持久副作用
    CHECK(read(target) == before);
    CHECK(count_rows(*db, "symbols") == symbols_before);
    CHECK_FALSE(fs::exists(fp_dir / "Fix03j.pretty" / "MANIFEST_NAME.kicad_mod"));
    CHECK_FALSE(fs::exists(fp_dir / "Fix03j.pretty" / "ROOT_NAME.kicad_mod"));

    // 名字对得上、但符号里没有 Footprint 属性：宿主按同一份产物安全补上
    request.plugin_id = "fix03j.consistent";
    request.operation_id = "fix03j-consistent";
    auto added = importer.run(request);
    if (!added)
        MESSAGE("footprint property insertion failed: ", added.error().format_message());
    REQUIRE(added.has_value());
    CHECK(added->value("footprint_reference", std::string("")) == "Fix03j:ROOT_NAME");

    // 文件与数据库说的是同一件事
    CHECK(read(target).find("\"Fix03j:ROOT_NAME\"") != std::string::npos);
    auto written = parser::SymbolLibParser::parse(target);
    REQUIRE(written.has_value());
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(
            added->value("symbol_id", std::string("")));
        REQUIRE(sym.has_value());
        CHECK(sym->footprint() == "Fix03j:ROOT_NAME");
    }
    bool found = false;
    for (const auto& item : written->items)
        if (item.name() == "FIX03_STRICT")
        {
            found = true;
            CHECK(item.footprint() == "Fix03j:ROOT_NAME");
        }
    CHECK(found);
}

// ------------------------------------------------------------
// FIX03-K：产物里显式给出的 KF/Pre 必须复制到行专用字段；
// 没有 incoming KF 时保留数据库的自动 hash 语义，但绝不往文件里强塞新 KF。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 staged import copies explicit KF ids without inventing new ones")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib_fix03k";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Fix03k.kicad_sym";
    write(target, audit_symbol_file_text("EXISTING"));
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);

    auto scan = temp.path() / "plugins_fix03k";
    write(scan / "explicit" / "manifest.json", FIX03_META_V1_MANIFEST);
    write(scan / "explicit" / "plugin.py", fix03_kf_plugin(true));
    write(scan / "plain" / "manifest.json", FIX03_META_V2_MANIFEST);
    write(scan / "plain" / "plugin.py", fix03_kf_plugin(false));
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "fix03.meta1";
    request.external_id = "C708";
    request.target_library_id = library_id;
    request.footprint_mode = "new";
    request.operation_id = "fix03k-explicit";

    auto explicit_kf = importer.run(request);
    if (!explicit_kf)
        MESSAGE("explicit KF import failed: ", explicit_kf.error().format_message());
    REQUIRE(explicit_kf.has_value());
    std::string symbol_id = explicit_kf->value("symbol_id", std::string(""));
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(sym.has_value());
        CHECK(sym->Kicad_Forge_ID() == "KF-EXPLICIT-1");
        CHECK(sym->Pre_Kicad_Forge_ID() == "KF-EXPLICIT-0");
    }
    {
        auto written = parser::SymbolLibParser::parse(target);
        REQUIRE(written.has_value());
        const core::Symbol* node = nullptr;
        for (const auto& item : written->items)
            if (item.name() == "FIX03_KF")
                node = &item;
        REQUIRE(node != nullptr);
        CHECK(node->Kicad_Forge_ID() == "KF-EXPLICIT-1");       // 源文件与 DB 一致
        CHECK(node->Pre_Kicad_Forge_ID() == "KF-EXPLICIT-0");
    }

    // 同一个来源重跑（新的操作 ID）：最终输出一致 = no-op，KF 不漂移
    api::StagedImporter::Request repeat = request;
    repeat.operation_id = "fix03k-explicit-2";
    auto again = importer.run(repeat);
    if (!again)
        MESSAGE("repeat KF import failed: ", again.error().format_message());
    REQUIRE(again.has_value());
    CHECK(again->value("noop", false));
    CHECK(again->value("operation_id", std::string("")) == "fix03k-explicit-2");
    CHECK(again->value("symbol_id", std::string("")) == symbol_id);

    // 另一个来源的产物里没有 KF：数据库仍按自动 hash 语义给出非空 ID，
    // 但宿主不往文件里写它 —— 文件里没有 Kicad_Forge_ID 属性
    auto plain_target = sym_dir / "Fix03kPlain.kicad_sym";
    write(plain_target, audit_symbol_file_text("EXISTING"));
    auto plain_library = audit_register_user_library(*db, plain_target, sym_dir, fp_dir);
    api::StagedImporter::Request plain = request;
    plain.plugin_id = "fix03.meta2";
    plain.external_id = "C709";
    plain.target_library_id = plain_library;
    plain.operation_id = "fix03k-plain";
    auto no_kf = importer.run(plain);
    if (!no_kf)
        MESSAGE("plain import failed: ", no_kf.error().format_message());
    REQUIRE(no_kf.has_value());
    {
        auto session = db->session();
        auto sym = storage::SymbolRepository(session).find_by_id(
            no_kf->value("symbol_id", std::string("")));
        REQUIRE(sym.has_value());
        CHECK_FALSE(sym->Kicad_Forge_ID().empty());  // 自动 hash 语义保留
    }
    CHECK(read(plain_target).find("Kicad_Forge_ID") == std::string::npos);
}

// ------------------------------------------------------------
// FIX03-L：来源映射查询失败必须报错，不能当成「查不到」继续写
// 用 SQLite authorizer 拒掉 import_sources 的读：run 必须失败，文件/数据库不动。
// ------------------------------------------------------------
namespace
{
int deny_import_sources_read(void*, int action, const char* arg1, const char*, const char*,
                             const char*)
{
    if (action == SQLITE_READ && arg1 != nullptr && std::string(arg1) == "import_sources")
        return SQLITE_DENY;
    return SQLITE_OK;
}

// 只拒 symbols 的读：其它 SELECT/INSERT 全部放行，故障必须精确落在符号表读取上
int deny_symbols_read(void*, int action, const char* arg1, const char*, const char*, const char*)
{
    if (action == SQLITE_READ && arg1 != nullptr && std::string(arg1) == "symbols")
        return SQLITE_DENY;
    return SQLITE_OK;
}
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "FIX03 a failed source lookup aborts the staged import")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib_fix03l";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Fix03l.kicad_sym";
    write(target, audit_symbol_file_text("EXISTING"));
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);
    std::string before = read(target);
    int symbols_before = count_rows(*db, "symbols");

    auto scan = temp.path() / "plugins_fix03l";
    write(scan / "v1" / "manifest.json", FIX03_META_V1_MANIFEST);
    write(scan / "v1" / "plugin.py", fix03_meta_plugin("FIX03-V1", "MPN-V1"));
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "fix03.meta1";
    request.external_id = "C710";
    request.target_library_id = library_id;
    request.footprint_mode = "new";
    request.operation_id = "fix03l-denied";

    {
        auto session = db->session();
        REQUIRE(sqlite3_set_authorizer(session.handle(), deny_import_sources_read, nullptr) ==
                SQLITE_OK);
    }
    auto result = importer.run(request);
    {
        auto session = db->session();
        sqlite3_set_authorizer(session.handle(), nullptr, nullptr);
    }
    if (result)
        MESSAGE("denied source lookup still imported: ", result->dump());
    CHECK_FALSE(result.has_value());

    // 零持久副作用：文件、符号行、来源映射、操作记录、封装产物都没动
    CHECK(read(target) == before);
    CHECK(count_rows(*db, "symbols") == symbols_before);
    CHECK(count_rows(*db, "import_sources") == 0);
    CHECK(count_rows(*db, "import_operations") == 0);
    CHECK(count_rows(*db, "file_mutations") == 0);
    CHECK_FALSE(fs::exists(fp_dir / "Fix03l.pretty"));
}

// ------------------------------------------------------------
// FIX03-M：Windows 保留设备名按第一个点之前的主干判定
// 名字只出现在清单里，夹具不会在磁盘上创建真实设备名。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "FIX03 reserved device names in the manifest are rejected")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "userlib_fix03m";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Fix03m.kicad_sym";
    write(target, audit_symbol_file_text("EXISTING"));
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);
    std::string before = read(target);
    int symbols_before = count_rows(*db, "symbols");

    auto scan = temp.path() / "plugins_fix03m";
    const std::vector<std::string> names = {"CON.foo", "NUL.txt", "com1.kicad_mod"};
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        auto id = "fix03m." + std::to_string(index);
        auto dir = scan / ("v" + std::to_string(index));
        write(dir / "manifest.json", audit_stub_manifest(id));
        write(dir / "plugin.py", fix03_strict_plugin(names[index], "ROOT_NAME", true));
    }
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        api::StagedImporter::Request request;
        request.plugin_id = "fix03m." + std::to_string(index);
        request.external_id = "C711";
        request.target_library_id = library_id;
        request.footprint_mode = "new";
        request.operation_id = "fix03m-" + std::to_string(index);
        auto result = importer.run(request);
        if (result)
            MESSAGE("reserved name accepted: ", names[index], " -> ", result->dump());
        CHECK_FALSE(result.has_value());
        if (!result)
            CHECK(result.error().kind() == util::Error::Kind::ValidationError);
    }
    // 零持久副作用：清单里的名字从不参与磁盘路径
    CHECK(read(target) == before);
    CHECK(count_rows(*db, "symbols") == symbols_before);
    CHECK_FALSE(fs::exists(fp_dir / "Fix03m.pretty"));
}

// ============================================================
// FIX04 —— 类型修改的持久化：失败不能假成功
// 约束同 LOCAL_AUDIT：只用 fixture 的临时目录与临时数据库；
// 单例注册表在本段内显式绑到临时配置文件，并在用例结束时（RegistrySnapshot）
// 连定义带持久化路径一起还原，不触碰真实用户数据目录。
// ============================================================
#include "interface/api/type_mutation.h"

#include <atomic>

namespace
{
/// 类型定义的规范化文本（断言"内存没变"用）
std::string fix04_types_json(const std::vector<core::TypeEntry>& entries)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& entry : entries)
    {
        nlohmann::json item;
        item["id"] = entry.id;
        item["name"] = entry.name;
        item["icon"] = entry.icon;
        item["color"] = entry.color;
        item["category"] = entry.category;
        array.push_back(std::move(item));
    }
    return array.dump();
}

/// 两个线程自旋对齐后同时开工（不 sleep），返回时都已 join
template <typename First, typename Second>
void fix04_run_together(First first, Second second)
{
    std::atomic<bool> go{false};
    std::thread a([&]
                  {
                      while (!go.load(std::memory_order_acquire)) {}
                      first();
                  });
    std::thread b([&]
                  {
                      while (!go.load(std::memory_order_acquire)) {}
                      second();
                  });
    go.store(true, std::memory_order_release);
    a.join();
    b.join();
}
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "FIX04 a blocked type file write fails without touching memory or the file")
{
    RegistrySnapshot restore;
    auto& reg = core::TypeRegistry::instance();
    auto comp = temp.path() / "fix04_fault_comp.json";
    auto pkg = temp.path() / "fix04_fault_pkg.json";
    write(comp, R"({"types":[{"id":"CompKeep","name":"CompKeep","icon":"c","color":"#111"}]})");
    write(pkg, R"({"types":[{"id":"PkgKeep","name":"PkgKeep","category":"Other"}]})");
    REQUIRE(reg.load_component_types(comp.string()));
    REQUIRE(reg.load_package_types(pkg.string()));

    std::string comp_bytes = read(comp);
    std::string pkg_bytes = read(pkg);
    std::string comp_json = fix04_types_json(reg.component_types());
    std::string pkg_json = fix04_types_json(reg.package_types());

    SUBCASE("the temp file path is occupied")
    {
        fs::create_directories(comp.string() + ".tmp");
        fs::create_directories(pkg.string() + ".tmp");
    }
    SUBCASE("the target path is a directory")
    {
        auto blocked = temp.path() / "fix04_blocked_dir";
        fs::create_directories(blocked);
        reg.bind_component_types_path(blocked.string());
        reg.bind_package_types_path(blocked.string());
    }

    // 故障态：六种持久化变更都要报错，且内存与磁盘一个字节都不动
    core::TypeEntry new_comp;
    new_comp.name = "FaultComp";
    core::TypeEntry new_pkg;
    new_pkg.name = "FaultPkg";
    new_pkg.category = "Other";
    core::TypePatch rename_comp;
    rename_comp.name = "CompRenamed";
    core::TypePatch rename_pkg;
    rename_pkg.name = "PkgRenamed";

    CHECK_FALSE(reg.add_component_type_persistent(new_comp).has_value());
    CHECK_FALSE(reg.update_component_type_persistent("CompKeep", rename_comp).has_value());
    CHECK_FALSE(reg.remove_component_type_persistent("CompKeep").has_value());
    CHECK_FALSE(reg.add_package_type_persistent(new_pkg).has_value());
    CHECK_FALSE(reg.update_package_type_persistent("PkgKeep", rename_pkg).has_value());
    CHECK_FALSE(reg.remove_package_type_persistent("PkgKeep").has_value());

    CHECK(fix04_types_json(reg.component_types()) == comp_json);
    CHECK(fix04_types_json(reg.package_types()) == pkg_json);
    CHECK(read(comp) == comp_bytes);
    CHECK(read(pkg) == pkg_bytes);
    // 索引与定义一致：失败后没有半新半旧的 map
    CHECK(reg.find_component("CompKeep").has_value());
    CHECK(reg.find_package("PkgKeep").has_value());
    CHECK_FALSE(reg.find_component("FaultComp").has_value());
    CHECK_FALSE(reg.find_component("CompRenamed").has_value());
    CHECK_FALSE(reg.component_by_name("CompRenamed").has_value());
    CHECK(reg.component_by_name("CompKeep").has_value());
    CHECK_FALSE(reg.find_package("PkgRenamed").has_value());

    // 解除故障：同样的操作必须成功并真的落盘
    if (fs::exists(comp.string() + ".tmp"))
    {
        fs::remove_all(comp.string() + ".tmp");
        fs::remove_all(pkg.string() + ".tmp");
    }
    reg.bind_component_types_path(comp.string());
    reg.bind_package_types_path(pkg.string());

    auto added_comp = reg.add_component_type_persistent(new_comp);
    REQUIRE(added_comp.has_value());
    auto added_pkg = reg.add_package_type_persistent(new_pkg);
    REQUIRE(added_pkg.has_value());
    REQUIRE(reg.update_component_type_persistent("CompKeep", rename_comp).has_value());
    REQUIRE(reg.update_package_type_persistent("PkgKeep", rename_pkg).has_value());
    REQUIRE(reg.remove_component_type_persistent(*added_comp).has_value());
    REQUIRE(reg.remove_package_type_persistent(*added_pkg).has_value());

    CHECK(read(comp) != comp_bytes);
    CHECK(read(pkg) != pkg_bytes);
    std::string expected_comp = fix04_types_json(reg.component_types());
    std::string expected_pkg = fix04_types_json(reg.package_types());

    // 重启等价：换掉内存再从文件读回来，必须一致
    reg.load_component_types_json(R"({"types":[{"name":"Reset"}]})");
    reg.load_package_types_json(R"({"types":[{"name":"ResetPkg"}]})");
    REQUIRE(reg.load_component_types(comp.string()));
    REQUIRE(reg.load_package_types(pkg.string()));
    CHECK(fix04_types_json(reg.component_types()) == expected_comp);
    CHECK(fix04_types_json(reg.package_types()) == expected_pkg);
    // CompRenamed/PkgRenamed 是 name：改名只动名字，ID 仍是原来的 CompKeep/PkgKeep
    auto renamed_comp = reg.find_component("CompKeep");
    REQUIRE(renamed_comp.has_value());
    CHECK(renamed_comp->name == "CompRenamed");
    CHECK(reg.component_by_name("CompRenamed").has_value());
    CHECK_FALSE(reg.component_by_name("CompKeep").has_value());
    auto renamed_pkg = reg.find_package("PkgKeep");
    REQUIRE(renamed_pkg.has_value());
    CHECK(renamed_pkg->name == "PkgRenamed");
    CHECK(reg.package_by_name("PkgRenamed").has_value());
    CHECK_FALSE(reg.package_by_name("PkgKeep").has_value());
    CHECK_FALSE(reg.find_component("FaultComp").has_value());
    CHECK_FALSE(reg.find_package("FaultPkg").has_value());
}

TEST_CASE_FIXTURE(LibraryFixture, "FIX04 a type update distinguishes omitted fields from explicit empty")
{
    RegistrySnapshot restore;
    auto& reg = core::TypeRegistry::instance();
    auto path = temp.path() / "fix04_patch.json";
    write(path, R"({"types":[{"name":"Keep","icon":"*","color":"#fff"}]})");
    REQUIRE(reg.load_component_types(path.string()));

    // 只改名：图标/颜色必须原样保留（旧实现会一起清空）
    core::TypePatch rename;
    rename.name = "Renamed";
    REQUIRE(reg.update_component_type_persistent("Keep", rename).has_value());
    auto renamed = reg.find_component("Keep");  // ID 不变
    REQUIRE(renamed.has_value());
    CHECK(renamed->name == "Renamed");
    CHECK(renamed->icon == "*");
    CHECK(renamed->color == "#fff");
    CHECK(reg.component_by_name("Renamed").has_value());
    CHECK_FALSE(reg.component_by_name("Keep").has_value());

    // 显式空串 = 清空，且只清显式给出的那一个
    core::TypePatch clear_icon;
    clear_icon.icon = "";
    REQUIRE(reg.update_component_type_persistent("Keep", clear_icon).has_value());
    auto cleared = reg.find_component("Keep");
    REQUIRE(cleared.has_value());
    CHECK(cleared->icon.empty());
    CHECK(cleared->color == "#fff");
    // 磁盘与内存一致：icon 已清（字段省略），color 没被碰过（字段与原值都在）
    auto on_disk = nlohmann::json::parse(read(path));
    REQUIRE(on_disk.contains("types"));
    REQUIRE(on_disk["types"].size() == 1);
    CHECK_FALSE(on_disk["types"][0].contains("icon"));
    REQUIRE(on_disk["types"][0].contains("color"));
    CHECK(on_disk["types"][0]["color"] == "#fff");

    // 显式清空 color：字段与值都真的没了，重载后依然为空
    core::TypePatch clear_color;
    clear_color.color = "";
    REQUIRE(reg.update_component_type_persistent("Keep", clear_color).has_value());
    auto emptied = reg.find_component("Keep");
    REQUIRE(emptied.has_value());
    CHECK(emptied->icon.empty());
    CHECK(emptied->color.empty());
    auto emptied_disk = nlohmann::json::parse(read(path));
    REQUIRE(emptied_disk["types"].size() == 1);
    CHECK_FALSE(emptied_disk["types"][0].contains("color"));
    CHECK_FALSE(emptied_disk["types"][0].contains("icon"));
    REQUIRE(reg.load_component_types(path.string()));
    auto reloaded = reg.find_component("Keep");
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->name == "Renamed");
    CHECK(reloaded->icon.empty());
    CHECK(reloaded->color.empty());
}

TEST_CASE_FIXTURE(LibraryFixture, "FIX04 concurrent persistent type mutations keep every change on disk")
{
    RegistrySnapshot restore;
    auto& reg = core::TypeRegistry::instance();
    auto comp = temp.path() / "fix04_thread_comp.json";
    auto pkg = temp.path() / "fix04_thread_pkg.json";
    write(comp, R"({"types":[{"name":"CompBase"}]})");
    write(pkg, R"({"types":[{"name":"PkgBase","category":"Other"}]})");
    REQUIRE(reg.load_component_types(comp.string()));
    REQUIRE(reg.load_package_types(pkg.string()));

    // RaceOne/RaceTwo 是 name；真实的 ID 由持久化入口生成（make_custom_id），
    // 断言必须用返回的 ID，不能拿名字当 ID 查
    bool first_ok = false;
    bool second_ok = false;
    std::string race_one_id;
    std::string race_two_id;
    fix04_run_together(
        [&]
        {
            core::TypeEntry entry;
            entry.name = "RaceOne";
            auto added = reg.add_component_type_persistent(entry);
            first_ok = added.has_value();
            if (added) race_one_id = *added;
        },
        [&]
        {
            core::TypeEntry entry;
            entry.name = "RaceTwo";
            auto added = reg.add_component_type_persistent(entry);
            second_ok = added.has_value();
            if (added) race_two_id = *added;
        });
    CHECK(first_ok);
    CHECK(second_ok);
    CHECK_FALSE(race_one_id.empty());
    CHECK_FALSE(race_two_id.empty());
    CHECK(race_one_id != race_two_id);

    reg.load_component_types_json(R"({"types":[{"name":"Reset"}]})");
    REQUIRE(reg.load_component_types(comp.string()));
    auto race_one = reg.find_component(race_one_id);
    REQUIRE(race_one.has_value());
    CHECK(race_one->name == "RaceOne");
    auto race_two = reg.find_component(race_two_id);
    REQUIRE(race_two.has_value());
    CHECK(race_two->name == "RaceTwo");
    CHECK(reg.component_by_name("RaceOne").has_value());
    CHECK(reg.component_by_name("RaceTwo").has_value());

    // 两种 kind 各自并发：两份文件都要留下各自的条目（同样按返回的 ID 查）
    std::string thread_comp_id;
    std::string thread_pkg_id;
    fix04_run_together(
        [&]
        {
            core::TypeEntry entry;
            entry.name = "ThreadComp";
            auto added = reg.add_component_type_persistent(entry);
            first_ok = added.has_value();
            if (added) thread_comp_id = *added;
        },
        [&]
        {
            core::TypeEntry entry;
            entry.name = "ThreadPkg";
            entry.category = "Other";
            auto added = reg.add_package_type_persistent(entry);
            second_ok = added.has_value();
            if (added) thread_pkg_id = *added;
        });
    CHECK(first_ok);
    CHECK(second_ok);
    CHECK_FALSE(thread_comp_id.empty());
    CHECK_FALSE(thread_pkg_id.empty());

    reg.load_component_types_json(R"({"types":[{"name":"Reset"}]})");
    reg.load_package_types_json(R"({"types":[{"name":"ResetPkg"}]})");
    REQUIRE(reg.load_component_types(comp.string()));
    REQUIRE(reg.load_package_types(pkg.string()));
    for (const auto& id : {race_one_id, race_two_id, thread_comp_id})
    {
        auto found = reg.find_component(id);
        REQUIRE(found.has_value());
    }
    auto thread_comp = reg.find_component(thread_comp_id);
    REQUIRE(thread_comp.has_value());
    CHECK(thread_comp->name == "ThreadComp");
    CHECK(reg.component_by_name("RaceOne").has_value());
    CHECK(reg.component_by_name("RaceTwo").has_value());
    CHECK(reg.component_by_name("ThreadComp").has_value());
    auto thread_pkg = reg.find_package(thread_pkg_id);
    REQUIRE(thread_pkg.has_value());
    CHECK(thread_pkg->name == "ThreadPkg");
    CHECK(reg.package_by_name("ThreadPkg").has_value());
}

TEST_CASE_FIXTURE(LibraryFixture, "FIX04 malformed type files are rejected whole and never become the save target")
{
    RegistrySnapshot restore;
    auto& reg = core::TypeRegistry::instance();
    auto good = temp.path() / "fix04_good.json";
    write(good, R"({"types":[{"name":"Good"}]})");
    REQUIRE(reg.load_component_types(good.string()));
    std::string good_bytes = read(good);
    std::string comp_json = fix04_types_json(reg.component_types());
    std::string pkg_json = fix04_types_json(reg.package_types());

    // 字段类型不对：整份文件拒绝，不能只丢这一条（旧实现直接抛出/悄悄丢条目）
    auto broken = temp.path() / "fix04_broken.json";
    write(broken, R"({"types":[{"name":"WouldDrop"},{"name":"Bad","icon":7}]})");
    std::string broken_bytes = read(broken);
    CHECK_FALSE(reg.load_component_types(broken.string()));
    CHECK_FALSE(reg.find_component("WouldDrop").has_value());
    CHECK(fix04_types_json(reg.component_types()) == comp_json);
    // 坏文件绝不能成为新的保存目标：否则下次保存会把它盖掉
    CHECK(reg.component_types_path() == good.string());
    CHECK(read(broken) == broken_bytes);

    auto bad_pkg = temp.path() / "fix04_bad_pkg.json";
    write(bad_pkg, R"({"types":[{"name":"P1","category":"Other"},{"id":"P2","name":"P2","category":9}]})");
    CHECK_FALSE(reg.load_package_types(bad_pkg.string()));
    CHECK_FALSE(reg.find_package("P1").has_value());
    CHECK(fix04_types_json(reg.package_types()) == pkg_json);

    // 文件缺失：记住路径（首次保存要能落在那里），内存保持不动
    auto fresh = temp.path() / "fix04_missing.json";
    CHECK_FALSE(reg.load_component_types(fresh.string()));
    CHECK(reg.component_types_path() == fresh.string());
    CHECK(fix04_types_json(reg.component_types()) == comp_json);

    core::TypeEntry entry;
    entry.name = "Written";
    auto written_id = reg.add_component_type_persistent(entry);
    REQUIRE(written_id.has_value());
    CHECK(fs::exists(fresh));
    CHECK(read(broken) == broken_bytes);
    CHECK(read(good) == good_bytes);
    // Written 是 name；ID 是生成的，落盘后按 ID 与名字都要能查到
    CHECK(reg.find_component(*written_id).has_value());
    CHECK(reg.component_by_name("Written").has_value());
    REQUIRE(reg.load_component_types(fresh.string()));
    CHECK(reg.find_component(*written_id).has_value());
    CHECK(reg.component_by_name("Written").has_value());
}

TEST_CASE_FIXTURE(LibraryFixture, "FIX04 a directory instead of a type file keeps the old save target")
{
    RegistrySnapshot restore;
    auto& reg = core::TypeRegistry::instance();
    auto good_comp = temp.path() / "fix04_dir_good_comp.json";
    auto good_pkg = temp.path() / "fix04_dir_good_pkg.json";
    write(good_comp, R"({"types":[{"id":"CompKeep","name":"CompKeep","icon":"c","color":"#111"}]})");
    write(good_pkg, R"({"types":[{"id":"PkgKeep","name":"PkgKeep","category":"Other"}]})");
    REQUIRE(reg.load_component_types(good_comp.string()));
    REQUIRE(reg.load_package_types(good_pkg.string()));
    std::string comp_bytes = read(good_comp);
    std::string pkg_bytes = read(good_pkg);
    std::string comp_json = fix04_types_json(reg.component_types());
    std::string pkg_json = fix04_types_json(reg.package_types());

    // 目录冒充类型文件：读取失败不是「文件缺失」，保存目标必须留在原来的好文件上
    auto bad_comp = temp.path() / "fix04_dir_bad_comp.json";
    auto bad_pkg = temp.path() / "fix04_dir_bad_pkg.json";
    fs::create_directories(bad_comp);
    fs::create_directories(bad_pkg);
    CHECK_FALSE(reg.load_component_types(bad_comp.string()));
    CHECK_FALSE(reg.load_package_types(bad_pkg.string()));
    CHECK(reg.component_types_path() == good_comp.string());
    CHECK(reg.package_types_path() == good_pkg.string());
    CHECK(fix04_types_json(reg.component_types()) == comp_json);
    CHECK(fix04_types_json(reg.package_types()) == pkg_json);

    // 保存仍落旧 good 文件，两个坏目录一个字节都不动
    core::TypeEntry comp_entry;
    comp_entry.name = "AfterDirComp";
    auto comp_id = reg.add_component_type_persistent(comp_entry);
    REQUIRE(comp_id.has_value());
    core::TypeEntry pkg_entry;
    pkg_entry.name = "AfterDirPkg";
    pkg_entry.category = "Other";
    auto pkg_id = reg.add_package_type_persistent(pkg_entry);
    REQUIRE(pkg_id.has_value());

    CHECK(read(good_comp) != comp_bytes);
    CHECK(read(good_pkg) != pkg_bytes);
    CHECK(fs::is_directory(bad_comp));
    CHECK(fs::is_directory(bad_pkg));
    CHECK(fs::is_empty(bad_comp));
    CHECK(fs::is_empty(bad_pkg));

    // 重载 old good 文件：新条目按生成 ID 与名字都能查到，旧条目仍在
    reg.load_component_types_json(R"({"types":[{"name":"Reset"}]})");
    reg.load_package_types_json(R"({"types":[{"name":"ResetPkg"}]})");
    REQUIRE(reg.load_component_types(good_comp.string()));
    REQUIRE(reg.load_package_types(good_pkg.string()));
    CHECK(reg.find_component(*comp_id).has_value());
    CHECK(reg.component_by_name("AfterDirComp").has_value());
    CHECK(reg.find_package(*pkg_id).has_value());
    CHECK(reg.package_by_name("AfterDirPkg").has_value());
    CHECK(reg.find_component("CompKeep").has_value());
    CHECK(reg.find_package("PkgKeep").has_value());
}

TEST_CASE_FIXTURE(LibraryFixture, "FIX04 persistent delete refuses referenced types and a failed reference query")
{
    RegistrySnapshot restore;
    auto& reg = core::TypeRegistry::instance();
    auto comp = temp.path() / "fix04_del_comp.json";
    auto pkg = temp.path() / "fix04_del_pkg.json";
    write(comp, R"({"types":[{"name":"CompBase"}]})");
    write(pkg, R"({"types":[{"name":"PkgBase","category":"Other"}]})");
    REQUIRE(reg.load_component_types(comp.string()));
    REQUIRE(reg.load_package_types(pkg.string()));
    // 小注册表重载后，局部匹配配置也换成合法的空表并重载：
    // 种子规则引用的内置类型已不在案，删除入口传入的 store 必须与当前注册表一致。
    // u04a_config 定义在文件后段，这里直接写符合 schema 的 JSON。
    write(matching_config, R"({"schema_version":1,"revision":0,"rules":[],"exceptions":[]})");
    REQUIRE(matching_store.load(matching_config.string()).has_value());

    core::TypeEntry comp_entry;
    comp_entry.name = "Deletable";
    auto comp_id = reg.add_component_type_persistent(comp_entry);
    REQUIRE(comp_id.has_value());
    core::TypeEntry pkg_entry;
    pkg_entry.name = "DeletablePkg";
    pkg_entry.category = "Other";
    auto pkg_id = reg.add_package_type_persistent(pkg_entry);
    REQUIRE(pkg_id.has_value());

    auto sym = symbol();
    {
        auto session = db->session();
        storage::SymbolRepository repo(session);
        auto row = repo.find_by_id(sym.id());
        REQUIRE(row.has_value());
        row->package_type_id = *pkg_id;
        REQUIRE(repo.update(*row).has_value());
    }
    std::string comp_bytes = read(comp);
    std::string pkg_bytes = read(pkg);
    std::string comp_json = fix04_types_json(reg.component_types());
    std::string pkg_json = fix04_types_json(reg.package_types());

    // 引用查询被打断：这是错误，不是"0 引用"，必须拒绝删除
    static int progress_calls = 0;
    progress_calls = 0;
    {
        auto session = db->session();
        sqlite3_progress_handler(session.handle(), 1,
                                 [](void* data)
                                 {
                                     ++*static_cast<int*>(data);
                                     return 1;
                                 },
                                 &progress_calls);
    }
    auto interrupted = api::remove_package_type(*db, *pkg_id);
    {
        auto session = db->session();
        sqlite3_progress_handler(session.handle(), 0, nullptr, nullptr);
    }
    CHECK(progress_calls > 0);
    CHECK_FALSE(interrupted.has_value());
    CHECK(reg.find_package(*pkg_id).has_value());
    CHECK(fix04_types_json(reg.package_types()) == pkg_json);
    CHECK(read(pkg) == pkg_bytes);

    // symbols.package_type 引用（footprints 里没有）：照样拒删，并回报引用数
    auto referenced = api::remove_package_type(*db, *pkg_id);
    REQUIRE(referenced.has_value());
    CHECK_FALSE(referenced->removed);
    CHECK(referenced->references == 1);
    CHECK(reg.find_package(*pkg_id).has_value());
    CHECK(fix04_types_json(reg.package_types()) == pkg_json);
    CHECK(read(pkg) == pkg_bytes);

    // 元件类型看 symbols.component_type
    {
        auto session = db->session();
        storage::SymbolRepository repo(session);
        auto row = repo.find_by_id(sym.id());
        REQUIRE(row.has_value());
        row->component_type_id = *comp_id;
        REQUIRE(repo.update(*row).has_value());
    }
    auto comp_referenced = api::remove_component_type(*db, *comp_id, &matching_store);
    REQUIRE(comp_referenced.has_value());
    CHECK_FALSE(comp_referenced->removed);
    CHECK(comp_referenced->references == 1);
    CHECK(reg.find_component(*comp_id).has_value());
    CHECK(fix04_types_json(reg.component_types()) == comp_json);
    CHECK(read(comp) == comp_bytes);

    // 内置类型仍然拒删（引用计数对它们没有意义）
    auto builtin = api::remove_component_type(*db, "Capacitor", &matching_store);
    REQUIRE(builtin.has_value());
    CHECK_FALSE(builtin->removed);

    // 解除引用后可删，磁盘上也要真的没有了
    {
        auto session = db->session();
        storage::SymbolRepository repo(session);
        auto row = repo.find_by_id(sym.id());
        REQUIRE(row.has_value());
        row->component_type_id.clear();
        row->package_type_id.clear();
        REQUIRE(repo.update(*row).has_value());
    }
    auto comp_removed = api::remove_component_type(*db, *comp_id, &matching_store);
    REQUIRE(comp_removed.has_value());
    CHECK(comp_removed->removed);
    CHECK(comp_removed->references == 0);
    auto pkg_removed = api::remove_package_type(*db, *pkg_id);
    REQUIRE(pkg_removed.has_value());
    CHECK(pkg_removed->removed);
    CHECK_FALSE(reg.find_component(*comp_id).has_value());
    CHECK_FALSE(reg.find_package(*pkg_id).has_value());

    std::string comp_after = fix04_types_json(reg.component_types());
    std::string pkg_after = fix04_types_json(reg.package_types());
    REQUIRE(reg.load_component_types(comp.string()));
    REQUIRE(reg.load_package_types(pkg.string()));
    CHECK(fix04_types_json(reg.component_types()) == comp_after);
    CHECK(fix04_types_json(reg.package_types()) == pkg_after);
}

// ============================================================
// LCSC01 —— Import LCSC 重写：元件库包（符号库 + 封装库）作为导入目标
// 目标是一个 component_libraries 行（包）：符号写进包的符号路径
// （目录 → LCSC.kicad_sym；直接配置的 .kicad_sym 文件 → 该文件），
// 封装写进包的封装路径（.pretty → 直接落该目录；普通根目录 → 其下
// <符号文件 stem>.pretty）。空包首次导入不提前登记符号库行，
// 提交成功后按来源键补齐。旧 target_library_id 入口语义保持不变。
// ============================================================
namespace
{
/// 只登记元件库包（component_libraries 行），不预建符号库行 ——
/// 「空包首次导入」的前提
std::string lcsc01_register_package(storage::Database& db, const std::string& name,
                                    const std::string& symbol_path,
                                    const std::string& footprint_path, bool enabled = true)
{
    auto session = db.session();
    storage::ComponentLibraryRepository::ComponentLibrary group;
    group.name = name;
    group.symbol_path = symbol_path;
    group.footprint_path = footprint_path;
    group.enabled = enabled;
    auto inserted = storage::ComponentLibraryRepository(session).insert(group);
    REQUIRE(inserted.has_value());
    return inserted->id;
}

/// 直接改包的锁定标志（模拟用户在界面上锁库）
void lcsc01_lock_package(storage::Database& db, const std::string& id)
{
    auto session = db.session();
    auto row = storage::ComponentLibraryRepository(session).find_all();
    REQUIRE(row.has_value());
    for (const auto& group : *row)
    {
        if (group.id != id)
            continue;
        auto updated = group;
        updated.locked = true;
        REQUIRE(storage::ComponentLibraryRepository(session).update(updated).has_value());
        return;
    }
    FAIL("package not found: ", id);
}

/// 一个「元件库包模式」的请求骨架
api::StagedImporter::Request lcsc01_request(const std::string& package_id,
                                            const std::string& external_id,
                                            const std::string& operation_id)
{
    api::StagedImporter::Request request;
    request.plugin_id = "stub.lcsc";
    request.external_id = external_id;
    request.target_component_library_id = package_id;
    request.footprint_mode = "new";
    request.operation_id = operation_id;
    return request;
}

// LCSC01 专用桩：内容只由零件号决定（Stamp 用零件号，不用暂存目录名）。
// 通用桩的 Stamp 每次重跑都不同，那是给「同源更新是替换」用的；
// 元件库包模式要验证 no-op（同源 + 最终宿主输出一致），需要可复现的产物。
const char* LCSC01_STUB_PLUGIN_PY = R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
lcsc = args.get("lcsc_id", "")
# 尾缀 X 只是换个来源编号，转换出的符号名相同 —— 用来构造「异源同名」
base = lcsc[:-1] if lcsc.endswith("X") else lcsc
name = "STUB_" + base
symbol = ('(symbol "%s" (property "Reference" "U") (property "Footprint" "old:fp")'
          ' (property "Stamp" "%s"))') % (name, base)
with open(os.path.join(staging, name + ".kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator stub)\n" + symbol + "\n)\n")
mode = (args.get("footprint_choice") or {}).get("mode", "new")
entry = None
if mode != "reuse":
    with open(os.path.join(staging, "STUBFP.kicad_mod"), "w", encoding="utf-8") as f:
        f.write('(footprint "STUBFP" (layer "F.Cu"))')
    entry = {"name": "STUBFP", "file": "STUBFP.kicad_mod"}
manifest = {"protocol": "staged-import-v1", "lcsc_id": lcsc,
            "symbols": [{"name": name, "file": name + ".kicad_sym"}], "footprint": entry}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY";

/// 最小 staged-import-v1 插件环境（LCSC01 桩按编号产出可复现的符号名）。
/// PluginManager 不可拷贝/移动，用指针返回，调用方就地使用。
std::unique_ptr<plugin::PluginManager> lcsc01_plugins(const fs::path& scan)
{
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", LCSC01_STUB_PLUGIN_PY);
    auto manager = std::make_unique<plugin::PluginManager>(std::vector<fs::path>{scan});
    manager->discover();
    return manager;
}
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB protocol rejects a child library outside package mode")
{
    nlohmann::json body{{"lcsc_id", "C811"}, {"target_library_id", "legacy"},
                        {"symbol_library_choice", {{"mode", "existing"},
                                                    {"path", "Device.kicad_sym"}}}};
    CHECK_FALSE(api::parse_staged_request(body, "stub.lcsc", "import").has_value());
    body.erase("target_library_id");
    body["target_component_library_id"] = "package";
    body["footprint_choice"] = {{"mode", "new"}};
    for (const auto& malformed : {nlohmann::json(nullptr), nlohmann::json::object(),
                                  nlohmann::json{{"mode", "existing"}},
                                  nlohmann::json{{"mode", "new"}, {"name", "../Outside"}},
                                  nlohmann::json{{"mode", "existing"},
                                                 {"path", "../Outside.kicad_sym"}},
                                  nlohmann::json{{"mode", "new"},
                                                 {"name", "X.kicad_sym.KICAD_SYM"}},
                                  nlohmann::json{{"mode", "new"}, {"name", 42}},
                                  nlohmann::json{{"mode", "existing"},
                                                 {"path", "C:/Outside.kicad_sym"}},
                                  nlohmann::json{{"mode", "existing"},
                                                 {"path", "nested\\Device.kicad_sym"}},
                                  nlohmann::json{{"mode", "existing"},
                                                 {"path", "nested//Device.kicad_sym"}},
                                  nlohmann::json{{"mode", "new"}, {"name", "CON"}}})
    {
        body["symbol_library_choice"] = malformed;
        CHECK_FALSE(api::parse_staged_request(body, "stub.lcsc", "import").has_value());
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB selected nested child is the actual import target")
{
    if (!audit_python_available()) return;
    const auto root = temp.path() / "lcscsub_selected";
    const auto symbols = root / "symbols";
    const auto chosen = symbols / "nested" / "Device.kicad_sym";
    const auto untouched = symbols / "Device.kicad_sym";
    write(chosen, "");  // 真正的零字节空子库同样可导入。
    write(untouched, source);
    const auto owner = lcsc01_register_package(*db, "Selected child", symbols.string(),
                                              (root / "footprints").string());
    std::string registered_id;
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "Device";
        meta.file_path = chosen;
        meta.component_library_id = owner;
        auto registered = storage::LibraryRepository(session).insert(meta);
        REQUIRE(registered.has_value());
        registered_id = registered->id;
    }
    auto plugins = lcsc01_plugins(temp.path() / "lcscsub_plugins");
    api::StagedImporter importer(*db, *plugins, &matching_store);
    nlohmann::json body{{"lcsc_id", "C811"}, {"target_component_library_id", owner},
                        {"operation_id", "lcscsub-selected"},
                        {"footprint_choice", {{"mode", "new"}}},
                        {"symbol_library_choice", {{"mode", "existing"},
                                                    {"path", "nested/Device.kicad_sym"}}}};
    auto parsed = api::parse_staged_request(body, "stub.lcsc", "import");
    REQUIRE(parsed.has_value());
    auto imported = importer.run(parsed->request);
    REQUIRE(imported.has_value());
    CHECK(read(chosen).find("STUB_C811") != std::string::npos);
    CHECK(read(untouched) == source);
    CHECK_FALSE(fs::exists(symbols / "LCSC.kicad_sym"));
    {
        auto session = db->session();
        auto target = storage::LibraryRepository(session).find_by_source_key(storage::source_key(chosen));
        REQUIRE(target.has_value());
        CHECK(target->component_library_id == owner);
        CHECK(target->id == registered_id);
    }
    const auto selected_content = read(chosen);
    body["symbol_library_choice"]["path"] = "Device.kicad_sym";
    auto changed = api::parse_staged_request(body, "stub.lcsc", "import");
    REQUIRE(changed.has_value());
    CHECK_FALSE(importer.run(changed->request).has_value());
    CHECK(read(chosen) == selected_content);
    CHECK(read(untouched) == source);
    changed->request.operation_id = "lcscsub-other-child";
    auto other = importer.run(changed->request);
    REQUIRE(other.has_value());
    CHECK(other->at("symbol_id") != imported->at("symbol_id"));
    CHECK(read(untouched).find("STUB_C811") != std::string::npos);
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB new child name chooses its own file and footprint library")
{
    if (!audit_python_available()) return;
    const auto root = temp.path() / "lcscsub_new";
    const auto symbols = root / "symbols";
    const auto owner = lcsc01_register_package(*db, "New child", symbols.string(),
                                              (root / "footprints").string());
    auto plugins = lcsc01_plugins(temp.path() / "lcscsub_new_plugins");
    api::StagedImporter importer(*db, *plugins, &matching_store);
    nlohmann::json body{{"lcsc_id", "C812"}, {"target_component_library_id", owner},
                        {"operation_id", "lcscsub-new"},
                        {"footprint_choice", {{"mode", "new"}}},
                        {"symbol_library_choice", {{"mode", "new"},
                                                    {"name", "Switches.kicad_sym"}}}};
    auto parsed = api::parse_staged_request(body, "stub.lcsc", "import");
    REQUIRE(parsed.has_value());
    auto imported = importer.run(parsed->request);
    REQUIRE(imported.has_value());
    CHECK(fs::is_regular_file(symbols / "Switches.kicad_sym"));
    CHECK_FALSE(fs::exists(symbols / "LCSC.kicad_sym"));
    CHECK(imported->value("footprint_reference", "") == "Switches:STUBFP");
    auto replayed = importer.run(parsed->request);
    REQUIRE(replayed.has_value());
    CHECK(replayed->value("replayed", false));
    parsed->request.operation_id = "lcscsub-new-collision";
    CHECK_FALSE(importer.run(parsed->request).has_value());
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB listing includes physical empty and unregistered nested files")
{
    const auto root = temp.path() / "lcscsub_list";
    const auto symbols = root / "symbols";
    write(symbols / "Device.kicad_sym", "");
    write(symbols / "nested" / "Device.kicad_sym", "");
    write(symbols / "Unparsed.kicad_sym", "this is deliberately not a parsed symbol library");
    fs::create_directories(symbols / "Directory.kicad_sym");
    const auto owner = lcsc01_register_package(*db, "Listing", symbols.string(),
                                              (root / "footprints").string());
    std::string registered_id;
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "Device";
        meta.file_path = symbols / "Device.kicad_sym";
        meta.component_library_id = owner;
        auto registered = storage::LibraryRepository(session).insert(meta);
        REQUIRE(registered.has_value());
        registered_id = registered->id;
    }
    auto listed = api::list_component_symbol_libraries(*db, owner);
    REQUIRE(listed.has_value());
    CHECK(listed->value("can_create", false));
    REQUIRE(listed->at("libraries").size() == 3);
    CHECK(listed->at("libraries")[0] == nlohmann::json{{"name", "Device"},
          {"path", "Device.kicad_sym"}, {"id", registered_id}});
    CHECK(listed->at("libraries")[2] == nlohmann::json{{"name", "Device"},
          {"path", "nested/Device.kicad_sym"}, {"id", ""}});
    auto session = db->session();
    auto registered = storage::LibraryRepository(session).find_all();
    REQUIRE(registered.has_value());
    CHECK(registered->size() == 1);  // 只读列表不会登记物理文件。
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB listing distinguishes missing directories and single files")
{
    const auto root = temp.path() / "lcscsub_missing";
    const auto missing_dir = root / "not-created";
    const auto directory_owner = lcsc01_register_package(*db, "Directory", missing_dir.string(),
                                                        (root / "footprints").string());
    auto directory = api::list_component_symbol_libraries(*db, directory_owner);
    REQUIRE(directory.has_value());
    CHECK(directory->at("libraries").empty());
    CHECK(directory->at("can_create") == true);
    CHECK_FALSE(fs::exists(missing_dir));
    const auto direct = root / "Direct.kicad_sym";
    const auto file_owner = lcsc01_register_package(*db, "Direct", direct.string(),
                                                   (root / "direct-footprints").string());
    auto missing = api::list_component_symbol_libraries(*db, file_owner);
    REQUIRE(missing.has_value());
    CHECK(missing->at("libraries").empty());
    CHECK(missing->at("can_create") == false);
    write(direct, "");
    auto existing = api::list_component_symbol_libraries(*db, file_owner);
    REQUIRE(existing.has_value());
    CHECK(existing->at("libraries") == nlohmann::json::array({
        {{"name", "Direct"}, {"path", "Direct.kicad_sym"}, {"id", ""}}}));
    CHECK_FALSE(api::list_component_symbol_libraries(*db, "missing-package").has_value());
    write(root / "wrong-kind", "ordinary file");
    const auto wrong_owner = lcsc01_register_package(*db, "Wrong", (root / "wrong-kind").string(),
                                                    (root / "wrong-fp").string());
    CHECK_FALSE(api::list_component_symbol_libraries(*db, wrong_owner).has_value());
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB existing selection rejects absent directories and foreign ownership")
{
    const auto root = temp.path() / "lcscsub_bad_existing";
    const auto symbols = root / "symbols";
    fs::create_directories(symbols / "Directory.kicad_sym");
    write(symbols / "Foreign.kicad_sym", source);
    const auto owner = lcsc01_register_package(*db, "Mine", symbols.string(),
                                              (root / "footprints").string());
    const auto other = lcsc01_register_package(*db, "Other", (root / "elsewhere").string(),
                                              (root / "other-fp").string());
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "Foreign";
        meta.file_path = symbols / "Foreign.kicad_sym";
        meta.component_library_id = other;
        REQUIRE(storage::LibraryRepository(session).insert(meta).has_value());
    }
    plugin::PluginManager plugins(std::vector<fs::path>{});  // 预检失败须早于插件查找。
    api::StagedImporter importer(*db, plugins, &matching_store);
    auto request = lcsc01_request(owner, "C813", "lcscsub-invalid-existing");
    request.symbol_library_mode = "existing";
    for (const auto* path : {"Missing.kicad_sym", "Directory.kicad_sym", "Foreign.kicad_sym",
                              "../Outside.kicad_sym", "/Outside.kicad_sym", "C:/Outside.kicad_sym"})
    {
        request.symbol_library_path = path;
        auto rejected = importer.run(request);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().kind() == util::Error::Kind::ValidationError);
    }
    CHECK_FALSE(api::list_component_symbol_libraries(*db, owner).has_value());
    CHECK(read(symbols / "Foreign.kicad_sym") == source);
    CHECK_FALSE(fs::exists(symbols / "LCSC.kicad_sym"));
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB single-file configuration permits only its own existing file")
{
    if (!audit_python_available()) return;
    const auto root = temp.path() / "lcscsub_direct";
    const auto direct = root / "Direct.kicad_sym";
    write(direct, "");
    const auto owner = lcsc01_register_package(*db, "Direct", direct.string(),
                                              (root / "footprints").string());
    auto plugins = lcsc01_plugins(temp.path() / "lcscsub_direct_plugins");
    api::StagedImporter importer(*db, *plugins, &matching_store);
    auto request = lcsc01_request(owner, "C814", "lcscsub-direct");
    request.symbol_library_mode = "new";
    request.symbol_library_name = "Other";
    CHECK_FALSE(importer.run(request).has_value());
    request.symbol_library_mode = "existing";
    request.symbol_library_name.clear();
    request.symbol_library_path = "Else.kicad_sym";
    CHECK_FALSE(importer.run(request).has_value());
    request.symbol_library_path = "nested/Direct.kicad_sym";
    CHECK_FALSE(importer.run(request).has_value());
    request.symbol_library_path = "Direct.kicad_sym";
    auto imported = importer.run(request);
    REQUIRE(imported.has_value());
    CHECK(read(direct).find("STUB_C814") != std::string::npos);
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB new child commit failure leaves no file or registration")
{
    if (!audit_python_available()) return;
    const auto root = temp.path() / "lcscsub_rollback";
    const auto symbols = root / "symbols";
    const auto owner = lcsc01_register_package(*db, "Rollback", symbols.string(),
                                              (root / "footprints").string());
    auto plugins = lcsc01_plugins(temp.path() / "lcscsub_rollback_plugins");
    api::StagedImporter importer(*db, *plugins, &matching_store);
    REQUIRE(db->execute("CREATE TRIGGER lcscsub_fail_symbol BEFORE INSERT ON symbols "
                        "BEGIN SELECT RAISE(ABORT, 'lcscsub forced rollback'); END").has_value());
    auto request = lcsc01_request(owner, "C815", "lcscsub-rollback");
    request.symbol_library_mode = "new";
    request.symbol_library_name = "Rollback";
    CHECK_FALSE(importer.run(request).has_value());
    CHECK_FALSE(fs::exists(symbols / "Rollback.kicad_sym"));
    CHECK_FALSE(fs::exists(root / "footprints" / "Rollback.pretty" / "STUBFP.kicad_mod"));
    {
        auto session = db->session();
        auto libraries = storage::LibraryRepository(session).find_all();
        REQUIRE(libraries.has_value());
        CHECK(libraries->empty());
    }
    REQUIRE(db->execute("DROP TRIGGER lcscsub_fail_symbol").has_value());
    CHECK(importer.run(request).has_value());
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB Unicode child names retain file names and footprint references")
{
    if (!audit_python_available()) return;
    const auto root = temp.path() / services::path_from_utf8("lcscsub_中文目录");
    const auto symbols = root / services::path_from_utf8("符号");
    const auto owner = lcsc01_register_package(*db, "Unicode", services::path_to_utf8(symbols),
                                              services::path_to_utf8(root / "footprints"));
    auto plugins = lcsc01_plugins(temp.path() / "lcscsub_unicode_plugins");
    api::StagedImporter importer(*db, *plugins, &matching_store);
    auto request = lcsc01_request(owner, "C816", "lcscsub-unicode");
    request.symbol_library_mode = "new";
    request.symbol_library_name = "开关.kicad_sym";
    auto imported = importer.run(request);
    if (!imported) MESSAGE(imported.error().format_message());
    REQUIRE(imported.has_value());
    const auto chosen = symbols / services::path_from_utf8("开关.kicad_sym");
    CHECK(fs::is_regular_file(chosen));
    CHECK(imported->value("footprint_reference", "") == "开关:STUBFP");
    auto listed = api::list_component_symbol_libraries(*db, owner);
    REQUIRE(listed.has_value());
    REQUIRE(listed->at("libraries").size() == 1);
    CHECK(listed->at("libraries")[0].at("path") == "开关.kicad_sym");
    request.symbol_library_mode = "existing";
    request.symbol_library_name.clear();
    request.symbol_library_path = "开关.kicad_sym";
    request.operation_id = "lcscsub-unicode-update";
    auto updated = importer.run(request);
    REQUIRE(updated.has_value());
    CHECK(updated->at("symbol_id") == imported->at("symbol_id"));
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB directory redirections are neither traversed nor selectable")
{
    if (!audit_python_available()) return;
    const auto root = temp.path() / "lcscsub_redirect";
    const auto symbols = root / "symbols";
    const auto outside = root / "outside";
    const auto redirect = symbols / "Linked";
    write(symbols / "Visible.kicad_sym", "");
    write(outside / "Hidden.kicad_sym", "");
    const auto scan = temp.path() / "lcscsub_redirect_plugins";
    auto plugins = lcsc01_plugins(scan);
    // 只在本测试临时树内建立重定向。Windows 使用不需要符号链接特权的目录联接。
    write(scan / "stub" / "plugin.py", R"PY(
import json, os, subprocess, sys
args = json.loads(sys.argv[2])
if os.name == 'nt':
    result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', args['link'], args['target']],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    print(json.dumps({'ok': result.returncode == 0,
                      'error': result.stderr.decode(errors='replace')}))
else:
    os.symlink(args['target'], args['link'], target_is_directory=True)
    print(json.dumps({'ok': True}))
)PY");
    auto created = plugins->execute("stub.lcsc", "import", nlohmann::json{
        {"link", services::path_to_utf8(redirect)},
        {"target", services::path_to_utf8(outside)}}.dump());
    REQUIRE(created.has_value());
    auto result = nlohmann::json::parse(*created);
    if (!result.value("ok", false)) MESSAGE(result.dump());
    REQUIRE(result.value("ok", false));
    const auto owner = lcsc01_register_package(*db, "Redirect", symbols.string(),
                                              (root / "footprints").string());
    auto listed = api::list_component_symbol_libraries(*db, owner);
    REQUIRE(listed.has_value());
    REQUIRE(listed->at("libraries").size() == 1);
    CHECK(listed->at("libraries")[0].at("path") == "Visible.kicad_sym");
    api::StagedImporter importer(*db, *plugins, &matching_store);
    auto request = lcsc01_request(owner, "C817", "lcscsub-redirect");
    request.symbol_library_mode = "existing";
    request.symbol_library_path = "Linked/Hidden.kicad_sym";
    auto rejected = importer.run(request);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().kind() == util::Error::Kind::ValidationError);
    const auto alias_owner = lcsc01_register_package(*db, "Alias root", redirect.string(),
                                                    (root / "alias-footprints").string());
    CHECK_FALSE(api::list_component_symbol_libraries(*db, alias_owner).has_value());
    CHECK(read(outside / "Hidden.kicad_sym").empty());
    CHECK(count_rows(*db, "libraries") == 0);
    CHECK(count_rows(*db, "import_operations") == 0);
    // 联接先解除，临时目录清理不能有机会递归进它的目标。
    std::error_code remove_error;
    CHECK(fs::remove(redirect, remove_error));
    CHECK_FALSE(remove_error);
    CHECK(fs::is_regular_file(outside / "Hidden.kicad_sym"));
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB file and package changes during plugin execution abort the import")
{
    if (!audit_python_available()) return;
    const auto root = temp.path() / "lcscsub_drift";
    const auto symbols = root / "symbols";
    const auto chosen = symbols / "Chosen.kicad_sym";
    const auto owner = lcsc01_register_package(*db, "Drift", symbols.string(),
                                              (root / "footprints").string());
    const auto scan = temp.path() / "lcscsub_drift_plugins";
    auto plugins = lcsc01_plugins(scan);
    api::StagedImporter importer(*db, *plugins, &matching_store);
    auto request = lcsc01_request(owner, "C818", "lcscsub-drift");
    request.symbol_library_mode = "new";
    request.symbol_library_name = "Chosen";
    const auto literal = [](const auto& text) { return nlohmann::json(text).dump(); };
    std::string prefix;
    bool competitor_file = false;
    bool deleted_existing = false;
    SUBCASE("another process creates the new child")
    {
        competitor_file = true;
        prefix = "import pathlib\n_p=pathlib.Path(" + literal(services::path_to_utf8(chosen)) +
                 ")\n_p.parent.mkdir(parents=True,exist_ok=True)\n_p.write_text('competitor',encoding='utf-8')\n";
    }
    SUBCASE("selected existing child disappears")
    {
        deleted_existing = true;
        write(chosen, "");
        request.symbol_library_mode = "existing";
        request.symbol_library_name.clear();
        request.symbol_library_path = "Chosen.kicad_sym";
        prefix = "import pathlib\npathlib.Path(" + literal(services::path_to_utf8(chosen)) + ").unlink()\n";
    }
    SUBCASE("the package becomes locked")
    {
        prefix = "import sqlite3\n_c=sqlite3.connect(" + literal(services::path_to_utf8(db->path())) +
                 ")\n_c.execute('UPDATE component_libraries SET locked=1 WHERE id=?',(" +
                 literal(owner) + ",))\n_c.commit()\n_c.close()\n";
    }
    SUBCASE("the package becomes disabled")
    {
        prefix = "import sqlite3\n_c=sqlite3.connect(" + literal(services::path_to_utf8(db->path())) +
                 ")\n_c.execute('UPDATE component_libraries SET enabled=0 WHERE id=?',(" +
                 literal(owner) + ",))\n_c.commit()\n_c.close()\n";
    }
    SUBCASE("the package symbol root changes")
    {
        prefix = "import sqlite3\n_c=sqlite3.connect(" + literal(services::path_to_utf8(db->path())) +
                 ")\n_c.execute('UPDATE component_libraries SET symbol_path=? WHERE id=?',(" +
                 literal(services::path_to_utf8(root / "different-symbols")) + "," + literal(owner) +
                 "))\n_c.commit()\n_c.close()\n";
    }
    REQUIRE_FALSE(prefix.empty());
    write(scan / "stub" / "plugin.py", prefix + LCSC01_STUB_PLUGIN_PY);
    auto rejected = importer.run(request);
    if (rejected) MESSAGE(rejected->dump());
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().kind() == util::Error::Kind::ValidationError);
    if (competitor_file)
        CHECK(read(chosen) == "competitor");
    else
        CHECK_FALSE(fs::exists(chosen));
    if (deleted_existing) CHECK_FALSE(fs::exists(chosen));
    CHECK_FALSE(fs::exists(root / "footprints" / "Chosen.pretty" / "STUBFP.kicad_mod"));
    CHECK(count_rows(*db, "libraries") == 0);
    CHECK(count_rows(*db, "symbols") == 0);
    CHECK(count_rows(*db, "import_sources") == 0);
    CHECK(count_rows(*db, "import_operations") == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB directory and reserved new names never become import destinations")
{
    const auto root = temp.path() / "lcscsub_new_names";
    const auto symbols = root / "symbols";
    fs::create_directories(symbols / "Taken.kicad_sym");
    const auto owner = lcsc01_register_package(*db, "Names", symbols.string(),
                                              (root / "footprints").string());
    plugin::PluginManager plugins(std::vector<fs::path>{});
    api::StagedImporter importer(*db, plugins, &matching_store);
    auto request = lcsc01_request(owner, "C819", "lcscsub-new-name");
    request.symbol_library_mode = "new";
    for (const auto& name : {std::string("Taken"), std::string("../Outside"), std::string("NUL.txt"),
                              std::string("Tail."), std::string("Tail "), std::string(""),
                              std::string("X.kicad_sym.kicad_sym"), std::string(250, 'A')})
    {
        request.symbol_library_name = name;
        auto rejected = importer.run(request);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().kind() == util::Error::Kind::ValidationError);
    }
    CHECK(fs::is_directory(symbols / "Taken.kicad_sym"));
    CHECK(count_rows(*db, "libraries") == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSCSUB a late new-file claimant survives atomic group publication")
{
    const auto owner = group();
    const auto newly_claimed = file.parent_path() / services::path_from_utf8("Late_中文.kicad_sym");
    const std::string competitor = "(kicad_symbol_lib (version 20231120) (symbol \"OTHER_PROCESS\"))";
    struct ClaimAtPreparedCommit
    {
        fs::path target;
        std::string content;
        bool fired = false;
        bool written = false;
    } claim{newly_claimed, competitor};
    {
        auto session = db->session();
        // prepared日志事务提交前，全部exists/digest预检已经做完，真实文件尚未替换。
        // 这里稳定模拟外部进程恰在这个窗口抢先创建同名目标，无睡眠/时间竞争。
        sqlite3_commit_hook(session.handle(), [](void* value) -> int
        {
            auto& context = *static_cast<ClaimAtPreparedCommit*>(value);
            if (!context.fired)
            {
                context.fired = true;
                std::ofstream output(context.target, std::ios::binary);
                output << context.content;
                output.close();
                context.written = !output.fail();
            }
            return 0;
        }, &claim);
    }
    bool business_called = false;
    io::SourceMutationService mutations(*db);
    auto changed = mutations.apply(
        {io::FileChange{.path = file,
                        .content = "(kicad_symbol_lib (version 20231120) (symbol \"FIRST_CHANGED\"))",
                        .expected_digest = io::content_digest(std::string(source)),
                        .owner = owner},
         io::FileChange{.path = newly_claimed,
                        .content = "(kicad_symbol_lib (version 20231120) (symbol \"OUR_NEW_FILE\"))",
                        .allow_missing = true,
                        .expected_missing = true,
                        .owner = owner}},
        [&](storage::DbSession&) -> util::Result<void>
        {
            business_called = true;
            return {};
        });
    {
        auto session = db->session();
        sqlite3_commit_hook(session.handle(), nullptr, nullptr);
    }
    CHECK(claim.fired);
    CHECK(claim.written);
    CHECK_FALSE(changed.has_value());
    if (!changed)
        CHECK_NOTHROW(nlohmann::json({{"error", changed.error().format_message()}}).dump());
    CHECK_FALSE(business_called);
    CHECK(read(file) == source);
    CHECK(read(newly_claimed) == competitor);
    CHECK(count_rows(*db, "file_mutations") == 0);
    CHECK(count_rows(*db, "libraries") == 0);
    for (const auto& entry : fs::directory_iterator(file.parent_path()))
    {
        CHECK(entry.path().extension() != ".tmp");
        CHECK(entry.path().extension() != ".bak");
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 component library mode imports into an empty package")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01a";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "LCSC", sym_dir.string(), fp_dir.string());

    auto manager = lcsc01_plugins(temp.path() / "plugins_lcsc01a");
    api::StagedImporter importer(*db, *manager, &matching_store);
    auto request = lcsc01_request(package_id, "C801", "lcsc01-a");

    auto result = importer.run(request);
    if (!result)
        MESSAGE("component library import failed: ", result.error().format_message());
    REQUIRE(result.has_value());
    CHECK(result->value("ok", false));

    auto symbol_file = sym_dir / "LCSC.kicad_sym";
    REQUIRE(fs::exists(symbol_file));
    CHECK(read(symbol_file).find("STUB_C801") != std::string::npos);
    CHECK(read(symbol_file).find("\"LCSC:STUBFP\"") != std::string::npos);
    CHECK(fs::exists(fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod"));
    CHECK(result->value("footprint_reference", std::string("")) == "LCSC:STUBFP");
    CHECK(result->value("created_library", false));

    std::string symbol_id = result->value("symbol_id", std::string(""));
    std::string footprint_id = result->value("footprint_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());
    REQUIRE_FALSE(footprint_id.empty());

    // 空包没有被提前写行；提交成功后按来源键登记，归属就是选中的包
    std::string library_id;
    {
        auto session = db->session();
        auto library = storage::LibraryRepository(session).find_by_source_key(
            storage::existing_source_key(symbol_file));
        REQUIRE(library.has_value());
        CHECK(library->component_library_id == package_id);
        CHECK(library->name == "LCSC");
        auto symbol = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(symbol.has_value());
        CHECK(symbol->library_id() == library->id);
        CHECK(symbol->footprint() == "LCSC:STUBFP");
        auto footprint = storage::FootprintRepository(session).find_by_id(footprint_id);
        REQUIRE(footprint.has_value());
        auto link = storage::RelationshipRepository(session).find_footprint_for_symbol(symbol_id);
        REQUIRE(link.has_value());
        REQUIRE(link->has_value());
        CHECK(**link == footprint_id);
        library_id = library->id;
    }
    // 会话已释放，才能用会自取会话的辅助函数
    CHECK(audit_scalar_text(*db,
                            "SELECT target_library_id FROM import_sources "
                            "WHERE provider='LCSC' AND external_id='C801'") == library_id);

    // 同 operation_id 回放：返回原结果，不写第二遍
    const std::string after_first = read(symbol_file);
    auto replay = importer.run(request);
    REQUIRE(replay.has_value());
    CHECK(replay->value("replayed", false));
    CHECK(replay->value("symbol_id", std::string("")) == symbol_id);
    CHECK(read(symbol_file) == after_first);

    // 不同 operation_id 的同来源导入：最终输出一致 = no-op，UUID 与文件都不变
    auto repeat = request;
    repeat.operation_id = "lcsc01-a2";
    auto again = importer.run(repeat);
    if (!again)
        MESSAGE("repeat import failed: ", again.error().format_message());
    REQUIRE(again.has_value());
    CHECK(again->value("noop", false));
    CHECK(again->value("symbol_id", std::string("")) == symbol_id);
    CHECK(again->value("footprint_id", std::string("")) == footprint_id);
    CHECK(read(symbol_file) == after_first);
    CHECK(count_rows(*db, "symbols") == 1);

    // 同一 operation_id 换了目标包 → 拒绝（摘要身份包含包 ID 与目标路径）
    auto other_root = temp.path() / "lcsc01a_other";
    fs::create_directories(other_root / "symbols");
    fs::create_directories(other_root / "footprints");
    auto other_package = lcsc01_register_package(*db, "LCSC2", (other_root / "symbols").string(),
                                                 (other_root / "footprints").string());
    auto switched = request;
    switched.target_component_library_id = other_package;
    CHECK_FALSE(importer.run(switched).has_value());
    CHECK_FALSE(fs::exists(other_root / "symbols" / "LCSC.kicad_sym"));

    // 同一 operation_id 换了模式 → 拒绝
    auto reused_mode = request;
    reused_mode.footprint_mode = "reuse";
    reused_mode.footprint_id = footprint_id;
    CHECK_FALSE(importer.run(reused_mode).has_value());
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 component library mode reuses a footprint without copying it")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01b";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "LCSC", sym_dir.string(), fp_dir.string());
    auto existing = footprint("CHOSEN");

    auto manager = lcsc01_plugins(temp.path() / "plugins_lcsc01b");
    api::StagedImporter importer(*db, *manager, &matching_store);
    auto request = lcsc01_request(package_id, "C802", "lcsc01-b");
    request.footprint_mode = "reuse";
    request.footprint_id = existing.id();

    auto result = importer.run(request);
    if (!result)
        MESSAGE("reuse import failed: ", result.error().format_message());
    REQUIRE(result.has_value());
    CHECK(result->value("mode", std::string("")) == "reuse");

    auto symbol_file = sym_dir / "LCSC.kicad_sym";
    REQUIRE(fs::exists(symbol_file));
    CHECK(read(symbol_file).find("\"Pkg:CHOSEN\"") != std::string::npos);
    // 复用不写封装文件：包的封装路径里不能出现新建的 .pretty
    CHECK_FALSE(fs::exists(fp_dir / "LCSC.pretty"));
    CHECK(result->value("footprint_id", std::string("")) == existing.id());

    std::string symbol_id = result->value("symbol_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());
    {
        auto session = db->session();
        auto symbol = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(symbol.has_value());
        CHECK(symbol->footprint() == "Pkg:CHOSEN");
        auto link = storage::RelationshipRepository(session).find_footprint_for_symbol(symbol_id);
        REQUIRE(link.has_value());
        REQUIRE(link->has_value());
        CHECK(**link == existing.id());
    }

    // 同一 operation_id 换一个复用封装 UUID → 拒绝
    auto other = footprint("OTHER");
    auto switched = request;
    switched.footprint_id = other.id();
    CHECK_FALSE(importer.run(switched).has_value());

    // 复用必须显式给候选 UUID：空值直接拒绝，不能默认 new、也不能取 top1
    auto missing_id = request;
    missing_id.operation_id = "lcsc01-b2";
    missing_id.footprint_id.clear();
    auto rejected = importer.run(missing_id);
    if (rejected)
        MESSAGE("reuse without a candidate id was accepted: ", rejected->dump());
    CHECK_FALSE(rejected.has_value());
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 component library mode rejects unusable targets")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01c";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "LCSC", sym_dir.string(), fp_dir.string());
    auto manager = lcsc01_plugins(temp.path() / "plugins_lcsc01c");
    api::StagedImporter importer(*db, *manager, &matching_store);

    // 未知包
    {
        auto request = lcsc01_request("no-such-package", "C803", "lcsc01-c1");
        CHECK_FALSE(importer.run(request).has_value());
    }
    // disabled
    {
        auto off_root = temp.path() / "lcsc01c_off";
        fs::create_directories(off_root / "symbols");
        auto disabled = lcsc01_register_package(*db, "Off", (off_root / "symbols").string(),
                                                (off_root / "footprints").string(), false);
        auto request = lcsc01_request(disabled, "C803", "lcsc01-c2");
        CHECK_FALSE(importer.run(request).has_value());
        CHECK_FALSE(fs::exists(off_root / "symbols" / "LCSC.kicad_sym"));
    }
    // locked
    {
        auto locked_root = temp.path() / "lcsc01c_locked";
        fs::create_directories(locked_root / "symbols");
        auto locked = lcsc01_register_package(*db, "Locked", (locked_root / "symbols").string(),
                                              (locked_root / "footprints").string());
        lcsc01_lock_package(*db, locked);
        auto request = lcsc01_request(locked, "C803", "lcsc01-c3");
        CHECK_FALSE(importer.run(request).has_value());
        CHECK_FALSE(fs::exists(locked_root / "symbols" / "LCSC.kicad_sym"));
    }
    // 缺符号路径 / 缺封装路径
    {
        auto half = lcsc01_register_package(*db, "Half", "", fp_dir.string());
        CHECK_FALSE(importer.run(lcsc01_request(half, "C803", "lcsc01-c4")).has_value());
        auto other_half = lcsc01_register_package(*db, "Half2", sym_dir.string(), "");
        CHECK_FALSE(importer.run(lcsc01_request(other_half, "C803", "lcsc01-c5")).has_value());
        CHECK_FALSE(fs::exists(sym_dir / "LCSC.kicad_sym"));
    }
    // 同时给旧目标与新目标 → 拒绝（新字段独立，不做静默取舍）
    {
        auto request = lcsc01_request(package_id, "C803", "lcsc01-c6");
        request.target_library_id = "some-library-uuid";
        CHECK_FALSE(importer.run(request).has_value());
    }
    // 未知封装模式
    {
        auto request = lcsc01_request(package_id, "C803", "lcsc01-c7");
        request.footprint_mode = "guess";
        CHECK_FALSE(importer.run(request).has_value());
    }
    // 普通文件当符号路径（不是 .kicad_sym 也不是目录）→ 拒绝
    {
        auto plain_root = temp.path() / "lcsc01c_plain";
        auto plain = plain_root / "notasymbol.txt";
        write(plain, "plain");
        auto wrong = lcsc01_register_package(*db, "Wrong", plain.string(), fp_dir.string());
        auto request = lcsc01_request(wrong, "C803", "lcsc01-c8");
        CHECK_FALSE(importer.run(request).has_value());
    }
    // 不存在的符号路径按「待创建目录」处理：其下建 LCSC.kicad_sym
    {
        auto fresh_root = temp.path() / "lcsc01c_fresh";
        auto fresh_sym = fresh_root / "not-yet-there";
        auto fresh_fp = fresh_root / "footprints";
        fs::create_directories(fresh_fp);
        auto fresh = lcsc01_register_package(*db, "Fresh", fresh_sym.string(), fresh_fp.string());
        auto request = lcsc01_request(fresh, "C803", "lcsc01-c9");
        auto created = importer.run(request);
        if (!created)
            MESSAGE("to-be-created directory target rejected: ", created.error().format_message());
        REQUIRE(created.has_value());
        CHECK(fs::exists(fresh_sym / "LCSC.kicad_sym"));
    }
    // 已有符号库行属于别的包 → 拒绝，且不改旧归属
    {
        auto foreign_root = temp.path() / "lcsc01c_foreign";
        auto foreign_sym = foreign_root / "symbols";
        fs::create_directories(foreign_sym);
        auto foreign_file = foreign_sym / "LCSC.kicad_sym";
        write(foreign_file, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                            "  (symbol \"OTHER_PART\" (property \"Reference\" \"U\"))\n)\n");
        auto other_package = lcsc01_register_package(*db, "Other", foreign_sym.string(),
                                                     (foreign_root / "footprints").string());
        {
            auto session = db->session();
            core::LibraryMeta meta;
            meta.name = "LCSC";
            meta.file_path = foreign_file;
            meta.component_library_id = other_package;
            REQUIRE(storage::LibraryRepository(session).insert(meta).has_value());
        }
        const std::string before = read(foreign_file);
        auto mine = lcsc01_register_package(*db, "Mine", foreign_sym.string(),
                                            (foreign_root / "footprints").string());
        auto request = lcsc01_request(mine, "C803", "lcsc01-c10");
        auto denied = importer.run(request);
        if (denied)
            MESSAGE("foreign-owned library was overwritten: ", denied->dump());
        CHECK_FALSE(denied.has_value());
        CHECK(read(foreign_file) == before);
        auto session = db->session();
        auto row = storage::LibraryRepository(session).find_by_source_key(
            storage::existing_source_key(foreign_file));
        REQUIRE(row.has_value());
        CHECK(row->component_library_id == other_package);  // 旧归属没被改
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 component library mode honors direct symbol file and .pretty paths")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01d";
    auto direct = root / "Direct.kicad_sym";
    write(direct, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"EXISTING\" (property \"Reference\" \"U\"))\n)\n");
    auto pretty = root / "Pkgs.pretty";
    fs::create_directories(pretty);
    auto package_id = lcsc01_register_package(*db, "Direct", direct.string(), pretty.string());

    auto manager = lcsc01_plugins(temp.path() / "plugins_lcsc01d");
    api::StagedImporter importer(*db, *manager, &matching_store);
    auto request = lcsc01_request(package_id, "C804", "lcsc01-d");

    auto result = importer.run(request);
    if (!result)
        MESSAGE("direct-path import failed: ", result.error().format_message());
    REQUIRE(result.has_value());

    const std::string merged = read(direct);
    CHECK(merged.find("EXISTING") != std::string::npos);  // 原有符号保留
    CHECK(merged.find("STUB_C804") != std::string::npos);
    CHECK(merged.find("\"Pkgs:STUBFP\"") != std::string::npos);  // 前缀 = .pretty 的 stem
    CHECK(fs::exists(pretty / "STUBFP.kicad_mod"));
    CHECK_FALSE(fs::exists(pretty / "Pkgs.pretty"));  // 不再多套一层
    CHECK(result->value("footprint_reference", std::string("")) == "Pkgs:STUBFP");

    {
        auto session = db->session();
        auto library = storage::LibraryRepository(session).find_by_source_key(
            storage::existing_source_key(direct));
        REQUIRE(library.has_value());
        CHECK(library->name == "Direct");
        CHECK(library->component_library_id == package_id);
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 same-named packages stay distinct by id and path")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01e";
    auto first_root = root / "first";
    auto second_root = root / "second";
    fs::create_directories(first_root / "symbols");
    fs::create_directories(second_root / "symbols");
    auto first_package = lcsc01_register_package(*db, "Same", (first_root / "symbols").string(),
                                                 (first_root / "footprints").string());
    auto second_package = lcsc01_register_package(*db, "Same", (second_root / "symbols").string(),
                                                  (second_root / "footprints").string());
    REQUIRE(first_package != second_package);

    auto manager = lcsc01_plugins(temp.path() / "plugins_lcsc01e");
    api::StagedImporter importer(*db, *manager, &matching_store);

    auto first = importer.run(lcsc01_request(first_package, "C805", "lcsc01-e1"));
    if (!first)
        MESSAGE("first package import failed: ", first.error().format_message());
    REQUIRE(first.has_value());
    auto second = importer.run(lcsc01_request(second_package, "C805", "lcsc01-e2"));
    if (!second)
        MESSAGE("second package import failed: ", second.error().format_message());
    REQUIRE(second.has_value());

    // 两个包各自成库：文件、归属、符号行都不混
    CHECK(fs::exists(first_root / "symbols" / "LCSC.kicad_sym"));
    CHECK(fs::exists(second_root / "symbols" / "LCSC.kicad_sym"));
    CHECK(fs::exists(first_root / "footprints" / "LCSC.pretty" / "STUBFP.kicad_mod"));
    CHECK(fs::exists(second_root / "footprints" / "LCSC.pretty" / "STUBFP.kicad_mod"));
    {
        auto session = db->session();
        auto first_library = storage::LibraryRepository(session).find_by_source_key(
            storage::existing_source_key(first_root / "symbols" / "LCSC.kicad_sym"));
        REQUIRE(first_library.has_value());
        CHECK(first_library->component_library_id == first_package);
        auto second_library = storage::LibraryRepository(session).find_by_source_key(
            storage::existing_source_key(second_root / "symbols" / "LCSC.kicad_sym"));
        REQUIRE(second_library.has_value());
        CHECK(second_library->component_library_id == second_package);
        CHECK(first_library->id != second_library->id);
        CHECK(first->value("symbol_id", std::string("")) !=
              second->value("symbol_id", std::string("")));
    }
    // 会话已释放，才能用会自取会话的辅助函数
    // 同一个来源编号在两个包里各有一条映射（目标库不同不是重复导入）
    CHECK(count_rows(*db, "import_sources") == 2);
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 a failed commit restores the file and leaves no orphan rows")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01f";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "LCSC", sym_dir.string(), fp_dir.string());

    auto manager = lcsc01_plugins(temp.path() / "plugins_lcsc01f");
    api::StagedImporter importer(*db, *manager, &matching_store);
    auto first = importer.run(lcsc01_request(package_id, "C806", "lcsc01-f1"));
    if (!first)
        MESSAGE("first import failed: ", first.error().format_message());
    REQUIRE(first.has_value());

    auto symbol_file = sym_dir / "LCSC.kicad_sym";
    const std::string before = read(symbol_file);
    const auto footprint_file = fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod";
    const std::string footprint_before = read(footprint_file);

    // 桩插件的 "X" 尾缀：不同来源编号、相同符号名 —— 业务事务必然冲突
    auto conflicting = lcsc01_request(package_id, "C806X", "lcsc01-f2");
    auto failed = importer.run(conflicting);
    if (failed)
        MESSAGE("same-name conflict was accepted: ", failed->dump());
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().kind() == util::Error::Kind::ValidationError);

    // 文件回到原状、没有孤立行、恢复日志不留残渣
    CHECK(read(symbol_file) == before);
    CHECK(read(footprint_file) == footprint_before);
    CHECK(count_rows(*db, "symbols") == 1);
    CHECK(count_rows(*db, "import_sources") == 1);
    CHECK(count_rows(*db, "import_operations") == 1);
    // 失败那次变更自己的日志必须撤掉；剩下的 1 条是首次成功导入留下的 applied
    // 记录（既有语义：applied 行由 recover() 收尾，见 source mutation 回归）。
    // 关键断言：不能留下任何 prepared（未决）行。
    CHECK(count_rows(*db, "file_mutations") == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM file_mutations WHERE state='prepared'") ==
          0);
    CHECK_FALSE(fs::exists(sym_dir / "LCSC.pretty"));
}

TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 component library mode propagates source lookup failures")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01g";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "LCSC", sym_dir.string(), fp_dir.string());

    auto manager = lcsc01_plugins(temp.path() / "plugins_lcsc01g");
    api::StagedImporter importer(*db, *manager, &matching_store);
    auto request = lcsc01_request(package_id, "C809", "lcsc01-g1");
    REQUIRE(importer.run(request).has_value());

    auto symbol_file = sym_dir / "LCSC.kicad_sym";
    const std::string before = read(symbol_file);
    auto denied_request = request;
    denied_request.operation_id = "lcsc01-g2";

    {
        auto session = db->session();
        REQUIRE(sqlite3_set_authorizer(session.handle(), deny_import_sources_read, nullptr) ==
                SQLITE_OK);
    }
    auto denied = importer.run(denied_request);
    {
        auto session = db->session();
        sqlite3_set_authorizer(session.handle(), nullptr, nullptr);
    }
    if (denied)
        MESSAGE("denied source lookup still imported: ", denied->dump());
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().kind() == util::Error::Kind::DbError);

    CHECK(read(symbol_file) == before);
    CHECK(count_rows(*db, "symbols") == 1);
    CHECK(count_rows(*db, "import_sources") == 1);
    CHECK(count_rows(*db, "import_operations") == 1);
    // 同 -F：只剩首次成功导入的 applied 行，不能留 prepared 未决行
    CHECK(count_rows(*db, "file_mutations") == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM file_mutations WHERE state='prepared'") ==
          0);
    CHECK(fs::exists(fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod"));
    // 查询失败不能顺手建第二个包目录
    CHECK_FALSE(fs::exists(sym_dir / "LCSC.pretty"));
}

// ------------------------------------------------------------
// LCSC01-J：符号表读取失败必须报错回滚
// 只拒 symbols 的读（其它 SELECT/INSERT 放行）：空包首导必须 DbError，
// 文件与新登记的符号库行一起回滚，不能把读失败当成「库里没有同名符号」插第二行。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 a failed symbol table read aborts the component import")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01j";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "LCSC", sym_dir.string(), fp_dir.string());

    auto manager = lcsc01_plugins(temp.path() / "plugins_lcsc01j");
    api::StagedImporter importer(*db, *manager, &matching_store);
    auto request = lcsc01_request(package_id, "C810", "lcsc01-j1");

    const int libraries_before = count_rows(*db, "libraries");
    const int sources_before = count_rows(*db, "import_sources");

    {
        auto session = db->session();
        REQUIRE(sqlite3_set_authorizer(session.handle(), deny_symbols_read, nullptr) == SQLITE_OK);
    }
    auto denied = importer.run(request);
    {
        auto session = db->session();
        sqlite3_set_authorizer(session.handle(), nullptr, nullptr);
    }
    if (denied)
        MESSAGE("denied symbol read still imported: ", denied->dump());
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().kind() == util::Error::Kind::DbError);

    // 文件、符号库登记行、来源映射、操作记录全部回滚
    CHECK_FALSE(fs::exists(sym_dir / "LCSC.kicad_sym"));
    // 变更过程中新建的封装产物必须撤掉（恢复系统只回滚文件，不回滚自己建的目录）
    CHECK_FALSE(fs::exists(fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod"));
    CHECK(count_rows(*db, "libraries") == libraries_before);
    CHECK(count_rows(*db, "symbols") == 0);
    CHECK(count_rows(*db, "import_sources") == sources_before);
    CHECK(count_rows(*db, "import_operations") == 0);
    CHECK(count_rows(*db, "file_mutations") == 0);
}

// ------------------------------------------------------------
// LCSC01-H：existence 预期覆盖「目标以目录形态出现」
// 只断言 allow_missing 时，目录（或任何不可读目标）会被当成「不存在的新文件」，
// 一路走到替换阶段才失败；必须和「文件被抢先建出来」一样在锁内直接拒绝。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 expected_missing refuses a target that appeared as a directory")
{
    auto target = temp.path() / "symbols" / "pkgdir.kicad_sym";
    fs::create_directories(target);  // 目标以目录形态出现（不是常规文件）
    auto owner = group();

    io::SourceMutationService mutations(*db);
    auto ok = [](storage::DbSession&) -> util::Result<void> { return {}; };
    auto result = mutations.apply({io::FileChange{.path = target,
                                                  .content = "(kicad_symbol_lib (version 20231120))",
                                                  .allow_missing = true,
                                                  .expected_missing = true,
                                                  .owner = owner}},
                                  ok);
    if (result)
        MESSAGE("directory was overwritten as a new file");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind() == util::Error::Kind::ValidationError);
    CHECK(fs::is_directory(target));  // 别人的目录原样保留
    CHECK(count_rows(*db, "file_mutations") == 0);

    // 目录存在但没有 existence 预期时，同样不能把它当新文件覆盖
    auto second = mutations.apply({io::FileChange{.path = target,
                                                  .content = "(kicad_symbol_lib (version 20231120))",
                                                  .allow_missing = true,
                                                  .owner = owner}},
                                  ok);
    CHECK_FALSE(second.has_value());
    CHECK(fs::is_directory(target));
    CHECK(count_rows(*db, "file_mutations") == 0);
}

// ------------------------------------------------------------
// LCSC01-I：旧符号库入口语义不变（直接给符号库 UUID）
// 新增 target_component_library_id 是独立字段，不能改变旧请求的行为。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 the legacy symbol library entry keeps its behavior")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01i";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    auto target = sym_dir / "Legacy.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"EXISTING\" (property \"Reference\" \"U\"))\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, fp_dir);

    auto manager = lcsc01_plugins(temp.path() / "plugins_lcsc01i");
    api::StagedImporter importer(*db, *manager, &matching_store);
    api::StagedImporter::Request request;
    request.plugin_id = "stub.lcsc";
    request.external_id = "C810";
    request.target_library_id = library_id;  // 旧入口：符号库 UUID
    request.footprint_mode = "new";
    request.operation_id = "lcsc01-i";

    auto result = importer.run(request);
    if (!result)
        MESSAGE("legacy entry failed: ", result.error().format_message());
    REQUIRE(result.has_value());
    CHECK(read(target).find("STUB_C810") != std::string::npos);
    CHECK(read(target).find("\"Legacy:STUBFP\"") != std::string::npos);
    CHECK(fs::exists(fp_dir / "Legacy.pretty" / "STUBFP.kicad_mod"));
}

// ============================================================
// LCSC01-K：HTTP 请求体解析（README/docs 的嵌套写法与旧的顶层写法等价）
// 解析实现在 staged_import.cpp，routes 与测试共用同一份，避免文档/解析两套写法。
// ============================================================
namespace
{
const char* kPackageId = "11111111-1111-1111-1111-111111111111";
const char* kFootprintA = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
const char* kFootprintB = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";

nlohmann::json component_body(const nlohmann::json& choice)
{
    nlohmann::json body{{"lcsc_id", "C14663"}, {"target_component_library_id", kPackageId}};
    if (!choice.is_null())
        body["footprint_choice"] = choice;
    return body;
}
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 request parsing accepts nested and top-level footprint ids")
{
    // 文档示例写法 footprint_choice.footprint_id
    auto nested = api::parse_staged_request(
        component_body({{"mode", "reuse"}, {"footprint_id", kFootprintA}}),
        "com.kicad_forge.lcsc_import", "import");
    if (!nested)
        MESSAGE("nested footprint_id rejected: ", nested.error().format_message());
    REQUIRE(nested.has_value());
    CHECK(nested->request.footprint_mode == "reuse");
    CHECK(nested->request.footprint_id == kFootprintA);
    CHECK(nested->request.external_id == "C14663");
    CHECK(nested->request.provider == "LCSC");  // 缺省 provider 保持 LCSC

    // footprint_choice.id 别名
    auto alias = api::parse_staged_request(component_body({{"mode", "reuse"}, {"id", kFootprintA}}),
                                           "com.kicad_forge.lcsc_import", "import");
    REQUIRE(alias.has_value());
    CHECK(alias->request.footprint_id == kFootprintA);

    // 旧前端的顶层 footprint_id
    auto top = component_body({{"mode", "reuse"}});
    top["footprint_id"] = kFootprintA;
    auto top_parsed = api::parse_staged_request(top, "com.kicad_forge.lcsc_import", "import");
    REQUIRE(top_parsed.has_value());
    CHECK(top_parsed->request.footprint_id == kFootprintA);

    // 三处写法给出同一个 UUID：一致就接受
    auto same =
        component_body({{"mode", "reuse"}, {"id", kFootprintA}, {"footprint_id", kFootprintA}});
    same["footprint_id"] = kFootprintA;
    auto same_parsed = api::parse_staged_request(same, "com.kicad_forge.lcsc_import", "import");
    REQUIRE(same_parsed.has_value());
    CHECK(same_parsed->request.footprint_id == kFootprintA);

    // 同给不同 UUID：请求自相矛盾，必须拒绝
    auto conflict = component_body({{"mode", "reuse"}, {"id", kFootprintA}});
    conflict["footprint_id"] = kFootprintB;
    auto conflict_parsed =
        api::parse_staged_request(conflict, "com.kicad_forge.lcsc_import", "import");
    REQUIRE_FALSE(conflict_parsed.has_value());
    CHECK(conflict_parsed.error().kind() == util::Error::Kind::ValidationError);
    CHECK(conflict_parsed.error().format_message().find("agree") != std::string::npos);

    // 组件包模式缺 footprint_choice / reuse 缺 ID
    auto missing_choice =
        api::parse_staged_request(component_body(nullptr), "com.kicad_forge.lcsc_import", "import");
    REQUIRE_FALSE(missing_choice.has_value());
    CHECK(missing_choice.error().format_message().find("footprint_choice is required") !=
          std::string::npos);
    auto missing_id = api::parse_staged_request(component_body({{"mode", "reuse"}}),
                                                "com.kicad_forge.lcsc_import", "import");
    REQUIRE_FALSE(missing_id.has_value());
    CHECK(missing_id.error().format_message().find("reuse requires") != std::string::npos);

    // new 不能保留任何旧 UUID（嵌套与顶层都算）
    auto new_with_id = api::parse_staged_request(component_body({{"mode", "new"}, {"id", kFootprintA}}),
                                                 "com.kicad_forge.lcsc_import", "import");
    REQUIRE_FALSE(new_with_id.has_value());
    CHECK(new_with_id.error().format_message().find("only valid in reuse mode") !=
          std::string::npos);
    auto new_with_top = component_body({{"mode", "new"}});
    new_with_top["footprint_id"] = kFootprintA;
    auto new_top_parsed =
        api::parse_staged_request(new_with_top, "com.kicad_forge.lcsc_import", "import");
    REQUIRE_FALSE(new_top_parsed.has_value());
    CHECK(new_top_parsed.error().format_message().find("only valid in reuse mode") !=
          std::string::npos);
    auto plain_new = api::parse_staged_request(component_body({{"mode", "new"}}),
                                               "com.kicad_forge.lcsc_import", "import");
    REQUIRE(plain_new.has_value());
    CHECK(plain_new->request.footprint_id.empty());  // new 不保留任何旧 UUID

    // 非法 mode 的消息保持原样
    auto bad_mode = api::parse_staged_request(component_body({{"mode", "top"}}),
                                              "com.kicad_forge.lcsc_import", "import");
    REQUIRE_FALSE(bad_mode.has_value());
    CHECK(bad_mode.error().format_message().find("Unknown footprint mode") != std::string::npos);

    // 类型错误：给具体消息，不落 "Execute error"
    auto bad_nested = component_body({{"mode", "reuse"}, {"id", 42}});
    auto bad_nested_parsed =
        api::parse_staged_request(bad_nested, "com.kicad_forge.lcsc_import", "import");
    REQUIRE_FALSE(bad_nested_parsed.has_value());
    CHECK(bad_nested_parsed.error().format_message() == "footprint_choice.id must be a string");
    auto bad_top = component_body({{"mode", "reuse"}});
    bad_top["footprint_id"] = 7;
    auto bad_top_parsed =
        api::parse_staged_request(bad_top, "com.kicad_forge.lcsc_import", "import");
    REQUIRE_FALSE(bad_top_parsed.has_value());
    CHECK(bad_top_parsed.error().format_message() == "footprint_id must be a string");
    // provider 的类型检查必须发生在取用之前（先 value() 会抛异常被外层吞掉）
    auto provider_body = component_body({{"mode", "new"}});
    provider_body["provider"] = 3;
    auto provider_parsed =
        api::parse_staged_request(provider_body, "com.kicad_forge.lcsc_import", "import");
    REQUIRE_FALSE(provider_parsed.has_value());
    CHECK(provider_parsed.error().format_message() == "provider must be a string");
    CHECK(provider_parsed.error().kind() == util::Error::Kind::ValidationError);
    auto bad_choice_type = component_body(nlohmann::json::array());
    auto bad_choice_parsed =
        api::parse_staged_request(bad_choice_type, "com.kicad_forge.lcsc_import", "import");
    REQUIRE_FALSE(bad_choice_parsed.has_value());
    CHECK(bad_choice_parsed.error().format_message() == "footprint_choice must be an object");

    // 旧符号库入口：footprint_choice 可省略，mode 默认 new，library/name 透出给调用方查库
    nlohmann::json legacy{{"lcsc_id", "C1"}, {"target_library_id", "lib-uuid"}};
    auto legacy_parsed = api::parse_staged_request(legacy, "stub.lcsc", "import");
    REQUIRE(legacy_parsed.has_value());
    CHECK(legacy_parsed->request.footprint_mode == "new");
    CHECK(legacy_parsed->request.target_library_id == "lib-uuid");
    CHECK(legacy_parsed->legacy_library.empty());
    nlohmann::json legacy_choice{{"lcsc_id", "C1"},
                                 {"target_library_id", "lib-uuid"},
                                 {"footprint_choice",
                                  {{"mode", "reuse"}, {"library", "Pkgs"}, {"name", "STUBFP"}}}};
    auto legacy_named = api::parse_staged_request(legacy_choice, "stub.lcsc", "import");
    REQUIRE(legacy_named.has_value());
    CHECK(legacy_named->request.footprint_mode == "reuse");
    CHECK(legacy_named->request.footprint_id.empty());  // 等调用方按 (库, 名) 唯一定位
    CHECK(legacy_named->legacy_library == "Pkgs");
    CHECK(legacy_named->legacy_name == "STUBFP");
}

// ============================================================
// LCSC01-L：编号规范化发生在任何幂等/来源逻辑之前（真实 LCSC 插件 + 元件库包模式）
// ' c123 ' / C123 / 123 必须落在同一条来源上；非法编号在插件执行之前拒绝；
// 第三方插件的通用外部编号不受这条规则影响。
// ============================================================
namespace
{
const char* LCSC01_REAL_MANIFEST = R"JSON({
  "id": "com.kicad_forge.lcsc_import",
  "name": "Stub LCSC (offline)",
  "version": "1.0.0",
  "actions": [ { "id": "import", "name": "Import", "protocol": "staged-import-v1" } ]
})JSON";

/// 离线桩：只按宿主给的（已规范化的）编号产出固定产物，不联网；
/// 每次执行在插件目录里追加一行，用来证明「非法编号没有执行插件」。
const char* LCSC01_REAL_STUB_PY = R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "invocations.log"), "a", encoding="utf-8") as log:
    log.write("run\n")
lcsc = args.get("lcsc_id", "")
name = "STUB_" + lcsc
os.makedirs(staging, exist_ok=True)
symbol = ('(symbol "%s" (property "Reference" "U") (property "Footprint" "old:fp")'
          ' (property "Stamp" "%s"))') % (name, lcsc)
with open(os.path.join(staging, name + ".kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator stub)\n" + symbol + "\n)\n")
with open(os.path.join(staging, "STUBFP.kicad_mod"), "w", encoding="utf-8") as f:
    f.write('(footprint "STUBFP" (layer "F.Cu"))')
manifest = {"protocol": "staged-import-v1", "lcsc_id": lcsc,
            "symbols": [{"name": name, "file": name + ".kicad_sym"}],
            "footprint": {"name": "STUBFP", "file": "STUBFP.kicad_mod"}}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
print(json.dumps({"ok": True}))
)PY";

/// 真实 LCSC 插件 ID 的请求（离线桩按这个 ID 注册）
api::StagedImporter::Request lcsc01_real_request(const std::string& package_id,
                                                 const std::string& external_id,
                                                 const std::string& operation_id)
{
    auto request = lcsc01_request(package_id, external_id, operation_id);
    request.plugin_id = "com.kicad_forge.lcsc_import";
    return request;
}

int count_lines(const fs::path& path)
{
    const std::string text = read(path);
    int lines = 0;
    for (char c : text)
        if (c == '\n')
            ++lines;
    return lines;
}
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 the LCSC part number is canonicalized before source identity")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01l";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "LCSC", sym_dir.string(), fp_dir.string());

    auto scan = temp.path() / "plugins_lcsc01l";
    write(scan / "lcsc" / "manifest.json", LCSC01_REAL_MANIFEST);
    write(scan / "lcsc" / "plugin.py", LCSC01_REAL_STUB_PY);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    auto symbol_file = sym_dir / "LCSC.kicad_sym";
    auto mark = scan / "lcsc" / "invocations.log";

    // 首导用原始写法 ' c123 '：宿主先规范成 C123 再算来源/摘要并交给插件
    auto first = importer.run(lcsc01_real_request(package_id, " c123 ", "lcsc01-l1"));
    if (!first)
        MESSAGE("canonical first import failed: ", first.error().format_message());
    REQUIRE(first.has_value());
    CHECK(first->value("lcsc_id", std::string("")) == "C123");
    CHECK(read(symbol_file).find("STUB_C123") != std::string::npos);
    CHECK(read(symbol_file).find(" c123 ") == std::string::npos);
    const std::string symbol_id = first->value("symbol_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());
    CHECK(count_lines(mark) == 1);

    // 规范写法 + 同一个 operation_id：摘要一致 → 回放，不重跑插件
    auto replay = importer.run(lcsc01_real_request(package_id, "C123", "lcsc01-l1"));
    if (!replay)
        MESSAGE("canonical replay rejected: ", replay.error().format_message());
    REQUIRE(replay.has_value());
    CHECK(replay->value("replayed", false));
    CHECK(replay->value("symbol_id", std::string("")) == symbol_id);
    CHECK(count_lines(mark) == 1);

    // 纯数字、不同 operation_id：同源同产物 → no-op，一条来源、同一个符号
    auto digits = importer.run(lcsc01_real_request(package_id, "123", "lcsc01-l2"));
    if (!digits)
        MESSAGE("digit form treated as a different source: ", digits.error().format_message());
    REQUIRE(digits.has_value());
    CHECK(digits->value("noop", false));
    CHECK(digits->value("symbol_id", std::string("")) == symbol_id);
    CHECK(count_rows(*db, "import_sources") == 1);
    CHECK(count_rows(*db, "symbols") == 1);
    CHECK(count_lines(mark) == 2);

    // 非法编号：插件没有被执行，文件与数据库一个字节都没动
    const std::string before = read(symbol_file);
    auto invalid_id = importer.run(lcsc01_real_request(package_id, "C12A3", "lcsc01-l3"));
    if (invalid_id)
        MESSAGE("invalid part number was imported: ", invalid_id->dump());
    REQUIRE_FALSE(invalid_id.has_value());
    CHECK(invalid_id.error().kind() == util::Error::Kind::ValidationError);
    CHECK(invalid_id.error().format_message().find("Invalid LCSC part number") != std::string::npos);
    CHECK(count_lines(mark) == 2);  // 插件没有被执行
    CHECK(read(symbol_file) == before);
    CHECK(count_rows(*db, "import_sources") == 1);
    CHECK(count_rows(*db, "symbols") == 1);
    // l1（首导）与 l2（no-op）各一条；非法编号那次没有留下任何记录
    CHECK(count_rows(*db, "import_operations") == 2);

    // 第三方插件的通用外部编号不受 LCSC 规则约束：原样进插件、原样登记
    auto other_root = temp.path() / "lcsc01l2";
    auto other_sym = other_root / "symbols";
    auto other_fp = other_root / "footprints";
    fs::create_directories(other_sym);
    fs::create_directories(other_fp);
    auto other_package = lcsc01_register_package(*db, "Other", other_sym.string(), other_fp.string());
    auto other_plugins = lcsc01_plugins(temp.path() / "plugins_lcsc01l2");
    api::StagedImporter other_importer(*db, *other_plugins, &matching_store);
    auto third = other_importer.run(lcsc01_request(other_package, "abc123", "lcsc01-l4"));
    if (!third)
        MESSAGE("third-party external id was rewritten: ", third.error().format_message());
    REQUIRE(third.has_value());
    CHECK(third->value("lcsc_id", std::string("")) == "abc123");
    CHECK(read(other_sym / "LCSC.kicad_sym").find("STUB_abc123") != std::string::npos);
}

// ============================================================
// LCSC01-M：封装路径尾部分隔符不能让 .pretty 判定失效
// '…/Pkgs.pretty/' 必须仍然落在 Pkgs.pretty 本身，不能再套一层 <stem>.pretty。
// ============================================================
TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 a trailing separator on the .pretty path does not nest a directory")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01m";
    auto direct = root / "Trail.kicad_sym";
    write(direct, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n"
                  "  (symbol \"EXISTING\" (property \"Reference\" \"U\"))\n)\n");
    auto pretty = root / "Pkgs.pretty";
    fs::create_directories(pretty);
    // 用户配置里带了尾部分隔符（Windows 上同样会出现在界面上）
    auto package_id =
        lcsc01_register_package(*db, "Trail", direct.string(), pretty.string() + "/");

    auto manager = lcsc01_plugins(temp.path() / "plugins_lcsc01m");
    api::StagedImporter importer(*db, *manager, &matching_store);
    auto request = lcsc01_request(package_id, "C811", "lcsc01-m");
    auto result = importer.run(request);
    if (!result)
        MESSAGE("trailing-separator import failed: ", result.error().format_message());
    REQUIRE(result.has_value());

    CHECK(result->value("footprint_reference", std::string("")) == "Pkgs:STUBFP");
    CHECK(fs::exists(pretty / "STUBFP.kicad_mod"));
    // 没有嵌套的第二层 .pretty（修复前会新建 Pkgs.pretty/Trail.pretty 并引用 Trail:）
    CHECK_FALSE(fs::exists(pretty / "Trail.pretty"));
    CHECK_FALSE(fs::exists(pretty / "LCSC.pretty"));
    CHECK_FALSE(fs::exists(root / "Trail.pretty"));
    CHECK(read(direct).find("EXISTING") != std::string::npos);
    CHECK(read(direct).find("\"Pkgs:STUBFP\"") != std::string::npos);

    // 数据库里的落点与引用前缀和磁盘一致
    const std::string footprint_id = result->value("footprint_id", std::string(""));
    const std::string symbol_id = result->value("symbol_id", std::string(""));
    REQUIRE_FALSE(footprint_id.empty());
    REQUIRE_FALSE(symbol_id.empty());
    {
        auto session = db->session();
        auto footprint = storage::FootprintRepository(session).find_by_id(footprint_id);
        REQUIRE(footprint.has_value());
        CHECK(footprint->footprint_lib_name() == "Pkgs");
        CHECK(fs::path(footprint->library_path()) == pretty / "STUBFP.kicad_mod");
        auto symbol = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(symbol.has_value());
        CHECK(symbol->footprint() == "Pkgs:STUBFP");
    }
}

// ============================================================
// LCSC01-N：existence 预期不跟随符号链接
// fs::exists 对悬空链接返回 false，会把已经占位的目录项当成「不存在的新文件」。
// ============================================================
TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 expected_missing refuses a dangling symlink")
{
    auto owner = group();
    io::SourceMutationService mutations(*db);
    auto ok = [](storage::DbSession&) -> util::Result<void> { return {}; };

    // 目录与普通文件两条老路径不依赖创建链接的能力，先在同一用例内复验：
    // 链接无法构造时用例会提前返回，放在返回之后它们就永远执行不到。
    auto directory = temp.path() / "symbols" / "still-a-dir.kicad_sym";
    fs::create_directories(directory);
    auto directory_result =
        mutations.apply({io::FileChange{.path = directory,
                                        .content = "(kicad_symbol_lib (version 20231120))",
                                        .allow_missing = true,
                                        .expected_missing = true,
                                        .owner = owner}},
                        ok);
    CHECK_FALSE(directory_result.has_value());
    CHECK(fs::is_directory(directory));
    auto present = temp.path() / "symbols" / "already-there.kicad_sym";
    write(present, "(kicad_symbol_lib (version 20231120))");
    auto present_result =
        mutations.apply({io::FileChange{.path = present,
                                        .content = "(kicad_symbol_lib (version 20231120))",
                                        .allow_missing = true,
                                        .expected_missing = true,
                                        .owner = owner}},
                        ok);
    CHECK_FALSE(present_result.has_value());
    CHECK(read(present) == "(kicad_symbol_lib (version 20231120))");
    CHECK(count_rows(*db, "file_mutations") == 0);

    // 悬空链接一条：链接本身占位、目标不存在
    auto target = temp.path() / "symbols" / "dangling.kicad_sym";
    fs::create_directories(target.parent_path());
    const fs::path link_target = temp.path() / "missing-target.kicad_sym";
    std::error_code ec;
    fs::create_symlink(link_target, target, ec);
#ifdef _WIN32
    if (ec)
    {
        // MinGW libstdc++ 的 create_symlink 在 Windows 上直接判「未实现」（ENOSYS），
        // 改用官方 Win32 API 在同一测试临时目录里重试：需要开发者模式或创建符号
        // 链接特权，两者都没有时如实报告无权限（不改机器开发者模式）。
        if (!::CreateSymbolicLinkW(target.c_str(), link_target.c_str(),
                                   SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        {
            // 开发者模式关闭时 unprivileged flag 被判成非法参数，去掉标志再试一次
            if (::CreateSymbolicLinkW(target.c_str(), link_target.c_str(), 0))
                ec.clear();
            else
                ec.assign(static_cast<int>(::GetLastError()), std::system_category());
        }
        else
        {
            ec.clear();
        }
    }
#endif
    if (ec)
    {
        // 无法构造这一形态：如实报告能力缺失，不把未执行的分支算成通过
        MESSAGE("cannot create a symlink here (", ec.message(),
                ") — dangling-symlink branch not executed");
        return;
    }

    const auto check_link_entry = [&] {
#ifdef _WIN32
        // 部分 MinGW 标准库不能正确报告悬空链接类型，使用不跟随目标的
        // Win32 目录项信息验证 reparse tag；不能把链接检查降级为 exists。
        WIN32_FIND_DATAW entry{};
        const HANDLE found = ::FindFirstFileW(target.c_str(), &entry);
        REQUIRE(found != INVALID_HANDLE_VALUE);
        ::FindClose(found);
        REQUIRE((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
        CHECK(entry.dwReserved0 == IO_REPARSE_TAG_SYMLINK);
#else
        CHECK(fs::is_symlink(target));
#endif
        CHECK_FALSE(fs::exists(link_target));
    };
    check_link_entry();  // 先证明夹具确实是悬空链接，再执行被测操作。

    auto result = mutations.apply({io::FileChange{.path = target,
                                                  .content = "(kicad_symbol_lib (version 20231120))",
                                                  .allow_missing = true,
                                                  .expected_missing = true,
                                                  .owner = owner}},
                                  ok);
    if (result)
        MESSAGE("dangling symlink was treated as a missing file");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind() == util::Error::Kind::ValidationError);
    check_link_entry();  // 链接本身保留，悬空目标没有被创建。
    CHECK(count_rows(*db, "file_mutations") == 0);
}

// ============================================================
// LCSC01-O：只拒 symbol_footprint_links 的读，空包首导必须 DbError 并整体回滚
// 关联表查询失败 ≠「没有旧关联」，不能吞成未找到继续插一条新关联。
// ============================================================
namespace
{
int deny_symbol_footprint_links_read(void*, int action, const char* arg1, const char*, const char*,
                                     const char*)
{
    if (action == SQLITE_READ && arg1 != nullptr &&
        std::string(arg1) == "symbol_footprint_links")
        return SQLITE_DENY;
    return SQLITE_OK;
}
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "LCSC01 a failed footprint link read aborts the first component import")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "lcsc01o";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "LCSC", sym_dir.string(), fp_dir.string());

    auto manager = lcsc01_plugins(temp.path() / "plugins_lcsc01o");
    api::StagedImporter importer(*db, *manager, &matching_store);
    auto request = lcsc01_request(package_id, "C812", "lcsc01-o1");

    const int libraries_before = count_rows(*db, "libraries");
    const int sources_before = count_rows(*db, "import_sources");

    {
        auto session = db->session();
        REQUIRE(sqlite3_set_authorizer(session.handle(), deny_symbol_footprint_links_read,
                                       nullptr) == SQLITE_OK);
    }
    auto denied = importer.run(request);
    {
        auto session = db->session();
        sqlite3_set_authorizer(session.handle(), nullptr, nullptr);
    }
    if (denied)
        MESSAGE("denied link read still imported: ", denied->dump());
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().kind() == util::Error::Kind::DbError);

    // 文件与全部业务表一起回滚
    CHECK_FALSE(fs::exists(sym_dir / "LCSC.kicad_sym"));
    CHECK_FALSE(fs::exists(fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod"));
    CHECK(count_rows(*db, "libraries") == libraries_before);
    CHECK(count_rows(*db, "symbols") == 0);
    CHECK(count_rows(*db, "footprints") == 0);
    CHECK(count_rows(*db, "symbol_footprint_links") == 0);
    CHECK(count_rows(*db, "import_sources") == sources_before);
    CHECK(count_rows(*db, "import_operations") == 0);
    CHECK(count_rows(*db, "file_mutations") == 0);
}

// ============================================================
// FPSTATUS —— 封装/3D 存在性的统一判定
// 参考缺陷：符号写着 "My_Library:SW-SMD_SS-0323PG-5N5WW"、库里没有这条记录时，
// 旧接口只看「引用非空 / 名称非空 / UUID 非空」就报正常。
// 这里全部用 TempDirectory 里的真实 .kicad_mod / 模型文件做证据。
// ============================================================
namespace
{
using namespace kforge;

/// 一站式取「解析结果 + JSON 适配器字段」
struct FpStatusProbe
{
    services::Resolution resolution;
    nlohmann::json json;
};

FpStatusProbe fpstatus_probe(LibraryFixture& fixture, const std::string& sym_id)
{
    FpStatusProbe probe;
    probe.resolution = fixture.resolve(sym_id);
    api::apply_footprint_status(probe.json, probe.resolution);
    return probe;
}

/// sqlite3_trace_v2 计数：只数 SELECT（不管行数），证明批量读的语句数是固定的
struct SelectCounter
{
    int selects{0};
};

int count_selects(unsigned mask, void* context, void* statement, void* sql_text)
{
    auto* counter = static_cast<SelectCounter*>(context);
    if ((mask & SQLITE_TRACE_STMT) != 0 && counter != nullptr)
    {
        auto* sql = static_cast<const char*>(sql_text);
        if (sql != nullptr && std::string(sql).find("SELECT") != std::string::npos)
        {
            ++counter->selects;
        }
    }
    (void) statement;
    return 0;
}

/// u8 字面量 → UTF-8 std::string（绑定到局部变量再取迭代器，避免两个临时对象）
std::string utf8_text(const char8_t* text)
{
    std::u8string value(text);
    return std::string(value.begin(), value.end());
}

/// authorizer：拒掉指定表的所有读（按表名切换）
thread_local std::string g_denied_table;
int deny_table_read(void*, int action, const char* arg1, const char*, const char*, const char*)
{
    if (action == SQLITE_READ && arg1 != nullptr && g_denied_table == arg1)
    {
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}

/// 读「中途」失败注入（对应正式环境里的 IO/中断错误）：authorizer 看到关联表的
/// 列读取时打开中断标志，progress handler 在下一次触发时返回 1 —— 语句以
/// SQLITE_INTERRUPT 结束，而不是读完之后的 SQLITE_DONE。
/// 与拒读整表（prepare 就失败）不同，这条路径专门证明最终 step 的错误也会传播。
thread_local bool g_interrupt_armed = false;
int arm_interrupt_on_links_read(void*, int action, const char* arg1, const char*, const char*,
                                const char*)
{
    if (action == SQLITE_READ && arg1 != nullptr &&
        std::string(arg1) == "symbol_footprint_links")
    {
        g_interrupt_armed = true;
    }
    return SQLITE_OK;
}

int interrupt_when_armed(void*)
{
    if (!g_interrupt_armed)
    {
        return 0;
    }
    g_interrupt_armed = false;
    return 1;  // 非 0 = 中断当前语句
}
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS unassigned and missing references are never reported as existing")
{
    // 1) 根本没有引用 → unassigned（也不拿数据库里的陈旧关联反向恢复）
    auto bare = symbol();
    set_reference(bare.id(), "");
    auto orphan = footprint_in("Pkg.pretty", "Ghost");
    link_footprint(bare.id(), orphan.id());
    auto unassigned = fpstatus_probe(*this, bare.id());
    CHECK(unassigned.resolution.status == services::FootprintStatus::Unassigned);
    CHECK(unassigned.resolution.reason == std::string(services::fp_reason::kNoReference));
    CHECK(unassigned.resolution.footprint_id.empty());
    CHECK_FALSE(unassigned.json["has_footprint"].get<bool>());
    CHECK_FALSE(unassigned.json["has_3d_model"].get<bool>());

    // 2) 用户示例：写了引用、库里没有这条记录 → not_found，原始引用照样留着
    auto sym = extra_symbol("R_LCSC", "My_Library:SW-SMD_SS-0323PG-5N5WW");
    auto missing = fpstatus_probe(*this, sym.id());
    CHECK(missing.resolution.status == services::FootprintStatus::NotFound);
    CHECK(missing.resolution.reason == std::string(services::fp_reason::kNoMatch));
    CHECK(missing.resolution.footprint_id.empty());
    CHECK(missing.resolution.reference == "My_Library:SW-SMD_SS-0323PG-5N5WW");
    CHECK(missing.json["footprint_reference"].get<std::string>() ==
          "My_Library:SW-SMD_SS-0323PG-5N5WW");
    CHECK_FALSE(missing.json["has_footprint"].get<bool>());
    CHECK_FALSE(missing.json["has_3d_model"].get<bool>());
    CHECK(missing.json["footprint_status"].get<std::string>() == "not_found");
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS available needs the real file and reports each stale reason")
{
    auto sym = symbol();
    set_reference(sym.id(), "Pkg:Part");
    auto fp = footprint_in("Pkg.pretty", "Part", true, true);
    auto ok = fpstatus_probe(*this, sym.id());
    CHECK(ok.resolution.status == services::FootprintStatus::Available);
    CHECK(ok.resolution.footprint_id == fp.id());
    CHECK(ok.resolution.resolved_reference == "Pkg:Part");
    CHECK(ok.json["has_footprint"].get<bool>());
    CHECK_FALSE(ok.json["has_3d_model"].get<bool>());  // 没有模型关联

    // 文件被删除 → stale/missing_file（DB 行还在也不代表存在）
    auto path = fs::path(fp.library_path());
    fs::remove(path);
    auto gone = fpstatus_probe(*this, sym.id());
    CHECK(gone.resolution.status == services::FootprintStatus::Stale);
    CHECK(gone.resolution.reason == std::string(services::fp_reason::kMissingFile));
    CHECK(gone.resolution.footprint_id == fp.id());  // 身份仍在，不自动切到别的行
    CHECK_FALSE(gone.json["has_footprint"].get<bool>());

    // 路径被目录替代 → stale/not_regular_file
    fs::create_directories(path);
    auto dir = fpstatus_probe(*this, sym.id());
    CHECK(dir.resolution.status == services::FootprintStatus::Stale);
    CHECK(dir.resolution.reason == std::string(services::fp_reason::kNotRegularFile));
    fs::remove_all(path);

    // 空路径 → stale/empty_path
    {
        auto session = db->session();
        REQUIRE(storage::FootprintRepository(session).remove(fp.id()).has_value());
    }
    auto empty = footprint_row("Part", fs::path());
    set_reference(sym.id(), "Part");
    auto empty_result = fpstatus_probe(*this, sym.id());
    CHECK(empty_result.resolution.status == services::FootprintStatus::Stale);
    CHECK(empty_result.resolution.reason == std::string(services::fp_reason::kEmptyPath));
    CHECK(empty_result.resolution.footprint_id == empty.id());
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS a changed source identity is stale even though the path exists")
{
    auto sym = symbol();
    set_reference(sym.id(), "Pkg:Part");
    auto path = temp.path() / "Pkg.pretty" / "Part.kicad_mod";
    write(path, "(footprint \"Part\" (layer \"F.Cu\"))");
    // 来源键指向别处：文件在、身份不对 → stale/source_key_mismatch
    auto other = temp.path() / "Elsewhere.pretty" / "Part.kicad_mod";
    auto fp = footprint_row("Part", path, storage::source_key(other));
    auto result = fpstatus_probe(*this, sym.id());
    CHECK(result.resolution.status == services::FootprintStatus::Stale);
    CHECK(result.resolution.reason == std::string(services::fp_reason::kSourceKeyMismatch));
    CHECK(result.resolution.footprint_id == fp.id());

    // 同一个文件、正确的来源键 → available
    auto fixed = footprint_row("Part2", path, storage::source_key(path));
    set_reference(sym.id(), "Pkg:Part2");
    auto ok = fpstatus_probe(*this, sym.id());
    CHECK(ok.resolution.footprint_id == fixed.id());
    CHECK(ok.resolution.status == services::FootprintStatus::Available);
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS never falls back across libraries with the same name")
{
    auto sym = symbol();
    // 别的库里有同名封装且文件真实存在 —— 但用户引用的是 PkgA
    footprint_in("PkgB.pretty", "Part");
    set_reference(sym.id(), "PkgA:Part");
    auto cross = fpstatus_probe(*this, sym.id());
    CHECK(cross.resolution.status == services::FootprintStatus::NotFound);
    CHECK(cross.resolution.reason == std::string(services::fp_reason::kNoMatch));
    CHECK(cross.resolution.footprint_id.empty());
    CHECK_FALSE(cross.json["has_footprint"].get<bool>());

    // 无前缀引用按裸名全局匹配：跨库同名 → ambiguous，不猜一个
    footprint_in("PkgC.pretty", "Part");
    set_reference(sym.id(), "Part");
    auto bare = fpstatus_probe(*this, sym.id());
    CHECK(bare.resolution.status == services::FootprintStatus::Ambiguous);
    CHECK(bare.resolution.reason == std::string(services::fp_reason::kMultipleMatches));
    CHECK(bare.resolution.footprint_id.empty());
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS same name in the same library is ambiguous until a link pins the UUID")
{
    auto sym = symbol();
    // 同库同名、两个不同来源（两个 .pretty 目录都登记成 "Pkg"）
    auto first = footprint_in("one/Pkg.pretty", "Part", true, true, "Pkg");
    auto second = footprint_in("two/Pkg.pretty", "Part", true, true, "Pkg");
    set_reference(sym.id(), "Pkg:Part");
    auto ambiguous = fpstatus_probe(*this, sym.id());
    CHECK(ambiguous.resolution.status == services::FootprintStatus::Ambiguous);
    CHECK(ambiguous.resolution.footprint_id.empty());

    // 明确关联一条 → 歧义解除，身份就是被关联的那条
    link_footprint(sym.id(), first.id());
    auto pinned = fpstatus_probe(*this, sym.id());
    CHECK(pinned.resolution.status == services::FootprintStatus::Available);
    CHECK(pinned.resolution.footprint_id == first.id());

    // 两条不同的匹配关联：仍然 ambiguous，不能按顺序挑
    link_footprint(sym.id(), second.id());
    auto two_links = fpstatus_probe(*this, sym.id());
    CHECK(two_links.resolution.status == services::FootprintStatus::Ambiguous);
    CHECK(two_links.resolution.footprint_id.empty());
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS aggregates every link instead of reading the first row")
{
    auto sym = symbol();
    set_reference(sym.id(), "Pkg:Part");
    auto other = footprint_in("Pkg.pretty", "Other", true, true);
    auto part = footprint_in("Pkg.pretty", "Part", true, true);
    // 第一条关联与当前引用不匹配：不能盖过匹配的那条
    link_footprint(sym.id(), other.id());
    link_footprint(sym.id(), part.id());
    auto result = fpstatus_probe(*this, sym.id());
    CHECK(result.resolution.status == services::FootprintStatus::Available);
    CHECK(result.resolution.footprint_id == part.id());

    // 外部把引用改成 New：只关联了 Old 的符号不能因为旧关联就报正常
    set_reference(sym.id(), "Pkg:New");
    auto changed = fpstatus_probe(*this, sym.id());
    CHECK(changed.resolution.status == services::FootprintStatus::NotFound);
    CHECK(changed.resolution.footprint_id.empty());
    CHECK_FALSE(changed.json["has_footprint"].get<bool>());
    // 读接口不删关联
    {
        auto session = db->session();
        auto rows = storage::RelationshipRepository(session).find_symbol_link_rows(sym.id());
        REQUIRE(rows.has_value());
        CHECK(rows->size() == 2);
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS checks each unique path once per response")
{
    auto fp = footprint_in("Pkg.pretty", "Part", true, true);
    std::vector<std::string> symbols;
    for (int i = 0; i < 3; ++i)
    {
        auto sym = extra_symbol("R_shared" + std::to_string(i), "Pkg:Part");
        link_footprint(sym.id(), fp.id());
        symbols.push_back(sym.id());
    }
    services::Snapshot snapshot;
    std::vector<core::Symbol> rows;
    {
        auto session = db->session();
        for (const auto& id : symbols)
        {
            auto row = storage::SymbolRepository(session).find_by_id(id);
            REQUIRE(row.has_value());
            rows.push_back(*row);
        }
        auto loaded = services::load_snapshot(session);
        REQUIRE(loaded.has_value());
        snapshot = std::move(*loaded);
    }
    services::FileCheckContext files;
    for (const auto& row : rows)
    {
        auto result = services::resolve_symbol(row, snapshot, files);
        CHECK(result.status == services::FootprintStatus::Available);
    }
    CHECK(files.stat_calls() == 1);  // 同一个文件只 stat 一次
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS snapshot loading uses a fixed number of queries")
{
    auto measure = [this]() -> int
    {
        SelectCounter counter;
        auto session = db->session();
        sqlite3_trace_v2(session.handle(), SQLITE_TRACE_STMT, count_selects, &counter);
        auto loaded = services::load_snapshot(session);
        sqlite3_trace_v2(session.handle(), 0, nullptr, nullptr);
        REQUIRE(loaded.has_value());
        return counter.selects;
    };
    auto fp = footprint_in("Pkg.pretty", "Part", false, false);
    auto one = extra_symbol("R_query0", "Pkg:Part");
    link_footprint(one.id(), fp.id());
    const int baseline = measure();
    CHECK(baseline >= 4);  // 封装/模型/关联行/模型关联各一条
    for (int i = 0; i < 20; ++i)
    {
        auto extra = extra_symbol("R_query" + std::to_string(i + 1), "Pkg:Part");
        link_footprint(extra.id(), fp.id());
    }
    CHECK(measure() == baseline);  // 语句数不随符号数增长
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS model status follows the real model files")
{
    auto sym = symbol();
    set_reference(sym.id(), "Pkg:Part");
    auto fp = footprint_in("Pkg.pretty", "Part", true, true);

    auto none = fpstatus_probe(*this, sym.id());
    CHECK(none.resolution.model_status == services::ModelStatus::Unassigned);
    CHECK(none.resolution.model_reason == std::string(services::fp_reason::kNoModelLink));
    CHECK_FALSE(none.json["has_3d_model"].get<bool>());

    // 关联文件存在 → available
    auto present = model_file("present.step", true);
    link_model(fp.id(), present.id());
    auto linked = fpstatus_probe(*this, sym.id());
    CHECK(linked.resolution.model_status == services::ModelStatus::Available);
    CHECK(linked.resolution.model_id == present.id());
    CHECK(linked.resolution.model_name == "present.step");
    CHECK(linked.json["has_3d_model"].get<bool>());

    // 多关联、一个在另一个缺 → available（选存在的那个）
    auto missing = model_file("missing.step", false);
    link_model(fp.id(), missing.id());
    auto partial = fpstatus_probe(*this, sym.id());
    CHECK(partial.resolution.model_status == services::ModelStatus::Available);
    CHECK(partial.resolution.model_id == present.id());
    CHECK(partial.json["has_3d_model"].get<bool>());

    // 全部缺文件 → stale，且不能报正常
    fs::remove(fs::path(present.file_path()));
    auto stale = fpstatus_probe(*this, sym.id());
    CHECK(stale.resolution.model_status == services::ModelStatus::Stale);
    CHECK(stale.resolution.model_reason == std::string(services::fp_reason::kModelFilesMissing));
    CHECK(stale.resolution.model_id.empty());
    CHECK_FALSE(stale.json["has_3d_model"].get<bool>());

    // 封装本身失效时，3D 一律不能显示正常（即使模型文件还在）
    auto again = model_file("again.step", true);
    link_model(fp.id(), again.id());
    fs::remove(fs::path(fp.library_path()));
    auto broken = fpstatus_probe(*this, sym.id());
    CHECK(broken.resolution.status == services::FootprintStatus::Stale);
    CHECK(broken.resolution.model_status != services::ModelStatus::Available);
    CHECK_FALSE(broken.json["has_3d_model"].get<bool>());
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS propagates database read failures instead of faking an empty catalog")
{
    auto fp = footprint_in("Pkg.pretty", "Part", true, true);
    auto sym = symbol();
    link_footprint(sym.id(), fp.id());
    set_reference(sym.id(), "Pkg:Part");

    const char* tables[] = {"footprints", "models_3d", "symbol_footprint_links",
                            "footprint_model_links"};
    for (const char* table : tables)
    {
        g_denied_table = table;
        {
            auto session = db->session();
            REQUIRE(sqlite3_set_authorizer(session.handle(), deny_table_read, nullptr) == SQLITE_OK);
        }
        util::Result<services::Snapshot> denied = [&]
        {
            auto session = db->session();
            return services::load_snapshot(session);
        }();
        CHECK_FALSE(denied.has_value());
        if (!denied)
        {
            CHECK(denied.error().kind() == util::Error::Kind::DbError);
        }
        {
            auto session = db->session();
            sqlite3_set_authorizer(session.handle(), nullptr, nullptr);
        }
        g_denied_table.clear();
        auto recovered = fpstatus_probe(*this, sym.id());
        CHECK(recovered.resolution.status == services::FootprintStatus::Available);
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS resolves utf8 chinese source paths through char8_t")
{
    auto sym = symbol();
    // 中文目录名/文件名：数据库里存的是 UTF-8 字节，落盘用的是同一串
    auto directory = temp.path() / fs::path(std::u8string(u8"中文库.pretty"));
    auto path = directory / fs::path(std::u8string(u8"封装.kicad_mod"));
    write(path, "(footprint \"Part\" (layer \"F.Cu\"))");
    auto utf8_path = utf8_text(path.u8string().c_str());
    auto utf8_lib = utf8_text(u8"中文库");
    auto utf8_name = utf8_text(u8"封装");
    core::Footprint fp;
    fp.set_name(utf8_name);
    fp.set_library_path(utf8_path);
    fp.set_footprint_lib_name(utf8_lib);
    fp.set_source_key(storage::existing_source_key(path));
    {
        auto session = db->session();
        auto inserted = storage::FootprintRepository(session).insert(fp);
        REQUIRE(inserted.has_value());
        fp = *inserted;
    }
    set_reference(sym.id(), utf8_lib + ":" + fp.name());
    auto ok = fpstatus_probe(*this, sym.id());
    CHECK(ok.resolution.status == services::FootprintStatus::Available);
    CHECK(ok.resolution.footprint_id == fp.id());

    std::error_code error;
    fs::remove(path, error);
    REQUIRE_FALSE(error);
    auto gone = fpstatus_probe(*this, sym.id());
    CHECK(gone.resolution.status == services::FootprintStatus::Stale);
    CHECK(gone.resolution.reason == std::string(services::fp_reason::kMissingFile));
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS binding refuses unavailable targets and replaces every old link")
{
    auto sym = symbol();
    auto old = footprint_in("Pkg.pretty", "Old", true, true);
    auto doomed = footprint_in("Pkg.pretty", "Doomed", true, true);
    link_footprint(sym.id(), old.id(), "imported");
    link_footprint(sym.id(), doomed.id(), "imported");  // 历史遗留的第二条旧关联
    fs::remove(fs::path(doomed.library_path()));

    services::SymbolBindingManager manager(*db);
    auto refused = manager.assign_footprint(sym.id(), doomed.id());
    CHECK_FALSE(refused.has_value());
    CHECK(read(file) == source);  // 源文件没有被改写
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(sym.id());
        REQUIRE(row.has_value());
        CHECK(row->footprint() == "Pkg:Old");
        auto links = storage::RelationshipRepository(session).find_symbol_link_rows(sym.id());
        REQUIRE(links.has_value());
        CHECK(links->size() == 2);  // 旧关联原样保留
    }

    // 正常替换：成功后只剩目标那一条
    auto next = footprint_in("Pkg.pretty", "Next", true, true);
    auto assigned = manager.assign_footprint(sym.id(), next.id());
    REQUIRE(assigned.has_value());
    {
        auto session = db->session();
        auto links = storage::RelationshipRepository(session).find_symbol_link_rows(sym.id());
        REQUIRE(links.has_value());
        REQUIRE(links->size() == 1);
        CHECK(links->front().footprint_id == next.id());
    }
    auto bound = fpstatus_probe(*this, sym.id());
    CHECK(bound.resolution.status == services::FootprintStatus::Available);
    CHECK(bound.resolution.footprint_id == next.id());
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS refuses to bind a model to a footprint whose file is gone"){
    auto fp = footprint_in("Pkg.pretty", "Part", true, true);
    auto model = model_file("manual.step", true);
    services::SymbolBindingManager manager(*db);
    REQUIRE(manager.assign_model(fp.id(), model.id()).has_value());
    fs::remove(fs::path(fp.library_path()));
    auto other = model_file("other.step", true);
    auto refused = manager.assign_model(fp.id(), other.id());
    CHECK_FALSE(refused.has_value());
    {
        auto session = db->session();
        auto links = storage::RelationshipRepository(session).find_models_for_footprint(fp.id());
        REQUIRE(links.has_value());
        REQUIRE(links->size() == 1);
        CHECK(links->front() == model.id());  // 旧关联没有被换掉
    }
}

namespace
{
/// 执行期间删掉复用封装文件的桩插件（路径/计数文件直接嵌进脚本）
std::string fpstatus_delete_stub(const fs::path& victim, const fs::path& marker)
{
    std::string script = R"PY(
import json, os, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
lcsc = args.get("lcsc_id", "")
name = "STUB_" + lcsc
symbol = ('(symbol "%s" (property "Reference" "U") (property "Footprint" "old:fp"))') % name
with open(os.path.join(staging, name + ".kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator stub)\n" + symbol + "\n)\n")
manifest = {"protocol": "staged-import-v1", "lcsc_id": lcsc,
            "symbols": [{"name": name, "file": name + ".kicad_sym"}], "footprint": None}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
try:
    os.remove(r"__VICTIM__")
except OSError:
    pass
with open(r"__MARKER__", "a", encoding="utf-8") as f:
    f.write("run\n")
print(json.dumps({"ok": True}))
)PY";
    auto replace_all = [](std::string& text, const std::string& needle, const std::string& value)
    {
        for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at))
            text.replace(at, needle.size(), value);
    };
    replace_all(script, "__VICTIM__", victim.string());
    replace_all(script, "__MARKER__", marker.string());
    return script;
}
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS staged reuse deleted during the plugin run rolls back everything")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged reuse test skipped");
        return;
    }

    auto root = temp.path() / "fpstatus_stage";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "LCSC", sym_dir.string(), fp_dir.string());

    // 先正常导入一次，得到一个真实的封装行
    auto seed_manager = lcsc01_plugins(temp.path() / "plugins_fpstatus_seed");
    api::StagedImporter seed_importer(*db, *seed_manager, &matching_store);
    auto seeded = seed_importer.run(lcsc01_request(package_id, "C901", "fpstatus-seed"));
    if (!seeded)
        MESSAGE("seed import failed: ", seeded.error().format_message());
    REQUIRE(seeded.has_value());
    const std::string footprint_id = seeded->value("footprint_id", std::string(""));
    REQUIRE_FALSE(footprint_id.empty());
    auto reused_path = fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod";
    REQUIRE(fs::exists(reused_path));
    auto symbol_file = sym_dir / "LCSC.kicad_sym";
    const std::string symbol_before = read(symbol_file);
    const int operations_before = count_rows(*db, "import_operations");
    const int links_before = count_rows(*db, "symbol_footprint_links");

    // 第二次请求复用这个封装，而插件在执行期间把它删掉
    auto marker = temp.path() / "delete_runs.txt";
    auto scan = temp.path() / "plugins_fpstatus_delete";
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", fpstatus_delete_stub(reused_path, marker));
    plugin::PluginManager manager(std::vector<fs::path>{scan});
    manager.discover();
    api::StagedImporter importer(*db, manager, &matching_store);

    auto request = lcsc01_request(package_id, "C902", "fpstatus-reuse");
    request.footprint_mode = "reuse";
    request.footprint_id = footprint_id;
    auto result = importer.run(request);
    CHECK_FALSE(result.has_value());
    CHECK(count_lines(marker) == 1);  // 插件确实跑过（不是被前置检查拦下）
    CHECK_FALSE(fs::exists(reused_path));  // 复用文件在插件执行期间消失

    // 整次失败：源文件回滚、没有新关联、没有新操作记录
    CHECK(read(symbol_file) == symbol_before);
    CHECK(count_rows(*db, "import_operations") == operations_before);
    CHECK(count_rows(*db, "symbol_footprint_links") == links_before);
    CHECK(count_rows(*db, "import_sources") == 1);
    CHECK(count_rows(*db, "symbols") == 1);
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS replays a recorded operation without reimporting a vanished reuse source")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged replay test skipped");
        return;
    }

    auto root = temp.path() / "fpstatus_replay";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "LCSC", sym_dir.string(), fp_dir.string());

    auto manager = lcsc01_plugins(temp.path() / "plugins_fpstatus_replay");
    api::StagedImporter importer(*db, *manager, &matching_store);
    auto seeded = importer.run(lcsc01_request(package_id, "C903", "fpstatus-replay-seed"));
    REQUIRE(seeded.has_value());
    const std::string footprint_id = seeded->value("footprint_id", std::string(""));
    REQUIRE_FALSE(footprint_id.empty());

    auto request = lcsc01_request(package_id, "C904", "fpstatus-replay");
    request.footprint_mode = "reuse";
    request.footprint_id = footprint_id;
    auto first = importer.run(request);
    if (!first)
        MESSAGE("reuse import failed: ", first.error().format_message());
    REQUIRE(first.has_value());
    CHECK(first->value("mode", std::string("")) == "reuse");
    CHECK(first->value("footprint_reference", std::string("")) == "LCSC:STUBFP");
    const std::string symbol_id = first->value("symbol_id", std::string(""));
    const int operations = count_rows(*db, "import_operations");

    // 复用来源随后失效：回放仍然返回原结果（不为修状态重跑），但列表要显示失效
    auto reused_path = fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod";
    std::error_code error;
    fs::remove(reused_path, error);
    REQUIRE_FALSE(error);
    auto replay = importer.run(request);
    if (!replay)
        MESSAGE("replay of a vanished reuse source failed: ", replay.error().format_message());
    REQUIRE(replay.has_value());
    CHECK(replay->value("replayed", false));
    CHECK(replay->value("symbol_id", std::string("")) == symbol_id);
    CHECK(count_rows(*db, "import_operations") == operations);

    auto live = fpstatus_probe(*this, symbol_id);
    CHECK(live.resolution.status == services::FootprintStatus::Stale);
    CHECK(live.resolution.reason == std::string(services::fp_reason::kMissingFile));
    CHECK_FALSE(live.json["has_footprint"].get<bool>());
}

// ============================================================
// FPSTATUS round1 —— 针对「当前实现仍然存在」的缺陷补行为回归。
// 全部用 TempDirectory 里的真实文件/真实 SQLite 行做证据；不为制造红色
// 而放宽断言，也不修改产品实现。
// ============================================================
namespace
{
void fpstatus_substitute(std::string& text, const std::string& needle, const std::string& value)
{
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at))
        text.replace(at, needle.size(), value);
}

/// 产物与 LCSC01 桩一致（符号名由零件号决定），但会在插件执行期间按内嵌 SQL
/// 改一次库，并且每执行一次就往计数文件追加一行 —— 用来区分「宿主拒绝」与
/// 「前置检查就把插件挡掉了」。
std::string fpstatus_mutating_stub(const fs::path& marker, const fs::path& db_path,
                                   const std::string& sql)
{
    std::string script = R"PY(
import json, os, sqlite3, sys
args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
staging = args.get("staging_dir", "")
os.makedirs(staging, exist_ok=True)
lcsc = args.get("lcsc_id", "")
base = lcsc[:-1] if lcsc.endswith("X") else lcsc
name = "STUB_" + base
symbol = ('(symbol "%s" (property "Reference" "U") (property "Footprint" "old:fp"))') % name
with open(os.path.join(staging, name + ".kicad_sym"), "w", encoding="utf-8") as f:
    f.write("(kicad_symbol_lib (version 20231120) (generator stub)\n" + symbol + "\n)\n")
mode = (args.get("footprint_choice") or {}).get("mode", "new")
entry = None
if mode != "reuse":
    with open(os.path.join(staging, "STUBFP.kicad_mod"), "w", encoding="utf-8") as f:
        f.write('(footprint "STUBFP" (layer "F.Cu"))')
    entry = {"name": "STUBFP", "file": "STUBFP.kicad_mod"}
manifest = {"protocol": "staged-import-v1", "lcsc_id": lcsc,
            "symbols": [{"name": name, "file": name + ".kicad_sym"}], "footprint": entry}
with open(os.path.join(staging, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f)
mutation = r"__SQL__"
if mutation:
    con = sqlite3.connect(r"__DB__")
    con.execute(mutation)
    con.commit()
    con.close()
with open(r"__MARKER__", "a", encoding="utf-8") as f:
    f.write("run\n")
print(json.dumps({"ok": True}))
)PY";
    fpstatus_substitute(script, "__SQL__", sql);
    fpstatus_substitute(script, "__DB__", db_path.string());
    fpstatus_substitute(script, "__MARKER__", marker.string());
    return script;
}

/// 复用导入的最小环境：包 + 桩 + 导入器（返回导入器与计数文件）
struct FpstatusReuse
{
    storage::Database* db{nullptr};
    classifier::TypeMatchingStore* matching_store{nullptr};
    fs::path root;
    fs::path sym_dir;
    fs::path fp_dir;
    fs::path scan;
    fs::path marker;
    fs::path db_path;
    std::string package_id;
    std::unique_ptr<plugin::PluginManager> manager;
    std::unique_ptr<api::StagedImporter> importer;

    void stage(LibraryFixture& fixture, const std::string& name, const std::string& sql)
    {
        db = fixture.db.get();
        matching_store = &fixture.matching_store;
        root = fixture.temp.path() / name;
        sym_dir = root / "symbols";
        fp_dir = root / "footprints";
        fs::create_directories(sym_dir);
        fs::create_directories(fp_dir);
        marker = fixture.temp.path() / (name + "_runs.txt");
        db_path = fixture.temp.path() / "test.db";
        REQUIRE(fs::exists(db_path));
        package_id = lcsc01_register_package(*fixture.db, "LCSC", sym_dir.string(), fp_dir.string());
        scan = fixture.temp.path() / ("plugins_" + name);
        write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
        write(scan / "stub" / "plugin.py", fpstatus_mutating_stub(marker, db_path, sql));
        manager = std::make_unique<plugin::PluginManager>(std::vector<fs::path>{scan});
        manager->discover();
        importer = std::make_unique<api::StagedImporter>(*fixture.db, *manager, matching_store);
    }

    /// 换一份插件实现（同一个扫描目录、同一个计数文件）。
    /// 先销毁旧导入器再换管理器：导入器持有管理器引用，不能留下悬空引用。
    void rewrite(const std::string& sql)
    {
        importer.reset();
        write(scan / "stub" / "plugin.py", fpstatus_mutating_stub(marker, db_path, sql));
        manager = std::make_unique<plugin::PluginManager>(std::vector<fs::path>{scan});
        manager->discover();
        importer = std::make_unique<api::StagedImporter>(*db, *manager, matching_store);
    }
};
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS keeps case-sensitive footprint identity")
{
    // 库里叫 foo（lower/Pkg.pretty 下的真实文件），符号引用写 Pkg:Foo —— 不同身份，不能折叠大小写命中。
    // 两个大小写变体必须落在两个不同的 .pretty 目录：Windows 上同一物理路径只有一条
    // source_key（唯一索引），第二行会被 INSERT OR IGNORE 静默丢掉，那不是被测的「同名」场景。
    footprint_in("lower/Pkg.pretty", "foo", true, true, "Pkg");
    auto sym = symbol();
    set_reference(sym.id(), "Pkg:Foo");
    auto folded = fpstatus_probe(*this, sym.id());
    CHECK(folded.resolution.status == services::FootprintStatus::NotFound);
    CHECK(folded.resolution.reason == std::string(services::fp_reason::kNoMatch));
    CHECK(folded.resolution.footprint_id.empty());
    CHECK_FALSE(folded.json["has_footprint"].get<bool>());

    // 精确的那一行进来以后：Pkg:Foo 唯一定位到它，不是「同名歧义」。
    // 两行的物理来源互不相同（lower/upper 两个真实目录，footprint_lib_name 都是 Pkg），
    // 大小写差异只体现在 KiCad 引用身份上，不掺进文件系统层面的同一路径
    auto exact_row = footprint_in("upper/Pkg.pretty", "Foo", true, true, "Pkg");
    auto exact = fpstatus_probe(*this, sym.id());
    CHECK(exact.resolution.status == services::FootprintStatus::Available);
    CHECK(exact.resolution.footprint_id == exact_row.id());
    CHECK(exact.resolution.resolved_reference == "Pkg:Foo");

    // 库名也只认精确：pkg:Foo 不等于 Pkg:Foo
    set_reference(sym.id(), "pkg:Foo");
    auto lib_case = fpstatus_probe(*this, sym.id());
    CHECK(lib_case.resolution.status == services::FootprintStatus::NotFound);
    CHECK(lib_case.resolution.footprint_id.empty());

    // 无前缀的裸名同样按精确名解析：只有 Foo 命中，没有第二个「同名」候选
    set_reference(sym.id(), "Foo");
    auto bare = fpstatus_probe(*this, sym.id());
    CHECK(bare.resolution.status == services::FootprintStatus::Available);
    CHECK(bare.resolution.footprint_id == exact_row.id());

    // 显式 UUID 关联必须与当前引用精确匹配：关联到 foo 的那条不能替 Pkg:Foo 作证
    set_reference(sym.id(), "Pkg:Foo");
    std::string lower_id;
    {
        auto session = db->session();
        auto rows = storage::FootprintRepository(session).find_all();
        REQUIRE(rows.has_value());
        for (const auto& row : *rows)
            if (row.name() == "foo") lower_id = row.id();
    }  // 会话必须先释放：db->session() 不可重入，link_footprint 内部还会再取一个
    REQUIRE_FALSE(lower_id.empty());
    link_footprint(sym.id(), lower_id);
    link_footprint(sym.id(), exact_row.id());
    auto pinned = fpstatus_probe(*this, sym.id());
    CHECK(pinned.resolution.status == services::FootprintStatus::Available);
    CHECK(pinned.resolution.footprint_id == exact_row.id());
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS treats a missing parent directory as a stale file and caches repeated checks")
{
    // 父路径是普通文件：/blocker/Child.kicad_mod 既不是「查不动」，也不是可用文件
    auto blocker = temp.path() / "blocker";
    write(blocker, "not a directory");
    auto broken_path = blocker / "Child.kicad_mod";
    footprint_row("Child", broken_path);
    auto sym = symbol();
    set_reference(sym.id(), "blocker:Child");
    auto broken = fpstatus_probe(*this, sym.id());
    CHECK(broken.resolution.status == services::FootprintStatus::Stale);
    CHECK(broken.resolution.reason == std::string(services::fp_reason::kMissingFile));

    // 同一批符号引用同一个缺失路径：FileCheckContext 只查一次。判定所需的
    // 「存在 / 是目录 / 是普通文件」全部来自那一次 status 的结果，判定里不查第二遍。
    // stat_calls 只统计 Context 内的显式 status（去重缓存生效的直接证据），
    // 不宣称整个进程的文件系统访问只有一次。
    auto missing_path = temp.path() / "Pkg.pretty" / "gone.kicad_mod";
    auto missing_row = footprint_row("Gone", missing_path);
    std::vector<core::Symbol> missing_syms;
    for (int i = 0; i < 3; ++i)
    {
        auto row = extra_symbol("R_missing" + std::to_string(i), "Pkg:Gone");
        link_footprint(row.id(), missing_row.id());
        missing_syms.push_back(row);
    }
    services::Snapshot snapshot;
    {
        auto session = db->session();
        auto loaded = services::load_snapshot(session);
        REQUIRE(loaded.has_value());
        snapshot = std::move(*loaded);
    }
    services::FileCheckContext files;
    for (const auto& row : missing_syms)
    {
        auto result = services::resolve_symbol(row, snapshot, files);
        CHECK(result.status == services::FootprintStatus::Stale);
        CHECK(result.reason == std::string(services::fp_reason::kMissingFile));
    }
    CHECK(files.stat_calls() == 1);  // 缺失路径同样走去重缓存
    // fresh=true 绕过去重：变更前复核必须看当前磁盘状态
    files.check(missing_path, true);
    CHECK(files.stat_calls() == 2);

    // 路径被目录替代也一样去重、一样是 stale/not_regular_file
    auto dir_path = temp.path() / "Pkg.pretty" / "Adir.kicad_mod";
    fs::create_directories(dir_path);
    auto dir_row = footprint_row("Adir", dir_path);
    std::vector<core::Symbol> dir_syms;
    for (int i = 0; i < 3; ++i)
    {
        auto row = extra_symbol("R_directory" + std::to_string(i), "Pkg:Adir");
        link_footprint(row.id(), dir_row.id());
        dir_syms.push_back(row);
    }
    // 这一组的行/关联是上一份快照之后才写进去的：必须重新加载快照。否则关联的 UUID
    // 不在快照里，解析只能走到 not_found，测不到「目录替代」这条路径
    services::Snapshot dir_snapshot;
    {
        auto session = db->session();
        auto loaded = services::load_snapshot(session);
        REQUIRE(loaded.has_value());
        dir_snapshot = std::move(*loaded);
    }
    services::FileCheckContext dir_files;
    for (const auto& row : dir_syms)
    {
        auto result = services::resolve_symbol(row, dir_snapshot, dir_files);
        CHECK(result.status == services::FootprintStatus::Stale);
        CHECK(result.reason == std::string(services::fp_reason::kNotRegularFile));
    }
    CHECK(dir_files.stat_calls() == 1);
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS keeps database read failures as DbError through bind and bind-model")
{
    auto sym = symbol();
    auto old = footprint_in("Pkg.pretty", "Old", true, true);
    auto next = footprint_in("Pkg.pretty", "Next", true, true);
    auto model = model_file("bind.step", true);
    link_footprint(sym.id(), old.id(), "imported");
    const std::string source_before = read(file);
    const int links_before = count_rows(*db, "symbol_footprint_links");
    const int models_before = count_rows(*db, "footprint_model_links");
    const int operations_before = count_rows(*db, "import_operations");

    services::SymbolBindingManager manager(*db);
    g_denied_table = "footprints";
    {
        auto session = db->session();
        REQUIRE(sqlite3_set_authorizer(session.handle(), deny_table_read, nullptr) == SQLITE_OK);
    }
    auto refused = manager.assign_footprint(sym.id(), next.id());
    CHECK_FALSE(refused.has_value());
    if (!refused)
    {
        MESSAGE("assign_footprint error: ", refused.error().format_message());
        CHECK(refused.error().kind() == util::Error::Kind::DbError);
    }
    auto model_refused = manager.assign_model(old.id(), model.id());
    CHECK_FALSE(model_refused.has_value());
    if (!model_refused)
    {
        MESSAGE("assign_model error: ", model_refused.error().format_message());
        CHECK(model_refused.error().kind() == util::Error::Kind::DbError);
    }
    {
        auto session = db->session();
        sqlite3_set_authorizer(session.handle(), nullptr, nullptr);
    }
    g_denied_table.clear();

    // 拒绝必须零副作用：源字节、引用、两类关联、操作记录都不动
    CHECK(read(file) == source_before);
    CHECK(count_rows(*db, "symbol_footprint_links") == links_before);
    CHECK(count_rows(*db, "footprint_model_links") == models_before);
    CHECK(count_rows(*db, "import_operations") == operations_before);
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(sym.id());
        REQUIRE(row.has_value());
        CHECK(row->footprint() == "Pkg:Old");
    }

    // 故障解除后同一个 manager 正常绑定
    auto assigned = manager.assign_footprint(sym.id(), next.id());
    if (!assigned) MESSAGE("assign after recovery: ", assigned.error().format_message());
    CHECK(assigned.has_value());
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS staged reuse propagates a denied read and never runs the plugin")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged reuse test skipped");
        return;
    }

    FpstatusReuse reuse;
    reuse.stage(*this, "fpstatus_denied", "");

    // 先正常导入一次，拿到真实可复用的封装行（计数文件此时 1 次执行）
    auto seeded = reuse.importer->run(lcsc01_request(reuse.package_id, "C921", "fpstatus-denied-seed"));
    if (!seeded) MESSAGE("seed import failed: ", seeded.error().format_message());
    REQUIRE(seeded.has_value());
    const std::string footprint_id = seeded->value("footprint_id", std::string(""));
    REQUIRE_FALSE(footprint_id.empty());
    REQUIRE(count_lines(reuse.marker) == 1);
    const int operations_before = count_rows(*db, "import_operations");

    auto request = lcsc01_request(reuse.package_id, "C922", "fpstatus-denied");
    request.footprint_mode = "reuse";
    request.footprint_id = footprint_id;

    g_denied_table = "footprints";
    {
        auto session = db->session();
        REQUIRE(sqlite3_set_authorizer(session.handle(), deny_table_read, nullptr) == SQLITE_OK);
    }
    auto denied = reuse.importer->run(request);
    CHECK_FALSE(denied.has_value());
    if (!denied)
    {
        MESSAGE("denied reuse error: ", denied.error().format_message());
        CHECK(denied.error().kind() == util::Error::Kind::DbError);
    }
    {
        auto session = db->session();
        sqlite3_set_authorizer(session.handle(), nullptr, nullptr);
    }
    g_denied_table.clear();

    // 读故障不是「封装不存在」：插件没有被调用，也没有留下操作记录
    CHECK(count_lines(reuse.marker) == 1);
    CHECK(count_rows(*db, "import_operations") == operations_before);

    // 故障解除后同一请求正常导入
    auto recovered = reuse.importer->run(request);
    if (!recovered) MESSAGE("reuse after recovery: ", recovered.error().format_message());
    CHECK(recovered.has_value());
    CHECK(count_lines(reuse.marker) == 2);
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS a manager recovers from an index failure without a database change")
{
    auto fp = footprint_in("Pkg.pretty", "Part", true, true);
    const int footprints_before = count_rows(*db, "footprints");
    const int links_before = count_rows(*db, "symbol_footprint_links");
    const auto version_before = db->data_version();

    g_denied_table = "footprints";
    {
        auto session = db->session();
        REQUIRE(sqlite3_set_authorizer(session.handle(), deny_table_read, nullptr) == SQLITE_OK);
    }
    services::SymbolBindingManager manager(*db);  // 构造期读索引就被拒
    auto denied = manager.search_footprints("");
    CHECK_FALSE(denied.has_value());
    if (!denied)
    {
        CHECK(denied.error().kind() == util::Error::Kind::DbError);
    }
    {
        auto session = db->session();
        sqlite3_set_authorizer(session.handle(), nullptr, nullptr);
    }
    g_denied_table.clear();

    // 同一个 manager：故障期间的失败不能变成永久 sticky（不写库、不换版本也要能恢复）
    auto recovered = manager.search_footprints("");
    if (!recovered) MESSAGE("search after recovery: ", recovered.error().format_message());
    REQUIRE(recovered.has_value());
    bool found = false;
    for (const auto& ref : *recovered)
        if (ref.id == fp.id()) found = true;
    CHECK(found);
    auto models = manager.search_models("");
    CHECK(models.has_value());

    CHECK(db->data_version() == version_before);
    CHECK(count_rows(*db, "footprints") == footprints_before);
    CHECK(count_rows(*db, "symbol_footprint_links") == links_before);
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS staged reuse rejects a row renamed during the plugin run")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged reuse test skipped");
        return;
    }

    FpstatusReuse reuse;
    reuse.stage(*this, "fpstatus_rename", "");
    auto seeded = reuse.importer->run(lcsc01_request(reuse.package_id, "C931", "fpstatus-rename-seed"));
    if (!seeded) MESSAGE("seed import failed: ", seeded.error().format_message());
    REQUIRE(seeded.has_value());
    const std::string footprint_id = seeded->value("footprint_id", std::string(""));
    REQUIRE_FALSE(footprint_id.empty());
    auto symbol_file = reuse.sym_dir / "LCSC.kicad_sym";
    REQUIRE(fs::exists(symbol_file));

    // 对照组：同样复用、但不改库 → 成功。用来证明后面的失败确实由「身份漂移」引起，
    // 而不是这条复用路径本身就走不通。
    auto control = lcsc01_request(reuse.package_id, "C932", "fpstatus-rename-control");
    control.footprint_mode = "reuse";
    control.footprint_id = footprint_id;
    auto control_result = reuse.importer->run(control);
    if (!control_result) MESSAGE("control reuse failed: ", control_result.error().format_message());
    REQUIRE(control_result.has_value());
    const std::string control_symbol = control_result->value("symbol_id", std::string(""));
    REQUIRE_FALSE(control_symbol.empty());
    REQUIRE(count_lines(reuse.marker) == 2);  // 桩每执行一次追加一行：种子 + 对照各一次

    const std::string symbol_before = read(symbol_file);
    const int operations_before = count_rows(*db, "import_operations");
    const int links_before = count_rows(*db, "symbol_footprint_links");
    const int symbols_before = count_rows(*db, "symbols");

    // 漂移组：插件执行期间把复用行的名字改掉（UUID、文件都没变）
    reuse.rewrite("UPDATE footprints SET name='RENAMED' WHERE id='" + footprint_id + "'");
    auto request = lcsc01_request(reuse.package_id, "C933", "fpstatus-rename-drift");
    request.footprint_mode = "reuse";
    request.footprint_id = footprint_id;
    auto drifted = reuse.importer->run(request);
    CHECK_FALSE(drifted.has_value());
    CHECK(count_lines(reuse.marker) == 3);  // 插件确实跑过（种子+对照+漂移），不是被前置检查拦下

    // 宿主持久副作用为零：符号源文件、关联、操作记录、符号行都不动
    CHECK(read(symbol_file) == symbol_before);
    CHECK(count_rows(*db, "import_operations") == operations_before);
    CHECK(count_rows(*db, "symbol_footprint_links") == links_before);
    CHECK(count_rows(*db, "symbols") == symbols_before);
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(control_symbol);
        REQUIRE(row.has_value());
        CHECK(row->footprint() == "LCSC:STUBFP");
        auto links = storage::RelationshipRepository(session).find_symbol_link_rows(control_symbol);
        REQUIRE(links.has_value());
        REQUIRE(links->size() == 1);
        CHECK(links->front().footprint_id == footprint_id);
    }
    // 插件自己的外部改动不属于宿主要回滚的东西：改名照旧留在库里
    {
        auto session = db->session();
        auto row = storage::FootprintRepository(session).find_by_id(footprint_id);
        REQUIRE(row.has_value());
        CHECK(row->name() == "RENAMED");
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS staged reuse rejects a retargeted row during the plugin run")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged reuse test skipped");
        return;
    }

    FpstatusReuse reuse;
    reuse.stage(*this, "fpstatus_retarget", "");
    auto seeded = reuse.importer->run(lcsc01_request(reuse.package_id, "C941", "fpstatus-retarget-seed"));
    if (!seeded) MESSAGE("seed import failed: ", seeded.error().format_message());
    REQUIRE(seeded.has_value());
    const std::string footprint_id = seeded->value("footprint_id", std::string(""));
    REQUIRE_FALSE(footprint_id.empty());
    auto symbol_file = reuse.sym_dir / "LCSC.kicad_sym";
    REQUIRE(fs::exists(symbol_file));

    auto control = lcsc01_request(reuse.package_id, "C942", "fpstatus-retarget-control");
    control.footprint_mode = "reuse";
    control.footprint_id = footprint_id;
    REQUIRE(reuse.importer->run(control).has_value());
    REQUIRE(count_lines(reuse.marker) == 2);  // 桩每执行一次追加一行：种子 + 对照各一次

    const std::string symbol_before = read(symbol_file);
    const int operations_before = count_rows(*db, "import_operations");
    const int links_before = count_rows(*db, "symbol_footprint_links");

    // 另一个真实可用的封装文件：把复用行的路径/来源键整体换过去（文件存在，
    // 但已经不是用户当初选中的那个来源）
    auto other_path = reuse.fp_dir / "Other.pretty" / "STUBFP.kicad_mod";
    write(other_path, "(footprint \"STUBFP\" (layer \"F.Cu\"))");
    const std::string other_key = storage::existing_source_key(other_path);
    REQUIRE_FALSE(other_key.empty());
    reuse.rewrite("UPDATE footprints SET library_path='" + other_path.string() +
                  "', source_key='" + other_key + "', footprint_lib_name='Other' WHERE id='" +
                  footprint_id + "'");

    auto request = lcsc01_request(reuse.package_id, "C943", "fpstatus-retarget-drift");
    request.footprint_mode = "reuse";
    request.footprint_id = footprint_id;
    auto drifted = reuse.importer->run(request);
    CHECK_FALSE(drifted.has_value());
    CHECK(count_lines(reuse.marker) == 3);  // 插件确实跑过（种子+对照+漂移），不是被前置检查拦下

    CHECK(read(symbol_file) == symbol_before);
    CHECK(count_rows(*db, "import_operations") == operations_before);
    CHECK(count_rows(*db, "symbol_footprint_links") == links_before);
    {
        // 外部的换源改动保留（不是宿主的回滚对象），但宿主没有提交指向旧引用的新关联
        auto session = db->session();
        auto row = storage::FootprintRepository(session).find_by_id(footprint_id);
        REQUIRE(row.has_value());
        CHECK(row->library_path() == other_path.string());
        CHECK(row->source_key() == other_key);
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS replays a recorded reuse after its database row was deleted")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged replay test skipped");
        return;
    }

    FpstatusReuse reuse;
    reuse.stage(*this, "fpstatus_vanish", "");
    auto seeded = reuse.importer->run(lcsc01_request(reuse.package_id, "C951", "fpstatus-vanish-seed"));
    if (!seeded) MESSAGE("seed import failed: ", seeded.error().format_message());
    REQUIRE(seeded.has_value());
    const std::string footprint_id = seeded->value("footprint_id", std::string(""));
    REQUIRE_FALSE(footprint_id.empty());
    REQUIRE(count_lines(reuse.marker) == 1);

    auto request = lcsc01_request(reuse.package_id, "C952", "fpstatus-vanish");
    request.footprint_mode = "reuse";
    request.footprint_id = footprint_id;
    auto first = reuse.importer->run(request);
    if (!first) MESSAGE("reuse import failed: ", first.error().format_message());
    REQUIRE(first.has_value());
    CHECK_FALSE(first->value("replayed", false));
    const std::string symbol_id = first->value("symbol_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());
    REQUIRE(count_lines(reuse.marker) == 2);
    const int operations = count_rows(*db, "import_operations");

    // 复用来源的 DB 行被删除（关联随外键级联），文件也不再是判定依据
    {
        auto session = db->session();
        REQUIRE(storage::FootprintRepository(session).remove(footprint_id).has_value());
        auto gone = storage::FootprintRepository(session).find_by_id(footprint_id);
        REQUIRE_FALSE(gone.has_value());
    }
    const int links_after_delete = count_rows(*db, "symbol_footprint_links");
    const int symbols_before = count_rows(*db, "symbols");

    // 同 operation_id 再次请求：历史成功回放，不重跑插件、不写库
    auto replay = reuse.importer->run(request);
    if (!replay) MESSAGE("replay failed: ", replay.error().format_message());
    REQUIRE(replay.has_value());
    CHECK(replay->value("replayed", false));
    CHECK(replay->value("symbol_id", std::string("")) == symbol_id);
    CHECK(count_lines(reuse.marker) == 2);  // 插件没有被执行
    CHECK(count_rows(*db, "import_operations") == operations);
    CHECK(count_rows(*db, "symbols") == symbols_before);
    CHECK(count_rows(*db, "symbol_footprint_links") == links_after_delete);

    // 新的操作请求拿同一个失效 UUID：必须拒绝，且不执行插件
    auto fresh = lcsc01_request(reuse.package_id, "C953", "fpstatus-vanish-new");
    fresh.footprint_mode = "reuse";
    fresh.footprint_id = footprint_id;
    auto rejected = reuse.importer->run(fresh);
    CHECK_FALSE(rejected.has_value());
    CHECK(count_lines(reuse.marker) == 2);
    CHECK(count_rows(*db, "import_operations") == operations);
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS keeps the linked UUID when its file is gone but a twin exists")
{
    // 同库同名两条来源：明确关联的那条文件已被删除，另一条文件真实存在。
    // 身份由关联 UUID 钉死 —— 绝不能因为「另一个同名文件还在」就自动切过去。
    auto sym = symbol();
    auto gone = footprint_in("one/Pkg.pretty", "Part", false, false, "Pkg");
    auto twin = footprint_in("two/Pkg.pretty", "Part", true, true, "Pkg");
    link_footprint(sym.id(), gone.id());
    set_reference(sym.id(), "Pkg:Part");

    auto result = fpstatus_probe(*this, sym.id());
    CHECK(result.resolution.status == services::FootprintStatus::Stale);
    CHECK(result.resolution.reason == std::string(services::fp_reason::kMissingFile));
    CHECK(result.resolution.footprint_id == gone.id());  // 还是被选中的那一条
    CHECK(result.resolution.footprint_id != twin.id());
    CHECK_FALSE(result.json["has_footprint"].get<bool>());

    // 关联被换成真实存在的那条之后才可能是 available（是用户/导入改的关联，不是自动切换）
    link_footprint(sym.id(), twin.id());
    auto switched = fpstatus_probe(*this, sym.id());
    CHECK(switched.resolution.status == services::FootprintStatus::Ambiguous);
    CHECK(switched.resolution.footprint_id.empty());
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS propagates a mid-read failure of the relationship tables")
{
    auto fp = footprint_in("Pkg.pretty", "Part", true, true);
    auto sym = symbol();
    link_footprint(sym.id(), fp.id());
    set_reference(sym.id(), "Pkg:Part");

    // 只在读到关联表时开中断：最终 step 以 SQLITE_INTERRUPT 结束（不是 SQLITE_DONE）。
    // 与「整表拒读」（prepare 失败）不同，这条路径证明的正是**读完之前出错**也不能
    // 被当成「表就这些行」—— 半截关联清单会被当成「这些符号没有关联」。
    {
        auto session = db->session();
        REQUIRE(sqlite3_set_authorizer(session.handle(), arm_interrupt_on_links_read, nullptr) ==
                SQLITE_OK);
        sqlite3_progress_handler(session.handle(), 1, interrupt_when_armed, nullptr);
        auto loaded = services::load_snapshot(session);
        sqlite3_progress_handler(session.handle(), 0, nullptr, nullptr);
        sqlite3_set_authorizer(session.handle(), nullptr, nullptr);
        CHECK_FALSE(loaded.has_value());
        if (!loaded)
        {
            MESSAGE("mid-read failure: ", loaded.error().format_message());
            CHECK(loaded.error().kind() == util::Error::Kind::DbError);
        }
    }
    // 故障解除后同一份数据照常读出（不是「空快照也算成功」）
    auto recovered = fpstatus_probe(*this, sym.id());
    CHECK(recovered.resolution.status == services::FootprintStatus::Available);
    CHECK(recovered.resolution.footprint_id == fp.id());
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS footprint identity treats rename retarget and case drift as drift")
{
    auto row = footprint_in("one/Pkg.pretty", "Part", true, true, "Pkg");
    auto identity_of = [this](const std::string& id)
    {
        auto session = db->session();
        auto found = storage::FootprintRepository(session).find_by_id(id);
        REQUIRE(found.has_value());
        return services::capture_footprint_identity(*found);
    };
    auto mutate = [this](const std::string& id, const auto& apply)
    {
        auto session = db->session();
        auto found = storage::FootprintRepository(session).find_by_id(id);
        REQUIRE(found.has_value());
        apply(*found);
        REQUIRE(storage::FootprintRepository(session).update(*found).has_value());
    };
    const auto base = identity_of(row.id());
    // 同一行重新读：身份相等（这是提交前复核放行的唯一条件）
    CHECK(services::same_footprint_identity(base, identity_of(row.id())));

    // 改名：同 UUID、同文件、同来源也不是同一身份
    mutate(row.id(), [](core::Footprint& fp) { fp.set_name("Renamed"); });
    CHECK_FALSE(services::same_footprint_identity(base, identity_of(row.id())));
    mutate(row.id(), [](core::Footprint& fp) { fp.set_name("Part"); });
    CHECK(services::same_footprint_identity(base, identity_of(row.id())));

    // 换库名：引用前缀会跟着变
    mutate(row.id(), [](core::Footprint& fp) { fp.set_footprint_lib_name("Other"); });
    CHECK_FALSE(services::same_footprint_identity(base, identity_of(row.id())));
    mutate(row.id(), [](core::Footprint& fp) { fp.set_footprint_lib_name("Pkg"); });
    CHECK(services::same_footprint_identity(base, identity_of(row.id())));

    // 挪到另一份真实文件：路径与来源键一起变
    auto other = temp.path() / "Elsewhere.pretty" / "Part.kicad_mod";
    write(other, "(footprint \"Part\" (layer \"F.Cu\"))");
    mutate(row.id(),
           [&](core::Footprint& fp)
           {
               fp.set_library_path(utf8_text(other.u8string().c_str()));
               fp.set_source_key(storage::existing_source_key(other));
           });
    CHECK_FALSE(services::same_footprint_identity(base, identity_of(row.id())));

    // 只改名字大小写：KiCad 引用身份精确，大小写不同就是不同身份
    mutate(row.id(), [](core::Footprint& fp) { fp.set_name("part"); });
    CHECK_FALSE(services::same_footprint_identity(base, identity_of(row.id())));
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS round-trips chinese footprint and model paths through bind and get")
{
    // 中文目录 + 中文封装名：路径文本是 UTF-8 契约，落盘、入库、读回都必须是同一串。
    // 模型放在同一个中文目录下（文件名保持 ASCII，引用文本走既有 path_util 解析）。
    // 文本一律用 u8 字面量构造：窄字面量的执行字符集取决于编译器，u8 才是 UTF-8 契约
    const auto utf8_lib = utf8_text(u8"中文库");
    const auto utf8_name = utf8_text(u8"封装");
    auto directory = temp.path() / fs::path(std::u8string(u8"中文库.pretty"));
    auto file_path = directory / fs::path(std::u8string(u8"封装.kicad_mod"));
    write(file_path, "(footprint \"" + utf8_name + "\" (layer \"F.Cu\") (model \"part.step\"))");
    write(directory / "part.step", "solid chinese");

    const auto utf8_path = utf8_text(file_path.u8string().c_str());

    core::Footprint fp;
    fp.set_name(utf8_name);
    fp.set_library_path(utf8_path);
    fp.set_footprint_lib_name(utf8_lib);
    fp.set_source_key(storage::existing_source_key(file_path));
    REQUIRE_FALSE(fp.source_key().empty());
    {
        auto session = db->session();
        auto inserted = storage::FootprintRepository(session).insert(fp);
        REQUIRE(inserted.has_value());
        // DB 往返：UTF-8 路径与名称逐字节回来
        auto stored = storage::FootprintRepository(session).find_by_id(inserted->id());
        REQUIRE(stored.has_value());
        CHECK(stored->library_path() == utf8_path);
        CHECK(stored->name() == utf8_name);
        CHECK(stored->footprint_lib_name() == utf8_lib);
        fp = *stored;
    }

    auto sym = symbol();
    set_reference(sym.id(), utf8_lib + ":" + utf8_name);
    auto readable = fpstatus_probe(*this, sym.id());
    CHECK(readable.resolution.status == services::FootprintStatus::Available);
    CHECK(readable.resolution.footprint_id == fp.id());
    CHECK(readable.resolution.resolved_reference == utf8_lib + ":" + utf8_name);

    // 绑定：中文封装路径必须真的被解析（读文件 + parser），写下的引用是 UTF-8
    services::SymbolBindingManager manager(*db);
    auto assigned = manager.assign_footprint(sym.id(), fp.id());
    if (!assigned) MESSAGE("chinese bind failed: ", assigned.error().format_message());
    REQUIRE(assigned.has_value());
    {
        auto session = db->session();
        auto updated = storage::SymbolRepository(session).find_by_id(sym.id());
        REQUIRE(updated.has_value());
        CHECK(updated->footprint() == utf8_lib + ":" + utf8_name);
        // 中文目录下的模型：文件路径按 UTF-8 落库，读回后仍是同一份文件
        auto models = storage::RelationshipRepository(session).find_models_for_footprint(fp.id());
        REQUIRE(models.has_value());
        REQUIRE(models->size() == 1);
        auto model = storage::Model3DRepository(session).find_by_id(models->front());
        REQUIRE(model.has_value());
        CHECK(storage::source_key(model->file_path()) ==
              storage::source_key(directory / "part.step"));
        CHECK(std::filesystem::is_regular_file(model->file_path()));
    }
    // 完整链路：manager.get 的展示字段同样带 UTF-8 引用与模型名
    auto binding = manager.get(sym.id());
    REQUIRE(binding.has_value());
    CHECK(binding->status == services::FootprintStatus::Available);
    CHECK(binding->footprint_id == fp.id());
    CHECK(binding->resolved_reference == utf8_lib + ":" + utf8_name);
    CHECK(binding->model_status == services::ModelStatus::Available);
    CHECK(binding->model_name == "part.step");
}

TEST_CASE_FIXTURE(LibraryFixture, "FPSTATUS binds a footprint whose model reference is also chinese")
{
    // 上一个用例把「目录」和「封装名」换成中文，但 3D 模型名仍是 ASCII 的 part.step：
    // 它证明不了「封装文本里的模型引用本身也是中文」这一段链路。这里文件名、引用文本、
    // 磁盘上的真实文件三者是同一串 UTF-8 中文名，模型必须真的被解析出来、入库并关联。
    // 文本一律用 u8 字面量：窄字面量的执行字符集取决于编译器，u8 才是 UTF-8 契约。
    const auto utf8_lib = utf8_text(u8"中文模型库");
    const auto utf8_name = utf8_text(u8"中文封装");
    const auto utf8_model = utf8_text(u8"模型.step");
    auto directory = temp.path() / fs::path(std::u8string(u8"中文模型库.pretty"));
    auto file_path = directory / fs::path(std::u8string(u8"中文封装.kicad_mod"));
    auto model_path = directory / fs::path(std::u8string(u8"模型.step"));
    write(file_path, "(footprint \"" + utf8_name + "\" (layer \"F.Cu\") (model \"" + utf8_model + "\"))");
    write(model_path, "solid chinese model");
    REQUIRE(fs::is_regular_file(model_path));

    const auto utf8_path = utf8_text(file_path.u8string().c_str());

    core::Footprint fp;
    fp.set_name(utf8_name);
    fp.set_library_path(utf8_path);
    fp.set_footprint_lib_name(utf8_lib);
    fp.set_source_key(storage::existing_source_key(file_path));
    REQUIRE_FALSE(fp.source_key().empty());
    {
        auto session = db->session();
        auto inserted = storage::FootprintRepository(session).insert(fp);
        REQUIRE(inserted.has_value());
        auto stored = storage::FootprintRepository(session).find_by_id(inserted->id());
        REQUIRE(stored.has_value());
        CHECK(stored->library_path() == utf8_path);
        CHECK(stored->name() == utf8_name);
        CHECK(stored->footprint_lib_name() == utf8_lib);
        fp = *stored;
    }   // 会话先释放：DbSession 不可重入，下面的 service 调用自己开会话

    auto sym = symbol();
    set_reference(sym.id(), utf8_lib + ":" + utf8_name);
    auto readable = fpstatus_probe(*this, sym.id());
    CHECK(readable.resolution.status == services::FootprintStatus::Available);
    CHECK(readable.resolution.footprint_id == fp.id());

    services::SymbolBindingManager manager(*db);
    auto assigned = manager.assign_footprint(sym.id(), fp.id());
    if (!assigned) MESSAGE("chinese model bind failed: ", assigned.error().format_message());
    REQUIRE(assigned.has_value());

    {
        auto session = db->session();
        auto updated = storage::SymbolRepository(session).find_by_id(sym.id());
        REQUIRE(updated.has_value());
        CHECK(updated->footprint() == utf8_lib + ":" + utf8_name);
        auto models = storage::RelationshipRepository(session).find_models_for_footprint(fp.id());
        REQUIRE(models.has_value());
        // 中文模型引用必须解析成真实文件并只关联一条：解析不到就是空关联，这里会红
        REQUIRE(models->size() == 1);
        auto model = storage::Model3DRepository(session).find_by_id(models->front());
        REQUIRE(model.has_value());
        CHECK(model->format() == "step");
        CHECK(storage::source_key(model->file_path()) == storage::source_key(model_path));
        CHECK(fs::is_regular_file(model->file_path()));
        // 名称原文：文件名逐字节还是那串 UTF-8 中文，不能变成 ? 或乱码
        CHECK(services::path_to_utf8(model->file_path().filename()) == utf8_model);
    }
    // 完整链路：manager.get 的展示名同样是原文，模型状态为可用
    auto binding = manager.get(sym.id());
    REQUIRE(binding.has_value());
    CHECK(binding->status == services::FootprintStatus::Available);
    CHECK(binding->footprint_id == fp.id());
    CHECK(binding->resolved_reference == utf8_lib + ":" + utf8_name);
    CHECK(binding->model_status == services::ModelStatus::Available);
    CHECK(binding->model_name == utf8_model);
}

// ============================================================
// U03 —— 图标/颜色的显式字段校验：只校验这次真正写下去的字段
// 约束同 FIX04：只用 fixture 的临时目录与临时数据库；单例注册表绑到临时
// 配置文件，用例结束由 RegistrySnapshot 连定义带路径一起还原。
// ============================================================

namespace
{
/// UTF-8 字节数（图标上限按字节算，不按字符算）
std::size_t u03_bytes(const std::string& text) { return text.size(); }
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "U03 icon and color validation only applies to explicitly written fields")
{
    RegistrySnapshot restore;
    auto& reg = core::TypeRegistry::instance();
    auto path = temp.path() / "u03_fields.json";
    // 历史配置：三位缩写色值在今天的新规则下非法，但它早就在用户文件里 ——
    // 加载不能拒绝，改名也不能顺手把它改掉
    write(path, R"({"types":[{"id":"Legacy","name":"Legacy","icon":"x","color":"#fff"}]})");
    REQUIRE(reg.load_component_types(path.string()));
    std::string before = read(path);

    // 只改名：图标/颜色原样保留，文件里也还在
    core::TypePatch rename;
    rename.name = "LegacyRenamed";
    REQUIRE(reg.update_component_type_persistent("Legacy", rename).has_value());
    auto renamed = reg.find_component("Legacy");
    REQUIRE(renamed.has_value());
    CHECK(renamed->name == "LegacyRenamed");
    CHECK(renamed->icon == "x");
    CHECK(renamed->color == "#fff");
    auto renamed_disk = nlohmann::json::parse(read(path));
    REQUIRE(renamed_disk["types"].size() == 1);
    CHECK(renamed_disk["types"][0]["color"] == "#fff");

    std::string legacy_bytes = read(path);

    // 显式写非法颜色：整次更新拒绝，内存与文件一个字节都不动
    core::TypePatch bad_color;
    bad_color.color = "#fff";
    auto rejected = reg.update_component_type_persistent("Legacy", bad_color);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().kind() == util::Error::Kind::ValidationError);
    CHECK(rejected.error().format_message().find("#RRGGBB") != std::string::npos);
    CHECK(read(path) == legacy_bytes);
    CHECK(reg.find_component("Legacy")->color == "#fff");

    // 其它非法形状同样拒绝：缺 #、长度不对、非十六进制
    for (const char* bad : {"red", "123456", "#12345", "#1234567", "#GGGGGG"})
    {
        core::TypePatch patch;
        patch.color = bad;
        CHECK_FALSE(reg.update_component_type_persistent("Legacy", patch).has_value());
    }
    CHECK(read(path) == legacy_bytes);

    // 合法颜色大小写皆可，落盘原样保留
    core::TypePatch upper;
    upper.color = "#AABBCC";
    REQUIRE(reg.update_component_type_persistent("Legacy", upper).has_value());
    CHECK(reg.find_component("Legacy")->color == "#AABBCC");
    core::TypePatch lower;
    lower.color = "#aabbcd";
    REQUIRE(reg.update_component_type_persistent("Legacy", lower).has_value());
    CHECK(reg.find_component("Legacy")->color == "#aabbcd");

    // 图标：emoji 与多字节中文都合法（按 UTF-8 字节计）
    const std::string icon_emoji = utf8_text(u8"↯");
    const std::string icon_chinese = utf8_text(u8"继电器");
    core::TypePatch emoji;
    emoji.icon = icon_emoji;
    REQUIRE(reg.update_component_type_persistent("Legacy", emoji).has_value());
    CHECK(reg.find_component("Legacy")->icon == icon_emoji);
    core::TypePatch chinese;
    chinese.icon = icon_chinese;
    REQUIRE(reg.update_component_type_persistent("Legacy", chinese).has_value());
    CHECK(reg.find_component("Legacy")->icon == icon_chinese);

    // 上限 64 字节：刚好 64 合法，65 拒绝且原因可读
    std::string at_limit(64, 'x');
    CHECK(u03_bytes(at_limit) == 64);
    core::TypePatch fits;
    fits.icon = at_limit;
    REQUIRE(reg.update_component_type_persistent("Legacy", fits).has_value());
    CHECK(reg.find_component("Legacy")->icon == at_limit);

    std::string too_long = at_limit + "y";
    CHECK(u03_bytes(too_long) == 65);
    core::TypePatch overflow;
    overflow.icon = too_long;
    auto overflow_result = reg.update_component_type_persistent("Legacy", overflow);
    REQUIRE_FALSE(overflow_result.has_value());
    CHECK(overflow_result.error().kind() == util::Error::Kind::ValidationError);
    CHECK(overflow_result.error().format_message().find("64") != std::string::npos);
    CHECK(u03_bytes(reg.find_component("Legacy")->icon) == 64);
    auto overflow_disk = nlohmann::json::parse(read(path));
    CHECK(overflow_disk["types"][0]["icon"] == at_limit);

    // 显式空串 = 清空：合法，且只清显式给出的那一个字段
    core::TypePatch clear_icon;
    clear_icon.icon = "";
    REQUIRE(reg.update_component_type_persistent("Legacy", clear_icon).has_value());
    CHECK(reg.find_component("Legacy")->icon.empty());
    CHECK(reg.find_component("Legacy")->color == "#aabbcd");
    core::TypePatch clear_color;
    clear_color.color = "";
    REQUIRE(reg.update_component_type_persistent("Legacy", clear_color).has_value());
    CHECK(reg.find_component("Legacy")->color.empty());

    // 重启等价：从同一份临时配置重建，名称/图标/颜色按最后一次写入读回
    core::TypePatch final_patch;
    final_patch.name = icon_chinese;
    final_patch.icon = icon_emoji;
    final_patch.color = "#123456";
    REQUIRE(reg.update_component_type_persistent("Legacy", final_patch).has_value());
    reg.load_component_types_json(R"({"types":[{"name":"Reset"}]})");
    REQUIRE(reg.load_component_types(path.string()));
    auto rebuilt = reg.find_component("Legacy");
    REQUIRE(rebuilt.has_value());
    CHECK(rebuilt->id == "Legacy");
    CHECK(rebuilt->name == icon_chinese);
    CHECK(rebuilt->icon == icon_emoji);
    CHECK(rebuilt->color == "#123456");
    CHECK(read(path) != before);
}

TEST_CASE_FIXTURE(LibraryFixture, "U03 adding a type validates icon and color before the file is touched")
{
    RegistrySnapshot restore;
    auto& reg = core::TypeRegistry::instance();
    auto path = temp.path() / "u03_add.json";
    write(path, R"({"types":[{"id":"Keep","name":"Keep","icon":"k","color":"#111111"}]})");
    REQUIRE(reg.load_component_types(path.string()));
    std::string bytes = read(path);
    std::string snapshot = fix04_types_json(reg.component_types());

    // 新增：非法颜色/超长图标一律拒绝，文件与内存保持原样
    core::TypeEntry bad_color;
    bad_color.name = "BadColor";
    bad_color.color = "#12345";
    auto color_result = reg.add_component_type_persistent(bad_color);
    REQUIRE_FALSE(color_result.has_value());
    CHECK(color_result.error().kind() == util::Error::Kind::ValidationError);
    CHECK(read(path) == bytes);

    core::TypeEntry bad_icon;
    bad_icon.name = "BadIcon";
    bad_icon.icon = std::string(65, 'x');
    auto icon_result = reg.add_component_type_persistent(bad_icon);
    REQUIRE_FALSE(icon_result.has_value());
    CHECK(icon_result.error().format_message().find("64") != std::string::npos);
    CHECK(read(path) == bytes);
    CHECK(fix04_types_json(reg.component_types()) == snapshot);
    CHECK_FALSE(reg.find_component("BadColor").has_value());
    CHECK_FALSE(reg.find_component("BadIcon").has_value());
    CHECK_FALSE(reg.component_by_name("BadColor").has_value());

    // 合法新增：ID 由后端生成（custom: 前缀），图标/颜色原样落盘
    const std::string name_chinese = utf8_text(u8"继电器");
    const std::string icon_emoji = utf8_text(u8"↯");
    core::TypeEntry good;
    good.name = name_chinese;
    good.icon = icon_emoji;
    good.color = "#123456";
    auto added = reg.add_component_type_persistent(good);
    REQUIRE(added.has_value());
    CHECK(added->starts_with("custom:"));

    // 写盘失败（.tmp 被占）：内存与文件都不动，报错不能假成功
    std::string after_add = read(path);
    std::string after_snapshot = fix04_types_json(reg.component_types());
    fs::create_directories(path.string() + ".tmp");
    core::TypeEntry blocked;
    blocked.name = "Blocked";
    blocked.icon = icon_emoji;
    blocked.color = "#abcdef";
    CHECK_FALSE(reg.add_component_type_persistent(blocked).has_value());
    core::TypePatch blocked_patch;
    blocked_patch.icon = "o";
    CHECK_FALSE(reg.update_component_type_persistent(*added, blocked_patch).has_value());
    CHECK(read(path) == after_add);
    CHECK(fix04_types_json(reg.component_types()) == after_snapshot);
    CHECK(reg.find_component(*added).has_value());  // 名字不是身份，ID 才是
    CHECK_FALSE(reg.component_by_name("Blocked").has_value());
    fs::remove_all(path.string() + ".tmp");

    // 重启等价：新增条目的名称/图标/颜色都在文件里
    reg.load_component_types_json(R"({"types":[{"name":"Reset"}]})");
    REQUIRE(reg.load_component_types(path.string()));
    auto rebuilt = reg.find_component(*added);
    REQUIRE(rebuilt.has_value());
    CHECK(rebuilt->name == name_chinese);
    CHECK(rebuilt->icon == icon_emoji);
    CHECK(rebuilt->color == "#123456");
    CHECK(reg.find_component("Keep")->icon == "k");
}

TEST_CASE_FIXTURE(LibraryFixture, "U03 loading a config without icons never rewrites or invents the user file")
{
    RegistrySnapshot restore;
    auto& reg = core::TypeRegistry::instance();
    auto path = temp.path() / "u03_seed.json";
    // 分发种子式的旧配置：Varistor 只有 id/name，没有图标与颜色
    const std::string relay_icon = utf8_text(u8"⏻");
    const std::string varistor_name = utf8_text(u8"压敏电阻");
    // 拼接时 icon 的值正好是 relay_icon：多写一个引号会得到 "icon":""⏻"（语法无效）
    write(path, R"({"types":[{"id":"Varistor","name":"Varistor"},{"id":"Relay","name":"Relay","icon":")" +
                  relay_icon + R"(","color":"#FF9500"}]})");
    std::string bytes = read(path);

    REQUIRE(reg.load_component_types(path.string()));
    // 加载绝不写回：缺图标就是空，不猜一个值补上去
    CHECK(read(path) == bytes);
    auto varistor = reg.find_component("Varistor");
    REQUIRE(varistor.has_value());
    CHECK(varistor->icon.empty());
    CHECK(varistor->color.empty());
    auto relay = reg.find_component("Relay");
    REQUIRE(relay.has_value());
    CHECK(relay->icon == relay_icon);
    CHECK(relay->color == "#FF9500");

    // 只改 Varistor 的名称：仍然不写 icon/color（省略字段 = 保持原值）
    core::TypePatch rename;
    rename.name = varistor_name;
    REQUIRE(reg.update_component_type_persistent("Varistor", rename).has_value());
    auto renamed = reg.find_component("Varistor");
    REQUIRE(renamed.has_value());
    CHECK(renamed->name == varistor_name);
    CHECK(renamed->icon.empty());
    CHECK(renamed->color.empty());
    auto on_disk = nlohmann::json::parse(read(path));
    bool found = false;
    for (const auto& item : on_disk["types"])
    {
        if (item.value("id", "") != "Varistor") continue;
        found = true;
        CHECK_FALSE(item.contains("icon"));
        CHECK_FALSE(item.contains("color"));
        CHECK(item["name"] == varistor_name);
    }
    CHECK(found);
    // Relay 的图标/颜色没有被这次写入弄丢
    bool relay_kept = false;
    for (const auto& item : on_disk["types"])
    {
        if (item.value("id", "") != "Relay") continue;
        relay_kept = true;
        CHECK(item["icon"] == relay_icon);
        CHECK(item["color"] == "#FF9500");
    }
    CHECK(relay_kept);
}

// ============================================================
// U04-0：类型来源只能是「真实 Reference 属性」
// 只加红测试，不改产品；全部调用当前已存在的生产入口 ——
//   ClassificationService::classify_all（/api/classify 走的同一个内核）、
//   ImportPipeline 的 symbols_from → execute（普通导入）、
//   StagedImporter::run（暂存导入，真实 Python 桩）。
// 目标语义：reference 只认实际 property("reference")，不认名称或 reference_prefix
// 派生值；已有类型（内置/custom/未注册历史 ID）保护，只有空/Unknown 才可补。
// 每条红都是独立断言，不靠 REQUIRE 提前 return；旧用例一个都不改。
// ============================================================

TEST_CASE_FIXTURE(LibraryFixture, "U04-0 classification needs a real reference property instead of the name or prefix")
{
    auto root = temp.path() / "u04_classify_source";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "Source.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, root / "footprints");

    // reference 传空串 = 这一行没有真实 Reference 属性，只有旧的前缀列
    auto insert = [&](const std::string& name, const std::string& prefix,
                      const std::string& reference, const std::string& type)
    {
        core::Symbol part;
        part.set_name(name);
        part.set_reference_prefix(prefix);
        if (!reference.empty())
            part.set_property("reference", reference);
        part.component_type_id = type;
        fix02_add_pins(part, 2);
        return fix02_insert_symbol(*db, library_id, part);
    };

    // 红：名称/前缀不是位号来源 —— 没有真实 Reference 的行必须保持未分类
    const std::string no_reference_r = insert("U04NoRefR123", "R", "", "Unknown");
    const std::string no_reference_led = insert("U04NoRefLED123", "LED", "", "Unknown");
    // 绿对照：真有 Reference 属性的同类输入按位号补类型
    const std::string real_reference = insert("U04RealRefR", "R", "R", "Unknown");
    // 绿对照：已有类型（custom / 未注册历史 ID）即使位号认得出也必须原样保留
    const std::string custom_type = insert("U04CustomRefR", "R", "R", "custom:local-only");
    const std::string legacy_type = insert("U04LegacyRefR", "R", "R", "LegacyRawId");

    // 输入前提：两条红输入确实没有 property("reference")，前缀是它仅有的「依据」
    {
        auto session = db->session();
        storage::SymbolRepository repo(session);
        auto no_ref_row = repo.find_by_id(no_reference_r);
        REQUIRE(no_ref_row.has_value());
        REQUIRE_FALSE(no_ref_row->property("reference").has_value());
        CHECK(no_ref_row->reference_prefix() == "R");
        CHECK(no_ref_row->component_type_id == "Unknown");
        auto led_row = repo.find_by_id(no_reference_led);
        REQUIRE(led_row.has_value());
        REQUIRE_FALSE(led_row->property("reference").has_value());
        CHECK(led_row->reference_prefix() == "LED");
    }

    // 会话已释放：分类服务自己取会话（同一线程嵌套 session 会死锁）
    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());
    REQUIRE(summary.has_value());
    MESSAGE("classification: total=", summary->total, " matched=", summary->matched,
            " type_updated=", summary->type_updated);

    CHECK(fix02_stored_type(*db, no_reference_r) == "Unknown");   // 红：当前按前缀 R 落 Resistor
    CHECK(fix02_stored_type(*db, no_reference_led) == "Unknown"); // 红：当前按前缀 LED 落 LED
    CHECK(fix02_stored_type(*db, real_reference) == "Resistor");  // 绿：真实 Reference=R
    CHECK(fix02_stored_type(*db, custom_type) == "custom:local-only"); // 绿：已知 custom 保护
    CHECK(fix02_stored_type(*db, legacy_type) == "LegacyRawId");       // 绿：历史 Raw ID 保护
    CHECK(summary->type_updated == 1);   // 只有真正带 Reference 属性的那一行被写回
}

// ------------------------------------------------------------
// U04-0 0B：真实 Reference 属性要胜过对不上的前缀列（未来默认可编辑规则
// ^R[0-9?]*$ 的集成输入）。旧缓存列与真实属性不一致时，只能读属性。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-0 classification reads the stored reference property even when the prefix disagrees")
{
    auto root = temp.path() / "u04_classify_explicit";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "Explicit.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, root / "footprints");

    core::Symbol part;
    part.set_name("U04ExplicitReference");
    part.set_reference_prefix("U");          // 旧列：与真实属性故意不一致
    part.set_property("reference", "R?");    // 真实属性原文（带 ?）
    part.component_type_id = "Unknown";
    fix02_add_pins(part, 2);
    const std::string symbol_id = fix02_insert_symbol(*db, library_id, part);

    // 输入前提：属性原文 R? 在、前缀仍是 U
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        auto reference = row->property("reference");
        REQUIRE(reference.has_value());
        REQUIRE(*reference == "R?");              // 原文往返，没有被规范化成 R
        REQUIRE(row->reference_prefix() == "U");
        REQUIRE(row->component_type_id == "Unknown");
    }

    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());
    REQUIRE(summary.has_value());
    MESSAGE("classification: total=", summary->total, " matched=", summary->matched,
            " type_updated=", summary->type_updated);

    CHECK(fix02_stored_type(*db, symbol_id) == "Resistor");  // 红：当前读前缀 U，仍是 Unknown
    CHECK(summary->type_updated == 1);                       // 红：当前一条都没补
}

// ------------------------------------------------------------
// U04-0 0C：普通首次导入不许按名字给文件补出一个 Reference。
// 文件里只有 Value、没有 Reference 属性；解析器仍会按名字取前缀（根因证据，
// 不作最终断言，解析器本轮不改），但类型匹配不能采用它。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-0 ordinary import does not invent a reference for a symbol without one")
{
    auto root = temp.path() / "u04_import_no_reference";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "NoReference.kicad_sym";
    write(target, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "R123" (property "Value" "R123"))))KICAD");

    // 真实 parser 先确认输入前提：文件里没有 Reference 属性
    auto parsed = parser::SymbolLibParser::parse(target);
    if (!parsed)
        MESSAGE("parse failed: ", parsed.error().format_message());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 1);
    REQUIRE_FALSE(parsed->items.front().property("reference").has_value());
    MESSAGE("parser derived prefix='", parsed->items.front().reference_prefix(),
            "' from the symbol name (root cause evidence, not an assertion)");

    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    auto imported = pipe | services::symbols_from{sym_dir.string(), ""} | services::execute;
    MESSAGE("import: symbols=", imported.symbols, " skipped=", imported.skipped);
    REQUIRE(imported.symbols == 1);

    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_name("R123");
        if (!row)
            MESSAGE("symbol read failed: ", row.error().format_message());
        REQUIRE(row.has_value());
        MESSAGE("imported R123: prefix='", row->reference_prefix(), "' type='",
                row->component_type_id, "'");
        CHECK(row->component_type_id == "Unknown");   // 红：当前按名字前缀落 Resistor
        REQUIRE_FALSE(row->property("reference").has_value());  // 导入没有替文件编出 Reference
    }
}

// ------------------------------------------------------------
// U04-0 0C 绿对照：同样的名字，文件里真写了 Reference R? 就必须按它补类型。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-0 ordinary import matches the real reference property")
{
    auto root = temp.path() / "u04_import_real_reference";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "RealReference.kicad_sym";
    write(target, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "R123" (property "Reference" "R?") (property "Value" "R123"))))KICAD");

    auto parsed = parser::SymbolLibParser::parse(target);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 1);
    auto reference = parsed->items.front().property("reference");
    REQUIRE(reference.has_value());
    CHECK(*reference == "R?");

    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    auto imported = pipe | services::symbols_from{sym_dir.string(), ""} | services::execute;
    MESSAGE("import: symbols=", imported.symbols, " skipped=", imported.skipped);
    REQUIRE(imported.symbols == 1);

    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_name("R123");
        if (!row)
            MESSAGE("symbol read failed: ", row.error().format_message());
        REQUIRE(row.has_value());
        auto stored_reference = row->property("reference");
        REQUIRE(stored_reference.has_value());
        CHECK(*stored_reference == "R?");
        CHECK(row->component_type_id == "Resistor");   // 绿：真实 Reference 命中默认位号规则
    }
}

// ------------------------------------------------------------
// U04-0 0D：普通导入的「已有 Unknown 行」在来源更新后要重新按新来源补类型。
// 第一次 Reference=U → Unknown；第二次同路径改 Reference=R?、Value=v2，
// mtime 推进 2 秒（不 sleep、不删快照）后同一条 pipeline 再导入。
// 已知类型保护变体（OpAmp）是绿对照。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-0 an ordinary source update fills a still unknown type")
{
    auto root = temp.path() / "u04_import_update";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "Switchable.kicad_sym";
    write(target, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04Switchable" (property "Reference" "U") (property "Value" "v1"))
  (symbol "U04Protected" (property "Reference" "U") (property "Value" "v1"))))KICAD");

    auto load = [&](const std::string& name)
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_name(name);
        if (!row)
            MESSAGE("symbol read failed: ", row.error().format_message());
        REQUIRE(row.has_value());
        return *row;
    };

    services::ImportPipeline first_pipe(*db, nullptr, false, &matching_store);
    auto first = first_pipe | services::symbols_from{sym_dir.string(), ""} | services::execute;
    MESSAGE("first import: symbols=", first.symbols, " skipped=", first.skipped);
    REQUIRE(first.symbols == 1);

    const core::Symbol switchable_before = load("U04Switchable");
    const core::Symbol protected_before = load("U04Protected");
    // 输入前提：Reference U 推不出具体类型，两条都落 Unknown
    REQUIRE(switchable_before.component_type_id == "Unknown");
    REQUIRE(protected_before.component_type_id == "Unknown");
    REQUIRE(switchable_before.default_value() == "v1");
    auto first_reference = switchable_before.property("reference");
    REQUIRE(first_reference.has_value());
    REQUIRE(*first_reference == "U");

    // 已知类型保护对照：人工定成 OpAmp 的行不能被来源更新改掉
    REQUIRE(db->execute("UPDATE symbols SET component_type = 'OpAmp' WHERE id = '" +
                        protected_before.id() + "'")
                .has_value());

    // 源文件真的变了（Reference U→R?、Value v1→v2），并把 mtime 推进 2 秒：
    // 靠真实 mtime 触发重导，不 sleep、不删导入快照
    write(target, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04Switchable" (property "Reference" "R?") (property "Value" "v2"))
  (symbol "U04Protected" (property "Reference" "R?") (property "Value" "v2"))))KICAD");
    fs::last_write_time(target, fs::last_write_time(target) + std::chrono::seconds(2));

    services::ImportPipeline second_pipe(*db, nullptr, false, &matching_store);
    auto second = second_pipe | services::symbols_from{sym_dir.string(), ""} | services::execute;
    MESSAGE("second import: symbols=", second.symbols, " skipped=", second.skipped);
    REQUIRE(second.symbols == 1);   // 没有被 mtime 快照跳过

    const core::Symbol switchable_after = load("U04Switchable");
    const core::Symbol protected_after = load("U04Protected");
    // 来源字段确实更新了、UUID 没变
    CHECK(switchable_after.id() == switchable_before.id());
    CHECK(protected_after.id() == protected_before.id());
    REQUIRE(switchable_after.default_value() == "v2");
    auto updated_reference = switchable_after.property("reference");
    REQUIRE(updated_reference.has_value());
    REQUIRE(*updated_reference == "R?");

    CHECK(switchable_after.component_type_id == "Resistor");  // 红：当前更新分支保留 Unknown
    CHECK(protected_after.component_type_id == "OpAmp");      // 绿：已知类型保护
}

// ------------------------------------------------------------
// U04-0 绿控制：真实 Reference 属性经 repo insert/find/update 原文往返；
// 只调 set_reference_prefix 不会产生 Reference 属性（它不是真实来源）。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-0 the stored reference property survives repository round trips")
{
    auto root = temp.path() / "u04_repo_roundtrip";
    auto sym_dir = root / "symbols";
    auto target = sym_dir / "RoundTrip.kicad_sym";
    write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");
    auto library_id = audit_register_user_library(*db, target, sym_dir, root / "footprints");

    core::Symbol part;
    part.set_name("U04RoundTrip");
    part.set_reference_prefix("R");          // 只有前缀列，没有真实属性
    part.component_type_id = "Unknown";
    fix02_add_pins(part, 2);
    const std::string symbol_id = fix02_insert_symbol(*db, library_id, part);

    {
        auto session = db->session();
        storage::SymbolRepository repo(session);
        auto row = repo.find_by_id(symbol_id);
        REQUIRE(row.has_value());
        CHECK(row->reference_prefix() == "R");
        REQUIRE_FALSE(row->property("reference").has_value());   // 前缀不是真实来源
        row->set_property("reference", "C?");                    // 写入真实属性（原文带 ?）
        row->set_reference_prefix("U");                          // 前缀列与属性故意不一致
        REQUIRE(repo.update(*row).has_value());
    }

    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        auto reference = row->property("reference");
        REQUIRE(reference.has_value());
        CHECK(*reference == "C?");                  // 原文往返，不被规范化成 C
        CHECK(row->reference_prefix() == "U");      // 前缀是独立列，不覆盖真实属性
        CHECK(row->component_type_id == "Unknown"); // 仓储往返不改类型
    }
}

// ------------------------------------------------------------
// U04-0 0E：暂存导入的漏匹配 —— 产物带真实 Reference R? 的新符号也要补类型。
// 局部复制共享桩、只把产物 Reference 换成 R?（查找替换必须命中，不改共享常量），
// 由真实 StagedImporter 新建。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-0 staged import matches the real reference of a new symbol")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    // 局部复制 LCSC01 桩：只改产物的 Reference 属性，共享常量原样不动
    std::string script = LCSC01_STUB_PLUGIN_PY;
    const std::string needle = "(property \"Reference\" \"U\")";
    const std::string replacement = "(property \"Reference\" \"R?\")";
    REQUIRE(count_occurrences(script, needle) == 1);
    script.replace(script.find(needle), needle.size(), replacement);

    auto root = temp.path() / "u04_staged_new";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "U04PKG", sym_dir.string(), fp_dir.string());

    auto scan = temp.path() / "plugins_u04_staged_new";
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", script);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    auto request = lcsc01_request(package_id, "C901", "u04-staged-new");
    auto result = importer.run(request);
    if (!result)
        MESSAGE("staged import failed: ", result.error().format_message());
    REQUIRE(result.has_value());
    CHECK(result->value("ok", false));

    const std::string symbol_id = result->value("symbol_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        if (!row)
            MESSAGE("symbol read failed: ", row.error().format_message());
        REQUIRE(row.has_value());
        auto reference = row->property("reference");
        if (!reference)
            MESSAGE("staged row has no reference property (premise for the red assertion)");
        REQUIRE(reference.has_value());
        CHECK(*reference == "R?");                     // 产物里的真实 Reference 原样落库
        CHECK(row->component_type_id == "Resistor");   // 红：当前暂存路径不推断 → Unknown
    }
}

// ------------------------------------------------------------
// U04-0 0E 绿对照：产物没有 Reference 属性的新符号保持未分类（不许按名字猜）。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-0 staged import without a real reference stays unknown")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    // 局部复制：删掉产物的 Reference 属性（含前导空格），共享常量原样不动
    std::string script = LCSC01_STUB_PLUGIN_PY;
    const std::string needle = " (property \"Reference\" \"U\")";
    REQUIRE(count_occurrences(script, needle) == 1);
    script.replace(script.find(needle), needle.size(), "");

    auto root = temp.path() / "u04_staged_no_reference";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "U04PKG2", sym_dir.string(), fp_dir.string());

    auto scan = temp.path() / "plugins_u04_staged_no_reference";
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", script);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    auto request = lcsc01_request(package_id, "C902", "u04-staged-noref");
    auto result = importer.run(request);
    if (!result)
        MESSAGE("staged import failed: ", result.error().format_message());
    REQUIRE(result.has_value());
    CHECK(result->value("ok", false));

    const std::string symbol_id = result->value("symbol_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        // 绿：没有真实 Reference 就没有可匹配的来源，保持 Unknown（名字前缀不作数）
        CHECK_FALSE(row->property("reference").has_value());
        CHECK(row->component_type_id == "Unknown");
    }
}

// ------------------------------------------------------------
// U04-0 0E：暂存导入的旧 Unknown 行在同源更新后也要按新的最终来源补类型。
// 第一次产物 Reference=U（同 provider/external_id 建行），局部脚本换成 R? 后
// 用新 operation_id 再导入：UUID 不变、来源属性更新，类型应为 Resistor。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-0 staged import refills an unknown type after the source changes")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    std::string script = LCSC01_STUB_PLUGIN_PY;
    const std::string needle = "(property \"Reference\" \"U\")";
    const std::string replacement = "(property \"Reference\" \"R?\")";
    REQUIRE(count_occurrences(script, needle) == 1);
    const auto needle_at = script.find(needle);

    auto root = temp.path() / "u04_staged_update";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "U04PKG3", sym_dir.string(), fp_dir.string());

    auto scan = temp.path() / "plugins_u04_staged_update";
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", script);   // 第一版：Reference=U
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    auto request = lcsc01_request(package_id, "C903", "u04-staged-update-1");
    auto first = importer.run(request);
    if (!first)
        MESSAGE("first staged import failed: ", first.error().format_message());
    REQUIRE(first.has_value());
    const std::string symbol_id = first->value("symbol_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        auto reference = row->property("reference");
        REQUIRE(reference.has_value());
        REQUIRE(*reference == "U");                       // 输入前提：产物 Reference=U
        REQUIRE(row->component_type_id == "Unknown");     // 输入前提：新行落 Unknown
    }

    // 同 provider/external_id、新 operation_id，局部脚本产物换成 Reference=R?
    script.replace(needle_at, needle.size(), replacement);
    write(scan / "stub" / "plugin.py", script);

    auto update = request;
    update.operation_id = "u04-staged-update-2";
    auto second = importer.run(update);
    if (!second)
        MESSAGE("second staged import failed: ", second.error().format_message());
    REQUIRE(second.has_value());
    CHECK(second->value("symbol_id", std::string("")) == symbol_id);   // 同一行、UUID 不变
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        auto reference = row->property("reference");
        REQUIRE(reference.has_value());
        REQUIRE(*reference == "R?");                      // 来源属性确实更新了
        CHECK(row->component_type_id == "Resistor");      // 红：当前保留 Unknown
    }
}

// ============================================================
// U04-A：独立配置仓储与元件类型匹配核心
// 用例只调用真实核心（classifier/type_matching.h）与真实仓储，不复制匹配算法：
//   * 配置文件全部写在 TempDirectory 里、路径显式注入，不读用户数据目录；
//   * TypeRegistry 单例的定义与绑定路径用完由 RegistrySnapshot 还原；
//   * 本段不接生产导入 / HTTP / UI —— 那属于后续阶段，U04-0 的红用例继续保留不变。
// ============================================================

// 分发种子只允许从「编译期确定的项目根」读取：test_data_integrity 目标注入这个宏
// （xmake.lua 里用 os.projectdir()），产品与 runtime 路径不消费它，测试也因此拿不到
// 任何用户数据目录。缺宏就直接编译失败，不退回按 cwd/祖先目录搜索。
#ifndef KFORGE_TEST_PROJECT_ROOT
#error "test_data_integrity 必须注入 KFORGE_TEST_PROJECT_ROOT（见 xmake.lua）"
#endif

namespace
{
using kforge::classifier::MatchEvidenceSource;
using kforge::classifier::MatchField;
using kforge::classifier::MatchSample;
using kforge::classifier::MatchStatus;
using kforge::classifier::TypeMatchingStore;

nlohmann::json u04a_rule(const std::string& id, const std::string& field, const std::string& pattern,
                         const std::string& type_id, int priority = 0, bool enabled = true,
                         bool case_sensitive = false)
{
    return nlohmann::json{{"id", id},
                          {"enabled", enabled},
                          {"field", field},
                          {"pattern", pattern},
                          {"case_sensitive", case_sensitive},
                          {"priority", priority},
                          {"type_id", type_id}};
}

nlohmann::json u04a_exception(const std::string& id, const std::string& field,
                              const std::string& key, const std::string& type_id, bool enabled = true)
{
    return nlohmann::json{
        {"id", id}, {"enabled", enabled}, {"field", field}, {"key", key}, {"type_id", type_id}};
}

std::string u04a_config(nlohmann::json rules, nlohmann::json exceptions = nlohmann::json::array(),
                        std::uint64_t revision = 0)
{
    nlohmann::json root{{"schema_version", 1},
                        {"revision", revision},
                        {"rules", std::move(rules)},
                        {"exceptions", std::move(exceptions)}};
    return root.dump(2);
}

/// 编译一份配置（不入仓储）；编译失败直接判用例失败。
std::shared_ptr<const classifier::MatchingSnapshot> u04a_snapshot(const std::string& json)
{
    auto snapshot = classifier::compile_matching_config(json);
    if (!snapshot)
        MESSAGE("compile failed: ", snapshot.error().format_message());
    REQUIRE(snapshot.has_value());
    return *snapshot;
}

/// 全部内置元件类型都登记在案：匹配配置校验的是「当前已加载的元件表」。
void u04a_load_full_registry()
{
    nlohmann::json types = nlohmann::json::array();
    for (int index = 0; index <= static_cast<int>(core::ComponentType::Unknown); ++index)
    {
        const char* id = core::builtin_component_id(static_cast<core::ComponentType>(index));
        types.push_back(nlohmann::json{{"id", id}, {"name", id}});
    }
    core::TypeRegistry::instance().load_component_types_json(nlohmann::json{{"types", types}}.dump());
}

/// U04-A 局部夹具：临时目录 + 临时配置路径 + 用完还原 TypeRegistry 单例。
struct U04AFixture
{
    test_support::TempDirectory temp;
    test_support::RestoreDefaultLogger restore;
    RegistrySnapshot registry;
    fs::path config = temp.path() / "component_type_matching.json";

    U04AFixture() { u04a_load_full_registry(); }
};

// ---------------- load/save 竞态的确定性屏障（测试专用） ----------------

/// 读取屏障：只截停「在自己的工作线程里武装过闸门」的读取。闸门按线程设置
/// （thread_local），所以主线程随后的 load/save 拿到真实结果后直接返回、不会被挡住；
/// 每个读者持有自己的闸门，放行与 join 互不影响 —— 「同样错误的第二次发布」这类用例
/// 需要两个各自独立的读者。限时等待只当死锁看门狗，不用「某操作若干毫秒还没完成」
/// 当正确性证据，也不 sleep 竞速。
struct ReadGate
{
    /// 读取器已经拿到真实结果之后调用：停在这里等本闸门被放行。
    void pause_after_read()
    {
        std::unique_lock lock(mutex_);
        paused_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    bool wait_until_paused(std::chrono::milliseconds limit)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, limit, [this] { return paused_ || released_; });
    }

    void release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool paused_ = false;
    bool released_ = false;
};

/// 本线程的读取闸门：只有 PendingLoad 的工作线程会设置它，主线程永远是 nullptr。
thread_local ReadGate* u04a_read_gate = nullptr;

/// 工作线程范围内武装本线程的读取闸门；退出作用域（含异常展开）一定清掉。
struct ScopedReadGate
{
    explicit ScopedReadGate(ReadGate& gate) { u04a_read_gate = &gate; }
    ~ScopedReadGate() { u04a_read_gate = nullptr; }
    ScopedReadGate(const ScopedReadGate&) = delete;
    ScopedReadGate& operator=(const ScopedReadGate&) = delete;
};

/// 注入仓储的读取器：必须先把生产读取器的真实结果拿到手，再按本线程的闸门暂停；
/// 不另写一套读取语义，也不硬编码内容。
classifier::ConfigFileReadFn u04a_gated_reader()
{
    return [](const std::string& path)
    {
        classifier::ConfigFileReadResult result = classifier::read_matching_config_file(path);
        if (u04a_read_gate != nullptr)
            u04a_read_gate->pause_after_read();
        return result;
    };
}

/// 后台线程里跑一次真实 store.load。退出作用域先放行屏障再 join，
/// 所以断言失败（REQUIRE 抛异常）也不会留下等待中的线程或挂住整个进程。
struct PendingLoad
{
    PendingLoad(ReadGate& gate, classifier::TypeMatchingStore& store, std::string path)
        : gate_(gate), store_(store), path_(std::move(path)),
          worker_(
              [this]
              {
                  // 注意成员顺序：outcome_ 必须排在 worker_ 前面先构造好，
                  // 否则新线程可能在它构造完成之前就写进去。
                  ScopedReadGate armed(gate_);
                  try
                  {
                      outcome_.emplace(store_.load(path_));
                  }
                  catch (const std::exception& thrown)
                  {
                      // 意外异常也要落成可定位的结果，不能让它把整个测试进程 terminate 掉。
                      outcome_.emplace(std::unexpect,
                                       util::Error::make<util::Error::Kind::ServiceError>(
                                           std::string("load threw: ") + thrown.what()));
                  }
                  catch (...)
                  {
                      outcome_.emplace(std::unexpect, util::Error::make<util::Error::Kind::ServiceError>(
                                                          "load threw an unknown exception"));
                  }
              })
    {
    }

    ~PendingLoad()
    {
        gate_.release();
        if (worker_.joinable())
            worker_.join();
    }

    /// 提前收尾：放行屏障并等这一次 load 真正返回。
    void join()
    {
        gate_.release();
        if (worker_.joinable())
            worker_.join();
    }

    ReadGate& gate_;
    classifier::TypeMatchingStore& store_;
    std::string path_;
    std::optional<util::Result<void>> outcome_;  // 声明顺序即构造顺序：必须早于 worker_
    std::thread worker_;
};

/// 仓库里的分发种子路径。只认编译期给定的项目根：测试目标必须注入
/// KFORGE_TEST_PROJECT_ROOT（见 xmake.lua 的 test_data_integrity / 本文件顶部），
/// 这里只拼那一个确定路径 —— 不按当前工作目录、可执行文件位置或祖先目录搜索同名
/// 文件，也不碰用户数据目录。文件不在就返回空，由用例明确失败，不静默跳过。
fs::path u04a_project_seed_path()
{
    std::error_code error;
    const fs::path root = fs::weakly_canonical(fs::path(KFORGE_TEST_PROJECT_ROOT), error);
    if (error)
        return {};
    const fs::path candidate = root / "config" / "component_type_matching.json";
    // 明确核对返回的确实是「项目根/config/...」这一个位置，而不是别处找到的同名文件。
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty() || *relative.begin() == fs::path(".."))
        return {};
    std::error_code exists_error;
    // 只接受普通文件：目录/坏路径不是「分发种子缺失」之外的东西，交给用例报错。
    if (!fs::is_regular_file(candidate, exists_error) || exists_error)
        return {};
    return candidate;
}

/// 临时切换当前工作目录，退出作用域（含断言抛出的异常展开）一定还原。
struct ScopedCurrentPath
{
    explicit ScopedCurrentPath(const fs::path& next)
    {
        std::error_code error;
        previous_ = fs::current_path(error);
        fs::current_path(next, error);
    }
    ~ScopedCurrentPath()
    {
        std::error_code error;
        fs::current_path(previous_, error);
    }
    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

private:
    fs::path previous_;
};
}  // namespace

// ------------------------------------------------------------
// U04-B2-S0：真实 StagedImporter 的匹配仓储显式注入 + 三字段真路径。
// 本轮头文件只增加第三个可选构造参数，run 尚不消费注入的 store，
// 所以三条用例的 DB 类型断言是真红；前提断言（配置保存、真实样本评估
// 命中 custom、来源字段原文落库）必须全绿，红才指向「尚未消费」这一事实。
// Reference 保持 U：本用例保存的配置不含任何 reference 规则/默认 R 规则，
// 命中只能来自各 subcase 选定字段的规则或特例，不按名字前缀猜。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture,
                  "U04-B2 staged: new imports use the final name value or model field")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    // 临时类型注册表文件 bind + persistent 添加 custom ID（RegistrySnapshot 负责还原）
    auto& registry = core::TypeRegistry::instance();
    registry.bind_component_types_path((temp.path() / "component_types.json").string());
    auto added = registry.add_component_type_persistent(
        core::TypeEntry{"", "U04B2Field", "", "", ""});
    REQUIRE(added.has_value());
    const std::string custom = *added;

    // 局部复制 LCSC01 桩：唯一 Footprint clause 后插静态 Value/MPN。
    // 固定字面量，不新增 %s、不改 (name, base) 格式 tuple；共享常量原样不动。
    std::string script = LCSC01_STUB_PLUGIN_PY;
    const std::string fp_needle = "(property \"Footprint\" \"old:fp\")";
    REQUIRE(count_occurrences(script, fp_needle) == 1);
    const std::string fp_props =
        "(property \"Footprint\" \"old:fp\") (property \"Value\" \"U04S_VALUE_v1\")"
        " (property \"MPN\" \"U04S_MPN_v1\")";
    script.replace(script.find(fp_needle), fp_needle.size(), fp_props);

    auto root = temp.path() / "u04b2";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "U04B2PKG", sym_dir.string(), fp_dir.string());

    auto scan = temp.path() / "plugins_u04b2";
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", script);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);

    // run 之前：用真实核心在「最终符号字段」样本上评估刚保存的配置，
    // 证明命中 custom 是配置本身的合法结论，不是测试复制匹配逻辑或按前缀猜。
    auto require_config_hits_custom = [&](const std::string& final_name)
    {
        core::Symbol expected;
        expected.set_name(final_name);
        expected.set_default_value("U04S_VALUE_v1");
        expected.set_mpn("U04S_MPN_v1");
        expected.set_property("reference", "U");
        const auto sample = MatchSample::from_symbol(expected);
        REQUIRE(sample.name.has_value());
        REQUIRE(sample.value.has_value());
        REQUIRE(sample.model.has_value());
        REQUIRE(sample.reference.has_value());
        const auto decision = matching_store.current()->evaluate(sample);
        REQUIRE(decision.status == MatchStatus::Matched);
        REQUIRE(decision.type_id == custom);
        return decision;
    };

    auto check_imported_row = [&](const std::string& symbol_id, const std::string& final_name,
                                  const std::string& footprint_uuid,
                                  const std::string& footprint_reference)
    {
        const fs::path symbol_file = sym_dir / "LCSC.kicad_sym";
        REQUIRE(fs::exists(symbol_file));
        auto session = db->session();
        auto library = storage::LibraryRepository(session).find_by_source_key(
            storage::existing_source_key(symbol_file));
        REQUIRE(library.has_value());
        CHECK(library->component_library_id == package_id);
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        CHECK(row->library_id() == library->id);
        // 三个最终来源字段都是输入原文；Reference 保持 U
        CHECK(row->name() == final_name);
        CHECK(row->default_value() == "U04S_VALUE_v1");
        CHECK(row->mpn() == "U04S_MPN_v1");
        auto reference = row->property("reference");
        REQUIRE(reference.has_value());
        CHECK(*reference == "U");
        CHECK(row->footprint() == footprint_reference);
        auto link = storage::RelationshipRepository(session).find_footprint_for_symbol(symbol_id);
        REQUIRE(link.has_value());
        REQUIRE(link->has_value());
        CHECK(**link == footprint_uuid);
        // 红：暂存导入尚未消费注入的匹配仓储，类型仍是 Unknown
        CHECK(row->component_type_id == custom);
    };

    SUBCASE("name")
    {
        // Name 用实际桩行为：name = "STUB_" + external 去尾 X 的 base
        REQUIRE(matching_store
                    .save(u04a_config(nlohmann::json::array(
                        {u04a_rule("u04b2-name", "name", "^STUB_C904$", custom)})),
                          0)
                    .has_value());
        require_config_hits_custom("STUB_C904");

        auto request = lcsc01_request(package_id, "C904", "u04b2-name");
        auto result = importer.run(request);
        if (!result)
            MESSAGE("staged import failed: ", result.error().format_message());
        REQUIRE(result.has_value());
        CHECK(result->value("ok", false));

        const std::string footprint_uuid = result->value("footprint_id", std::string(""));
        REQUIRE_FALSE(footprint_uuid.empty());
        REQUIRE(fs::exists(fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod"));
        check_imported_row(result->value("symbol_id", std::string("")), "STUB_C904",
                           footprint_uuid, "LCSC:STUBFP");
    }

    SUBCASE("value")
    {
        REQUIRE(matching_store
                    .save(u04a_config(
                        nlohmann::json::array(),
                        nlohmann::json::array(
                            {u04a_exception("u04b2-value", "value", "U04S_VALUE_v1", custom)})),
                          0)
                    .has_value());
        const auto decision = require_config_hits_custom("STUB_C905");
        REQUIRE(decision.evidence.size() == 1);
        CHECK(decision.evidence.front().source == MatchEvidenceSource::Exception);

        auto request = lcsc01_request(package_id, "C905", "u04b2-value");
        auto result = importer.run(request);
        if (!result)
            MESSAGE("staged import failed: ", result.error().format_message());
        REQUIRE(result.has_value());
        CHECK(result->value("ok", false));

        const std::string footprint_uuid = result->value("footprint_id", std::string(""));
        REQUIRE_FALSE(footprint_uuid.empty());
        REQUIRE(fs::exists(fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod"));
        check_imported_row(result->value("symbol_id", std::string("")), "STUB_C905",
                           footprint_uuid, "LCSC:STUBFP");
    }

    SUBCASE("model")
    {
        // model 字段来自 MPN（Symbol::mpn()），不是 3D 模型；本 subcase 走 reuse
        REQUIRE(matching_store
                    .save(u04a_config(nlohmann::json::array(
                        {u04a_rule("u04b2-model", "model", "^U04S_MPN_v1$", custom)})),
                          0)
                    .has_value());
        require_config_hits_custom("STUB_C906");

        const auto existing = footprint("CHOSEN");
        const std::string chosen_before = read(temp.path() / "Pkg.pretty" / "CHOSEN.kicad_mod");
        auto request = lcsc01_request(package_id, "C906", "u04b2-model");
        request.footprint_mode = "reuse";
        request.footprint_id = existing.id();

        auto result = importer.run(request);
        if (!result)
            MESSAGE("staged import failed: ", result.error().format_message());
        REQUIRE(result.has_value());
        CHECK(result->value("ok", false));

        // reuse：不新建封装、不改源封装文件
        CHECK(result->value("footprint_id", std::string("")) == existing.id());
        CHECK_FALSE(fs::exists(fp_dir / "LCSC.pretty"));
        CHECK(read(temp.path() / "Pkg.pretty" / "CHOSEN.kicad_mod") == chosen_before);
        check_imported_row(result->value("symbol_id", std::string("")), "STUB_C906",
                           existing.id(), "Pkg:CHOSEN");
    }
}

// ------------------------------------------------------------
// U04-B2：同源真实更新后的 Unknown 行按「最终来源字段」重新归类。
// 前提须全绿：首轮空配置真实导入落 Unknown 且 v1 字段原文入库；规则安装
// （CAS：candidate revision 1 == 已发布 revision，保存后发布 revision 2）后，
// 用真实 MatchSample/快照在「最终字段」上评估命中 custom；产物真变（非 noop）。
// 红：更新分支尚未消费注入的匹配仓储，Unknown 原样保留。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture,
                  "U04-B2 staged: genuine Unknown updates use the final name value or model field")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    // 临时类型注册表文件 bind + persistent 添加 custom ID（RegistrySnapshot 负责还原）
    auto& registry = core::TypeRegistry::instance();
    registry.bind_component_types_path((temp.path() / "component_types.json").string());
    auto added = registry.add_component_type_persistent(
        core::TypeEntry{"", "U04B2Update", "", "", ""});
    REQUIRE(added.has_value());
    const std::string custom = *added;

    // 局部复制 LCSC01 桩：唯一 Footprint clause 后插静态 Value/MPN（v1）。
    // Reference 保持桩自带的 U；固定字面量，不新增 %s、不改 (name, base) tuple。
    std::string script = LCSC01_STUB_PLUGIN_PY;
    const std::string fp_needle = "(property \"Footprint\" \"old:fp\")";
    REQUIRE(count_occurrences(script, fp_needle) == 1);
    script.replace(script.find(fp_needle), fp_needle.size(),
                   "(property \"Footprint\" \"old:fp\") (property \"Value\" \"U04S_VALUE_v1\")"
                   " (property \"MPN\" \"U04S_MPN_v1\")");

    auto root = temp.path() / "u04b2u";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "U04B2UPKG", sym_dir.string(), fp_dir.string());

    auto scan = temp.path() / "plugins_u04b2u";
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", script);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);

    // 初始空规则：真正已加载健康的配置（发布 revision 1）。
    REQUIRE(matching_store.save(u04a_config(nlohmann::json::array()), 0).has_value());

    auto exercise = [&](const std::string& external_id, const std::string& config_json,
                        bool empty_before_update = false)
    {
        const std::string final_name = "STUB_" + external_id;
        const fs::path symbol_file = sym_dir / "LCSC.kicad_sym";

        // 首轮：空配置真实导入，DB 落 Unknown，v1 字段原文入库
        auto first = importer.run(lcsc01_request(package_id, external_id, "u04b2u-first"));
        if (!first)
            MESSAGE("first staged import failed: ", first.error().format_message());
        REQUIRE(first.has_value());
        CHECK(first->value("ok", false));
        const std::string symbol_id = first->value("symbol_id", std::string(""));
        const std::string footprint_uuid = first->value("footprint_id", std::string(""));
        REQUIRE_FALSE(symbol_id.empty());
        REQUIRE_FALSE(footprint_uuid.empty());
        REQUIRE(fs::exists(symbol_file));
        const std::string file_v1 = read(symbol_file);
        {
            auto session = db->session();
            auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
            REQUIRE(row.has_value());
            CHECK(row->name() == final_name);
            CHECK(row->default_value() == "U04S_VALUE_v1");
            CHECK(row->mpn() == "U04S_MPN_v1");
            auto reference = row->property("reference");
            REQUIRE(reference.has_value());
            CHECK(*reference == "U");
            REQUIRE(row->component_type_id == "Unknown");   // 前提：首轮落 Unknown
        }

        if (empty_before_update)
        {
            auto session = db->session();
            storage::SymbolRepository repo(session);
            auto row = repo.find_by_id(symbol_id);
            REQUIRE(row.has_value());
            row->component_type_id.clear();
            REQUIRE(repo.update(*row).has_value());
            auto persisted = repo.find_by_id(symbol_id);
            REQUIRE(persisted.has_value());
            REQUIRE(persisted->component_type_id.empty());
        }

        // 安装对应字段的 custom 规则（candidate revision 1 == 当前已发布 revision）
        auto installed = matching_store.save(config_json, 1);
        if (!installed)
            MESSAGE("rule install failed: ", installed.error().format_message());
        REQUIRE(installed.has_value());
        CHECK(*installed == 2);

        // 绿前提：真实核心在「最终来源字段」样本上命中 custom
        core::Symbol expected;
        expected.set_name(final_name);
        expected.set_default_value("U04S_VALUE_v2");
        expected.set_mpn("U04S_MPN_v2");
        expected.set_property("reference", "U");
        const auto sample = MatchSample::from_symbol(expected);
        REQUIRE(sample.name.has_value());
        REQUIRE(sample.value.has_value());
        REQUIRE(sample.model.has_value());
        REQUIRE(sample.reference.has_value());
        const auto decision = matching_store.current()->evaluate(sample);
        REQUIRE(decision.status == MatchStatus::Matched);
        REQUIRE(decision.type_id == custom);

        // 脚本产物换成 v2（各唯一验证），写回同一 plugin.py，下一 run 即新输出
        REQUIRE(count_occurrences(script, "U04S_VALUE_v1") == 1);
        REQUIRE(count_occurrences(script, "U04S_MPN_v1") == 1);
        const std::string v1_value = "U04S_VALUE_v1";
        const std::string v1_mpn = "U04S_MPN_v1";
        script.replace(script.find(v1_value), v1_value.size(), "U04S_VALUE_v2");
        script.replace(script.find(v1_mpn), v1_mpn.size(), "U04S_MPN_v2");
        write(scan / "stub" / "plugin.py", script);

        // 同 provider/external/target、新 operation_id：同源真实更新
        auto second = importer.run(lcsc01_request(package_id, external_id, "u04b2u-second"));
        if (!second)
            MESSAGE("second staged import failed: ", second.error().format_message());
        REQUIRE(second.has_value());
        CHECK(second->value("ok", false));
        CHECK_FALSE(second->value("noop", false));   // 产物真变了，不是 no-op
        CHECK(second->value("symbol_id", std::string("")) == symbol_id);   // UUID 不变

        {
            auto session = db->session();
            // 用真实库 source_key 定位符号文件所在的库行
            auto library = storage::LibraryRepository(session).find_by_source_key(
                storage::existing_source_key(symbol_file));
            REQUIRE(library.has_value());
            CHECK(library->component_library_id == package_id);
            auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
            REQUIRE(row.has_value());
            CHECK(row->library_id() == library->id);
            CHECK(row->name() == final_name);   // 同名：不 rename
            CHECK(row->default_value() == "U04S_VALUE_v2");
            CHECK(row->mpn() == "U04S_MPN_v2");
            auto reference = row->property("reference");
            REQUIRE(reference.has_value());
            CHECK(*reference == "U");
            auto link = storage::RelationshipRepository(session).find_footprint_for_symbol(symbol_id);
            REQUIRE(link.has_value());
            REQUIRE(link->has_value());
            CHECK(**link == footprint_uuid);   // link 仍匹配首轮封装
        }
        CHECK(count_rows(*db, "symbol_footprint_links") == 1);   // 单一 link
        REQUIRE(fs::exists(fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod"));

        // 符号文件确实换成了 v2 产物
        const std::string file_v2 = read(symbol_file);
        CHECK(file_v2 != file_v1);
        CHECK(file_v2.find("U04S_VALUE_v2") != std::string::npos);
        CHECK(file_v2.find("U04S_MPN_v2") != std::string::npos);
        CHECK(file_v2.find("U04S_VALUE_v1") == std::string::npos);
        CHECK(file_v2.find("U04S_MPN_v1") == std::string::npos);

        CHECK(count_rows(*db, "import_sources") == 1);
        CHECK(count_rows(*db, "import_operations") == 2);

        // 红：同源更新后的 Unknown 行要按最终来源字段归类为 custom
        {
            auto session = db->session();
            auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
            REQUIRE(row.has_value());
            CHECK(row->component_type_id == custom);
        }
    };

    SUBCASE("name")
    {
        // name 不变；Value/MPN 变化产生新 digest，更新真实发生
        exercise("C914", u04a_config(nlohmann::json::array(
                                     {u04a_rule("u04b2u-name", "name", "^STUB_C914$", custom)}),
                                     nlohmann::json::array(), 1));
    }

    SUBCASE("value")
    {
        exercise("C915", u04a_config(
                             nlohmann::json::array(),
                             nlohmann::json::array({u04a_exception("u04b2u-value", "value",
                                                                   "U04S_VALUE_v2", custom)}),
                             1));
    }

    SUBCASE("model")
    {
        exercise("C916", u04a_config(nlohmann::json::array(
                                     {u04a_rule("u04b2u-model", "model", "^U04S_MPN_v2$", custom)}),
                                     nlohmann::json::array(), 1));
    }

    SUBCASE("empty existing type")
    {
        exercise("C917", u04a_config(nlohmann::json::array(
                                     {u04a_rule("u04b2u-empty", "name", "^STUB_C917$", custom)}),
                                     nlohmann::json::array(), 1), true);
    }
}

// ------------------------------------------------------------
// U04-B2：同源真实更新必须逐字保留已知类型（builtin / custom / legacy raw）。
// 前提全绿：首轮空配置真实导入后通过真实仓储标记类型；按「最终 v2 value」
// 精确特例安装 matched_target（与受保护类型不同），真实核心评估确命中
// matched_target；脚本换成 v2、新 operation_id，产物真变（非 noop）。
// 绿：更新分支保留行上已有类型，不被改写。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture,
                  "U04-B2 staged: genuine updates preserve known builtin custom and legacy types")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto& registry = core::TypeRegistry::instance();
    registry.bind_component_types_path((temp.path() / "component_types.json").string());
    // 两个不同的 custom：一个作为受保护类型，一个作为规则命中的 matched_target
    auto kept = registry.add_component_type_persistent(
        core::TypeEntry{"", "U04B2Keep", "", "", ""});
    REQUIRE(kept.has_value());
    auto aim = registry.add_component_type_persistent(
        core::TypeEntry{"", "U04B2Aim", "", "", ""});
    REQUIRE(aim.has_value());

    std::string script = LCSC01_STUB_PLUGIN_PY;
    const std::string fp_needle = "(property \"Footprint\" \"old:fp\")";
    REQUIRE(count_occurrences(script, fp_needle) == 1);
    script.replace(script.find(fp_needle), fp_needle.size(),
                   "(property \"Footprint\" \"old:fp\") (property \"Value\" \"U04S_VALUE_v1\")"
                   " (property \"MPN\" \"U04S_MPN_v1\")");

    auto root = temp.path() / "u04b2k";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "U04B2KPKG", sym_dir.string(), fp_dir.string());

    auto scan = temp.path() / "plugins_u04b2k";
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", script);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);
    REQUIRE(matching_store.save(u04a_config(nlohmann::json::array()), 0).has_value());

    auto exercise = [&](const std::string& external_id, const std::string& protected_id,
                        const std::string& matched_target)
    {
        REQUIRE(protected_id != matched_target);
        const std::string final_name = "STUB_" + external_id;
        const fs::path symbol_file = sym_dir / "LCSC.kicad_sym";

        // 首轮：空配置真实导入（落 Unknown），产物文件留下 v1 快照
        auto first = importer.run(lcsc01_request(package_id, external_id, "u04b2k-first"));
        if (!first)
            MESSAGE("first staged import failed: ", first.error().format_message());
        REQUIRE(first.has_value());
        CHECK(first->value("ok", false));
        const std::string symbol_id = first->value("symbol_id", std::string(""));
        const std::string footprint_uuid = first->value("footprint_id", std::string(""));
        REQUIRE_FALSE(symbol_id.empty());
        REQUIRE_FALSE(footprint_uuid.empty());
        REQUIRE(fs::exists(symbol_file));
        const std::string file_v1 = read(symbol_file);
        {
            auto session = db->session();
            storage::SymbolRepository repo(session);
            auto row = repo.find_by_id(symbol_id);
            REQUIRE(row.has_value());
            REQUIRE(row->component_type_id == "Unknown");   // 前提：首轮落 Unknown
            // 通过真实仓储标记受保护类型，其它字段原样保留
            row->component_type_id = protected_id;
            REQUIRE(repo.update(*row).has_value());
        }

        // 按「最终 v2 value」精确特例安装规则：matched_target 与受保护类型不同
        auto installed = matching_store.save(
            u04a_config(nlohmann::json::array(),
                        nlohmann::json::array({u04a_exception("u04b2k-keep", "value",
                                                              "U04S_VALUE_v2", matched_target)}),
                        1),
            1);
        if (!installed)
            MESSAGE("rule install failed: ", installed.error().format_message());
        REQUIRE(installed.has_value());
        CHECK(*installed == 2);

        // 绿前提：最终字段样本确实命中 matched_target，而不是受保护类型
        core::Symbol expected;
        expected.set_name(final_name);
        expected.set_default_value("U04S_VALUE_v2");
        expected.set_mpn("U04S_MPN_v2");
        expected.set_property("reference", "U");
        const auto decision =
            matching_store.current()->evaluate(MatchSample::from_symbol(expected));
        REQUIRE(decision.status == MatchStatus::Matched);
        REQUIRE(decision.type_id == matched_target);
        REQUIRE(decision.type_id != protected_id);

        // 脚本换成 v2 + 新 operation_id：同源真实更新，产物真变
        REQUIRE(count_occurrences(script, "U04S_VALUE_v1") == 1);
        REQUIRE(count_occurrences(script, "U04S_MPN_v1") == 1);
        const std::string v1_value = "U04S_VALUE_v1";
        const std::string v1_mpn = "U04S_MPN_v1";
        script.replace(script.find(v1_value), v1_value.size(), "U04S_VALUE_v2");
        script.replace(script.find(v1_mpn), v1_mpn.size(), "U04S_MPN_v2");
        write(scan / "stub" / "plugin.py", script);

        auto second = importer.run(lcsc01_request(package_id, external_id, "u04b2k-second"));
        if (!second)
            MESSAGE("second staged import failed: ", second.error().format_message());
        REQUIRE(second.has_value());
        CHECK(second->value("ok", false));
        CHECK_FALSE(second->value("noop", false));
        CHECK(second->value("symbol_id", std::string("")) == symbol_id);   // UUID 不变
        REQUIRE(second->contains("warnings"));
        REQUIRE((*second)["warnings"].is_array());
        CHECK((*second)["warnings"].empty());

        const std::string returned_fp = second->value("footprint_id", std::string(""));
        REQUIRE_FALSE(returned_fp.empty());

        {
            auto session = db->session();
            auto library = storage::LibraryRepository(session).find_by_source_key(
                storage::existing_source_key(symbol_file));
            REQUIRE(library.has_value());
            CHECK(library->component_library_id == package_id);
            auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
            REQUIRE(row.has_value());
            CHECK(row->library_id() == library->id);
            CHECK(row->name() == final_name);
            CHECK(row->default_value() == "U04S_VALUE_v2");   // 来源确为 v2
            CHECK(row->mpn() == "U04S_MPN_v2");
            auto link = storage::RelationshipRepository(session).find_footprint_for_symbol(symbol_id);
            REQUIRE(link.has_value());
            REQUIRE(link->has_value());
            CHECK(**link == footprint_uuid);   // link 仍匹配首轮封装
            auto returned = storage::FootprintRepository(session).find_by_id(returned_fp);
            REQUIRE(returned.has_value());     // 返回的封装真实存在
            // 绿：已知类型逐字保留，不被改写
            CHECK(row->component_type_id == protected_id);
        }
        CHECK(count_rows(*db, "symbol_footprint_links") == 1);   // 单一 link

        // 符号文件确实变化且为 v2 产物
        const std::string file_v2 = read(symbol_file);
        CHECK(file_v2 != file_v1);
        CHECK(file_v2.find("U04S_VALUE_v2") != std::string::npos);
        CHECK(file_v2.find("U04S_MPN_v1") == std::string::npos);

        CHECK(count_rows(*db, "import_sources") == 1);
        CHECK(count_rows(*db, "import_operations") == 2);
    };

    SUBCASE("builtin")
    {
        exercise("C924", "Resistor", "Capacitor");
    }

    SUBCASE("custom")
    {
        exercise("C925", *kept, *aim);
    }

    SUBCASE("legacy")
    {
        // 原始 ID 不在注册表：注册表查不到，但 DB 行上必须逐字保留
        exercise("C926", "LegacyFromOlderConfig", "Capacitor");
    }
}

// ------------------------------------------------------------
// U04-B2：坏匹配配置挡住全新操作、历史成功回放不受插件与健康状态影响。
// 前提全绿：fixture 已装健康默认种子，req1 真实导入成功（来源/操作日志各一条，
// content_digest/request_digest/result_json 均为真记录）；坏 JSON load 被拒后
// load_error 非空、current 仍持有最后有效快照、业务租约领取失败。
// 脚本临时换成必抛 Python 后，同 req1 只走历史回放并逐字复用原结果 ——
// 证明回放判定发生在插件运行与配置健康之前。
// 红（本轮不修产品）：恢复有效脚本后，坏配置下的全新 run 仍照常提交；
// 恢复配置后重试同 op 只能回放旧提交，CHECK_FALSE(replayed) 为红。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture,
                  "U04-B2 staged: bad matching config blocks fresh operations but preserves "
                  "successful replay")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto root = temp.path() / "u04b2h";
    auto sym_dir = root / "symbols";
    auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    auto package_id = lcsc01_register_package(*db, "U04B2HPKG", sym_dir.string(), fp_dir.string());

    auto scan = temp.path() / "plugins_u04b2h";
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", LCSC01_STUB_PLUGIN_PY);
    plugin::PluginManager manager({scan});
    manager.discover();

    api::StagedImporter importer(*db, manager, &matching_store);

    // ---- 1. 健康默认种子：req1 真实导入成功，来源/操作日志落真记录 ----
    const fs::path symbol_file = sym_dir / "LCSC.kicad_sym";
    const fs::path footprint_file = fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod";
    const auto req1 = lcsc01_request(package_id, "C924", "u04-b2-health-first");
    auto first = importer.run(req1);
    if (!first)
        MESSAGE("first staged import failed: ", first.error().format_message());
    REQUIRE(first.has_value());
    CHECK(first->value("ok", false));

    const std::string first_symbol_id = first->value("symbol_id", std::string(""));
    const std::string first_footprint_id = first->value("footprint_id", std::string(""));
    REQUIRE_FALSE(first_symbol_id.empty());
    REQUIRE_FALSE(first_footprint_id.empty());
    REQUIRE(fs::exists(symbol_file));
    REQUIRE(fs::exists(footprint_file));
    const std::string symbol_bytes_first = read(symbol_file);
    const std::string footprint_bytes_first = read(footprint_file);
    CHECK_FALSE(symbol_bytes_first.empty());
    CHECK_FALSE(footprint_bytes_first.empty());
    CHECK(audit_symbol_links(*db, first_symbol_id).size() == 1);

    {
        auto session = db->session();
        auto link =
            storage::RelationshipRepository(session).find_footprint_for_symbol(first_symbol_id);
        REQUIRE(link.has_value());
        REQUIRE(link->has_value());
        CHECK(**link == first_footprint_id);   // 真实单条关联指向返回的封装
    }

    // 真实 6 表计数（count_rows 查询失败返回 -1，>=0 同时排除假查询）；两日志表各一条
    REQUIRE(count_rows(*db, "libraries") >= 0);
    REQUIRE(count_rows(*db, "symbols") >= 0);
    REQUIRE(count_rows(*db, "footprints") >= 0);
    REQUIRE(count_rows(*db, "symbol_footprint_links") >= 0);
    REQUIRE(count_rows(*db, "import_sources") >= 0);
    REQUIRE(count_rows(*db, "import_operations") >= 0);
    REQUIRE(count_rows(*db, "import_sources") == 1);
    REQUIRE(count_rows(*db, "import_operations") == 1);
    const int libraries_before = count_rows(*db, "libraries");
    const int symbols_before = count_rows(*db, "symbols");
    const int footprints_before = count_rows(*db, "footprints");
    const int links_before = count_rows(*db, "symbol_footprint_links");

    // 会话已释放才能调用自取会话的 helper：逐字记录来源摘要与操作日志
    const std::string source_digest =
        audit_scalar_text(*db, "SELECT content_digest FROM import_sources LIMIT 1");
    REQUIRE_FALSE(source_digest.empty());
    const std::string op1_digest =
        audit_scalar_text(*db, "SELECT request_digest FROM import_operations "
                               "WHERE id = 'u04-b2-health-first'");
    REQUIRE_FALSE(op1_digest.empty());
    const std::string op1_json =
        audit_scalar_text(*db, "SELECT result_json FROM import_operations "
                               "WHERE id = 'u04-b2-health-first'");
    REQUIRE_FALSE(op1_json.empty());
    CHECK(nlohmann::json::parse(op1_json) == *first);

    // ---- 2. 坏 JSON：load 被拒，current 仍是最后有效快照，业务租约领不到 ----
    const std::string bad_config = "{ this is not json";
    write(matching_config, bad_config);
    auto bad_load = matching_store.load(matching_config.string());
    if (!bad_load)
        MESSAGE("bad config load failed as expected: ", bad_load.error().format_message());
    REQUIRE_FALSE(bad_load.has_value());
    CHECK_FALSE(matching_store.load_error().empty());
    REQUIRE(matching_store.current() != nullptr);
    CHECK_FALSE(matching_store.acquire_business_snapshot().has_value());

    // ---- 3. 历史回放先于插件与健康判定：必抛脚本下同 req1 仍回放原结果 ----
    const std::string throwing_script =
        "raise RuntimeError(\"u04-b2 replay must not run the plugin\")\n";
    write(scan / "stub" / "plugin.py", throwing_script);
    auto replay = importer.run(req1);
    if (!replay)
        MESSAGE("replay failed: ", replay.error().format_message());
    REQUIRE(replay.has_value());
    CHECK(replay->value("replayed", false));
    {
        nlohmann::json replay_core = *replay;
        replay_core.erase("replayed");
        CHECK(replay_core == *first);
    }
    CHECK(read(symbol_file) == symbol_bytes_first);
    CHECK(read(footprint_file) == footprint_bytes_first);
    CHECK(count_rows(*db, "libraries") == libraries_before);
    CHECK(count_rows(*db, "symbols") == symbols_before);
    CHECK(count_rows(*db, "footprints") == footprints_before);
    CHECK(count_rows(*db, "symbol_footprint_links") == links_before);
    CHECK(count_rows(*db, "import_sources") == 1);
    CHECK(count_rows(*db, "import_operations") == 1);
    CHECK(audit_scalar_text(*db, "SELECT content_digest FROM import_sources LIMIT 1") ==
          source_digest);
    CHECK(audit_scalar_text(*db, "SELECT request_digest FROM import_operations "
                                 "WHERE id = 'u04-b2-health-first'") == op1_digest);
    CHECK(audit_scalar_text(*db, "SELECT result_json FROM import_operations "
                                 "WHERE id = 'u04-b2-health-first'") == op1_json);
    CHECK(read(matching_config) == bad_config);   // 坏配置原文不动

    // ---- 4. 恢复有效脚本：坏配置下的全新 run 不得提交（红：当前 run 忽略仓储健康） ----
    write(scan / "stub" / "plugin.py", LCSC01_STUB_PLUGIN_PY);
    REQUIRE(read(scan / "stub" / "plugin.py") == LCSC01_STUB_PLUGIN_PY);
    const auto req2 = lcsc01_request(package_id, "C925", "u04-b2-health-fresh");
    auto fresh = importer.run(req2);
    CHECK_FALSE(fresh.has_value());   // 红：当前 run 忽略匹配仓储，坏配置下照常提交
    if (!fresh)
    {
        CHECK(fresh.error().kind() == util::Error::Kind::ServiceError);
        CHECK(fresh.error().format_message().find("matching config") != std::string::npos);
    }
    CHECK(read(symbol_file) == symbol_bytes_first);
    CHECK(read(footprint_file) == footprint_bytes_first);
    CHECK(count_rows(*db, "libraries") == libraries_before);
    CHECK(count_rows(*db, "symbols") == symbols_before);            // 红：全新 run 已提交
    CHECK(count_rows(*db, "footprints") == footprints_before);
    CHECK(count_rows(*db, "symbol_footprint_links") == links_before);
    CHECK(count_rows(*db, "import_sources") == 1);
    CHECK(count_rows(*db, "import_operations") == 1);   // 新操作记录数为 0
    CHECK(read(matching_config) == bad_config);
    CHECK(audit_scalar_text(*db, "SELECT content_digest FROM import_sources LIMIT 1") ==
          source_digest);
    CHECK(audit_scalar_text(*db, "SELECT request_digest FROM import_operations "
                                 "WHERE id = 'u04-b2-health-first'") == op1_digest);
    CHECK(audit_scalar_text(*db, "SELECT result_json FROM import_operations "
                                 "WHERE id = 'u04-b2-health-first'") == op1_json);

    // ---- 5. 恢复配置：重试同 op（红：旧错误提交只能被回放，replayed=true） ----
    write(matching_config, classifier::default_matching_config_json());
    auto healed = matching_store.load(matching_config.string());
    if (!healed)
        MESSAGE("config restore load failed: ", healed.error().format_message());
    REQUIRE(healed.has_value());
    CHECK(matching_store.load_error().empty());
    REQUIRE(matching_store.acquire_business_snapshot().has_value());

    auto retry = importer.run(req2);
    if (!retry)
        MESSAGE("retry failed: ", retry.error().format_message());
    REQUIRE(retry.has_value());
    CHECK_FALSE(retry->value("replayed", false));   // 红：step 4 已提交，这里只能回放
    CHECK_FALSE(retry->value("noop", false));
    CHECK(retry->value("symbol_id", std::string("")) != first_symbol_id);
    CHECK(retry->value("footprint_reference", std::string("")) == "LCSC:STUBFP");
    CHECK(count_rows(*db, "import_sources") == 2);
    CHECK(count_rows(*db, "import_operations") == 2);

    const std::string retry_symbol_id = retry->value("symbol_id", std::string(""));
    REQUIRE_FALSE(retry_symbol_id.empty());
    const std::string retry_footprint_id = retry->value("footprint_id", std::string(""));
    REQUIRE_FALSE(retry_footprint_id.empty());

    // op2 日志真实记录；回放返回值去掉 replayed 后与日志逐字一致
    const std::string op2_json =
        audit_scalar_text(*db, "SELECT result_json FROM import_operations "
                               "WHERE id = 'u04-b2-health-fresh'");
    REQUIRE_FALSE(op2_json.empty());
    {
        nlohmann::json retry_core = *retry;
        retry_core.erase("replayed");
        CHECK(nlohmann::json::parse(op2_json) == retry_core);
    }

    // 新符号/来源/关联真实匹配；FP 同名复用不新增行，不断言封装数量
    CHECK(audit_scalar_text(*db,
                            "SELECT symbol_id FROM import_sources WHERE external_id = 'C925'") ==
          retry_symbol_id);
    CHECK(audit_symbol_links(*db, retry_symbol_id).size() == 1);
    std::string retry_library_id;
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(retry_symbol_id);
        REQUIRE(row.has_value());
        CHECK(row->name() == "STUB_C925");
        auto link = storage::RelationshipRepository(session).find_footprint_for_symbol(retry_symbol_id);
        REQUIRE(link.has_value());
        REQUIRE(link->has_value());
        CHECK(**link == retry_footprint_id);

        // 来源身份是符号库 libraries UUID，不是包 component_libraries UUID：
        // 经既有 source_key 方式查真实库行核对，会话结束后再与
        // import_sources.target_library_id 对账
        auto library = storage::LibraryRepository(session).find_by_source_key(
            storage::existing_source_key(symbol_file));
        REQUIRE(library.has_value());
        retry_library_id = row->library_id();
        CHECK(retry_library_id == library->id);
        CHECK(library->component_library_id == package_id);
        CHECK(storage::source_key(library->file_path) ==
              storage::existing_source_key(symbol_file));

        // 首轮 symbol/UUID/link 逐字保留
        auto first_row = storage::SymbolRepository(session).find_by_id(first_symbol_id);
        REQUIRE(first_row.has_value());
        CHECK(first_row->name() == "STUB_C924");
        auto first_link =
            storage::RelationshipRepository(session).find_footprint_for_symbol(first_symbol_id);
        REQUIRE(first_link.has_value());
        REQUIRE(first_link->has_value());
        CHECK(**first_link == first_footprint_id);
    }
    REQUIRE_FALSE(retry_library_id.empty());
    CHECK(audit_scalar_text(*db, "SELECT target_library_id FROM import_sources "
                                 "WHERE external_id = 'C925'") == retry_library_id);
    CHECK(audit_symbol_links(*db, first_symbol_id).size() == 1);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A matching reads the four real source fields and never invents a reference")
{
    const std::string json = u04a_config(nlohmann::json::array({
        u04a_rule("by-name", "name", "^BYNAME$", "Resistor"),
        u04a_rule("by-value", "value", "^10k$", "Capacitor"),
        u04a_rule("by-model", "model", "^LM358$", "OpAmp"),
        u04a_rule("by-reference", "reference", "^R[0-9?]*$", "Resistor"),
    }));
    const auto snapshot = u04a_snapshot(json);

    // 符号 → 样本：四个字段来自真实来源（name / default_value / mpn / 实际 reference 属性）
    core::Symbol symbol;
    symbol.set_name("BYNAME");
    symbol.set_default_value("10k");
    symbol.set_mpn("LM358");
    symbol.set_property("reference", "R?");
    const MatchSample sample = MatchSample::from_symbol(symbol);
    REQUIRE(sample.name.has_value());
    CHECK(*sample.name == "BYNAME");
    REQUIRE(sample.value.has_value());
    CHECK(*sample.value == "10k");
    REQUIRE(sample.model.has_value());
    CHECK(*sample.model == "LM358");
    REQUIRE(sample.reference.has_value());
    CHECK(*sample.reference == "R?");

    // 四个字段同时命中、类型各不相同 = conflict，依据一条不少（不按顺序挑一条）
    auto decision = snapshot->evaluate(sample);
    REQUIRE(decision.status == MatchStatus::Conflict);
    CHECK(decision.type_id.empty());
    CHECK(decision.evidence.size() == 4);
    CHECK(decision.reason.find("conflicting") != std::string::npos);

    // 每个字段单独都能命中自己的规则
    MatchSample one;
    one.name = "BYNAME";
    decision = snapshot->evaluate(one);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Resistor");
    REQUIRE(decision.evidence.size() == 1);
    CHECK(decision.evidence.front().source == MatchEvidenceSource::Rule);
    CHECK(decision.evidence.front().id == "by-name");
    CHECK(decision.evidence.front().field == MatchField::Name);
    CHECK(decision.evidence.front().text == "BYNAME");

    one = MatchSample{};
    one.value = "10k";
    CHECK(snapshot->evaluate(one).type_id == "Capacitor");
    one = MatchSample{};
    one.model = "LM358";
    CHECK(snapshot->evaluate(one).type_id == "OpAmp");
    one = MatchSample{};
    one.reference = "R12";
    decision = snapshot->evaluate(one);
    CHECK(decision.type_id == "Resistor");
    REQUIRE(decision.evidence.size() == 1);
    CHECK(decision.evidence.front().id == "by-reference");
    CHECK(decision.evidence.front().field == MatchField::Reference);
    CHECK(decision.evidence.front().text == "R12");

    // reference 只看实际属性：只有 name/reference_prefix 的符号没有这个字段
    core::Symbol without_reference;
    without_reference.set_name("R123");
    without_reference.set_reference_prefix("R");
    without_reference.component_type_id = "Unknown";
    const MatchSample missing = MatchSample::from_symbol(without_reference);
    CHECK_FALSE(missing.reference.has_value());
    decision = snapshot->evaluate(missing);
    CHECK(decision.status == MatchStatus::Unmatched);  // 名字不是位号来源，不许兜底
    CHECK(decision.type_id.empty());

    // 「字段缺失」和「字段是空串」是两回事：^$ 只对后者成立，.* 也不匹配缺失字段
    const auto empty_snapshot = u04a_snapshot(
        u04a_config(nlohmann::json::array({u04a_rule("empty", "reference", "^$", "TestPoint")})));
    MatchSample explicit_empty;
    explicit_empty.reference = std::string();
    CHECK(empty_snapshot->evaluate(explicit_empty).type_id == "TestPoint");
    MatchSample absent;
    CHECK(empty_snapshot->evaluate(absent).status == MatchStatus::Unmatched);

    const auto any_snapshot = u04a_snapshot(
        u04a_config(nlohmann::json::array({u04a_rule("any", "reference", ".*", "TestPoint")})));
    CHECK(any_snapshot->evaluate(absent).status == MatchStatus::Unmatched);
    CHECK(any_snapshot->evaluate(explicit_empty).type_id == "TestPoint");
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A exact exceptions beat every regex and stay case sensitive")
{
    const std::string json = u04a_config(
        // 两条规则都区分大小写：这样下面「小写没命中特例」只能由特例本身的大小写语义解释
        nlohmann::json::array({
            u04a_rule("regex-name", "name", "SPECIAL_[0-9]+", "Resistor", 10, true, true),
            u04a_rule("fallback", "name", ".*", "Capacitor", 0, true, true),
        }),
        nlohmann::json::array({u04a_exception("exact-name", "name", "SPECIAL_7", "Switch")}));
    const auto snapshot = u04a_snapshot(json);

    // 特例命中后不再评估正则：否则这里会走到正则，按最高 priority 得到 Resistor
    // （regex-name 是 priority 10，fallback 是 0）—— 断言看到的是 Switch 才说明特例优先
    MatchSample sample;
    sample.name = "SPECIAL_7";
    auto decision = snapshot->evaluate(sample);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Switch");
    REQUIRE(decision.evidence.size() == 1);
    CHECK(decision.evidence.front().source == MatchEvidenceSource::Exception);
    CHECK(decision.evidence.front().id == "exact-name");
    CHECK(decision.evidence.front().text == "SPECIAL_7");

    // 特例区分大小写：小写走正则
    sample.name = "special_7";
    decision = snapshot->evaluate(sample);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Capacitor");  // 只有兜底规则命中

    // 特例不 trim：前后加空格就不再是同一个键，由正则接管（依据不再是特例）
    sample.name = " SPECIAL_7 ";
    decision = snapshot->evaluate(sample);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.evidence.front().source == MatchEvidenceSource::Rule);
    CHECK(decision.evidence.front().id == "regex-name");

    // 没有特例时按最高数值 priority 走正则（10 胜过 0）
    sample.name = "SPECIAL_7X";
    decision = snapshot->evaluate(sample);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Resistor");
    CHECK(decision.evidence.front().id == "regex-name");

    // 精确键里的特殊字符按原文比较，不做正则解释
    const auto literal = u04a_snapshot(u04a_config(
        nlohmann::json::array({u04a_rule("regex-literal", "value", "a+b[c]", "Capacitor")}),
        nlohmann::json::array({u04a_exception("exact-literal", "value", "a+b[c]|d", "Fuse")})));
    MatchSample special;
    special.value = "a+b[c]|d";
    decision = literal->evaluate(special);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Fuse");
    special.value = "aaabc";  // 正则 a+b[c] 的形状：说明两边用的是不同语义
    CHECK(literal->evaluate(special).type_id == "Capacitor");

    // 跨字段的特例同时命中不同类型 = conflict
    const auto cross = u04a_snapshot(u04a_config(
        nlohmann::json::array(),
        nlohmann::json::array({u04a_exception("ex-name", "name", "X1", "Resistor"),
                               u04a_exception("ex-value", "value", "X1", "Capacitor")})));
    MatchSample both;
    both.name = "X1";
    both.value = "X1";
    decision = cross->evaluate(both);
    REQUIRE(decision.status == MatchStatus::Conflict);
    CHECK(decision.type_id.empty());
    CHECK(decision.evidence.size() == 2);

    // 跨字段命中同一类型 = 成功，并带回全部依据
    const auto same = u04a_snapshot(u04a_config(
        nlohmann::json::array(),
        nlohmann::json::array({u04a_exception("ex-name", "name", "X1", "Resistor"),
                               u04a_exception("ex-value", "value", "X1", "Resistor")})));
    decision = same->evaluate(both);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Resistor");
    CHECK(decision.evidence.size() == 2);

    // disabled 特例不参与匹配
    const auto disabled = u04a_snapshot(u04a_config(
        nlohmann::json::array(),
        nlohmann::json::array({u04a_exception("off", "name", "X1", "Resistor", false)})));
    CHECK(disabled->evaluate(both).status == MatchStatus::Unmatched);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A regex search casing and numeric priority decide the winner")
{
    // regex_search：不写锚就是子串匹配，写 ^$ 才要求全文
    const auto search = u04a_snapshot(
        u04a_config(nlohmann::json::array({u04a_rule("sub", "name", "R[0-9]+", "Resistor")})));
    MatchSample sample;
    sample.name = "MYR12X";
    CHECK(search->evaluate(sample).type_id == "Resistor");
    const auto anchored = u04a_snapshot(
        u04a_config(nlohmann::json::array({u04a_rule("full", "name", "^R[0-9]+$", "Resistor")})));
    CHECK(anchored->evaluate(sample).status == MatchStatus::Unmatched);
    sample.name = "R12";
    CHECK(anchored->evaluate(sample).type_id == "Resistor");

    // case_sensitive=false 才加 icase
    MatchSample lower;
    lower.reference = "r?";
    const auto insensitive = u04a_snapshot(u04a_config(
        nlohmann::json::array({u04a_rule("i", "reference", "^R[0-9?]*$", "Resistor", 0, true, false)})));
    CHECK(insensitive->evaluate(lower).type_id == "Resistor");
    const auto sensitive = u04a_snapshot(u04a_config(
        nlohmann::json::array({u04a_rule("s", "reference", "^R[0-9?]*$", "Resistor", 0, true, true)})));
    CHECK(sensitive->evaluate(lower).status == MatchStatus::Unmatched);
    lower.reference = "R?";
    CHECK(sensitive->evaluate(lower).type_id == "Resistor");

    // 最高数值 priority 胜出：比的是大小（含 int32 最小值），不是绝对值也不是列表顺序
    const auto priority = u04a_snapshot(u04a_config(nlohmann::json::array({
        u04a_rule("low", "reference", "^R", "Capacitor", -10),
        u04a_rule("high", "reference", "^R", "Resistor", -5),
        u04a_rule("lowest", "reference", "^R", "Fuse", -2147483647 - 1),
    })));
    MatchSample reference;
    reference.reference = "R1";
    auto decision = priority->evaluate(reference);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Resistor");
    REQUIRE(decision.evidence.size() == 1);
    CHECK(decision.evidence.front().id == "high");
    CHECK(decision.evidence.front().priority == -5);

    // 同级同型：成功并返回全部依据
    const auto same = u04a_snapshot(u04a_config(nlohmann::json::array({
        u04a_rule("a", "reference", "^R", "Resistor", 3),
        u04a_rule("b", "reference", "^R", "Resistor", 3),
    })));
    decision = same->evaluate(reference);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Resistor");
    CHECK(decision.evidence.size() == 2);

    // 同级异型：显式冲突，不按列表顺序取第一条
    const auto conflict = u04a_snapshot(u04a_config(nlohmann::json::array({
        u04a_rule("first", "reference", "^R", "Capacitor", 3),
        u04a_rule("second", "reference", "^R", "Resistor", 3),
    })));
    decision = conflict->evaluate(reference);
    REQUIRE(decision.status == MatchStatus::Conflict);
    CHECK(decision.type_id.empty());
    CHECK(decision.evidence.size() == 2);

    // 低优先级的命中不参与：最高一级只有一个类型就是成功
    const auto mixed = u04a_snapshot(u04a_config(nlohmann::json::array({
        u04a_rule("top", "reference", "^R", "Resistor", 9),
        u04a_rule("below", "reference", "^R", "Capacitor", 8),
    })));
    decision = mixed->evaluate(reference);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Resistor");

    // disabled 规则不参与匹配
    const auto disabled = u04a_snapshot(
        u04a_config(nlohmann::json::array({u04a_rule("off", "reference", "^R", "Resistor", 0, false)})));
    CHECK(disabled->evaluate(reference).status == MatchStatus::Unmatched);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A an empty rule set matches nothing at all")
{
    const auto snapshot = u04a_snapshot(u04a_config(nlohmann::json::array(), nlohmann::json::array()));
    CHECK(snapshot->referenced_type_ids().empty());
    MatchSample sample;
    sample.reference = "R?";
    const auto decision = snapshot->evaluate(sample);
    CHECK(decision.status == MatchStatus::Unmatched);
    CHECK(decision.type_id.empty());
    CHECK(decision.evidence.empty());
    CHECK(std::string(classifier::match_status_name(decision.status)) == "unmatched");
    CHECK(std::string(classifier::match_status_name(MatchStatus::Conflict)) == "conflict");
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A the config parser rejects malformed definitions whole")
{
    auto rejects = [](const std::string& json, const std::string& needle)
    {
        auto compiled = classifier::compile_matching_config(json);
        if (compiled.has_value())
            MESSAGE("expected a rejection but the config compiled");
        REQUIRE_FALSE(compiled.has_value());
        MESSAGE("rejected with: ", compiled.error().format_message());
        if (!needle.empty())
            CHECK(compiled.error().message().find(needle) != std::string::npos);
    };

    const nlohmann::json rule = u04a_rule("r", "reference", "^R", "Resistor");
    const nlohmann::json exception = u04a_exception("e", "name", "X", "Resistor");
    const nlohmann::json good =
        nlohmann::json::parse(u04a_config(nlohmann::json::array({rule}), nlohmann::json::array({exception})));
    REQUIRE(classifier::compile_matching_config(good.dump()).has_value());

    auto patched = [](nlohmann::json entry, const std::string& key, const nlohmann::json& value)
    {
        entry[key] = value;
        return entry;
    };
    auto without = [](nlohmann::json entry, const std::string& key)
    {
        entry.erase(key);
        return entry;
    };
    auto with_rule = [&](const nlohmann::json& entry)
    {
        auto copy = good;
        copy["rules"] = nlohmann::json::array({entry});
        return copy.dump();
    };
    auto with_exception = [&](const nlohmann::json& entry)
    {
        auto copy = good;
        copy["exceptions"] = nlohmann::json::array({entry});
        return copy.dump();
    };

    // 整体形状
    rejects("not json at all", "invalid JSON");
    rejects("[1, 2, 3]", "JSON object");
    rejects(R"({"revision":0,"rules":[],"exceptions":[]})", "schema_version is missing");
    rejects(R"({"schema_version":2,"revision":0,"rules":[],"exceptions":[]})", "unsupported schema_version");
    rejects(R"({"schema_version":"1","revision":0,"rules":[],"exceptions":[]})",
            "schema_version must be an integer");
    rejects(R"({"schema_version":true,"revision":0,"rules":[],"exceptions":[]})",
            "schema_version must be an integer");
    rejects(R"({"schema_version":1.0,"revision":0,"rules":[],"exceptions":[]})",
            "schema_version must be an integer");
    rejects(R"({"schema_version":1,"rules":[],"exceptions":[]})", "revision is missing");
    rejects(R"({"schema_version":1,"revision":-1,"rules":[],"exceptions":[]})", "negative");
    rejects(R"({"schema_version":1,"revision":1.5,"rules":[],"exceptions":[]})", "revision must be an integer");
    rejects(R"({"schema_version":1,"revision":true,"rules":[],"exceptions":[]})", "revision must be an integer");
    rejects(R"({"schema_version":1,"revision":9007199254740992,"rules":[],"exceptions":[]})", "safe integer");
    rejects(R"({"schema_version":1,"revision":0,"exceptions":[]})", "rules must be an array");
    rejects(R"({"schema_version":1,"revision":0,"rules":[]})", "exceptions must be an array");
    rejects(u04a_config(rule), "rules must be an array");

    // 规则条目：缺字段、错类型、越界都拒绝整份配置
    rejects(with_rule(without(rule, "id")), "rules[0].id is missing");
    rejects(with_rule(patched(rule, "id", 7)), "rules[0].id must be a string");
    rejects(with_rule(patched(rule, "id", "")), "rules[0].id must not be empty");
    rejects(with_rule(without(rule, "enabled")), "rules[0].enabled is missing");
    rejects(with_rule(patched(rule, "enabled", "true")), "rules[0].enabled must be a boolean");
    rejects(with_rule(patched(rule, "field", "footprint")), "name/value/model/reference");
    rejects(with_rule(without(rule, "pattern")), "rules[0].pattern is missing");
    rejects(with_rule(without(rule, "case_sensitive")), "rules[0].case_sensitive is missing");
    rejects(with_rule(patched(rule, "case_sensitive", 1)), "rules[0].case_sensitive must be a boolean");
    rejects(with_rule(without(rule, "priority")), "rules[0].priority is missing");
    rejects(with_rule(patched(rule, "priority", 1.5)), "rules[0].priority must be an integer");
    rejects(with_rule(patched(rule, "priority", true)), "rules[0].priority must be an integer");
    rejects(with_rule(patched(rule, "priority", 2147483648LL)), "outside the int32 range");
    rejects(with_rule(patched(rule, "priority", -2147483649LL)), "outside the int32 range");
    rejects(with_rule(without(rule, "type_id")), "rules[0].type_id is missing");
    rejects(with_rule(patched(rule, "type_id", "")), "rules[0].type_id must not be empty");
    rejects(with_rule(patched(rule, "type_id", "custom:not-registered")), "not a loaded component type");
    rejects(with_rule(nlohmann::json("not an object")), "rules[0] must be an object");

    // 正则：坏表达式拒绝（引擎是 PCRE2，不是 ECMAScript）；disabled 条目一样编译，
    // 免得打开后才变坏
    rejects(with_rule(patched(rule, "pattern", "(")), "not a valid PCRE2 pattern");
    rejects(with_rule(patched(patched(rule, "enabled", false), "pattern", "(")),
            "not a valid PCRE2 pattern");

    // 特例条目
    rejects(with_exception(without(exception, "key")), "exceptions[0].key is missing");
    rejects(with_exception(patched(exception, "key", "")), "exceptions[0].key must not be empty");
    rejects(with_exception(without(exception, "enabled")), "exceptions[0].enabled is missing");
    rejects(with_exception(patched(exception, "field", "footprint")), "name/value/model/reference");
    rejects(with_exception(patched(exception, "type_id", "custom:not-registered")),
            "not a loaded component type");
    rejects(with_exception(nlohmann::json::array()), "exceptions[0] must be an object");

    // 重复 id：同一张表内、跨两张表、以及两条都 disabled 的重复都拒绝
    {
        auto copy = good;
        copy["rules"] = nlohmann::json::array({rule, u04a_rule("r", "value", "^X", "Capacitor")});
        rejects(copy.dump(), "duplicate id");
    }
    {
        auto copy = good;
        copy["exceptions"] = nlohmann::json::array({u04a_exception("r", "name", "X", "Capacitor")});
        rejects(copy.dump(), "duplicate id");
    }
    {
        auto copy = good;
        copy["rules"] = nlohmann::json::array(
            {patched(rule, "enabled", false),
             patched(u04a_rule("r", "value", "^X", "Capacitor"), "enabled", false)});
        rejects(copy.dump(), "duplicate id");
    }

    // 同 field + key 的重复特例拒绝（disabled 也一样）；不同 field 或不同大小写是合法的
    {
        auto copy = good;
        copy["exceptions"] = nlohmann::json::array(
            {u04a_exception("e1", "name", "X", "Resistor"), u04a_exception("e2", "name", "X", "Capacitor")});
        rejects(copy.dump(), "duplicates an existing exception");
    }
    {
        auto copy = good;
        copy["exceptions"] = nlohmann::json::array({u04a_exception("e1", "name", "X", "Resistor"),
                                                    u04a_exception("e2", "name", "X", "Capacitor", false)});
        rejects(copy.dump(), "duplicates an existing exception");
    }
    {
        auto copy = good;
        copy["exceptions"] = nlohmann::json::array(
            {u04a_exception("e1", "name", "X", "Resistor"), u04a_exception("e2", "value", "X", "Capacitor")});
        CHECK(classifier::compile_matching_config(copy.dump()).has_value());
    }
    {
        auto copy = good;
        copy["exceptions"] = nlohmann::json::array(
            {u04a_exception("e1", "name", "X", "Resistor"), u04a_exception("e2", "name", "x", "Capacitor")});
        CHECK(classifier::compile_matching_config(copy.dump()).has_value());
    }

    // 坏条目不能被丢掉后接受残缺数组：一条合法 + 一条坏 = 整份拒绝
    {
        auto copy = good;
        copy["rules"] = nlohmann::json::array({rule, patched(rule, "priority", 1.5)});
        rejects(copy.dump(), "rules[1].priority must be an integer");
    }

    // 未知的多余键被忽略（必需字段缺失仍会拒绝，键名打错不会静默变成另一条规则）
    {
        auto copy = good;
        copy["rules"][0]["note"] = "extra";
        CHECK(classifier::compile_matching_config(copy.dump()).has_value());
    }

    // priority 两端在 int32 全范围都合法
    CHECK(classifier::compile_matching_config(with_rule(patched(rule, "priority", 2147483647LL))).has_value());
    CHECK(classifier::compile_matching_config(with_rule(patched(rule, "priority", -2147483648LL))).has_value());

    // 容量：两张表各自 500 条，各自第 501 条拒绝
    {
        nlohmann::json many = nlohmann::json::array();
        for (int index = 0; index < 500; ++index)
            many.push_back(u04a_rule("rule-" + std::to_string(index), "reference", "^R", "Resistor"));
        CHECK(classifier::compile_matching_config(u04a_config(many)).has_value());
        many.push_back(u04a_rule("rule-500", "reference", "^R", "Resistor"));
        rejects(u04a_config(many), "too many rules");
    }
    {
        nlohmann::json many = nlohmann::json::array();
        for (int index = 0; index < 500; ++index)
            many.push_back(u04a_exception("exception-" + std::to_string(index), "name",
                                          "KEY-" + std::to_string(index), "Resistor"));
        CHECK(classifier::compile_matching_config(u04a_config(nlohmann::json::array(), many)).has_value());
        many.push_back(u04a_exception("exception-500", "name", "KEY-500", "Resistor"));
        rejects(u04a_config(nlohmann::json::array(), many), "too many exceptions");
    }

    // 模式 512 字节、特例键 4096 字节：刚好合法，多一字节拒绝
    CHECK(classifier::compile_matching_config(
              u04a_config(nlohmann::json::array({u04a_rule("long", "name", std::string(512, 'a'), "Resistor")})))
              .has_value());
    rejects(u04a_config(nlohmann::json::array(
                {u04a_rule("long", "name", std::string(513, 'a'), "Resistor")})),
            "pattern is 513 bytes");
    CHECK(classifier::compile_matching_config(u04a_config(
              nlohmann::json::array(),
              nlohmann::json::array({u04a_exception("long", "name", std::string(4096, 'k'), "Resistor")})))
              .has_value());
    rejects(u04a_config(nlohmann::json::array(),
                        nlohmann::json::array(
                            {u04a_exception("long", "name", std::string(4097, 'k'), "Resistor")})),
            "key is 4097 bytes");
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A oversized source fields are refused instead of truncated")
{
    const auto snapshot =
        u04a_snapshot(u04a_config(nlohmann::json::array({u04a_rule("r", "reference", "^R", "Resistor")})));

    MatchSample sample;
    sample.reference = std::string(4096, 'R');  // 上限之内：正常评估
    auto decision = snapshot->evaluate(sample);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Resistor");
    REQUIRE(decision.evidence.size() == 1);
    CHECK(decision.evidence.front().text.size() == 4096);  // 依据是原文，没有截断

    sample.reference = std::string(4097, 'R');
    decision = snapshot->evaluate(sample);
    CHECK(decision.status == MatchStatus::InputTooLong);
    CHECK(decision.type_id.empty());
    CHECK(std::string(classifier::match_status_name(decision.status)) == "input_too_long");
    CHECK(decision.reason.find("reference") != std::string::npos);

    // 四个字段都算待匹配字段：任何一个超长就整体拒绝，不做局部匹配
    MatchSample other;
    other.reference = "R1";
    other.name = std::string(4097, 'x');
    CHECK(snapshot->evaluate(other).status == MatchStatus::InputTooLong);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A a missing config installs the editable seed and binds the save target")
{
    TypeMatchingStore store;
    CHECK_FALSE(store.has_snapshot());
    CHECK(store.load_error().empty());
    CHECK_FALSE(fs::exists(config));  // 缺文件：加载不写盘

    REQUIRE(store.load(config.string()).has_value());
    CHECK(store.load_error().empty());
    CHECK(store.has_snapshot());
    CHECK(store.path() == config.string());
    CHECK_FALSE(fs::exists(config));

    const auto snapshot = store.current();
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->revision() == 0);

    // 种子就是分发的那份可编辑位号规则：没有任何私有隐藏映射
    const std::vector<std::string> expected_types = {
        "Capacitor", "Connector", "Crystal",     "Diode",        "FerriteBead",    "Fuse",
        "Inductor",  "Jumper",    "LED",         "MountingHole", "Relay",          "Resistor",
        "Switch",    "TestPoint", "Transformer", "Transistor",   "VoltageRegulator"};
    CHECK(snapshot->referenced_type_ids() == expected_types);

    auto classify = [&](const std::string& reference)
    {
        MatchSample sample;
        sample.reference = reference;
        return snapshot->evaluate(sample);
    };
    CHECK(classify("R?").type_id == "Resistor");
    CHECK(classify("R12").type_id == "Resistor");
    CHECK(classify("C1").type_id == "Capacitor");
    CHECK(classify("L4").type_id == "Inductor");
    CHECK(classify("D3").type_id == "Diode");
    CHECK(classify("LED1").type_id == "LED");
    CHECK(classify("LE2").type_id == "LED");
    CHECK(classify("SW1").type_id == "Switch");
    CHECK(classify("FB1").type_id == "FerriteBead");
    CHECK(classify("JP1").type_id == "Jumper");
    CHECK(classify("TP1").type_id == "TestPoint");
    CHECK(classify("VR3").type_id == "VoltageRegulator");
    CHECK(classify("Q1").type_id == "Transistor");
    CHECK(classify("J1").type_id == "Connector");
    CHECK(classify("X1").type_id == "Crystal");
    CHECK(classify("Y2").type_id == "Crystal");
    CHECK(classify("F1").type_id == "Fuse");
    CHECK(classify("K1").type_id == "Relay");
    CHECK(classify("T1").type_id == "Transformer");
    CHECK(classify("H1").type_id == "MountingHole");
    CHECK(classify("r5").type_id == "Resistor");  // 默认是 icase 的可编辑规则

    // 位号规则只认「整个文本就是位号」：U 不猜具体类型，型号名不是位号
    CHECK(classify("U1").status == MatchStatus::Unmatched);
    CHECK(classify("LM358").status == MatchStatus::Unmatched);
    CHECK(classify("LEDdriver").status == MatchStatus::Unmatched);
    MatchSample empty;
    CHECK(snapshot->evaluate(empty).status == MatchStatus::Unmatched);

    // 第一次保存把种子换成新配置并真的落盘，revision +1
    const std::string next = u04a_config(
        nlohmann::json::array({u04a_rule("only-r", "reference", "^R[0-9?]*$", "Resistor")}),
        nlohmann::json::array(), 0);
    auto saved = store.save(next, 0);
    if (!saved)
        MESSAGE("save failed: ", saved.error().format_message());
    REQUIRE(saved.has_value());
    CHECK(*saved == 1);
    CHECK(store.current()->revision() == 1);
    CHECK(fs::exists(config));
    {
        const auto on_disk = nlohmann::json::parse(read(config));
        CHECK(on_disk["revision"] == 1);
        CHECK(on_disk["schema_version"] == 1);
        REQUIRE(on_disk["rules"].size() == 1);
        CHECK(on_disk["rules"][0]["id"] == "only-r");
        CHECK(on_disk["exceptions"].empty());
    }

    // 重启等价：新仓储从磁盘读回同一份内容
    TypeMatchingStore reopened;
    REQUIRE(reopened.load(config.string()).has_value());
    CHECK(reopened.current()->revision() == 1);
    REQUIRE(reopened.current()->config().rules.size() == 1);
    CHECK(reopened.current()->config().rules[0].id == "only-r");

    // 清空规则并保存后，真实 Reference=R? 也必须 unmatched —— 没有隐藏兜底
    const std::string cleared = u04a_config(nlohmann::json::array(), nlohmann::json::array(), 1);
    REQUIRE(store.save(cleared, 1).has_value());
    CHECK(store.current()->revision() == 2);
    MatchSample reference;
    reference.reference = "R?";
    CHECK(store.current()->evaluate(reference).status == MatchStatus::Unmatched);
    CHECK(store.current()->referenced_type_ids().empty());
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A a failed save keeps the file bytes and the published snapshot")
{
    TypeMatchingStore store;
    REQUIRE(store.load(config.string()).has_value());
    const std::string good = u04a_config(
        nlohmann::json::array({u04a_rule("r", "reference", "^R", "Resistor")}), nlohmann::json::array(), 0);
    REQUIRE(store.save(good, 0).has_value());

    const std::string before = read(config);
    const auto published = store.current();
    REQUIRE(published != nullptr);
    CHECK(published->revision() == 1);

    // 过期 revision：拿旧的 expected 再存一次
    auto stale = store.save(good, 0);
    REQUIRE_FALSE(stale.has_value());
    MESSAGE("stale save: ", stale.error().format_message());
    CHECK(stale.error().message().find("revision mismatch") != std::string::npos);

    // 候选自带的 revision 与 expected 不一致
    const std::string mismatched = u04a_config(
        nlohmann::json::array({u04a_rule("r", "reference", "^C", "Capacitor")}), nlohmann::json::array(), 7);
    CHECK_FALSE(store.save(mismatched, 1).has_value());

    // 坏候选：未知类型 / 坏正则都不落盘
    CHECK_FALSE(store
                    .save(u04a_config(nlohmann::json::array(
                                          {u04a_rule("x", "reference", "^R", "custom:not-registered")}),
                                      nlohmann::json::array(), 1),
                          1)
                    .has_value());
    CHECK_FALSE(store
                    .save(u04a_config(nlohmann::json::array({u04a_rule("x", "reference", "(", "Resistor")}),
                                      nlohmann::json::array(), 1),
                          1)
                    .has_value());

    CHECK(read(config) == before);
    CHECK(store.current()->revision() == 1);
    CHECK(store.current().get() == published.get());  // 发布的还是同一份快照，没被替换

    // 写盘失败：目标 .tmp 被占住 —— 文件与快照都不动
    fs::create_directories(config.string() + ".tmp");
    const std::string rename = u04a_config(
        nlohmann::json::array({u04a_rule("r", "reference", "^C", "Capacitor")}), nlohmann::json::array(), 1);
    auto blocked = store.save(rename, 1);
    REQUIRE_FALSE(blocked.has_value());
    MESSAGE("blocked save: ", blocked.error().format_message());
    CHECK(read(config) == before);
    CHECK(store.current()->revision() == 1);
    CHECK(store.current().get() == published.get());

    // 解除故障后同一个保存必须成功并真的换掉文件
    fs::remove_all(config.string() + ".tmp");
    auto recovered = store.save(rename, 1);
    if (!recovered)
        MESSAGE("recovered save failed: ", recovered.error().format_message());
    REQUIRE(recovered.has_value());
    CHECK(*recovered == 2);
    CHECK(read(config) != before);
    CHECK(store.current()->revision() == 2);
    MatchSample sample;
    sample.reference = "R1";
    CHECK(store.current()->evaluate(sample).status == MatchStatus::Unmatched);  // 新配置只留了 ^C
    sample.reference = "C1";
    CHECK(store.current()->evaluate(sample).type_id == "Capacitor");
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A a bound target that is no longer a regular file fails the save")
{
    TypeMatchingStore store;
    REQUIRE(store.load(config.string()).has_value());
    REQUIRE(store
                .save(u04a_config(nlohmann::json::array({u04a_rule("r", "reference", "^R", "Resistor")}),
                                  nlohmann::json::array(), 0),
                      0)
                .has_value());
    const auto published = store.current();
    REQUIRE(published != nullptr);

    // 绑定过的目标被换成一个目录：写盘必须失败，快照与绑定都不变
    REQUIRE(fs::remove(config));
    REQUIRE(fs::create_directories(config));
    auto blocked = store.save(
        u04a_config(nlohmann::json::array({u04a_rule("r", "reference", "^C", "Capacitor")}),
                    nlohmann::json::array(), 1),
        1);
    REQUIRE_FALSE(blocked.has_value());
    MESSAGE("blocked save: ", blocked.error().format_message());
    CHECK(store.current()->revision() == 1);
    CHECK(store.current().get() == published.get());
    CHECK(store.path() == config.string());
    CHECK_FALSE(fs::exists(config.string() + ".tmp"));  // 失败路径不留半截临时文件
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A a bad file is never silently replaced by the seed")
{
    // 初始就是坏文件：没有快照、没有绑定目标、错误明确，普通保存被拒
    write(config, "{ not json");
    TypeMatchingStore store;
    auto first = store.load(config.string());
    REQUIRE_FALSE(first.has_value());
    MESSAGE("initial load error: ", first.error().format_message());
    CHECK(store.load_error().find("invalid JSON") != std::string::npos);
    CHECK_FALSE(store.has_snapshot());
    CHECK(store.path().empty());
    CHECK_FALSE(store.save(u04a_config(nlohmann::json::array(), nlohmann::json::array(), 0), 0).has_value());
    CHECK(read(config) == "{ not json");  // 原文件一个字节没动

    // 修好文件后重新加载即恢复
    const std::string good = u04a_config(
        nlohmann::json::array({u04a_rule("only-r", "reference", "^R", "Resistor")}), nlohmann::json::array(), 3);
    write(config, good);
    auto recovered = store.load(config.string());
    if (!recovered)
        MESSAGE("reload failed: ", recovered.error().format_message());
    REQUIRE(recovered.has_value());
    CHECK(store.load_error().empty());
    CHECK(store.has_snapshot());
    CHECK(store.path() == config.string());
    CHECK(store.current()->revision() == 3);

    // 已经有效之后再读到坏内容：保留最后有效快照与绑定，只记录错误
    const auto published = store.current();
    REQUIRE(published != nullptr);
    write(config,
          u04a_config(nlohmann::json::array({u04a_rule("x", "reference", "^R", "custom:not-registered")})));
    auto rejected = store.load(config.string());
    REQUIRE_FALSE(rejected.has_value());
    MESSAGE("reload rejection: ", rejected.error().format_message());
    CHECK(store.load_error().find("not a loaded component type") != std::string::npos);
    CHECK(store.has_snapshot());
    CHECK(store.current().get() == published.get());
    CHECK(store.current()->revision() == 3);
    CHECK(store.path() == config.string());
    // load_error 状态下普通保存/apply 一律拒绝，不能假装配置有效
    CHECK_FALSE(store.save(good, 3).has_value());
    CHECK_FALSE(store.acquire_reference_guard().has_value());

    // 再次修好并重载：错误清除，保存重新可用
    write(config, good);
    REQUIRE(store.load(config.string()).has_value());
    CHECK(store.load_error().empty());
    CHECK(store.acquire_reference_guard().has_value());
    auto saved = store.save(good, 3);
    if (!saved)
        MESSAGE("save after recovery failed: ", saved.error().format_message());
    REQUIRE(saved.has_value());
    CHECK(*saved == 4);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A a directory is a read failure and not a missing file")
{
    const auto directory = temp.path() / "u04a_directory_config";
    fs::create_directories(directory);

    TypeMatchingStore store;
    auto loaded = store.load(directory.string());
    REQUIRE_FALSE(loaded.has_value());
    MESSAGE("directory load error: ", loaded.error().format_message());
    CHECK_FALSE(store.has_snapshot());  // 目录不会被当成「文件缺失」装上种子
    CHECK(store.path().empty());
    CHECK_FALSE(store.load_error().empty());
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A type references are checked against the loaded registry not the builtin list")
{
    // 用户的历史配置可能省略某些内置行：现在表里只有 Resistor
    core::TypeRegistry::instance().load_component_types_json(
        R"({"types":[{"id":"Resistor","name":"Resistor"}]})");
    CHECK(core::is_builtin_component_id("Capacitor"));  // builtin 静态表里当然有
    CHECK_FALSE(core::TypeRegistry::instance().find_component("Capacitor").has_value());  // 当前表里没有

    // 引用 Capacitor 的配置必须被拒：只查 builtin 静态集合就会放过一个表里根本没有的 ID
    auto rejected = classifier::compile_matching_config(
        u04a_config(nlohmann::json::array({u04a_rule("c", "reference", "^C", "Capacitor")})));
    REQUIRE_FALSE(rejected.has_value());
    MESSAGE("rejected: ", rejected.error().format_message());
    CHECK(rejected.error().message().find("Capacitor") != std::string::npos);

    // 表里确实有的类型照常可用
    CHECK(classifier::compile_matching_config(
              u04a_config(nlohmann::json::array({u04a_rule("r", "reference", "^R", "Resistor")})))
              .has_value());

    // 缺文件时的默认种子引用了一堆不在表里的类型：明确报缺失 ID，不静默过滤种子规则，
    // 也不把路径绑成保存目标 —— 普通保存不会把种子写到用户文件上
    TypeMatchingStore store;
    auto loaded = store.load(config.string());
    REQUIRE_FALSE(loaded.has_value());
    MESSAGE("seed rejection: ", loaded.error().format_message());
    CHECK(store.load_error().find("Capacitor") != std::string::npos);
    CHECK_FALSE(store.has_snapshot());
    CHECK(store.path().empty());
    CHECK_FALSE(store.save(u04a_config(nlohmann::json::array(), nlohmann::json::array(), 0), 0).has_value());
    CHECK_FALSE(fs::exists(config));

    // 用户恢复/注册缺失类型之后再加载同一个路径就恢复：错误可见，但不是死局
    u04a_load_full_registry();
    auto recovered = store.load(config.string());
    if (!recovered)
        MESSAGE("reload after registry repair failed: ", recovered.error().format_message());
    REQUIRE(recovered.has_value());
    CHECK(store.load_error().empty());
    CHECK(store.has_snapshot());
    CHECK(store.path() == config.string());
    CHECK(store.current()->revision() == 0);
    CHECK_FALSE(fs::exists(config));  // 种子装在内存里，加载不写盘
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A the revision limit refuses to wrap around")
{
    constexpr std::uint64_t maximum = 9007199254740991ULL;
    const std::string at_limit = u04a_config(
        nlohmann::json::array({u04a_rule("r", "reference", "^R", "Resistor")}), nlohmann::json::array(),
        maximum);
    write(config, at_limit);

    TypeMatchingStore store;
    REQUIRE(store.load(config.string()).has_value());
    CHECK(store.current()->revision() == maximum);

    const std::string before = read(config);
    const auto published = store.current();
    auto saved = store.save(at_limit, maximum);
    REQUIRE_FALSE(saved.has_value());
    MESSAGE("limit save: ", saved.error().format_message());
    CHECK(saved.error().message().find("increment") != std::string::npos);
    CHECK(read(config) == before);
    CHECK(store.current().get() == published.get());
    CHECK(store.current()->revision() == maximum);

    // 上限 +1 在解析阶段就被拒绝，不回绕也不饱和
    auto too_big = classifier::compile_matching_config(
        u04a_config(nlohmann::json::array(), nlohmann::json::array(), maximum + 1));
    REQUIRE_FALSE(too_big.has_value());
    CHECK(too_big.error().message().find("safe integer") != std::string::npos);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A a held snapshot keeps its type references protected until released")
{
    // 注册一个自定义类型：没有推荐库规则的类型也能被引用
    auto added =
        core::TypeRegistry::instance().add_component_type(core::TypeEntry{"", "U04Selector", "", "", ""});
    REQUIRE(added.has_value());
    const std::string selector = *added;
    CHECK(selector.starts_with("custom:"));

    TypeMatchingStore store;
    REQUIRE(store.load(config.string()).has_value());
    const std::string first = u04a_config(
        nlohmann::json::array({u04a_rule("selector", "name", "^SPECIAL_", selector),
                               u04a_rule("off", "reference", "^C", "Capacitor", 0, false)}),
        nlohmann::json::array(), 0);
    REQUIRE(store.save(first, 0).has_value());

    {
        auto guard = store.acquire_reference_guard();
        if (!guard)
            MESSAGE("guard failed: ", guard.error().format_message());
        REQUIRE(guard.has_value());
        CHECK(guard->references(selector));     // enabled 规则引用的自定义类型
        CHECK(guard->references("Capacitor"));  // disabled 规则也算引用
        CHECK_FALSE(guard->references("Resistor"));
        const auto& ids = guard->type_ids();
        bool sorted = true;
        for (std::size_t index = 1; index < ids.size(); ++index)
        {
            if (ids[index - 1] >= ids[index])
                sorted = false;
        }
        CHECK(sorted);
        CHECK(std::find(ids.begin(), ids.end(), selector) != ids.end());
    }

    // 领一份快照并一直持有：它引用的类型在此期间必须继续受保护
    auto lease = store.current();
    REQUIRE(lease != nullptr);
    const std::string second = u04a_config(
        nlohmann::json::array({u04a_rule("only-r", "reference", "^R", "Resistor")}),
        nlohmann::json::array(), 1);
    REQUIRE(store.save(second, 1).has_value());
    {
        const auto current_ids = store.current()->referenced_type_ids();
        CHECK(std::find(current_ids.begin(), current_ids.end(), selector) == current_ids.end());
    }
    {
        auto guard = store.acquire_reference_guard();
        REQUIRE(guard.has_value());
        CHECK(guard->references(selector));     // 旧 lease 还在 -> 仍然受保护
        CHECK(guard->references("Capacitor"));  // 旧 lease 里的 disabled 规则也算
        CHECK(guard->references("Resistor"));
    }

    // 释放旧快照后引用解除（弱引用自然过期，下一次查询就不再算数）
    lease.reset();
    {
        auto guard = store.acquire_reference_guard();
        REQUIRE(guard.has_value());
        CHECK_FALSE(guard->references(selector));
        CHECK_FALSE(guard->references("Capacitor"));
        CHECK(guard->references("Resistor"));
    }

    // 后来坏掉的加载：guard 必须报错，不能把未知配置当成 0 引用
    write(config, "{ broken");
    REQUIRE_FALSE(store.load(config.string()).has_value());
    CHECK_FALSE(store.acquire_reference_guard().has_value());

    // 一开始就是坏文件时同样拒绝
    const auto broken_path = temp.path() / "u04a_broken_config.json";
    write(broken_path, "[]");
    TypeMatchingStore fresh;
    REQUIRE_FALSE(fresh.load(broken_path.string()).has_value());
    CHECK_FALSE(fresh.has_snapshot());
    CHECK_FALSE(fresh.acquire_reference_guard().has_value());
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A a DTO whose field is not one of the four known fields is rejected")
{
    // 真实 DTO（不走 JSON 文本）：先解析一份合法配置，再只把 field 改成越界枚举值，
    // 其余字段全部合法 —— 唯一缺陷就是 field 本身。JSON 里写未知 field 文本的既有用例
    // （"footprint" 被拒）继续保留，这里补的是直接构造 DTO 时同一份语义。
    const std::string good =
        u04a_config(nlohmann::json::array({u04a_rule("r", "reference", "^R[0-9?]*$", "Resistor")}),
                    nlohmann::json::array({u04a_exception("e", "name", "SPECIAL", "Resistor")}), 0);
    auto parsed = classifier::parse_matching_config_json(good);
    REQUIRE(parsed.has_value());

    auto bad_rule_field = [&](MatchField field, bool enabled)
    {
        classifier::MatchingConfig candidate = *parsed;
        candidate.rules[0].field = field;
        candidate.rules[0].enabled = enabled;
        return candidate;
    };
    auto bad_exception_field = [&](MatchField field, bool enabled)
    {
        classifier::MatchingConfig candidate = *parsed;
        candidate.exceptions[0].field = field;
        candidate.exceptions[0].enabled = enabled;
        return candidate;
    };
    auto rejects = [](const classifier::MatchingConfig& candidate, const std::string& what)
    {
        auto compiled =
            classifier::compile_matching_config(candidate, classifier::registry_has_component_type);
        if (compiled.has_value())
            MESSAGE("expected a rejection for ", what);
        return !compiled.has_value();
    };

    // 越界枚举必须被明确拒绝，disabled 条目不能漏（打开后不能变坏）：
    // 这个值既不可能匹配任何字段，又会在序列化时被写成另一条正常规则。
    CHECK(rejects(bad_rule_field(static_cast<MatchField>(99), true), "rules[0].field=99"));
    CHECK(rejects(bad_rule_field(static_cast<MatchField>(-1), true), "rules[0].field=-1"));
    CHECK(rejects(bad_rule_field(static_cast<MatchField>(99), false), "disabled rules[0].field=99"));
    CHECK(rejects(bad_exception_field(static_cast<MatchField>(99), true), "exceptions[0].field=99"));
    CHECK(rejects(bad_exception_field(static_cast<MatchField>(-1), false),
                  "disabled exceptions[0].field=-1"));

    // 对照：同一份 DTO 换成合法 field 就能编译 —— 拒绝针对越界值，不是「DTO 一概不放行」
    CHECK(classifier::compile_matching_config(bad_rule_field(MatchField::Reference, true),
                                              classifier::registry_has_component_type)
              .has_value());
    // 对照：JSON 文本里的未知 field 现在已经拒绝
    CHECK_FALSE(classifier::compile_matching_config(
                    u04a_config(nlohmann::json::array({u04a_rule("bad", "footprint", "^R", "Resistor")})))
                    .has_value());

    // 仓储层：坏 DTO 保存必须失败，磁盘原字节、内存 revision 与已发布快照都不动
    auto save_must_fail = [&](const fs::path& target, classifier::MatchingConfig candidate,
                              const std::string& what)
    {
        candidate.revision = 1;  // 与仓储当前 revision 一致：唯一缺陷就是 field
        TypeMatchingStore store;
        REQUIRE(store.load(target.string()).has_value());
        const std::string baseline =
            u04a_config(nlohmann::json::array({u04a_rule("only-r", "reference", "^R[0-9?]*$", "Resistor")}),
                        nlohmann::json::array(), 0);
        REQUIRE(store.save(baseline, 0).has_value());
        const std::string before = read(target);
        const auto published = store.current();
        REQUIRE(published != nullptr);

        auto saved = store.save(candidate, 1);
        if (saved.has_value())
            MESSAGE("expected the save to fail for ", what);
        CHECK_FALSE(saved.has_value());
        CHECK(read(target) == before);
        CHECK(store.current()->revision() == 1);
        CHECK(store.current().get() == published.get());
    };

    save_must_fail(config, bad_rule_field(static_cast<MatchField>(99), true), "rules[0].field=99");
    save_must_fail(temp.path() / "u04a_dto_field_exception.json",
                   bad_exception_field(static_cast<MatchField>(-1), true), "exceptions[0].field=-1");
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A exception only type references are protected while the old snapshot is held")
{
    // 两个自定义类型只被 exceptions 引用：规则里一个都不出现，所以保护只能来自特例
    auto enabled_added =
        core::TypeRegistry::instance().add_component_type(core::TypeEntry{"", "U04ExceptionOnly", "", "", ""});
    REQUIRE(enabled_added.has_value());
    const std::string enabled_only = *enabled_added;
    auto disabled_added =
        core::TypeRegistry::instance().add_component_type(core::TypeEntry{"", "U04DisabledOnly", "", "", ""});
    REQUIRE(disabled_added.has_value());
    const std::string disabled_only = *disabled_added;
    CHECK(enabled_only != disabled_only);

    TypeMatchingStore store;
    REQUIRE(store.load(config.string()).has_value());
    REQUIRE(store
                .save(u04a_config(nlohmann::json::array({u04a_rule("r", "reference", "^R[0-9?]*$", "Resistor")}),
                                  nlohmann::json::array(
                                      {u04a_exception("ex-on", "name", "EX-ONLY", enabled_only, true),
                                       u04a_exception("ex-off", "name", "EX-OFF", disabled_only, false)}),
                                  0),
                      0)
                .has_value());
    {
        // 当前 guard 就包含它们：disabled 特例一样算引用
        auto guard = store.acquire_reference_guard();
        if (!guard)
            MESSAGE("guard failed: ", guard.error().format_message());
        REQUIRE(guard.has_value());
        CHECK(guard->references(enabled_only));
        CHECK(guard->references(disabled_only));
        CHECK(guard->references("Resistor"));
        const auto ids = guard->type_ids();
        CHECK(std::find(ids.begin(), ids.end(), enabled_only) != ids.end());
        CHECK(std::find(ids.begin(), ids.end(), disabled_only) != ids.end());
    }

    // 领一份快照并一直持有：它是唯一还引用这两个自定义类型的东西
    auto lease = store.current();
    REQUIRE(lease != nullptr);
    {
        REQUIRE(lease->config().rules.size() == 1);
        CHECK(lease->config().rules[0].type_id == "Resistor");  // 规则不引用任何自定义 ID
        const auto ids = lease->referenced_type_ids();
        CHECK(std::find(ids.begin(), ids.end(), enabled_only) != ids.end());
        CHECK(std::find(ids.begin(), ids.end(), disabled_only) != ids.end());
    }

    // 保存一份删掉全部 exceptions 的新配置：当前快照不再引用这两个 ID
    REQUIRE(store
                .save(u04a_config(
                          nlohmann::json::array({u04a_rule("only-c", "reference", "^C[0-9?]*$", "Capacitor")}),
                          nlohmann::json::array(), 1),
                      1)
                .has_value());
    {
        const auto ids = store.current()->referenced_type_ids();
        CHECK(std::find(ids.begin(), ids.end(), enabled_only) == ids.end());
        CHECK(std::find(ids.begin(), ids.end(), disabled_only) == ids.end());
        CHECK(std::find(ids.begin(), ids.end(), "Resistor") == ids.end());
    }
    {
        auto guard = store.acquire_reference_guard();
        REQUIRE(guard.has_value());
        CHECK(guard->references(enabled_only));   // 旧 lease 里的 enabled 特例
        CHECK(guard->references(disabled_only));  // 旧 lease 里的 disabled 特例
        CHECK(guard->references("Resistor"));     // 旧 lease 里的规则
        CHECK(guard->references("Capacitor"));    // 当前快照里的规则
    }

    // 释放旧快照后解除保护：只剩当前快照的引用
    lease.reset();
    {
        auto guard = store.acquire_reference_guard();
        REQUIRE(guard.has_value());
        CHECK_FALSE(guard->references(enabled_only));
        CHECK_FALSE(guard->references(disabled_only));
        CHECK_FALSE(guard->references("Resistor"));
        CHECK(guard->references("Capacitor"));
    }
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A repeated snapshot leases expire on their own when dropped")
{
    auto added =
        core::TypeRegistry::instance().add_component_type(core::TypeEntry{"", "U04DroppedLease", "", "", ""});
    REQUIRE(added.has_value());
    const std::string custom = *added;

    TypeMatchingStore store;
    REQUIRE(store.load(config.string()).has_value());
    REQUIRE(store
                .save(u04a_config(nlohmann::json::array({u04a_rule("custom", "name", "^ONLY_", custom)}),
                                  nlohmann::json::array(), 0),
                      0)
                .has_value());

    auto held = store.current();
    REQUIRE(held != nullptr);
    // 重复领取再放弃：登记表里的 weak_ptr 自然过期，还持有的那一份不受影响
    for (int round = 0; round < 4; ++round)
    {
        auto dropped = store.current();
        REQUIRE(dropped != nullptr);
        CHECK(dropped.get() == held.get());
    }
    {
        auto guard = store.acquire_reference_guard();
        REQUIRE(guard.has_value());
        CHECK(guard->references(custom));
    }

    // 新配置不再引用 custom：held 还在，所以仍然受保护
    REQUIRE(store
                .save(u04a_config(nlohmann::json::array({u04a_rule("r", "reference", "^R[0-9?]*$", "Resistor")}),
                                  nlohmann::json::array(), 1),
                      1)
                .has_value());
    {
        auto guard = store.acquire_reference_guard();
        REQUIRE(guard.has_value());
        CHECK(guard->references(custom));
        CHECK(guard->references("Resistor"));
    }

    // 放弃最后一份旧快照：下一次查询就解除，过期登记不会撑住保护
    held.reset();
    for (int round = 0; round < 4; ++round)
    {
        auto dropped = store.current();
        REQUIRE(dropped != nullptr);
        const auto ids = dropped->referenced_type_ids();
        CHECK(std::find(ids.begin(), ids.end(), custom) == ids.end());
        CHECK(std::find(ids.begin(), ids.end(), "Resistor") != ids.end());
    }
    {
        auto guard = store.acquire_reference_guard();
        REQUIRE(guard.has_value());
        CHECK_FALSE(guard->references(custom));
        CHECK(guard->references("Resistor"));
    }

    // 反复领取同一份快照不会让登记表无限增长：登记按快照身份去重、过期即清。
    // 这里只用可观察行为验证 —— 再领 20 次之后，当前快照的引用照样在，旧快照一释放就解除。
    auto again = store.current();
    REQUIRE(again != nullptr);
    for (int round = 0; round < 20; ++round)
    {
        auto same = store.current();
        REQUIRE(same != nullptr);
        CHECK(same.get() == again.get());
    }
    again.reset();
    {
        auto guard = store.acquire_reference_guard();
        REQUIRE(guard.has_value());
        CHECK(guard->references("Resistor"));
        CHECK_FALSE(guard->references(custom));
    }
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A business snapshot acquisition is atomic with the health check")
{
    // 还没 load：业务领取必须明确失败，不能靠 has_snapshot/load_error 两次调用拼健康检查
    TypeMatchingStore store;
    CHECK_FALSE(store.has_snapshot());
    auto before_load = store.acquire_business_snapshot();
    REQUIRE_FALSE(before_load.has_value());
    MESSAGE("not loaded: ", before_load.error().format_message());
    CHECK_FALSE(before_load.error().message().empty());

    // 正常临时 load：可以取得快照，并且这次领取登记了 lease
    auto added =
        core::TypeRegistry::instance().add_component_type(core::TypeEntry{"", "U04Business", "", "", ""});
    REQUIRE(added.has_value());
    const std::string business = *added;
    const std::string first = u04a_config(
        nlohmann::json::array({u04a_rule("selector", "name", "^SPECIAL_", business),
                               u04a_rule("off", "reference", "^C", "Capacitor", 0, false)}),
        nlohmann::json::array({u04a_exception("off-exception", "name", "KEY", "Capacitor", false)}), 0);
    write(config, first);
    REQUIRE(store.load(config.string()).has_value());
    auto lease = store.acquire_business_snapshot();
    if (!lease)
        MESSAGE("business acquisition failed: ", lease.error().format_message());
    REQUIRE(lease.has_value());
    CHECK((*lease)->revision() == 0);
    CHECK(store.load_error().empty());

    // 保存一份不再引用该类型的配置：旧 lease 还在，引用保护继续看得到它（disabled 条目也算）
    REQUIRE(store
                .save(u04a_config(nlohmann::json::array({u04a_rule("only-r", "reference", "^R", "Resistor")}),
                                  nlohmann::json::array(), 0),
                      0)
                .has_value());

    // 保存后发布的是新快照：current 身上已经没有这个类型，但旧 lease 还在
    CHECK(store.current()->revision() == 1);
    {
        auto guard = store.acquire_reference_guard();
        REQUIRE(guard.has_value());
        CHECK(guard->references(business));
        CHECK(guard->references("Capacitor"));  // disabled 规则与 disabled 特例都算
    }

    // 坏 reload：current 仍只读展示最后有效内容，但业务领取必须拒绝 ——
    // 坏/未知配置不能靠「还有旧快照」继续跑业务，也不能当成 0 引用放行
    write(config, "{ broken");
    REQUIRE_FALSE(store.load(config.string()).has_value());
    CHECK_FALSE(store.load_error().empty());
    REQUIRE(store.current() != nullptr);
    CHECK(store.current()->revision() == 1);
    auto blocked = store.acquire_business_snapshot();
    REQUIRE_FALSE(blocked.has_value());
    MESSAGE("bad load: ", blocked.error().format_message());
    CHECK(blocked.error().message().find("load error") != std::string::npos);
    CHECK_FALSE(store.acquire_reference_guard().has_value());

    // 修复文件再 reload：业务恢复；旧 lease 释放后引用保护随之解除
    write(config,
          u04a_config(nlohmann::json::array({u04a_rule("only-r", "reference", "^R", "Resistor")}),
                      nlohmann::json::array(), 1));
    REQUIRE(store.load(config.string()).has_value());
    CHECK(store.load_error().empty());
    auto recovered = store.acquire_business_snapshot();
    if (!recovered)
        MESSAGE("business acquisition after repair failed: ", recovered.error().format_message());
    REQUIRE(recovered.has_value());
    CHECK((*recovered)->revision() == 1);
    lease->reset();
    recovered->reset();
    for (int round = 0; round < 4; ++round)
    {
        auto dropped = store.current();
        REQUIRE(dropped != nullptr);
        const auto ids = dropped->referenced_type_ids();
        CHECK(std::find(ids.begin(), ids.end(), business) == ids.end());
    }
    {
        auto guard = store.acquire_reference_guard();
        REQUIRE(guard.has_value());
        CHECK_FALSE(guard->references(business));
        CHECK_FALSE(guard->references("Capacitor"));
        CHECK(guard->references("Resistor"));
    }

    // 一开始就是坏文件：业务失败，current 也不许凭空造一份
    const fs::path broken_path = temp.path() / "u04a_business_broken.json";
    write(broken_path, "{ not json");
    TypeMatchingStore fresh;
    CHECK_FALSE(fresh.acquire_business_snapshot().has_value());
    CHECK(fresh.current() == nullptr);
    REQUIRE_FALSE(fresh.load(broken_path.string()).has_value());
    CHECK_FALSE(fresh.load_error().empty());
    CHECK(fresh.current() == nullptr);  // 没有有效配置：只读口也不返回东西
    CHECK_FALSE(fresh.acquire_business_snapshot().has_value());
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A an explicitly empty read function falls back to the production reader")
{
    // 显式传入空 std::function：必须退回生产默认读取器，与默认构造一致，不出现 bad_function_call
    TypeMatchingStore fallback(classifier::registry_has_component_type, classifier::ConfigFileReadFn{});
    TypeMatchingStore plain;

    // 缺文件：两边都安装同一份内存种子并绑定同一个保存目标
    REQUIRE(fallback.load(config.string()).has_value());
    REQUIRE(plain.load(config.string()).has_value());
    CHECK(fallback.load_error().empty());
    CHECK(fallback.path() == config.string());
    CHECK(plain.path() == config.string());
    REQUIRE(fallback.current() != nullptr);
    REQUIRE(plain.current() != nullptr);
    CHECK(fallback.current()->referenced_type_ids() == plain.current()->referenced_type_ids());
    CHECK_FALSE(fs::exists(config));  // 缺文件初始化不写盘

    // 有效文件：注入空读取器的仓储照常读到真实内容
    const std::string valid =
        u04a_config(nlohmann::json::array({u04a_rule("r", "reference", "^R[0-9?]*$", "Resistor")}),
                    nlohmann::json::array(), 0);
    write(config, valid);
    TypeMatchingStore from_file(classifier::registry_has_component_type, classifier::ConfigFileReadFn{});
    REQUIRE(from_file.load(config.string()).has_value());
    CHECK(from_file.load_error().empty());
    REQUIRE(from_file.current() != nullptr);
    CHECK(from_file.current()->revision() == 0);
    MatchSample sample;
    sample.reference = "R?";
    CHECK(from_file.current()->evaluate(sample).type_id == "Resistor");

    // 目录不是缺文件：空读取器同样要走生产读取器的错误区分，不能当成种子初始化
    const fs::path directory = temp.path() / "u04a_dir_config";
    fs::create_directories(directory);
    TypeMatchingStore on_directory(classifier::registry_has_component_type, classifier::ConfigFileReadFn{});
    CHECK_FALSE(on_directory.load(directory.string()).has_value());
    CHECK_FALSE(on_directory.load_error().empty());
    CHECK_FALSE(on_directory.has_snapshot());
}

// ============================================================
// U04-A：正则引擎换到 PCRE2 之后的方言、预算与并发语义
// 用例全部走真实 compile/evaluate，不复制匹配算法，也不暴露任何 test-only 计数接口。
// ============================================================

TEST_CASE_FIXTURE(U04AFixture, "U04-A PCRE2 dialect treats anchors newlines dots and casing by bytes")
{
    // ^$ 是整段文本边界（DOLLAR_ENDONLY）：结尾的换行不算「末尾」
    const auto anchored = u04a_snapshot(u04a_config(
        nlohmann::json::array({u04a_rule("full", "name", "^R12$", "Resistor")})));
    MatchSample sample;
    sample.name = "R12";
    CHECK(anchored->evaluate(sample).type_id == "Resistor");
    sample.name = "R12\n";
    CHECK(anchored->evaluate(sample).status == MatchStatus::Unmatched);
    sample.name = "XR12";
    CHECK(anchored->evaluate(sample).status == MatchStatus::Unmatched);

    // ^ 不是行首：不开 multiline，换行后面的内容不能靠 ^ 命中
    sample.name = "abc\nR12";
    CHECK(anchored->evaluate(sample).status == MatchStatus::Unmatched);
    // 子串搜索（不写锚）不受影响：regex_search 语义保持不变
    const auto search =
        u04a_snapshot(u04a_config(nlohmann::json::array({u04a_rule("sub", "name", "R12", "Resistor")})));
    CHECK(search->evaluate(sample).type_id == "Resistor");

    // 点号默认不匹配 CR/LF（换行约定 ANYCRLF）
    const auto dot =
        u04a_snapshot(u04a_config(nlohmann::json::array({u04a_rule("dot", "name", "^a.c$", "Resistor")})));
    sample.name = "abc";
    CHECK(dot->evaluate(sample).type_id == "Resistor");
    sample.name = "a\nc";
    CHECK(dot->evaluate(sample).status == MatchStatus::Unmatched);
    sample.name = "a\rc";
    CHECK(dot->evaluate(sample).status == MatchStatus::Unmatched);

    // case_sensitive 只影响 ASCII 大小写折叠：false 命中大小写混写，true 完全按字节
    const auto insensitive = u04a_snapshot(u04a_config(nlohmann::json::array(
        {u04a_rule("i", "reference", "^r[0-9?]*$", "Resistor", 0, true, false)})));
    const auto sensitive = u04a_snapshot(u04a_config(
        nlohmann::json::array({u04a_rule("s", "reference", "^r[0-9?]*$", "Resistor", 0, true, true)})));
    MatchSample reference;
    reference.reference = "R?";
    CHECK(insensitive->evaluate(reference).type_id == "Resistor");
    CHECK(sensitive->evaluate(reference).status == MatchStatus::Unmatched);
    reference.reference = "r?";
    CHECK(sensitive->evaluate(reference).type_id == "Resistor");
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A PCRE2 matches bytes so utf8 text and embedded NULs stay explicit")
{
    // 中文是三个字节，不是三个「字符」：^.$ 只认一个字节，^...$ 才认这个字
    const auto one_byte =
        u04a_snapshot(u04a_config(nlohmann::json::array({u04a_rule("one", "value", "^.$", "Capacitor")})));
    const auto three_bytes = u04a_snapshot(
        u04a_config(nlohmann::json::array({u04a_rule("three", "value", "^...$", "Resistor")})));
    const auto han = u04a_snapshot(
        u04a_config(nlohmann::json::array({u04a_rule("han", "value", "^中$", "Resistor")})));
    MatchSample sample;
    sample.value = "中";
    REQUIRE(sample.value->size() == 3);
    CHECK(one_byte->evaluate(sample).status == MatchStatus::Unmatched);
    CHECK(three_bytes->evaluate(sample).type_id == "Resistor");
    const auto decision = han->evaluate(sample);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Resistor");
    CHECK(decision.evidence.front().text == "中");  // 依据是原文，没有按「字符」截断
    sample.value = "x";
    CHECK(one_byte->evaluate(sample).type_id == "Capacitor");

    // 内嵌 NUL：显式长度，匹配不会在 NUL 处截断
    const auto nul =
        u04a_snapshot(u04a_config(nlohmann::json::array({u04a_rule("nul", "name", "^R.X$", "Resistor")})));
    sample = MatchSample{};
    sample.name = std::string("R\0X", 3);
    REQUIRE(sample.name->size() == 3);
    CHECK(nul->evaluate(sample).type_id == "Resistor");
    sample.name = "RX";
    CHECK(nul->evaluate(sample).status == MatchStatus::Unmatched);

    // lookaround 与反向引用是 PCRE2 自带能力：不自己造禁词表，也不假装它们非法
    const auto advanced = u04a_snapshot(u04a_config(nlohmann::json::array(
        {u04a_rule("look", "reference", "^R(?=[0-9])(?![0-9]*[A-Z])[0-9]+$", "Resistor"),
         u04a_rule("backref", "name", "^(R)(?:-\\1)+$", "Capacitor")})));
    sample = MatchSample{};
    sample.reference = "R12";
    CHECK(advanced->evaluate(sample).type_id == "Resistor");
    sample.reference = "R1A";
    CHECK(advanced->evaluate(sample).status == MatchStatus::Unmatched);
    sample = MatchSample{};
    sample.name = "R-R-R";
    CHECK(advanced->evaluate(sample).type_id == "Capacitor");
    sample.name = "R-C-R";
    CHECK(advanced->evaluate(sample).status == MatchStatus::Unmatched);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A a bad PCRE2 pattern is rejected with its rule id and offset")
{
    auto rejected = classifier::compile_matching_config(u04a_config(nlohmann::json::array(
        {u04a_rule("good", "reference", "^R[0-9?]*$", "Resistor"),
         u04a_rule("broken-paren", "reference", "^R(", "Resistor")})));
    REQUIRE_FALSE(rejected.has_value());
    MESSAGE("rejected: ", rejected.error().format_message());
    const std::string message = rejected.error().message();
    CHECK(message.find("rules[1]") != std::string::npos);
    CHECK(message.find("broken-paren") != std::string::npos);  // 能定位到是哪条规则
    CHECK(message.find("offset") != std::string::npos);        // 以及模式里的位置
    CHECK(message.find("PCRE2") != std::string::npos);         // 说清是哪个引擎拒的

    // disabled 条目一样编译：打开之后不能变成坏模式
    CHECK_FALSE(classifier::compile_matching_config(u04a_config(nlohmann::json::array(
                    {u04a_rule("off", "reference", "a{2,1}", "Resistor", 0, false)})))
                    .has_value());
    CHECK_FALSE(classifier::compile_matching_config(u04a_config(
                    nlohmann::json::array({u04a_rule("class", "reference", "[z-a]", "Resistor")})))
                    .has_value());

    // PCRE2 支持的语法必须能编译：lookaround、反向引用、模式开头的动词
    CHECK(classifier::compile_matching_config(u04a_config(nlohmann::json::array(
              {u04a_rule("look", "reference", "^(?=R)R(?<=R)[0-9]+$", "Resistor"),
               u04a_rule("backref", "reference", "^(R)\\1$", "Resistor"),
               u04a_rule("verb", "reference", "(*NO_START_OPT)^R$", "Resistor")})))
              .has_value());

    // 空模式仍然是合法模式（regex_search 语义下命中一切非缺失字段），这里只锁住它不被误拒
    CHECK(classifier::compile_matching_config(
              u04a_config(nlohmann::json::array({u04a_rule("empty", "reference", "", "Resistor")})))
              .has_value());
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A the work budget cuts off a catastrophic pattern")
{
    const auto snapshot = u04a_snapshot(u04a_config(nlohmann::json::array(
        {u04a_rule("nested", "reference", "(*NO_AUTO_POSSESS)(*NO_START_OPT)^(a+)+$", "Resistor"),
         u04a_rule("designator", "reference", "^R[0-9?]*$", "Resistor"),
         u04a_rule("light", "name", "^LIGHT$", "Capacitor")})));

    // 嵌套量词 + 确定的不匹配后缀：真实引擎会在这里回溯爆炸，必须被预算截住
    MatchSample heavy;
    heavy.reference = std::string(30, 'a') + "!";
    const auto decision = snapshot->evaluate(heavy);
    REQUIRE(decision.status == MatchStatus::EvaluationLimit);
    CHECK(std::string(classifier::match_status_name(decision.status)) == "evaluation_limit");
    CHECK(decision.type_id.empty());
    CHECK(decision.evidence.empty());  // 已经收集到的命中一律不采用
    CHECK(decision.reason.find("nested") != std::string::npos);  // 触发的是哪条规则

    // 预算属于单次 evaluate：紧接着的普通样本照常工作，不被上一次超限拖累
    MatchSample light;
    light.name = "LIGHT";
    CHECK(snapshot->evaluate(light).type_id == "Capacitor");
    MatchSample normal;
    normal.reference = "R?";
    CHECK(snapshot->evaluate(normal).type_id == "Resistor");
    // 超限的那一条本身在普通输入上仍然正常工作
    MatchSample plain;
    plain.reference = "aaa";
    CHECK(snapshot->evaluate(plain).type_id == "Resistor");
    // 重复求值互不影响：计数按次重置，再一次同样的恶例仍然确定地超限
    CHECK(snapshot->evaluate(heavy).status == MatchStatus::EvaluationLimit);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A the work budget accumulates over start positions and rules")
{
    // 非锚起点累计：单个起点的开销很小，4096 个起点的总和才超过预算。
    std::string long_literal(200, 'a');
    long_literal.push_back('b');
    const auto unanchored = u04a_snapshot(u04a_config(nlohmann::json::array(
        {u04a_rule("scan", "name", "(*NO_START_OPT)" + long_literal, "Resistor")})));

    MatchSample scan_sample;
    scan_sample.name = std::string(4096, 'a');  // 上限之内的最长输入
    // 唯一那个 'b' 放在 4000 号位：任何「最后一个必需字符不在主题里」的快速失败都用不上，
    // 起点必须真的一个个试过去，而预算会在走到它之前就被耗尽。
    (*scan_sample.name)[4000] = 'b';
    const auto scanned = unanchored->evaluate(scan_sample);
    REQUIRE(scanned.status == MatchStatus::EvaluationLimit);
    CHECK(scanned.reason.find("scan") != std::string::npos);

    // 多规则累计：模式是两条永远不会命中的短分支，且没有重复组 ——
    // 每次尝试的回溯栈深度是常数（不先撞上 depth/heap 限额），单个起点开销很小。
    // 单条规则 4096 个起点加起来仍远在预算之内；同样的规则叠 40 条才超过预算。
    MatchSample linear_sample;
    linear_sample.name = std::string(4096, 'a');
    nlohmann::json rules = nlohmann::json::array();
    rules.push_back(u04a_rule("linear-0", "name", "(*NO_START_OPT)(?:bb|cc)", "Resistor"));
    const auto single = u04a_snapshot(u04a_config(rules));
    const auto single_decision = single->evaluate(linear_sample);
    REQUIRE(single_decision.status == MatchStatus::Unmatched);  // 一条规则：普通不匹配，没超限
    CHECK(single_decision.type_id.empty());

    for (int index = 1; index < 40; ++index)
        rules.push_back(u04a_rule("linear-" + std::to_string(index), "name", "(*NO_START_OPT)(?:bb|cc)",
                                  "Resistor"));
    const auto many = u04a_snapshot(u04a_config(rules));
    const auto combined = many->evaluate(linear_sample);
    REQUIRE(combined.status == MatchStatus::EvaluationLimit);
    CHECK(combined.type_id.empty());
    CHECK(combined.evidence.empty());
    CHECK(combined.reason.find("linear-") != std::string::npos);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A a limit or an engine error discards an already collected best hit")
{
    // 第一条规则先真的命中（较低优先级），随后最高优先级的重规则才把预算耗光：
    // 已经收集到的命中必须整次丢弃，不能返回它 —— 先命中后超限才是这条的关键。
    const std::string first_rule = "first-hit";
    MatchSample heavy;
    heavy.name = std::string(30, 'a') + "!";
    const auto first_only = u04a_snapshot(u04a_config(
        nlohmann::json::array({u04a_rule(first_rule, "name", "^a+!$", "Capacitor", 0)})));
    CHECK(first_only->evaluate(heavy).type_id == "Capacitor");  // 单独评估：这条规则确实命中

    const auto limited = u04a_snapshot(u04a_config(nlohmann::json::array(
        {u04a_rule(first_rule, "name", "^a+!$", "Capacitor", 0),
         u04a_rule("heavy-later", "name", "(*NO_AUTO_POSSESS)(*NO_START_OPT)^(a+)+$", "Resistor", 100)})));
    const auto decision = limited->evaluate(heavy);
    REQUIRE(decision.status == MatchStatus::EvaluationLimit);
    CHECK(std::string(classifier::match_status_name(decision.status)) == "evaluation_limit");
    CHECK(decision.type_id.empty());  // 不继承已收集到的 Capacitor
    CHECK(decision.evidence.empty());
    CHECK(decision.reason.find("heavy-later") != std::string::npos);  // 触发的是重规则
    CHECK(decision.reason.find(first_rule) == std::string::npos);

    // 同样的顺序换成真实引擎错误：官方 SDK 实测的递归循环模式在 subject "x" 上返回 -52，
    // 这不是编译错误、也不是不匹配，必须整次作废并指向触发规则。
    MatchSample symbol_x;
    symbol_x.reference = "x";
    const auto x_only = u04a_snapshot(u04a_config(
        nlohmann::json::array({u04a_rule(first_rule, "reference", "^x$", "Capacitor", 0)})));
    CHECK(x_only->evaluate(symbol_x).type_id == "Capacitor");  // 先确认这条规则真的命中

    const auto errored = u04a_snapshot(u04a_config(nlohmann::json::array(
        {u04a_rule(first_rule, "reference", "^x$", "Capacitor", 0),
         u04a_rule("recurseloop", "reference",
                   "(*NO_START_OPT)(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|b))(?&A)", "Resistor", 100)})));
    const auto failed = errored->evaluate(symbol_x);
    REQUIRE(failed.status == MatchStatus::EvaluationError);
    CHECK(std::string(classifier::match_status_name(failed.status)) == "evaluation_error");
    CHECK(failed.type_id.empty());  // 不继承已收集到的 Capacitor
    CHECK(failed.evidence.empty());
    CHECK(failed.reason.find("recurseloop") != std::string::npos);  // 触发规则 id
    CHECK(failed.reason.find("PCRE2 error") != std::string::npos);
    CHECK(failed.reason.find("-52") != std::string::npos);  // 真实引擎错误码，不是超限限额
    CHECK(failed.reason.find(first_rule) == std::string::npos);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A concurrent evaluations of one snapshot keep independent budgets")
{
    // 只有一条真实规则：两个线程共用这一份已编译 program，各自 evaluate 时创建自己的会话。
    // 正常样本 "aaa" 也命中同一条规则（不是靠缺字段跳过求值），只是不会触发回溯爆炸。
    const auto snapshot = u04a_snapshot(u04a_config(nlohmann::json::array(
        {u04a_rule("nested", "reference", "(*NO_AUTO_POSSESS)(*NO_START_OPT)^(a+)+$", "Resistor")})));

    constexpr int kRounds = 8;
    constexpr int kThreads = 2;
    std::barrier<> start(kThreads);  // 每轮共同起跑：串行调度不算并发证据
    std::vector<MatchStatus> heavy_status(kRounds, MatchStatus::Unmatched);
    std::vector<MatchStatus> light_status(kRounds, MatchStatus::Unmatched);
    std::vector<std::string> heavy_type(kRounds);
    std::vector<std::string> light_type(kRounds);
    std::vector<std::string> heavy_reason(kRounds);
    std::vector<std::string> light_rule(kRounds);
    std::string heavy_error;  // 各自线程写自己的字符串，join 之后主线程才读
    std::string light_error;

    std::thread heavy(
        [&]
        {
            for (int round = 0; round < kRounds; ++round)
            {
                start.arrive_and_wait();  // 异常也不能少到一次，否则另一个线程会卡在屏障上
                try
                {
                    MatchSample sample;
                    sample.reference = std::string(30, 'a') + "!";  // 确定的不匹配后缀
                    const auto decision = snapshot->evaluate(sample);
                    heavy_status[round] = decision.status;
                    heavy_type[round] = decision.type_id;
                    heavy_reason[round] = decision.reason;
                }
                catch (const std::exception& thrown)
                {
                    if (heavy_error.empty())
                        heavy_error = thrown.what();
                }
            }
        });
    std::thread light(
        [&]
        {
            for (int round = 0; round < kRounds; ++round)
            {
                start.arrive_and_wait();
                try
                {
                    MatchSample sample;
                    sample.reference = "aaa";  // 命中同一条 nested 规则，但不耗预算
                    const auto decision = snapshot->evaluate(sample);
                    light_status[round] = decision.status;
                    light_type[round] = decision.type_id;
                    light_rule[round] = decision.evidence.empty() ? std::string("(no evidence)")
                                                                 : decision.evidence.front().id;
                }
                catch (const std::exception& thrown)
                {
                    if (light_error.empty())
                        light_error = thrown.what();
                }
            }
        });
    heavy.join();
    light.join();

    CHECK(heavy_error.empty());
    CHECK(light_error.empty());
    for (int round = 0; round < kRounds; ++round)
    {
        // 重样本每轮都耗光自己的预算，另一个线程的预算不受影响
        CHECK(heavy_status[round] == MatchStatus::EvaluationLimit);
        CHECK(heavy_type[round].empty());
        CHECK(heavy_reason[round].find("nested") != std::string::npos);
        // 正常样本每轮都由同一个 compiled program 命中
        CHECK(light_status[round] == MatchStatus::Matched);
        CHECK(light_type[round] == "Resistor");
        CHECK(light_rule[round] == "nested");
    }
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A a real symbol with an explicitly empty reference still has the field")
{
    const auto snapshot = u04a_snapshot(u04a_config(nlohmann::json::array(
        {u04a_rule("empty", "reference", "^$", "TestPoint"),
         u04a_rule("any", "reference", ".*", "TestPoint")})));

    // 真实 Symbol 上显式设置空 reference：字段存在且为空串，^$ 与 .* 都该命中
    core::Symbol explicit_empty;
    explicit_empty.set_name("EMPTY_REF");
    explicit_empty.set_property("reference", "");
    const MatchSample present = MatchSample::from_symbol(explicit_empty);
    REQUIRE(present.reference.has_value());
    CHECK(present.reference->empty());
    CHECK(snapshot->evaluate(present).type_id == "TestPoint");

    // 对照：没有这个属性（哪怕 name 看着像位号、前缀也是 R）就是没有字段，两条规则都不命中
    core::Symbol missing;
    missing.set_name("R123");
    missing.set_reference_prefix("R");
    const MatchSample absent = MatchSample::from_symbol(missing);
    CHECK_FALSE(absent.reference.has_value());
    CHECK(snapshot->evaluate(absent).status == MatchStatus::Unmatched);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A capacity is counted in utf-8 bytes and each table allows 500 entries")
{
    const std::string han = "中";  // 3 字节
    auto repeat = [&han](std::size_t count)
    {
        std::string text;
        text.reserve(count * han.size());
        for (std::size_t index = 0; index < count; ++index)
            text += han;
        return text;
    };

    // pattern：510 字节合法，513 字节拒绝 —— 上限算的是字节，不是字符数
    CHECK(classifier::compile_matching_config(u04a_config(
              nlohmann::json::array({u04a_rule("han", "name", repeat(170), "Resistor")})))
              .has_value());
    auto long_pattern = classifier::compile_matching_config(u04a_config(
        nlohmann::json::array({u04a_rule("han", "name", repeat(171), "Resistor")})));
    REQUIRE_FALSE(long_pattern.has_value());
    MESSAGE("long pattern: ", long_pattern.error().format_message());
    CHECK(long_pattern.error().message().find("pattern is 513 bytes") != std::string::npos);

    // 特例 key：4095 字节合法，4098 字节拒绝
    CHECK(classifier::compile_matching_config(u04a_config(
              nlohmann::json::array(),
              nlohmann::json::array({u04a_exception("k", "name", repeat(1365), "Resistor")})))
              .has_value());
    auto long_key = classifier::compile_matching_config(u04a_config(
        nlohmann::json::array(),
        nlohmann::json::array({u04a_exception("k", "name", repeat(1366), "Resistor")})));
    REQUIRE_FALSE(long_key.has_value());
    MESSAGE("long key: ", long_key.error().format_message());
    CHECK(long_key.error().message().find("key is 4098 bytes") != std::string::npos);

    // 源字段：4095 字节照常评估，4098 字节整体拒绝
    const auto snapshot = u04a_snapshot(
        u04a_config(nlohmann::json::array({u04a_rule("r", "reference", "^R", "Resistor")})));
    MatchSample sample;
    sample.reference = repeat(1365);
    CHECK(snapshot->evaluate(sample).status == MatchStatus::Unmatched);
    sample.reference = repeat(1366);
    CHECK(snapshot->evaluate(sample).status == MatchStatus::InputTooLong);

    // 两张表各自 500：同一份配置里 500 + 500 合法，各自第 501 条拒绝
    nlohmann::json rules = nlohmann::json::array();
    nlohmann::json exceptions = nlohmann::json::array();
    for (int index = 0; index < 500; ++index)
    {
        rules.push_back(u04a_rule("rule-" + std::to_string(index), "reference", "^R", "Resistor"));
        exceptions.push_back(u04a_exception("exception-" + std::to_string(index), "name",
                                            "KEY-" + std::to_string(index), "Resistor"));
    }
    const auto joint = u04a_snapshot(u04a_config(rules, exceptions));
    CHECK(joint->config().rules.size() == 500);
    CHECK(joint->config().exceptions.size() == 500);

    nlohmann::json extra_rule = rules;
    extra_rule.push_back(u04a_rule("rule-500", "reference", "^R", "Resistor"));
    auto too_many_rules = classifier::compile_matching_config(u04a_config(extra_rule, exceptions));
    REQUIRE_FALSE(too_many_rules.has_value());
    CHECK(too_many_rules.error().message().find("too many rules") != std::string::npos);

    nlohmann::json extra_exception = exceptions;
    extra_exception.push_back(u04a_exception("exception-500", "name", "KEY-500", "Resistor"));
    auto too_many_exceptions = classifier::compile_matching_config(u04a_config(rules, extra_exception));
    REQUIRE_FALSE(too_many_exceptions.has_value());
    CHECK(too_many_exceptions.error().message().find("too many exceptions") != std::string::npos);
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A the distributed seed file is the same editable config as the built-in default")
{
    // 只读项目里的分发种子：路径由编译期项目根唯一确定；找不到就明确失败，
    // 不静默跳过，也不碰用户数据目录。
    const fs::path seed = u04a_project_seed_path();
    if (seed.empty())
        MESSAGE("distributed seed not found under ", KFORGE_TEST_PROJECT_ROOT);
    REQUIRE_FALSE(seed.empty());
    MESSAGE("distributed seed: ", seed.string());
    CHECK(seed.parent_path().filename() == "config");
    CHECK(seed.filename() == "component_type_matching.json");

    auto from_file = classifier::parse_matching_config_json(read(seed));
    if (!from_file)
        MESSAGE("distributed seed rejected: ", from_file.error().format_message());
    REQUIRE(from_file.has_value());
    const auto from_default = classifier::parse_matching_config_json(
        classifier::default_matching_config_json());
    REQUIRE(from_default.has_value());

    // 规范序列化逐字节相同 = 同一份可编辑配置，没有隐藏的第二套映射
    CHECK(classifier::matching_config_to_json(*from_file) ==
          classifier::matching_config_to_json(*from_default));
    CHECK(from_file->rules.size() == from_default->rules.size());
    CHECK(from_file->exceptions.size() == from_default->exceptions.size());

    // 分发文件也必须能被真实仓储装进快照（引用的类型都在完整注册表里）
    TypeMatchingStore store;
    REQUIRE(store.load(seed.string()).has_value());
    CHECK(store.load_error().empty());
    CHECK(store.current()->revision() == 0);
    const auto default_snapshot = u04a_snapshot(classifier::default_matching_config_json());
    CHECK(store.current()->referenced_type_ids() == default_snapshot->referenced_type_ids());
    CHECK_FALSE(store.current()->referenced_type_ids().empty());
    MatchSample sample;
    sample.reference = "R?";
    CHECK(store.current()->evaluate(sample).type_id == "Resistor");

    // 控制：把当前工作目录换成一个放着同名「假种子」的临时目录。helper 不搜索 cwd，
    // 只能给出项目里那一份；cwd 由 RAII 还原，断言失败也不会把进程留在别处。
    const fs::path trap = temp.path() / "u04a_cwd_trap";
    const fs::path trap_seed = trap / "config" / "component_type_matching.json";
    write(trap_seed,
          u04a_config(nlohmann::json::array({u04a_rule("trap", "reference", "^TRAP$", "Resistor")})));
    {
        ScopedCurrentPath moved(trap);
        std::error_code error;
        const bool inside_trap = fs::equivalent(fs::current_path(error), trap, error);
        CHECK(inside_trap);
        const fs::path again = u04a_project_seed_path();
        REQUIRE_FALSE(again.empty());
        CHECK(again == seed);  // 还是项目里那一份，不是 cwd 里的假种子
        std::error_code same_error;
        const bool points_at_trap = fs::equivalent(again, trap_seed, same_error);
        CHECK_FALSE(points_at_trap);
        CHECK(read(again) != read(trap_seed));
    }
    CHECK(u04a_project_seed_path() == seed);  // cwd 已还原，种子依旧可定位
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A a serialization failure is a save error and publishes nothing")
{
    TypeMatchingStore store;
    REQUIRE(store.load(config.string()).has_value());
    const std::string baseline = u04a_config(
        nlohmann::json::array({u04a_rule("only-r", "reference", "^R[0-9?]*$", "Resistor")}),
        nlohmann::json::array(), 0);
    REQUIRE(store.save(baseline, 0).has_value());
    const std::string before = read(config);
    const auto published = store.current();
    REQUIRE(published != nullptr);

    auto candidate = classifier::parse_matching_config_json(baseline);
    REQUIRE(candidate.has_value());
    candidate->revision = 1;
    // PCRE2 是 8-bit 按字节的引擎：这两个字节是合法模式，但不是合法 UTF-8，
    // JSON 序列化会抛 —— 公开接口只承诺 Result，必须转成明确错误且不发布任何东西。
    candidate->rules[0].pattern = std::string("\xff\xfe", 2);
    CHECK(classifier::compile_matching_config(*candidate, classifier::registry_has_component_type)
              .has_value());

    auto saved = store.save(*candidate, 1);
    if (saved.has_value())
        MESSAGE("expected the serialization to fail");
    REQUIRE_FALSE(saved.has_value());
    MESSAGE("serialization failure: ", saved.error().format_message());
    CHECK(read(config) == before);
    CHECK(store.current().get() == published.get());
    CHECK(store.current()->revision() == 1);
    CHECK(store.load_error().empty());
    CHECK_FALSE(fs::exists(config.string() + ".tmp"));
}

// ============================================================
// U04-A load-race：锁外读取与陈旧发布的确定性用例
//
// 屏障只负责事件顺序：被截停的那一次读取已经拿到真实结果 → 较新的操作完成 → 放行。
// 限时等待只是死锁看门狗，不用「某操作若干毫秒没完成」当正确性证据；也不 sleep 竞速。
// 前三组描述的是**期望语义**：陈旧读取必须被明确拒绝、不得回滚或污染已发布状态。
// 现阶段的 load 还没有代次核验，这三组预期为运行期语义红；第四组是绿控制。
// ============================================================

TEST_CASE_FIXTURE(U04AFixture, "U04-A load-race a stale good read must not roll back a newer save")
{
    const std::string first = u04a_config(
        nlohmann::json::array({u04a_rule("first", "reference", "^R[0-9?]*$", "Resistor")}),
        nlohmann::json::array(), 1);
    write(config, first);

    ReadGate gate;
    TypeMatchingStore store(classifier::registry_has_component_type, u04a_gated_reader());
    REQUIRE(store.load(config.string()).has_value());
    REQUIRE(store.current() != nullptr);
    CHECK(store.current()->revision() == 1);

    // 后台读者委托生产读取器，拿到 revision 1 的真实字节后停在自己的闸门里（配置锁外）
    PendingLoad stale(gate, store, config.string());
    const bool paused = gate.wait_until_paused(std::chrono::seconds(5));
    if (!paused)
        MESSAGE("the old read never reached the barrier");
    REQUIRE(paused);

    // 较新的操作先完成：真实写盘到 revision 2 并发布
    const std::string second = u04a_config(
        nlohmann::json::array({u04a_rule("second", "reference", "^C[0-9?]*$", "Capacitor")}),
        nlohmann::json::array(), 1);
    auto saved = store.save(second, 1);
    if (!saved)
        MESSAGE("newer save failed: ", saved.error().format_message());
    REQUIRE(saved.has_value());
    CHECK(*saved == 2);
    const std::string committed = read(config);
    const auto published = store.current();
    REQUIRE(published != nullptr);
    CHECK(published->revision() == 2);
    CHECK(store.path() == config.string());
    CHECK(store.load_error().empty());

    // 放行旧读取：它读到的是已经过期的 revision 1，必须返回明确的过期错误，
    // 磁盘、current、revision、绑定路径、load_error 一个都不许动
    stale.join();
    REQUIRE(stale.outcome_.has_value());
    REQUIRE_FALSE(stale.outcome_->has_value());
    MESSAGE("stale load: ", stale.outcome_->error().format_message());
    CHECK(stale.outcome_->error().message().find("stale") != std::string::npos);
    CHECK(read(config) == committed);
    CHECK(store.current().get() == published.get());
    CHECK(store.current()->revision() == 2);
    CHECK(store.path() == config.string());
    CHECK(store.load_error().empty());
    CHECK(store.acquire_reference_guard().has_value());
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A load-race a stale failed read must not overwrite a newer good state")
{
    const std::string good = u04a_config(
        nlohmann::json::array({u04a_rule("first", "reference", "^R[0-9?]*$", "Resistor")}),
        nlohmann::json::array(), 1);
    write(config, good);

    ReadGate gate;
    TypeMatchingStore store(classifier::registry_has_component_type, u04a_gated_reader());
    REQUIRE(store.load(config.string()).has_value());
    REQUIRE(store.current() != nullptr);

    // 磁盘换成坏内容：旧读取拿到的就是这份真实的坏字节，然后停在锁外
    write(config, "{ not json");
    PendingLoad stale(gate, store, config.string());
    const bool paused = gate.wait_until_paused(std::chrono::seconds(5));
    if (!paused)
        MESSAGE("the old read never reached the barrier");
    REQUIRE(paused);

    // 较新的成功保存先完成：真实写盘 revision 2，把坏内容覆盖掉
    const std::string second = u04a_config(
        nlohmann::json::array({u04a_rule("second", "reference", "^C[0-9?]*$", "Capacitor")}),
        nlohmann::json::array(), 1);
    auto saved = store.save(second, 1);
    if (!saved)
        MESSAGE("newer save failed: ", saved.error().format_message());
    REQUIRE(saved.has_value());
    const std::string committed = read(config);
    const auto published = store.current();
    REQUIRE(published != nullptr);
    CHECK(published->revision() == 2);
    CHECK(store.load_error().empty());

    // 放行旧读取：它带来的是一份陈旧的解析失败，不能盖掉刚发布的健康状态
    stale.join();
    REQUIRE(stale.outcome_.has_value());
    REQUIRE_FALSE(stale.outcome_->has_value());
    MESSAGE("stale load: ", stale.outcome_->error().format_message());
    CHECK(stale.outcome_->error().message().find("stale") != std::string::npos);
    CHECK(read(config) == committed);
    CHECK(store.current().get() == published.get());
    CHECK(store.path() == config.string());
    CHECK(store.load_error().empty());
    CHECK(store.acquire_reference_guard().has_value());
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A load-race a stale good read must not clear a newer failure state")
{
    const std::string good = u04a_config(
        nlohmann::json::array({u04a_rule("first", "reference", "^R[0-9?]*$", "Resistor")}),
        nlohmann::json::array(), 1);
    write(config, good);

    ReadGate crossing_gate;
    ReadGate after_failure_gate;
    TypeMatchingStore store(classifier::registry_has_component_type, u04a_gated_reader());
    REQUIRE(store.load(config.string()).has_value());
    const auto published = store.current();
    REQUIRE(published != nullptr);

    // 第一读者先拿到 revision 1 的真实好字节，停在锁外，随后要跨越两次坏 load。
    PendingLoad crossing(crossing_gate, store, config.string());
    REQUIRE(crossing_gate.wait_until_paused(std::chrono::seconds(5)));

    // 较新的一次失败 load 先完成：读的是另一个坏文件
    const fs::path bad = temp.path() / "u04a_bad_config.json";
    write(bad, "{ not json");
    REQUIRE_FALSE(store.load(bad.string()).has_value());
    const std::string failure = store.load_error();
    CHECK_FALSE(failure.empty());
    CHECK(store.current().get() == published.get());  // 失败保留最后有效快照
    CHECK(store.path() == config.string());

    // 第二读者必须在第一次失败已经发布 load_error 之后才启动：它拿到的仍是好字节，
    // 之后只跨越一次「完全相同的坏 load」—— 只按错误文本变化推进代次的实现
    // 会让它以为状态没变过，从而被这条分支检出来。
    PendingLoad after_failure(after_failure_gate, store, config.string());
    REQUIRE(after_failure_gate.wait_until_paused(std::chrono::seconds(5)));

    // 完全相同的坏 load 再来一次：错误字符串一个字都没变，但它仍是新的状态发布
    REQUIRE_FALSE(store.load(bad.string()).has_value());
    CHECK(store.load_error() == failure);

    // 放行两个读者（各自独立 release/join）：都必须是过期错误，且不能把较新的失败状态改回成功
    crossing.join();
    after_failure.join();
    REQUIRE(crossing.outcome_.has_value());
    REQUIRE(after_failure.outcome_.has_value());
    CHECK_FALSE(crossing.outcome_->has_value());
    if (crossing.outcome_->has_value())
        MESSAGE("the crossing reader was allowed to publish");
    else
        CHECK(crossing.outcome_->error().message().find("stale") != std::string::npos);
    CHECK_FALSE(after_failure.outcome_->has_value());
    if (after_failure.outcome_->has_value())
        MESSAGE("the reader started after the first failure was allowed to publish");
    else
        CHECK(after_failure.outcome_->error().message().find("stale") != std::string::npos);
    CHECK(store.load_error() == failure);
    CHECK(store.current().get() == published.get());
    CHECK(store.path() == config.string());
    CHECK_FALSE(store.acquire_reference_guard().has_value());  // 坏状态不能当成 0 引用
}

TEST_CASE_FIXTURE(U04AFixture, "U04-A load-race a failed save does not make an in-flight read stale")
{
    const std::string good = u04a_config(
        nlohmann::json::array({u04a_rule("first", "reference", "^R[0-9?]*$", "Resistor")}),
        nlohmann::json::array(), 1);
    write(config, good);

    ReadGate gate;
    TypeMatchingStore store(classifier::registry_has_component_type, u04a_gated_reader());
    REQUIRE(store.load(config.string()).has_value());
    const std::string before = read(config);
    const auto published = store.current();
    REQUIRE(published != nullptr);

    PendingLoad pending(gate, store, config.string());
    const bool paused = gate.wait_until_paused(std::chrono::seconds(5));
    if (!paused)
        MESSAGE("the read never reached the barrier");
    REQUIRE(paused);

    // 真实的写盘失败（.tmp 被占住）：失败保存不是状态发布，不该让在途读取过期
    fs::create_directories(config.string() + ".tmp");
    const std::string next = u04a_config(
        nlohmann::json::array({u04a_rule("second", "reference", "^C[0-9?]*$", "Capacitor")}),
        nlohmann::json::array(), 1);
    auto blocked = store.save(next, 1);
    REQUIRE_FALSE(blocked.has_value());
    MESSAGE("blocked save: ", blocked.error().format_message());
    CHECK(read(config) == before);
    CHECK(store.current().get() == published.get());
    CHECK(store.load_error().empty());
    CHECK(store.path() == config.string());

    // 绿控制：失败保存不是状态发布，在途读者照常成功 ——
    // 实现不能靠「保存尝试次数」推进代次，也不能因为一次保存尝试就取消旧读者。
    pending.join();
    REQUIRE(pending.outcome_.has_value());
    CHECK(pending.outcome_->has_value());  // ← 修正：这是一次正常成功的 load，不是失败
    if (!pending.outcome_->has_value())
        MESSAGE("the in-flight read was rejected: ", pending.outcome_->error().format_message());
    CHECK(store.load_error().empty());
    REQUIRE(store.current() != nullptr);
    CHECK(store.current()->revision() == 1);   // 读到的就是磁盘上那份 revision 1
    CHECK(store.path() == config.string());
    CHECK(read(config) == before);             // load 不写盘，失败保存也没改过文件
    MatchSample reloaded;
    reloaded.reference = "R7";
    CHECK(store.current()->evaluate(reloaded).type_id == "Resistor");  // 发布的是真实读到的内容
    fs::remove_all(config.string() + ".tmp");
}

// ============================================================
// U04-B1：普通导入与批量分类接统一核心（红测试）
//
// 只调用真实生产入口与真实核心：局部 TypeMatchingStore（临时文件、fixture 成员，
// 不动 singleton、不读用户数据目录）、ClassificationService、ImportPipeline、
// ImportManager。产品尚未消费 matcher 时的业务断言失败就是本轮红；
// 夹具/前提断言（REQUIRE）失败不算业务红，Root 需按语义区分。
// ============================================================

namespace
{

/// 内存里登记「全部内置 + 追加 custom 类型」；不写用户文件，RegistrySnapshot 负责还原。
void u04b1_register_types(const std::vector<std::string>& extra_ids)
{
    nlohmann::json types = nlohmann::json::array();
    for (int index = 0; index <= static_cast<int>(core::ComponentType::Unknown); ++index)
    {
        const char* id = core::builtin_component_id(static_cast<core::ComponentType>(index));
        types.push_back(nlohmann::json{{"id", id}, {"name", id}});
    }
    for (const auto& id : extra_ids)
        types.push_back(nlohmann::json{{"id", id}, {"name", id}});
    core::TypeRegistry::instance().load_component_types_json(nlohmann::json{{"types", types}}.dump());
}

/// 把规则写进临时配置文件并 reload 进局部 store；加载失败是夹具失败，不是业务红。
void u04b1_store_config(classifier::TypeMatchingStore& store, const fs::path& path,
                        nlohmann::json rules, nlohmann::json exceptions = nlohmann::json::array())
{
    write(path, u04a_config(std::move(rules), std::move(exceptions)));
    auto loaded = store.load(path.string());
    if (!loaded)
        MESSAGE("matching config load failed: ", loaded.error().format_message());
    REQUIRE(loaded.has_value());
}

/// 组装一条真实符号行：缓存前缀列与真实 Reference 属性分开给（nullopt = 没有该属性）。
core::Symbol u04b1_part(const std::string& name, const std::string& prefix,
                        std::optional<std::string> reference, const std::string& value = "",
                        const std::string& mpn = "", const std::string& type = "Unknown")
{
    core::Symbol part;
    part.set_name(name);
    part.set_reference_prefix(prefix);
    if (reference)
        part.set_property("reference", *reference);
    if (!value.empty())
        part.set_default_value(value);
    if (!mpn.empty())
        part.set_mpn(mpn);
    part.component_type_id = type;
    return part;
}

/// 直接执行一条 raw SQL（自己的短 session；调用方不得同时持有 session）
void u04b1_raw_exec(storage::Database& db, const std::string& sql)
{
    auto session = db.session();
    char* error = nullptr;
    const int rc = sqlite3_exec(session.handle(), sql.c_str(), nullptr, nullptr, &error);
    const std::string message = error ? error : std::string();
    sqlite3_free(error);
    if (rc != SQLITE_OK)
        MESSAGE("raw sql failed: ", message);
    REQUIRE(rc == SQLITE_OK);
}

/// 一列的原始读取结果：区分真 SQL NULL 与空串（空串和 NULL 不是一回事）
struct U04B1Column
{
    bool is_null = false;
    std::string text;
};

/// stmt 一 prepare 成功就交给 unique_ptr 接管：任何 REQUIRE 失败或字符串分配
/// 异常都会 finalize，不把句柄留给 Database::close/临时目录清理。
using U04B1Stmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

U04B1Column u04b1_raw_column(storage::Database& db, const std::string& id,
                             const std::string& column)
{
    U04B1Column value;
    auto session = db.session();
    sqlite3_stmt* raw_stmt = nullptr;
    const int prepared = sqlite3_prepare_v2(
        session.handle(), ("SELECT " + column + " FROM symbols WHERE id = ?1").c_str(), -1,
        &raw_stmt, nullptr);
    U04B1Stmt stmt(raw_stmt, &sqlite3_finalize);
    REQUIRE(prepared == SQLITE_OK);
    REQUIRE(stmt != nullptr);
    REQUIRE(sqlite3_bind_text(stmt.get(), 1, id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt.get()) == SQLITE_ROW);
    if (sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL)
        value.is_null = true;
    else
    {
        const unsigned char* text = sqlite3_column_text(stmt.get(), 0);
        value.text = text ? reinterpret_cast<const char*>(text) : std::string();
    }
    return value;
}

/// 按 where 片段数 symbols 行（例如 "component_type = 'Resistor'"）
int u04b1_count_symbols(storage::Database& db, const std::string& where)
{
    auto session = db.session();
    sqlite3_stmt* raw_stmt = nullptr;
    const int prepared = sqlite3_prepare_v2(
        session.handle(), ("SELECT COUNT(*) FROM symbols WHERE " + where).c_str(), -1, &raw_stmt,
        nullptr);
    U04B1Stmt stmt(raw_stmt, &sqlite3_finalize);
    REQUIRE(prepared == SQLITE_OK);
    REQUIRE(stmt != nullptr);
    REQUIRE(sqlite3_step(stmt.get()) == SQLITE_ROW);
    return sqlite3_column_int(stmt.get(), 0);
}

/// 按名字取符号 id（缺行是夹具失败）
std::string u04b1_symbol_id(storage::Database& db, const std::string& name)
{
    auto session = db.session();
    auto row = storage::SymbolRepository(session).find_by_name(name);
    if (!row)
        MESSAGE("symbol lookup failed for [", name, "]: ", row.error().format_message());
    REQUIRE(row.has_value());
    return row->id();
}

/// 已记录的导入文件快照条数（普通导入是否消费/记录了源文件）。
/// 走产品仓储读，避免测试自己建表/假设表已存在。
int u04b1_imported_files(storage::Database& db)
{
    auto session = db.session();
    auto all = storage::ImportedFilesRepository(session).all();
    if (!all)
        MESSAGE("imported_files read failed: ", all.error().format_message());
    REQUIRE(all.has_value());
    return static_cast<int>(all->size());
}

/// 临时根里的真实用户库 + 符号行工厂（真实 repo、真实库行）
struct U04B1Library
{
    storage::Database& db;
    classifier::TypeMatchingStore& store;
    fs::path root;
    fs::path symbol_dir;
    fs::path target;
    std::string library_id;

    U04B1Library(storage::Database& database, classifier::TypeMatchingStore& matching,
                 const fs::path& temp_root, const std::string& stem)
        : db(database), store(matching), root(temp_root / ("u04b1_" + stem)),
          symbol_dir(root / "symbols"), target(symbol_dir / (stem + ".kicad_sym"))
    {
        write(target, "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n)\n");
        library_id = audit_register_user_library(db, target, symbol_dir, root / "footprints");
    }

    std::string insert(const core::Symbol& part)
    {
        return fix02_insert_symbol(db, library_id, part);
    }

    core::Symbol read(const std::string& symbol_id)
    {
        auto session = db.session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        if (!row)
            MESSAGE("symbol read failed: ", row.error().format_message());
        REQUIRE(row.has_value());
        return *row;
    }

    std::string find_id(const std::string& name)
    {
        auto session = db.session();
        auto row = storage::SymbolRepository(session).find_by_name(name);
        if (!row)
            MESSAGE("symbol lookup failed for [", name, "]: ", row.error().format_message());
        REQUIRE(row.has_value());
        return row->id();
    }
};

// ---------------- 组 10/11 夹具：官方 SQLITE_TRACE_STMT + 独立第二连接 ----------------
//
// 机制已由 U04-source-race-review 的官方 SQLite 探针验证：主连接在首个
// BEGIN IMMEDIATE 开始执行时回调，此时写锁尚未取得；回调只做有界握手，
// 第二连接（独立 handle、独立锁、1.5 秒 busy timeout）提交来源更新后回调返回。
// 回调里不执行 SQL、不碰应用 Database/session、不抛异常；所有清理走 RAII。
struct U04B1SourceRace
{
    storage::Database& db;
    fs::path db_path;
    std::string worker_sql;
    sqlite3* main_handle = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    bool armed = true;       // 只截停本轮第一个 BEGIN IMMEDIATE
    bool begin_seen = false;
    bool worker_done = false;
    bool released = false;
    bool handshake_timeout = false;
    bool worker_started = false;
    bool worker_ok = false;
    bool worker_timeout = false;
    /// 回调里发生 C++ 异常（mutex/CV 调用理论上可抛）：记录后立刻返回，
    /// 绝不把异常抛过 C ABI；外层在 join 之后 REQUIRE 它为空。
    bool callback_error = false;
    std::string worker_error;
    std::thread worker;

    static int on_trace(unsigned mask, void* context, void* stmt_ptr, void* expanded)
    {
        if ((mask & SQLITE_TRACE_STMT) == 0 || context == nullptr)
            return 0;
        auto* self = static_cast<U04B1SourceRace*>(context);
        try
        {
            // 语句原文：sqlite3_sql 给原始 SQL；sqlite3_exec 的展开文本作为兜底
            const char* text =
                stmt_ptr ? sqlite3_sql(static_cast<sqlite3_stmt*>(stmt_ptr)) : nullptr;
            if (text == nullptr)
                text = static_cast<const char*>(expanded);
            if (text == nullptr)
                return 0;
            std::unique_lock lock(self->mutex);
            if (!self->armed)
                return 0;
            if (std::string_view(text).find("BEGIN IMMEDIATE") == std::string_view::npos)
                return 0;
            self->armed = false;
            self->begin_seen = true;
            self->cv.notify_all();
            if (!self->cv.wait_for(lock, std::chrono::seconds(3),
                                   [self] { return self->worker_done || self->released; }))
                self->handshake_timeout = true;   // 只记录：外层统一判失败
        }
        catch (...)
        {
            // 回调在 C ABI 边界内：只记录，不传播。读方在 join 之后才看这个标志。
            self->callback_error = true;
        }
        return 0;
    }

    U04B1SourceRace(storage::Database& database, const fs::path& path, std::string sql)
        : db(database), db_path(path), worker_sql(std::move(sql))
    {
        {
            auto session = db.session();   // 只借连接装 trace，随即释放（不跨线程用）
            main_handle = session.handle();
            REQUIRE(sqlite3_trace_v2(main_handle, SQLITE_TRACE_STMT, &U04B1SourceRace::on_trace,
                                     this) == SQLITE_OK);
        }
        try
        {
            worker = std::thread([this] { run_worker(); });
        }
        catch (...)
        {
            // 线程没建起来也不能留着带 this 的 trace context
            sqlite3_trace_v2(main_handle, 0, nullptr, nullptr);
            main_handle = nullptr;
            throw;
        }
    }

    /// 显式完成本轮握手：放行等待中的 worker 并 join。只能在 service 返回之后调用 ——
    /// 正常路径此时 worker 早已 done（来源提交发生在真正的 BEGIN 继续之前）；
    /// service 没进 BEGIN 的错误路径也不会留下还在写状态的 worker。
    /// 返回后所有成员都可安全读取（join 建立 happens-before）。
    void finish_join()
    {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }

    ~U04B1SourceRace()
    {
        if (main_handle != nullptr)
            sqlite3_trace_v2(main_handle, 0, nullptr, nullptr);   // 先摘 trace
        finish_join();                                            // 断言失败也放行/回收
    }

    U04B1SourceRace(const U04B1SourceRace&) = delete;
    U04B1SourceRace& operator=(const U04B1SourceRace&) = delete;

    void run_worker()
    {
        {
            std::unique_lock lock(mutex);
            worker_started = true;
            if (!cv.wait_for(lock, std::chrono::seconds(3),
                             [this] { return begin_seen || released; }))
            {
                worker_timeout = true;
                return;
            }
            if (!begin_seen)
                return;   // 析构先放行：不再写
        }
        // 独立连接与错误缓冲都用轻量 RAII：任何分支（含提前 return）都释放，
        // 且都在结束前记录 done/error，外层 join 后读到的状态是完整的。
        bool ok = false;
        std::string error;
        {
            sqlite3* conn = nullptr;
            std::unique_ptr<sqlite3, decltype(&sqlite3_close)> conn_guard(nullptr, &sqlite3_close);
            if (sqlite3_open_v2(db_path.string().c_str(), &conn, SQLITE_OPEN_READWRITE, nullptr) !=
                SQLITE_OK)
            {
                error = conn ? sqlite3_errmsg(conn) : std::string("open failed");
                if (conn != nullptr)
                    conn_guard.reset(conn);
            }
            else
            {
                conn_guard.reset(conn);
                sqlite3_busy_timeout(conn, 1500);   // 独立连接、有限等待，不用 sleep 碰运气
                char* raw_error = nullptr;
                std::unique_ptr<char, decltype(&sqlite3_free)> message(nullptr, &sqlite3_free);
                const int rc = sqlite3_exec(conn, worker_sql.c_str(), nullptr, nullptr, &raw_error);
                message.reset(raw_error);
                ok = rc == SQLITE_OK;
                if (!ok)
                    error = raw_error ? std::string(raw_error) : sqlite3_errmsg(conn);
            }
        }
        {
            std::lock_guard lock(mutex);
            worker_ok = ok;
            worker_error = std::move(error);
            worker_done = true;
        }
        cv.notify_all();
    }
};

// ---------------- B01/B03/B04 共用的真实样本（复制 A 已实测值，不自选 pattern） ----------------

/// A 已实测的 evaluation_limit 组合：低优先级先真命中，高优先级重模式随后超限。
constexpr const char* kU04B1LimitPattern = "(*NO_AUTO_POSSESS)(*NO_START_OPT)^(a+)+$";
/// A 已实测的 PCRE2 RECURSELOOP 样本（pcre2test 实跑 -52，编译成功）。
constexpr const char* kU04B1ErrorPattern =
    "(*NO_START_OPT)(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|b))(?&A)";
/// 超限样本输入：30 个 'a' 加一个 '!'（^a+!$ 命中，同时触发重模式回溯预算）。
inline std::string u04b1_limit_subject() { return std::string(30, 'a') + "!"; }

}  // namespace

// ------------------------------------------------------------
// 组 01：分类类型决策独立于推荐库 —— 旧引擎没有推荐结果也必须写 custom
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 classification uses the matching config even without a recommended library")
{
    u04b1_register_types({"custom:selector"});
    u04b1_store_config(matching_store, matching_config,
                       nlohmann::json::array({u04a_rule("b1-special", "name", "^SPECIAL_",
                                                        "custom:selector")}));

    U04B1Library lib(*db, matching_store, temp.path(), "Selector");
    // 0 pins、空 footprint、真实 Reference=U、原 Unknown：2 pins 会命中旧的 Small IC 规则
    const std::string symbol_id =
        lib.insert(u04b1_part("SPECIAL_PART", "U", std::string("U"), "", "", "Unknown"));

    const core::Symbol before = lib.read(symbol_id);
    REQUIRE(before.component_type_id == "Unknown");
    REQUIRE(before.pin_count() == 0);
    REQUIRE(before.footprint().empty());
    auto reference = before.property("reference");
    REQUIRE(reference.has_value());
    REQUIRE(*reference == "U");

    // 核心前提：真实快照对这个来源给出 custom:selector
    const auto decision =
        matching_store.current()->evaluate(classifier::MatchSample::from_symbol(before));
    if (decision.status != classifier::MatchStatus::Matched)
        MESSAGE("core decision: ", classifier::match_status_name(decision.status), " ",
                decision.reason);
    REQUIRE(decision.status == classifier::MatchStatus::Matched);
    REQUIRE(decision.type_id == "custom:selector");

    // 对照证据：旧的推荐库引擎对这个输入没有结论（0 pins 不会被偷偷当成 2 pins）
    classifier::RuleEngine legacy(classifier::RuleLoader::default_rules());
    const auto legacy_results = legacy.evaluate_symbol(before);
    MESSAGE("legacy recommendations: ", legacy_results.size());
    REQUIRE(legacy_results.empty());   // 前提：旧推荐库对这个输入没有结论

    services::ClassificationService service(db.get(), &matching_store);

    // 只读对照：单符号分类不得暗中写库
    auto single = service.classify_symbol(symbol_id);
    CHECK(single.has_value());
    CHECK(fix02_stored_type(*db, symbol_id) == "Unknown");

    auto summary = service.classify_all();
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());
    REQUIRE(summary.has_value());
    MESSAGE("total=", summary->total, " matched=", summary->matched,
            " type_updated=", summary->type_updated);
    CHECK(summary->type_updated == 1);   // 红：旧逻辑 results.empty() 直接 continue
    CHECK(fix02_stored_type(*db, symbol_id) == "custom:selector");
}

// ------------------------------------------------------------
// 组 03：清空规则阻止分类隐藏默认映射
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 an empty matching config stops the hidden reference mapping")
{
    u04b1_store_config(matching_store, matching_config, nlohmann::json::array());

    U04B1Library lib(*db, matching_store, temp.path(), "EmptyRules");
    // 三种未分类形态：真实 Reference R?、Reference 缺失、显式空 Reference
    const std::string with_reference =
        lib.insert(u04b1_part("U04B1EmptyRefR", "R", std::string("R?"), "", "", "Unknown"));
    const std::string missing_reference =
        lib.insert(u04b1_part("U04B1NoReference", "R", std::nullopt, "", "", "Unknown"));
    const std::string empty_reference =
        lib.insert(u04b1_part("U04B1BlankReference", "R", std::string(), "", "", "Unknown"));

    const core::Symbol sample = lib.read(with_reference);
    REQUIRE(sample.property("reference").has_value());
    REQUIRE(*sample.property("reference") == "R?");
    const core::Symbol missing_row = lib.read(missing_reference);
    REQUIRE_FALSE(missing_row.property("reference").has_value());       // 缺失 ≠ 空串
    const core::Symbol empty_row = lib.read(empty_reference);
    REQUIRE(empty_row.property("reference").has_value());
    REQUIRE(empty_row.property("reference")->empty());                  // 显式空串

    // 核心前提：空规则下三种输入一律 Unmatched（不是 Unknown，也不是 Resistor）
    for (const core::Symbol* row : {&sample, &missing_row, &empty_row})
    {
        const auto decision =
            matching_store.current()->evaluate(classifier::MatchSample::from_symbol(*row));
        REQUIRE(decision.status == classifier::MatchStatus::Unmatched);
        REQUIRE(decision.type_id.empty());
    }

    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());
    REQUIRE(summary.has_value());
    CHECK(summary->type_updated == 0);                          // 红：旧默认位号推断写 Resistor
    CHECK(fix02_stored_type(*db, with_reference) == "Unknown");   // 红
    CHECK(fix02_stored_type(*db, missing_reference) == "Unknown");
    CHECK(fix02_stored_type(*db, empty_reference) == "Unknown");
}

// ------------------------------------------------------------
// 组 04：普通导入基于真实 Value/MPN 与精确特例
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 ordinary import matches the real Value and MPN fields")
{
    u04b1_register_types({"custom:selector", "custom:exact"});
    u04b1_store_config(
        matching_store, matching_config,
        nlohmann::json::array({u04a_rule("b1-value", "value", "^SEL-", "custom:selector", 10),
                               u04a_rule("b1-mpn", "model", "^MPN-SEL-", "custom:selector", 5)}),
        nlohmann::json::array({u04a_exception("b1-exact", "value", "SEL-EXACT", "custom:exact")}));

    U04B1Library lib(*db, matching_store, temp.path(), "ImportFields");
    const fs::path source_file = lib.target;   // 写进已登记的单库文件，目录里只有一个 .kicad_sym
    write(source_file, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1Fields" (property "Reference" "U") (property "Value" "SEL-1") (property "MPN" "MPN-SEL-1"))))KICAD");

    // 真实 parser 前提：Value/MPN 进入 default_value/mpn
    auto parsed = parser::SymbolLibParser::parse(source_file);
    if (!parsed)
        MESSAGE("parse failed: ", parsed.error().format_message());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 1);
    REQUIRE(parsed->items.front().default_value() == "SEL-1");
    REQUIRE(parsed->items.front().mpn() == "MPN-SEL-1");

    // 核心前提：这个真实来源命中 custom:selector
    const auto decision = matching_store.current()->evaluate(
        classifier::MatchSample::from_symbol(parsed->items.front()));
    REQUIRE(decision.status == classifier::MatchStatus::Matched);
    REQUIRE(decision.type_id == "custom:selector");

    services::ImportPipeline first(*db, nullptr, false, &matching_store);
    auto imported = first | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
    MESSAGE("first import: symbols=", imported.symbols, " skipped=", imported.skipped);
    REQUIRE(imported.symbols == 1);
    const std::string symbol_id = lib.find_id("U04B1Fields");
    CHECK(fix02_stored_type(*db, symbol_id) == "custom:selector");   // 红：当前新行 Unknown

    // 已存在 Unknown 行：源更新到命中值后必须补齐（UUID 保留、字段真更新、mtime 推进 2 秒）
    U04B1Library update_lib(*db, matching_store, temp.path(), "ImportUpdate");
    const fs::path update_file = update_lib.target;
    write(update_file, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1Update" (property "Reference" "U") (property "Value" "v1") (property "MPN" "model1"))))KICAD");
    services::ImportPipeline first_pass(*db, nullptr, false, &matching_store);
    auto created = first_pass | services::symbols_from{update_lib.symbol_dir.string(), ""} |
                   services::execute;
    MESSAGE("update first pass: symbols=", created.symbols, " skipped=", created.skipped);
    REQUIRE(created.symbols == 1);
    const core::Symbol before = update_lib.read(update_lib.find_id("U04B1Update"));
    REQUIRE(before.component_type_id == "Unknown");
    REQUIRE(before.default_value() == "v1");
    REQUIRE(before.mpn() == "model1");

    write(update_file, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1Update" (property "Reference" "U") (property "Value" "SEL-2") (property "MPN" "MPN-SEL-2"))))KICAD");
    fs::last_write_time(update_file,
                        fs::last_write_time(update_file) + std::chrono::seconds(2));

    services::ImportPipeline second_pass(*db, nullptr, false, &matching_store);
    auto updated = second_pass | services::symbols_from{update_lib.symbol_dir.string(), ""} |
                   services::execute;
    MESSAGE("update second pass: symbols=", updated.symbols, " skipped=", updated.skipped);
    REQUIRE(updated.symbols == 1);   // 没有被 mtime 快照跳过
    const core::Symbol after = update_lib.read(before.id());
    CHECK(after.id() == before.id());           // UUID 保留
    REQUIRE(after.default_value() == "SEL-2");  // 源字段确实更新
    REQUIRE(after.mpn() == "MPN-SEL-2");
    CHECK(after.component_type_id == "custom:selector");   // 红：当前保留 Unknown

    // 精确特例：原文相等优先于低层正则。大小写/空格不等时特例不命中，
    // 之后是否落正则由 ^SEL- 的实际语义决定（不 trim、不折叠大小写）。
    MatchSample exact;
    exact.value = "SEL-EXACT";
    const auto exact_decision = matching_store.current()->evaluate(exact);
    REQUIRE(exact_decision.status == classifier::MatchStatus::Matched);
    REQUIRE(exact_decision.type_id == "custom:exact");
    REQUIRE(exact_decision.evidence.front().source == classifier::MatchEvidenceSource::Exception);

    // 小写原文：特例区分大小写（不是特例），默认 icase 的 ^SEL- 正则可命中
    MatchSample case_miss;
    case_miss.value = "sel-exact";
    const auto case_decision = matching_store.current()->evaluate(case_miss);
    REQUIRE(case_decision.status == classifier::MatchStatus::Matched);
    REQUIRE(case_decision.type_id == "custom:selector");   // 落到正则，不是特例
    REQUIRE(case_decision.evidence.front().source == classifier::MatchEvidenceSource::Rule);

    // 尾随空格：特例原文不等；^SEL- 不锚定结尾，仍落正则
    MatchSample trailing_miss;
    trailing_miss.value = "SEL-EXACT ";
    const auto trailing_decision = matching_store.current()->evaluate(trailing_miss);
    REQUIRE(trailing_decision.status == classifier::MatchStatus::Matched);
    REQUIRE(trailing_decision.type_id == "custom:selector");
    REQUIRE(trailing_decision.evidence.front().source == classifier::MatchEvidenceSource::Rule);

    // 前导空格：^SEL- 的 ^ 锚点不命中，整体 Unmatched、type/evidence 为空
    MatchSample leading_miss;
    leading_miss.value = " SEL-EXACT";
    const auto leading_decision = matching_store.current()->evaluate(leading_miss);
    if (leading_decision.status != classifier::MatchStatus::Unmatched)
        MESSAGE("leading-space sample: ", classifier::match_status_name(leading_decision.status),
                " ", leading_decision.reason);
    REQUIRE(leading_decision.status == classifier::MatchStatus::Unmatched);
    REQUIRE(leading_decision.type_id.empty());
    REQUIRE(leading_decision.evidence.empty());
}

// ------------------------------------------------------------
// 组 05：普通导入在清空规则下不改写类型（含已有 Unknown 行的源更新）
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 ordinary import keeps types unwritten under an empty config")
{
    u04b1_store_config(matching_store, matching_config, nlohmann::json::array());

    U04B1Library lib(*db, matching_store, temp.path(), "EmptyImport");
    const fs::path source_file = lib.target;   // 单库文件，目录里只有一个 .kicad_sym
    write(source_file, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1EmptyImport" (property "Reference" "R?") (property "Value" "v1"))))KICAD");

    services::ImportPipeline first(*db, nullptr, false, &matching_store);
    auto created = first | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
    REQUIRE(created.symbols == 1);
    const core::Symbol before = lib.read(lib.find_id("U04B1EmptyImport"));
    CHECK(before.component_type_id == "Unknown");   // 红：当前按前缀 R 落 Resistor

    // 源更新（Value 与真实 Reference 同时变）+ mtime 推进 2 秒
    write(source_file, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1EmptyImport" (property "Reference" "U") (property "Value" "v2"))))KICAD");
    fs::last_write_time(source_file,
                        fs::last_write_time(source_file) + std::chrono::seconds(2));

    services::ImportPipeline second(*db, nullptr, false, &matching_store);
    auto updated = second | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
    REQUIRE(updated.symbols == 1);
    const core::Symbol after = lib.read(before.id());
    CHECK(after.id() == before.id());
    REQUIRE(after.default_value() == "v2");
    REQUIRE(after.property("reference").has_value());
    REQUIRE(*after.property("reference") == "U");
    CHECK(after.component_type_id == "Unknown");    // 红：空配置下不许走私有默认映射

    // 核心全程 Unmatched（不是 Unknown 结论，也不是命中）
    const auto decision =
        matching_store.current()->evaluate(classifier::MatchSample::from_symbol(after));
    REQUIRE(decision.status == classifier::MatchStatus::Unmatched);
}

// ------------------------------------------------------------
// 组 06：已知类型身份在配置给出其他候选时仍原样保留（绿对照 + 配置前提）
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 known type identities survive a different config candidate")
{
    u04b1_register_types({"custom:selector"});
    u04b1_store_config(matching_store, matching_config,
                       nlohmann::json::array({u04a_rule("b1-known", "reference", "^R[0-9?]*$",
                                                        "Resistor")}));

    U04B1Library lib(*db, matching_store, temp.path(), "KnownTypes");
    const std::string builtin = lib.insert(u04b1_part("U04B1KnownOpAmp", "R", std::string("R?"), "", "", "OpAmp"));
    const std::string custom = lib.insert(u04b1_part("U04B1KnownCustom", "R", std::string("R?"), "", "", "custom:selector"));
    const std::string raw = lib.insert(u04b1_part("U04B1KnownRaw", "R", std::string("R?"), "", "", "LegacyRawId"));
    const std::string unknown = lib.insert(u04b1_part("U04B1KnownUnknown", "R", std::string("R?"), "", "", "Unknown"));

    // 配置前提：同一来源的核心结论确实是另一个类型（Resistor）
    const auto decision = matching_store.current()->evaluate(
        classifier::MatchSample::from_symbol(lib.read(unknown)));
    REQUIRE(decision.status == classifier::MatchStatus::Matched);
    REQUIRE(decision.type_id == "Resistor");

    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    REQUIRE(summary.has_value());
    CHECK(fix02_stored_type(*db, builtin) == "OpAmp");            // 绿：已知内置保护
    CHECK(fix02_stored_type(*db, custom) == "custom:selector");   // 绿：已登记 custom 保护
    CHECK(fix02_stored_type(*db, raw) == "LegacyRawId");          // 绿：未注册历史 raw ID 保护
    CHECK(fix02_stored_type(*db, unknown) == "Resistor");         // 绿对照：空/Unknown 才可补
}

// ------------------------------------------------------------
// 组 07：坏配置阻止分类，reload 后恢复；未 load 的显式 store 也拒绝业务
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a bad matching config blocks classification until reload")
{
    U04B1Library lib(*db, matching_store, temp.path(), "BadConfig");
    const std::string symbol_id =
        lib.insert(u04b1_part("U04B1BadConfig", "R", std::string("R?"), "", "", "Unknown"));

    // 先有效 load（fixture 已装默认种子），再写坏内容 load：last-valid 保留、业务拒绝
    write(matching_config, "{ this is not a matching config");
    auto bad = matching_store.load(matching_config.string());
    REQUIRE_FALSE(bad.has_value());                                         // 前提：坏内容确实被拒
    REQUIRE_FALSE(matching_store.load_error().empty());
    REQUIRE(matching_store.current() != nullptr);                           // 展示口仍是旧快照
    REQUIRE_FALSE(matching_store.acquire_business_snapshot().has_value());  // 业务领取必须失败

    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (summary)
        MESSAGE("bad-config classify_all unexpectedly succeeded: total=", summary->total,
                " type_updated=", summary->type_updated);
    CHECK_FALSE(summary.has_value());                                  // 红：旧服务忽略 matcher
    CHECK(fix02_stored_type(*db, symbol_id) == "Unknown");             // 红：DB 原样
    auto single = service.classify_symbol(symbol_id);
    CHECK_FALSE(single.has_value());                                   // 红：单符号同样必须失败

    // 未 load 的显式局部 store：不能用 singleton、也不能用 last-valid 冒充健康
    classifier::TypeMatchingStore fresh;
    services::ClassificationService unloaded(db.get(), &fresh);
    CHECK_FALSE(unloaded.classify_all().has_value());
    CHECK(fix02_stored_type(*db, symbol_id) == "Unknown");

    // 修复文件并 reload 后，同一个 service 恢复正常分类
    write(matching_config, classifier::default_matching_config_json());
    REQUIRE(matching_store.load(matching_config.string()).has_value());
    REQUIRE(matching_store.acquire_business_snapshot().has_value());
    auto fixed = service.classify_all();
    REQUIRE(fixed.has_value());
    CHECK(fix02_stored_type(*db, symbol_id) == "Resistor");   // 红：修复后必须真能补类型
}

// ------------------------------------------------------------
// 组 08：坏配置阻止普通导入、桥接与同源重试
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a bad matching config blocks ordinary import and allows retry after reload")
{
    U04B1Library lib(*db, matching_store, temp.path(), "BadImport");
    const fs::path source_file = lib.target;   // 单库文件：目录里只有一个 .kicad_sym
    write(source_file, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1BadImport" (property "Reference" "R?") (property "Value" "b1"))))KICAD");
    const std::string source_bytes = read(source_file);

    write(matching_config, "{ broken");
    REQUIRE_FALSE(matching_store.load(matching_config.string()).has_value());
    REQUIRE_FALSE(matching_store.acquire_business_snapshot().has_value());

    // 坏配置下的每个入口独立 SUBCASE：各自全新的目录/DB 前提，
    // 不会出现「首个入口已消费 mtime，后两个入口只是被跳过」的假独立证据。
    const int libraries_before = count_rows(*db, "libraries");
    const int snapshots_before = u04b1_imported_files(*db);

    SUBCASE("symbols_from entry")
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto blocked = pipe | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
        MESSAGE("blocked import: symbols=", blocked.symbols, " skipped=", blocked.skipped);
        CHECK(blocked.symbols == 0);                         // 红：旧管道照样导入
        CHECK(u04b1_count_symbols(*db, "name = 'U04B1BadImport'") == 0);
        CHECK(u04b1_imported_files(*db) == snapshots_before);   // 源快照未被消费
        CHECK(count_rows(*db, "libraries") == libraries_before); // 不新增库行
        CHECK(read(source_file) == source_bytes);               // 源文件未被改写
    }

    SUBCASE("footprints_from entry")
    {
        // 该目录确实含 .kicad_sym：footprints_from 的路径同样必须被健康门挡住
        services::ImportPipeline fp_pipe(*db, nullptr, false, &matching_store);
        auto fp_blocked =
            fp_pipe | services::footprints_from{lib.symbol_dir.string()} | services::execute;
        MESSAGE("blocked footprint phase: symbols=", fp_blocked.symbols);
        CHECK(u04b1_count_symbols(*db, "name = 'U04B1BadImport'") == 0);
        CHECK(u04b1_imported_files(*db) == snapshots_before);
        CHECK(count_rows(*db, "libraries") == libraries_before);
        CHECK(read(source_file) == source_bytes);
    }

    SUBCASE("import manager entry")
    {
        // 经最外层 ImportManager（显式注入同一 store）
        api::ImportManager manager(*db, nullptr, &matching_store);
        auto managed = manager.import_directory(lib.symbol_dir.string());
        MESSAGE("blocked manager import: symbols=", managed.symbols);
        CHECK(managed.symbols == 0);
        CHECK(u04b1_count_symbols(*db, "name = 'U04B1BadImport'") == 0);
        CHECK(u04b1_imported_files(*db) == snapshots_before);
        CHECK(count_rows(*db, "libraries") == libraries_before);
        CHECK(read(source_file) == source_bytes);
    }

    SUBCASE("library service bridge")
    {
        // util::Result 桥：真实符号目录（不用「单文件当目录」那条已知不支持路径）
        services::LibraryService bridge(db.get(), &matching_store);
        auto bridged = bridge.import_directory(lib.symbol_dir);
        if (bridged)
            MESSAGE("blocked bridge import unexpectedly succeeded: symbols=", bridged->symbols);
        CHECK_FALSE(bridged.has_value());                    // 红：旧桥忽略 matcher 照样成功
        CHECK(u04b1_count_symbols(*db, "name = 'U04B1BadImport'") == 0);
        CHECK(u04b1_imported_files(*db) == snapshots_before);
        CHECK(count_rows(*db, "libraries") == libraries_before);
        CHECK(read(source_file) == source_bytes);
    }

    // 修复配置 reload：完全相同的源、不改 mtime 也要能重试成功
    write(matching_config, classifier::default_matching_config_json());
    REQUIRE(matching_store.load(matching_config.string()).has_value());
    services::ImportPipeline retry(*db, nullptr, false, &matching_store);
    auto retried = retry | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
    MESSAGE("retry: symbols=", retried.symbols, " skipped=", retried.skipped);
    CHECK(retried.symbols == 1);   // 红：旧管道第一次已导入并记录 mtime，重试只会 skip
    CHECK(u04b1_count_symbols(*db, "name = 'U04B1BadImport'") == 1);
}

// ------------------------------------------------------------
// 组 10：正式来源竞态 —— worker 改了任一匹配输入，旧决策不得写回
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a concurrent source change is not overwritten by a stale decision")
{
    // 每个 SUBCASE 走一遍完整用例体：来源计划 + 第二连接要执行的 UPDATE（不含 WHERE）
    std::string plan_name = "U04B1RaceRef";
    std::string plan_prefix = "R";
    std::optional<std::string> plan_reference = std::string("R?");
    std::string plan_sql;
    std::string plan_expect_field;
    std::string plan_expect_value;

    SUBCASE("reference")
    {
        // 只改真实 Reference 属性：缓存前缀列保持 R，防「只比较旧 prefix 列」的假修复
        plan_sql = "UPDATE symbols SET properties_json = json_set(properties_json, '$.reference', 'U')";
        plan_expect_field = "reference";
        plan_expect_value = "U";
    }
    SUBCASE("name")
    {
        plan_sql = "UPDATE symbols SET name = 'U04B1RaceRenamed'";
        plan_expect_field = "name";
        plan_expect_value = "U04B1RaceRenamed";
    }
    SUBCASE("value")
    {
        plan_sql = "UPDATE symbols SET default_value = 'v2'";
        plan_expect_field = "value";
        plan_expect_value = "v2";
    }
    SUBCASE("model")
    {
        plan_sql = "UPDATE symbols SET mpn = 'm2'";
        plan_expect_field = "model";
        plan_expect_value = "m2";
    }
    SUBCASE("missing reference becomes explicit empty")
    {
        plan_name = "U04B1RaceMissing";
        plan_prefix = "R";
        plan_reference = std::nullopt;
        plan_sql = "UPDATE symbols SET properties_json = json_set(properties_json, '$.reference', '')";
        plan_expect_field = "reference";
        plan_expect_value = "";
        // 初始来源靠 name 命中 Resistor（旧的位号推断仍能进入写回：prefix R）
        u04b1_store_config(matching_store, matching_config,
                           nlohmann::json::array({u04a_rule("b1-race-name", "name",
                                                            "^U04B1RaceMissing$", "Resistor")}));
    }

    U04B1Library lib(*db, matching_store, temp.path(), "RaceSource");
    const std::string symbol_id =
        lib.insert(u04b1_part(plan_name, plan_prefix, plan_reference, "v1", "m1", "Unknown"));

    // 输入前提：原类型 Unknown、Reference 缺失/存在与计划一致
    const core::Symbol before = lib.read(symbol_id);
    REQUIRE(before.component_type_id == "Unknown");
    REQUIRE(before.property("reference").has_value() == plan_reference.has_value());

    // 核心前提：原来源在真实快照上命中 Resistor（只作验证，不参与产品写回）
    const auto decision =
        matching_store.current()->evaluate(classifier::MatchSample::from_symbol(before));
    if (decision.status != classifier::MatchStatus::Matched)
        MESSAGE("core decision: ", classifier::match_status_name(decision.status), " ",
                decision.reason);
    REQUIRE(decision.status == classifier::MatchStatus::Matched);
    REQUIRE(decision.type_id == "Resistor");

    U04B1SourceRace race(*db, db->path(), plan_sql + " WHERE id = '" + symbol_id + "'");
    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());

    // service 返回后先显式完成握手（放行 + join），再读任何跨线程状态：
    // 正常路径 worker 早已提交完；没进 BEGIN 的错误路径也不会与 worker 竞态。
    race.finish_join();
    REQUIRE_FALSE(race.callback_error);   // 回调异常只记录，不跨 C ABI

    // 夹具时序必须真的发生：否则测试失败，不当业务通过
    REQUIRE(race.worker_started);
    REQUIRE(race.begin_seen);
    REQUIRE_FALSE(race.worker_timeout);
    REQUIRE_FALSE(race.handshake_timeout);
    if (!race.worker_ok)
        MESSAGE("second connection failed: ", race.worker_error);
    REQUIRE(race.worker_ok);

    const core::Symbol after = lib.read(symbol_id);
    // 来源确实变了、类型仍 Unknown
    if (plan_expect_field == "reference")
    {
        REQUIRE(after.property("reference").has_value());
        if (plan_reference.has_value())
            CHECK(*after.property("reference") == plan_expect_value);
        else
            CHECK(after.property("reference")->empty());   // missing -> 显式空串
        CHECK(after.name() == plan_name);                  // 其他三个匹配输入未变
        CHECK(after.default_value() == "v1");
        CHECK(after.mpn() == "m1");
        CHECK(after.reference_prefix() == "R");            // 缓存前缀列没有被改动
    }
    else if (plan_expect_field == "name")
        CHECK(after.name() == plan_expect_value);
    else if (plan_expect_field == "value")
        CHECK(after.default_value() == plan_expect_value);
    else
        CHECK(after.mpn() == plan_expect_value);
    CHECK(after.component_type_id == "Unknown");       // 红：当前写 Resistor

    REQUIRE(summary.has_value());
    CHECK(summary->type_updated == 0);                 // 红：当前 1
}

// ------------------------------------------------------------
// 组 12：真 SQL NULL 的匹配、未匹配与 CAS 保护
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 real SQL NULL types are matched, preserved and CAS-protected")
{
    U04B1Library lib(*db, matching_store, temp.path(), "NullType");
    const std::string fillable =
        lib.insert(u04b1_part("U04B1NullFill", "R", std::string("R?"), "", "", "Unknown"));
    const std::string staying =
        lib.insert(u04b1_part("U04B1NullStay", "R", std::string("R?"), "", "", "Unknown"));

    // 真 NULL（不是空串）并先用 sqlite3_column_type 证明
    u04b1_raw_exec(*db, "UPDATE symbols SET component_type = NULL WHERE id IN ('" + fillable +
                            "', '" + staying + "')");
    REQUIRE(u04b1_raw_column(*db, fillable, "component_type").is_null);
    REQUIRE(u04b1_raw_column(*db, staying, "component_type").is_null);
    {
        const core::Symbol row = lib.read(fillable);
        REQUIRE(row.component_type_id.empty());   // repo 读回 empty，不规范化成 Unknown
        REQUIRE(row.property("reference").has_value());
        REQUIRE(*row.property("reference") == "R?");
    }

    services::ClassificationService service(db.get(), &matching_store);
    // 默认配置：真实 Reference R? 把真 NULL 补成 Resistor
    auto filled = service.classify_all();
    if (!filled)
        MESSAGE("classify_all failed: ", filled.error().format_message());
    REQUIRE(filled.has_value());
    CHECK(filled->type_updated == 2);                    // 两行真 NULL 都按真实 Reference 补齐
    CHECK(fix02_stored_type(*db, fillable) == "Resistor");
    CHECK(fix02_stored_type(*db, staying) == "Resistor");

    // 空配置：另一行真 NULL 保持 NULL，不被规范化成 Unknown/Resistor
    REQUIRE(db->execute("UPDATE symbols SET component_type = NULL WHERE id = '" + staying + "'")
                .has_value());
    REQUIRE(u04b1_raw_column(*db, staying, "component_type").is_null);
    u04b1_store_config(matching_store, matching_config, nlohmann::json::array());
    const core::Symbol staying_before = lib.read(staying);
    const auto decision =
        matching_store.current()->evaluate(classifier::MatchSample::from_symbol(staying_before));
    REQUIRE(decision.status == classifier::MatchStatus::Unmatched);

    auto unmatched = service.classify_all();
    REQUIRE(unmatched.has_value());
    CHECK(unmatched->type_updated == 0);                       // 红：旧默认映射写 Resistor
    CHECK(u04b1_raw_column(*db, staying, "component_type").is_null);   // 红：仍是真 NULL

    // CAS 原语：另取一行真 NULL（不被上面的批量分类触碰），
    // NULL 快照（expected=""）之后被改成已知类型 → 拒绝，新元数据保留
    const std::string cas_row =
        lib.insert(u04b1_part("U04B1NullCas", "R", std::string("R?"), "", "", "Unknown"));
    u04b1_raw_exec(*db, "UPDATE symbols SET component_type = NULL WHERE id = '" + cas_row + "'");
    REQUIRE(u04b1_raw_column(*db, cas_row, "component_type").is_null);
    const std::string expected = lib.read(cas_row).component_type_id;
    REQUIRE(expected.empty());
    u04b1_raw_exec(*db, "UPDATE symbols SET component_type = 'OpAmp', mpn = 'changed' WHERE id = '" +
                            cas_row + "'");
    {
        auto session = db->session();
        auto changed = storage::SymbolRepository(session).update_component_type_if_current(
            cas_row, expected, "Resistor");
        REQUIRE(changed.has_value());
        CHECK_FALSE(*changed);                                  // 期望值不匹配 -> 不覆盖
    }
    CHECK(fix02_stored_type(*db, cas_row) == "OpAmp");
    CHECK(lib.read(cas_row).mpn() == "changed");

    // 字面 Unknown 快照同样保护（期望值不能和 NULL/empty 混用）
    u04b1_raw_exec(*db,
                   "UPDATE symbols SET component_type = 'Unknown' WHERE id = '" + fillable + "'");
    const std::string unknown_expected = lib.read(fillable).component_type_id;
    REQUIRE(unknown_expected == "Unknown");
    u04b1_raw_exec(*db,
                   "UPDATE symbols SET component_type = 'custom:local' WHERE id = '" + fillable +
                       "'");
    {
        auto session = db->session();
        auto changed = storage::SymbolRepository(session).update_component_type_if_current(
            fillable, unknown_expected, "Capacitor");
        REQUIRE(changed.has_value());
        CHECK_FALSE(*changed);
    }
    CHECK(fix02_stored_type(*db, fillable) == "custom:local");
}

// ------------------------------------------------------------
// 组 13：真 NULL 的同批回滚
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a failed batch rolls a real NULL type back to NULL")
{
    U04B1Library lib(*db, matching_store, temp.path(), "NullRollback");
    // 名字排序（find_all ORDER BY name）让 NULL 行先处理，第二行触发失败
    const std::string first =
        lib.insert(u04b1_part("U04B1NullA", "R", std::string("R?"), "keep-value", "keep-mpn", "Unknown"));
    const std::string second =
        lib.insert(u04b1_part("U04B1NullB", "R", std::string("R?"), "", "", "Unknown"));

    u04b1_raw_exec(*db, "UPDATE symbols SET component_type = NULL WHERE id = '" + first + "'");
    REQUIRE(u04b1_raw_column(*db, first, "component_type").is_null);
    u04b1_raw_exec(*db,
                   "CREATE TRIGGER u04b1_null_block BEFORE UPDATE ON symbols WHEN NEW.id = '" +
                       second + "' BEGIN SELECT RAISE(ABORT, 'u04b1 blocked'); END");

    services::ClassificationService service(db.get(), &matching_store);
    auto failed = service.classify_all();
    if (failed)
        MESSAGE("classify_all unexpectedly succeeded: type_updated=", failed->type_updated);
    REQUIRE_FALSE(failed.has_value());
    const std::string message = failed.error().format_message();
    MESSAGE("batch error: ", message);
    CHECK(message.find("0 committed") != std::string::npos);   // 此前已提交 0
    CHECK(u04b1_raw_column(*db, first, "component_type").is_null);   // 回到真 NULL，不是 empty/Unknown
    const core::Symbol restored = lib.read(first);
    CHECK(restored.default_value() == "keep-value");                 // 其他字段保留
    CHECK(restored.mpn() == "keep-mpn");
    REQUIRE(restored.property("reference").has_value());
    CHECK(*restored.property("reference") == "R?");

    // 移除触发器后重试：同一生产方法把两行都更新
    u04b1_raw_exec(*db, "DROP TRIGGER u04b1_null_block");
    auto retry = service.classify_all();
    if (!retry)
        MESSAGE("retry failed: ", retry.error().format_message());
    REQUIRE(retry.has_value());
    CHECK(retry->type_updated == 2);
    CHECK(fix02_stored_type(*db, first) == "Resistor");
    CHECK(fix02_stored_type(*db, second) == "Resistor");
}

// ------------------------------------------------------------
// 组 14：500 条边界（部分提交 + 读取失败）
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 the 501st row fails after 500 committed and read failures are reported")
{
    U04B1Library lib(*db, matching_store, temp.path(), "BatchBoundary");
    std::vector<std::string> ids;
    ids.reserve(501);
    for (int index = 1; index <= 500; ++index)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "U04B1Batch%03d", index);
        ids.push_back(lib.insert(u04b1_part(name, "R", std::string("R?"), "", "", "Unknown")));
    }
    // 排序在最后（ORDER BY name）的第 501 行：触发器让它的 UPDATE 失败
    const std::string last =
        lib.insert(u04b1_part("U04B1BatchLast", "R", std::string("R?"), "", "", "Unknown"));
    ids.push_back(last);
    REQUIRE(ids.size() == 501);
    u04b1_raw_exec(*db,
                   "CREATE TRIGGER u04b1_batch_block BEFORE UPDATE ON symbols WHEN NEW.id = '" +
                       last + "' BEGIN SELECT RAISE(ABORT, 'u04b1 batch blocked'); END");

    services::ClassificationService service(db.get(), &matching_store);
    auto failed = service.classify_all();
    if (failed)
        MESSAGE("classify_all unexpectedly succeeded: type_updated=", failed->type_updated);
    REQUIRE_FALSE(failed.has_value());
    const std::string message = failed.error().format_message();
    MESSAGE("boundary error: ", message);
    CHECK(message.find("500 committed") != std::string::npos);   // 首批已提交计数必须准确
    CHECK(u04b1_count_symbols(*db, "component_type = 'Resistor'") == 500);
    CHECK(fix02_stored_type(*db, last) == "Unknown");            // 失败行原样
    CHECK(fix02_stored_type(*db, ids.front()) == "Resistor");    // 首批确实落库

    // 真实 find_all 读取错误（表不存在 → prepare 失败）：报错而不是空 Summary。
    // 本用例再无其他读取，放在最后一步做。
    u04b1_raw_exec(*db, "DROP TRIGGER u04b1_batch_block");
    u04b1_raw_exec(*db, "DROP TABLE symbols");
    auto unreadable = service.classify_all();
    if (unreadable)
        MESSAGE("read failure was reported as a summary: total=", unreadable->total);
    CHECK_FALSE(unreadable.has_value());   // 读失败必须报错
}

// ------------------------------------------------------------
// B01：四个独立来源字段各自携带精确特例，经真实分类落 DB
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 four independent source fields carry exact exceptions into classification")
{
    u04b1_register_types({"custom:selector", "custom:exact"});

    enum class B1Field
    {
        Name,
        Value,
        Model,
        Reference,
    };
    struct FieldPlan
    {
        B1Field field = B1Field::Name;
        std::string label;    // 配置字段名
        std::string prefix;   // 该字段文本前缀（正则为 ^<prefix>-）
        std::string exact;    // 精确特例 key（原文）
        std::string regex_only;
        std::string case_miss;
        std::string space_miss;
    };
    FieldPlan plan;
    SUBCASE("name field") { plan = {B1Field::Name, "name", "B1NAME"}; }
    SUBCASE("value field") { plan = {B1Field::Value, "value", "B1VALUE"}; }
    SUBCASE("model field") { plan = {B1Field::Model, "model", "B1MODEL"}; }
    SUBCASE("reference field") { plan = {B1Field::Reference, "reference", "B1REF"}; }
    plan.exact = plan.prefix + "-KEY";
    plan.regex_only = plan.prefix + "-REGEX";
    plan.case_miss = plan.prefix + "-key";   // 特例区分大小写 -> 不是特例
    plan.space_miss = " " + plan.exact;      // ^ 锚点 + 不 trim -> 不命中

    // 只登记所选字段的规则与特例：其他字段没有偷偷命中的机会
    // （model 样本尤其不能有 Value 规则更高优先级命中）。
    u04b1_store_config(
        matching_store, matching_config,
        nlohmann::json::array(
            {u04a_rule("b1-field-regex", plan.label, "^" + plan.prefix + "-", "custom:selector")}),
        nlohmann::json::array(
            {u04a_exception("b1-field-exact", plan.label, plan.exact, "custom:exact")}));

    auto sample_of = [&plan](const std::string& text)
    {
        classifier::MatchSample sample;
        switch (plan.field)
        {
            case B1Field::Name: sample.name = text; break;
            case B1Field::Value: sample.value = text; break;
            case B1Field::Model: sample.model = text; break;
            case B1Field::Reference: sample.reference = text; break;
        }
        return sample;
    };
    // 除 reference 样本外实际 Reference=U、0 pins、空 footprint：旧推荐库拿不到结论
    auto part_with = [&plan](const std::string& symbol_name, const std::string& text)
    {
        switch (plan.field)
        {
            case B1Field::Name:
                return u04b1_part(text, "U", std::string("U"), "plain-value", "plain-model");
            case B1Field::Value:
                return u04b1_part(symbol_name, "U", std::string("U"), text, "plain-model");
            case B1Field::Model:
                return u04b1_part(symbol_name, "U", std::string("U"), "plain-value", text);
            case B1Field::Reference:
                return u04b1_part(symbol_name, "U", text, "plain-value", "plain-model");
        }
        return core::Symbol();
    };

    U04B1Library lib(*db, matching_store, temp.path(), "FieldsCore");
    const std::string exact_id = lib.insert(part_with("U04B1B1Exact", plan.exact));
    const std::string regex_id = lib.insert(part_with("U04B1B1Regex", plan.regex_only));

    // 核心前提：精确原文 -> 特例；同字段不同原文 -> 低层正则
    const core::Symbol exact_row = lib.read(exact_id);
    REQUIRE(exact_row.pin_count() == 0);
    REQUIRE(exact_row.footprint().empty());
    const auto exact_decision =
        matching_store.current()->evaluate(classifier::MatchSample::from_symbol(exact_row));
    if (exact_decision.status != classifier::MatchStatus::Matched)
        MESSAGE("exact decision: ", classifier::match_status_name(exact_decision.status), " ",
                exact_decision.reason);
    REQUIRE(exact_decision.status == classifier::MatchStatus::Matched);
    REQUIRE(exact_decision.type_id == "custom:exact");
    REQUIRE_FALSE(exact_decision.evidence.empty());
    REQUIRE(exact_decision.evidence.front().source == classifier::MatchEvidenceSource::Exception);

    const auto regex_decision = matching_store.current()->evaluate(
        classifier::MatchSample::from_symbol(lib.read(regex_id)));
    REQUIRE(regex_decision.status == classifier::MatchStatus::Matched);
    REQUIRE(regex_decision.type_id == "custom:selector");
    REQUIRE_FALSE(regex_decision.evidence.empty());
    REQUIRE(regex_decision.evidence.front().source == classifier::MatchEvidenceSource::Rule);

    // 原文边界（依据该配置 regex 的实际语义，不写「全部仍匹配」的错误预期）
    const auto case_decision = matching_store.current()->evaluate(sample_of(plan.case_miss));
    REQUIRE(case_decision.status == classifier::MatchStatus::Matched);
    REQUIRE(case_decision.type_id == "custom:selector");
    REQUIRE(case_decision.evidence.front().source == classifier::MatchEvidenceSource::Rule);
    const auto space_decision = matching_store.current()->evaluate(sample_of(plan.space_miss));
    if (space_decision.status != classifier::MatchStatus::Unmatched)
        MESSAGE("space-miss decision: ", classifier::match_status_name(space_decision.status), " ",
                space_decision.reason);
    REQUIRE(space_decision.status == classifier::MatchStatus::Unmatched);
    REQUIRE(space_decision.type_id.empty());
    REQUIRE(space_decision.evidence.empty());

    services::ClassificationService service(db.get(), &matching_store);
    // 只读对照：单符号分类不得写库；旧推荐库没有结论时不假设一定有条目
    auto single = service.classify_symbol(exact_id);
    CHECK(single.has_value());
    if (single && !single->empty())
        MESSAGE("single-symbol type: ", single->front().type_id);
    CHECK(fix02_stored_type(*db, exact_id) == "Unknown");

    auto summary = service.classify_all();
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());
    REQUIRE(summary.has_value());   // 拆分复合断言：存在性是前提，计数是业务预期
    MESSAGE("field=", plan.label, " total=", summary->total,
            " type_updated=", summary->type_updated);
    CHECK(summary->type_updated == 2);                            // 红：旧路径无推荐结果就不写
    CHECK(fix02_stored_type(*db, exact_id) == "custom:exact");    // 红：应落精确特例
    CHECK(fix02_stored_type(*db, regex_id) == "custom:selector");
}

// ------------------------------------------------------------
// B02：精确特例与已知类型身份，经普通导入落 DB
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 exact exceptions and known identities survive ordinary import")
{
    u04b1_register_types({"custom:selector", "custom:exact"});

    SUBCASE("new row with an exact Value")
    {
        u04b1_store_config(
            matching_store, matching_config,
            nlohmann::json::array({u04a_rule("b2-value", "value", "^B2VAL-", "custom:selector")}),
            nlohmann::json::array(
                {u04a_exception("b2-value-exact", "value", "B2VAL-KEY", "custom:exact")}));

        U04B1Library lib(*db, matching_store, temp.path(), "ImportExactValue");
        write(lib.target, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1B2Value" (property "Reference" "U") (property "Value" "B2VAL-KEY"))))KICAD");

        auto parsed = parser::SymbolLibParser::parse(lib.target);
        if (!parsed)
            MESSAGE("parse failed: ", parsed.error().format_message());
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->items.size() == 1);
        REQUIRE(parsed->items.front().default_value() == "B2VAL-KEY");
        REQUIRE(parsed->items.front().property("reference").has_value());
        REQUIRE(*parsed->items.front().property("reference") == "U");
        const auto decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(parsed->items.front()));
        REQUIRE(decision.status == classifier::MatchStatus::Matched);
        REQUIRE(decision.type_id == "custom:exact");
        REQUIRE(decision.evidence.front().source == classifier::MatchEvidenceSource::Exception);

        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto imported = pipe | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
        MESSAGE("exact value import: symbols=", imported.symbols, " skipped=", imported.skipped);
        REQUIRE(imported.symbols == 1);   // 单库文件计数
        const std::string symbol_id = lib.find_id("U04B1B2Value");
        CHECK(fix02_stored_type(*db, symbol_id) == "custom:exact");   // 红：当前新行 Unknown
    }

    SUBCASE("old Unknown row updates to the exact Value")
    {
        u04b1_store_config(
            matching_store, matching_config,
            nlohmann::json::array({u04a_rule("b2-value", "value", "^B2VAL-", "custom:selector")}),
            nlohmann::json::array(
                {u04a_exception("b2-value-exact", "value", "B2VAL-KEY", "custom:exact")}));

        U04B1Library lib(*db, matching_store, temp.path(), "ImportExactUpdate");
        write(lib.target, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1B2Update" (property "Reference" "U") (property "Value" "v1"))))KICAD");

        services::ImportPipeline first(*db, nullptr, false, &matching_store);
        auto created = first | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
        REQUIRE(created.symbols == 1);
        const core::Symbol before = lib.read(lib.find_id("U04B1B2Update"));
        REQUIRE(before.component_type_id == "Unknown");
        REQUIRE(before.default_value() == "v1");
        const int snapshots_before = u04b1_imported_files(*db);
        REQUIRE(snapshots_before == 1);

        write(lib.target, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1B2Update" (property "Reference" "U") (property "Value" "B2VAL-KEY"))))KICAD");
        fs::last_write_time(lib.target,
                            fs::last_write_time(lib.target) + std::chrono::seconds(2));

        services::ImportPipeline second(*db, nullptr, false, &matching_store);
        auto updated = second | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
        MESSAGE("exact value update: symbols=", updated.symbols, " skipped=", updated.skipped);
        REQUIRE(updated.symbols == 1);   // 没有被 mtime 快照跳过
        const core::Symbol after = lib.read(before.id());
        CHECK(after.id() == before.id());                     // UUID 保留
        REQUIRE(after.default_value() == "B2VAL-KEY");        // 源字段确实更新
        CHECK(after.library_id() == before.library_id());     // 来源键保持
        CHECK(u04b1_imported_files(*db) == snapshots_before); // 不删快照
        CHECK(fix02_stored_type(*db, before.id()) == "custom:exact");   // 红：当前保留 Unknown
    }

    SUBCASE("new row with an exact MPN")
    {
        // 只有 model 规则：不会被 Value 字段更高优先级命中
        u04b1_store_config(
            matching_store, matching_config,
            nlohmann::json::array({u04a_rule("b2-model", "model", "^B2MPN-", "custom:selector")}),
            nlohmann::json::array(
                {u04a_exception("b2-model-exact", "model", "B2MPN-KEY", "custom:exact")}));

        U04B1Library lib(*db, matching_store, temp.path(), "ImportExactModel");
        write(lib.target, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1B2Model" (property "Reference" "U") (property "Value" "plain-1") (property "MPN" "B2MPN-KEY"))))KICAD");

        auto parsed = parser::SymbolLibParser::parse(lib.target);
        if (!parsed)
            MESSAGE("parse failed: ", parsed.error().format_message());
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->items.size() == 1);
        REQUIRE(parsed->items.front().mpn() == "B2MPN-KEY");
        REQUIRE(parsed->items.front().property("reference").has_value());
        REQUIRE(*parsed->items.front().property("reference") == "U");
        const auto decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(parsed->items.front()));
        REQUIRE(decision.status == classifier::MatchStatus::Matched);
        REQUIRE(decision.type_id == "custom:exact");
        REQUIRE(decision.evidence.front().source == classifier::MatchEvidenceSource::Exception);

        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto imported = pipe | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
        MESSAGE("exact model import: symbols=", imported.symbols, " skipped=", imported.skipped);
        REQUIRE(imported.symbols == 1);
        const std::string symbol_id = lib.find_id("U04B1B2Model");
        CHECK(fix02_stored_type(*db, symbol_id) == "custom:exact");   // 红：当前新行 Unknown
    }

    SUBCASE("known identities survive a matching source change")
    {
        u04b1_store_config(
            matching_store, matching_config,
            nlohmann::json::array({u04a_rule("b2-keep", "value", "^B2KEEP-", "custom:selector")}),
            nlohmann::json::array(
                {u04a_exception("b2-keep-exact", "value", "B2KEEP-KEY", "custom:exact")}));

        U04B1Library lib(*db, matching_store, temp.path(), "ImportKnown");
        write(lib.target, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1B2OpAmp" (property "Reference" "U") (property "Value" "v1"))
  (symbol "U04B1B2Custom" (property "Reference" "U") (property "Value" "v1"))
  (symbol "U04B1B2Raw" (property "Reference" "U") (property "Value" "v1"))))KICAD");

        services::ImportPipeline first(*db, nullptr, false, &matching_store);
        auto created = first | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
        REQUIRE(created.symbols == 1);   // 三条符号同属一个库文件
        const std::string opamp_id = lib.find_id("U04B1B2OpAmp");
        const std::string custom_id = lib.find_id("U04B1B2Custom");
        const std::string raw_id = lib.find_id("U04B1B2Raw");
        for (const std::string& id : {opamp_id, custom_id, raw_id})
            REQUIRE(fix02_stored_type(*db, id) == "Unknown");   // 前提：先以不匹配源导入

        // 真实 repo 把三行设成已知类型（测试前提，不是待验证目标）
        auto set_known = [&](const std::string& id, const std::string& type_id)
        {
            auto session = db->session();
            auto changed = storage::SymbolRepository(session).update_component_type_if_current(
                id, "Unknown", type_id);
            REQUIRE(changed.has_value());
            REQUIRE(*changed);
        };
        set_known(opamp_id, "OpAmp");
        set_known(custom_id, "custom:selector");
        set_known(raw_id, "LegacyRawId");

        // 同源更新成 core 会给出别的类型（先证明该来源的真实结论）
        write(lib.target, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1B2OpAmp" (property "Reference" "U") (property "Value" "B2KEEP-KEY") (property "MPN" "B2KEEP-MPN"))
  (symbol "U04B1B2Custom" (property "Reference" "U") (property "Value" "B2KEEP-KEY") (property "MPN" "B2KEEP-MPN"))
  (symbol "U04B1B2Raw" (property "Reference" "U") (property "Value" "B2KEEP-KEY") (property "MPN" "B2KEEP-MPN"))))KICAD");
        fs::last_write_time(lib.target,
                            fs::last_write_time(lib.target) + std::chrono::seconds(2));
        auto parsed = parser::SymbolLibParser::parse(lib.target);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->items.size() == 3);
        const auto decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(parsed->items.front()));
        REQUIRE(decision.status == classifier::MatchStatus::Matched);
        REQUIRE(decision.type_id == "custom:exact");

        services::ImportPipeline second(*db, nullptr, false, &matching_store);
        auto updated = second | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
        MESSAGE("known import update: symbols=", updated.symbols, " skipped=", updated.skipped);
        REQUIRE(updated.symbols == 1);
        CHECK(fix02_stored_type(*db, opamp_id) == "OpAmp");              // 绿：内置身份保护
        CHECK(fix02_stored_type(*db, custom_id) == "custom:selector");   // 绿：已登记 custom 保护
        CHECK(fix02_stored_type(*db, raw_id) == "LegacyRawId");          // 绿：历史 raw ID 原样
        for (const std::string& id : {opamp_id, custom_id, raw_id})
        {
            const core::Symbol row = lib.read(id);
            CHECK(row.default_value() == "B2KEEP-KEY");                  // 源字段确实刷新
            CHECK(row.mpn() == "B2KEEP-MPN");
            REQUIRE(row.property("reference").has_value());
            CHECK(*row.property("reference") == "U");
        }
    }
}

// ------------------------------------------------------------
// B03：冲突/过长/超限/引擎异常 —— 作废的决策不得写类型
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a discarded decision never writes a component type")
{
    SUBCASE("conflict keeps a true NULL row NULL")
    {
        // 同优先级、同字段、不同类型的两个合法正则 -> Conflict
        u04b1_store_config(matching_store, matching_config,
                           nlohmann::json::array(
                               {u04a_rule("b1-conflict-resistor", "value", "^B1CONFLICT$", "Resistor"),
                                u04a_rule("b1-conflict-capacitor", "value", "^B1CONFLICT$",
                                          "Capacitor")}));
        U04B1Library lib(*db, matching_store, temp.path(), "DecisionConflict");
        const std::string symbol_id = lib.insert(u04b1_part("U04B1DecisionConflict", "R",
                                                            std::string("R?"), "B1CONFLICT", "",
                                                            "Unknown"));
        // 至少 conflict 覆盖真 NULL：旧默认位号推断本来会补 Resistor
        u04b1_raw_exec(*db,
                       "UPDATE symbols SET component_type = NULL WHERE id = '" + symbol_id + "'");
        REQUIRE(u04b1_raw_column(*db, symbol_id, "component_type").is_null);
        const core::Symbol row = lib.read(symbol_id);
        REQUIRE(row.component_type_id.empty());

        const auto decision =
            matching_store.current()->evaluate(classifier::MatchSample::from_symbol(row));
        if (decision.status != classifier::MatchStatus::Conflict)
            MESSAGE("core conflict: ", classifier::match_status_name(decision.status), " ",
                    decision.reason);
        REQUIRE(decision.status == classifier::MatchStatus::Conflict);
        REQUIRE(decision.type_id.empty());   // 不替调用方选一个

        services::ClassificationService service(db.get(), &matching_store);
        auto summary = service.classify_all();
        if (!summary)
            MESSAGE("classify_all failed: ", summary.error().format_message());
        REQUIRE(summary.has_value());
        CHECK(summary->type_updated == 0);                                   // 红：旧默认映射会写
        CHECK(u04b1_raw_column(*db, symbol_id, "component_type").is_null);   // 红：真 NULL 保持
    }

    SUBCASE("input too long is not truncated into a match")
    {
        u04b1_store_config(
            matching_store, matching_config,
            nlohmann::json::array({u04a_rule("b1-toolong", "value", "^A+$", "Capacitor")}));
        U04B1Library lib(*db, matching_store, temp.path(), "DecisionTooLong");
        const std::string long_value(4097, 'A');   // 固定 ASCII，正好超过 4096 字节上限
        const std::string symbol_id = lib.insert(u04b1_part("U04B1DecisionTooLong", "R",
                                                            std::string("R?"), long_value, "",
                                                            "Unknown"));
        const core::Symbol row = lib.read(symbol_id);
        REQUIRE(row.default_value().size() == 4097);

        const auto decision =
            matching_store.current()->evaluate(classifier::MatchSample::from_symbol(row));
        if (decision.status != classifier::MatchStatus::InputTooLong)
            MESSAGE("core too-long: ", classifier::match_status_name(decision.status), " ",
                    decision.reason);
        REQUIRE(decision.status == classifier::MatchStatus::InputTooLong);   // 不是截断后的命中
        REQUIRE(decision.type_id.empty());
        REQUIRE(decision.evidence.empty());

        services::ClassificationService service(db.get(), &matching_store);
        auto summary = service.classify_all();
        if (!summary)
            MESSAGE("classify_all failed: ", summary.error().format_message());
        REQUIRE(summary.has_value());
        CHECK(summary->type_updated == 0);                            // 红
        CHECK(fix02_stored_type(*db, symbol_id) == "Unknown");        // 红：原值原样
    }

    SUBCASE("evaluation limit discards the already collected hit")
    {
        // A 已实测输入：先只用 first-hit 规则证明它真的命中（不是天生不匹配）
        const std::string subject = u04b1_limit_subject();
        auto first_only = u04a_snapshot(u04a_config(nlohmann::json::array(
            {u04a_rule("b1-limit-first", "value", "^a+!$", "Capacitor")})));
        classifier::MatchSample sample;
        sample.value = subject;
        const auto first_decision = first_only->evaluate(sample);
        REQUIRE(first_decision.status == classifier::MatchStatus::Matched);
        REQUIRE(first_decision.type_id == "Capacitor");

        // 完整配置：低优先级先命中，高优先级重模式随后触发 evaluation_limit
        u04b1_store_config(
            matching_store, matching_config,
            nlohmann::json::array(
                {u04a_rule("b1-limit-first", "value", "^a+!$", "Capacitor", 0),
                 u04a_rule("b1-limit-heavy", "value", kU04B1LimitPattern, "Resistor", 100)}));
        U04B1Library lib(*db, matching_store, temp.path(), "DecisionLimit");
        const std::string symbol_id = lib.insert(
            u04b1_part("U04B1DecisionLimit", "R", std::string("R?"), subject, "", "Unknown"));
        const core::Symbol row = lib.read(symbol_id);
        REQUIRE(row.default_value() == subject);

        const auto decision =
            matching_store.current()->evaluate(classifier::MatchSample::from_symbol(row));
        if (decision.status != classifier::MatchStatus::EvaluationLimit)
            MESSAGE("core limit: ", classifier::match_status_name(decision.status), " ",
                    decision.reason);
        REQUIRE(decision.status == classifier::MatchStatus::EvaluationLimit);   // 不能当 Unmatched
        REQUIRE(decision.type_id.empty());
        REQUIRE(decision.evidence.empty());   // 已收集的 Capacitor 命中被丢弃
        REQUIRE(decision.reason.find("b1-limit-heavy") != std::string::npos);
        REQUIRE(decision.reason.find("budget") != std::string::npos);

        services::ClassificationService service(db.get(), &matching_store);
        auto summary = service.classify_all();
        if (!summary)
            MESSAGE("classify_all failed: ", summary.error().format_message());
        REQUIRE(summary.has_value());
        CHECK(summary->type_updated == 0);                            // 红
        CHECK(fix02_stored_type(*db, symbol_id) == "Unknown");
    }

    SUBCASE("engine error discards the already collected hit")
    {
        // A 真实 PCRE2 RECURSELOOP 样本：先命中 ^x$，随后重规则求值报错
        auto first_only = u04a_snapshot(u04a_config(
            nlohmann::json::array({u04a_rule("b1-error-first", "value", "^x$", "Capacitor")})));
        classifier::MatchSample sample;
        sample.value = "x";
        const auto first_decision = first_only->evaluate(sample);
        REQUIRE(first_decision.status == classifier::MatchStatus::Matched);
        REQUIRE(first_decision.type_id == "Capacitor");

        u04b1_store_config(
            matching_store, matching_config,
            nlohmann::json::array(
                {u04a_rule("b1-error-first", "value", "^x$", "Capacitor", 0),
                 u04a_rule("b1-error-recursive", "value", kU04B1ErrorPattern, "Resistor", 100)}));
        U04B1Library lib(*db, matching_store, temp.path(), "DecisionError");
        const std::string symbol_id = lib.insert(
            u04b1_part("U04B1DecisionError", "R", std::string("R?"), "x", "", "Unknown"));
        const core::Symbol row = lib.read(symbol_id);
        REQUIRE(row.default_value() == "x");

        const auto decision =
            matching_store.current()->evaluate(classifier::MatchSample::from_symbol(row));
        if (decision.status != classifier::MatchStatus::EvaluationError)
            MESSAGE("core error: ", classifier::match_status_name(decision.status), " ",
                    decision.reason);
        REQUIRE(decision.status == classifier::MatchStatus::EvaluationError);   // 不是 Unmatched
        REQUIRE(decision.type_id.empty());
        REQUIRE(decision.evidence.empty());
        REQUIRE(decision.reason.find("b1-error-recursive") != std::string::npos);
        REQUIRE(decision.reason.find("PCRE2 error") != std::string::npos);
        REQUIRE(decision.reason.find("-52") != std::string::npos);

        services::ClassificationService service(db.get(), &matching_store);
        auto summary = service.classify_all();
        if (!summary)
            MESSAGE("classify_all failed: ", summary.error().format_message());
        REQUIRE(summary.has_value());
        CHECK(summary->type_updated == 0);                            // 红
        CHECK(fix02_stored_type(*db, symbol_id) == "Unknown");
    }
}

// ------------------------------------------------------------
// B04：普通导入对作废决策落 Unknown（源经真实 parser 进入）
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 ordinary import keeps discarded decisions unclassified")
{
    nlohmann::json rules = nlohmann::json::array();
    std::string stem;
    std::string symbol_name;
    std::string value_text;
    classifier::MatchStatus expected = classifier::MatchStatus::Unmatched;

    SUBCASE("conflict source")
    {
        stem = "ImportConflict";
        symbol_name = "U04B1ImpConflict";
        value_text = "B1CONFLICT";
        expected = classifier::MatchStatus::Conflict;
        rules = nlohmann::json::array(
            {u04a_rule("b1-imp-conflict-resistor", "value", "^B1CONFLICT$", "Resistor"),
             u04a_rule("b1-imp-conflict-capacitor", "value", "^B1CONFLICT$", "Capacitor")});
    }
    SUBCASE("too long source")
    {
        stem = "ImportTooLong";
        symbol_name = "U04B1ImpTooLong";
        value_text = std::string(4097, 'A');
        expected = classifier::MatchStatus::InputTooLong;
        rules = nlohmann::json::array({u04a_rule("b1-imp-toolong", "value", "^A+$", "Capacitor")});
    }
    SUBCASE("evaluation limit source")
    {
        stem = "ImportLimit";
        symbol_name = "U04B1ImpLimit";
        value_text = u04b1_limit_subject();
        expected = classifier::MatchStatus::EvaluationLimit;
        rules = nlohmann::json::array(
            {u04a_rule("b1-imp-limit-first", "value", "^a+!$", "Capacitor", 0),
             u04a_rule("b1-imp-limit-heavy", "value", kU04B1LimitPattern, "Resistor", 100)});
    }
    SUBCASE("engine error source")
    {
        stem = "ImportError";
        symbol_name = "U04B1ImpError";
        value_text = "x";
        expected = classifier::MatchStatus::EvaluationError;
        rules = nlohmann::json::array(
            {u04a_rule("b1-imp-error-first", "value", "^x$", "Capacitor", 0),
             u04a_rule("b1-imp-error-recursive", "value", kU04B1ErrorPattern, "Resistor", 100)});
    }

    u04b1_store_config(matching_store, matching_config, rules);
    U04B1Library lib(*db, matching_store, temp.path(), stem);
    write(lib.target, "(kicad_symbol_lib (version 20231120)\n  (symbol \"" + symbol_name +
                          "\" (property \"Reference\" \"R?\") (property \"Value\" \"" + value_text +
                          "\")))\n");

    // 真实 parser：字段原文进入 Symbol（过长也不截断）
    auto parsed = parser::SymbolLibParser::parse(lib.target);
    if (!parsed)
        MESSAGE("parse failed: ", parsed.error().format_message());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 1);
    REQUIRE(parsed->items.front().default_value() == value_text);
    REQUIRE(parsed->items.front().property("reference").has_value());
    REQUIRE(*parsed->items.front().property("reference") == "R?");
    const auto decision = matching_store.current()->evaluate(
        classifier::MatchSample::from_symbol(parsed->items.front()));
    if (decision.status != expected)
        MESSAGE("core: ", classifier::match_status_name(decision.status), " ", decision.reason);
    REQUIRE(decision.status == expected);   // 真实核心状态，不是 Unmatched
    REQUIRE(decision.type_id.empty());

    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    auto imported = pipe | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
    MESSAGE("discarded decision import: symbols=", imported.symbols, " skipped=", imported.skipped);
    REQUIRE(imported.symbols == 1);   // 源库只有一个 target 文件
    const std::string symbol_id = lib.find_id(symbol_name);
    CHECK(fix02_stored_type(*db, symbol_id) == "Unknown");                 // 红：旧管道误写
    CHECK(u04b1_count_symbols(*db, "component_type = 'Resistor'") == 0);   // 不能采用私有猜测
}

// ------------------------------------------------------------
// B05：正式竞态绿色对照 —— 无关来源变化不能被过度拒绝，CAS 继续有效
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 unrelated source changes keep classification writing")
{
    enum class Plan
    {
        Description,
        Properties,
        KnownBuiltin,
        KnownCustom,
        KnownRaw,
        Deleted,
    };
    Plan plan = Plan::Description;
    std::string known_type;
    SUBCASE("description and datasheet only") { plan = Plan::Description; }
    SUBCASE("unrelated properties key only") { plan = Plan::Properties; }
    SUBCASE("concurrent known builtin type")
    {
        plan = Plan::KnownBuiltin;
        known_type = "OpAmp";
    }
    SUBCASE("concurrent registered custom type")
    {
        plan = Plan::KnownCustom;
        known_type = "custom:selector";
    }
    SUBCASE("concurrent legacy raw type")
    {
        plan = Plan::KnownRaw;
        known_type = "LegacyRawId";
    }
    SUBCASE("row deleted during classification") { plan = Plan::Deleted; }

    u04b1_register_types({"custom:selector"});   // 已知类型子用例的登记前提
    U04B1Library lib(*db, matching_store, temp.path(), "RaceGreen");
    const std::string symbol_id =
        lib.insert(u04b1_part("U04B1RaceGreen", "R", std::string("R?"), "v1", "m1", "Unknown"));

    const core::Symbol before = lib.read(symbol_id);
    REQUIRE(before.component_type_id == "Unknown");
    REQUIRE(before.description().empty());
    REQUIRE(before.datasheet().empty());
    const auto decision =
        matching_store.current()->evaluate(classifier::MatchSample::from_symbol(before));
    REQUIRE(decision.status == classifier::MatchStatus::Matched);
    REQUIRE(decision.type_id == "Resistor");

    std::string worker_sql;
    switch (plan)
    {
        case Plan::Description:
            worker_sql =
                "UPDATE symbols SET description = 'race-desc', datasheet = 'https://example.invalid/race'";
            break;
        case Plan::Properties:
            worker_sql = "UPDATE symbols SET properties_json = json_set(properties_json, "
                         "'$.unrelated_key', 'race-value')";
            break;
        case Plan::KnownBuiltin:
        case Plan::KnownCustom:
        case Plan::KnownRaw:
            worker_sql =
                "UPDATE symbols SET component_type = '" + known_type + "', description = 'race-known'";
            break;
        case Plan::Deleted:
            worker_sql = "DELETE FROM symbols";
            break;
    }
    worker_sql += " WHERE id = '" + symbol_id + "'";

    U04B1SourceRace race(*db, db->path(), worker_sql);
    services::ClassificationService service(db.get(), &matching_store);
    auto summary = service.classify_all();
    race.finish_join();   // service 返回后先完成握手，再读任何跨线程状态
    REQUIRE_FALSE(race.callback_error);
    REQUIRE(race.worker_started);
    REQUIRE(race.begin_seen);
    REQUIRE_FALSE(race.worker_timeout);
    REQUIRE_FALSE(race.handshake_timeout);
    if (!race.worker_ok)
        MESSAGE("second connection failed: ", race.worker_error);
    REQUIRE(race.worker_ok);
    if (!summary)
        MESSAGE("classify_all failed: ", summary.error().format_message());
    REQUIRE(summary.has_value());   // 下面各分支解引用 summary：存在性用 REQUIRE 挡住

    if (plan == Plan::Deleted)
    {
        // 删除行是跳过：summary 仍成功，repo NotFound 不能当 DBError
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        CHECK_FALSE(row.has_value());
        CHECK(summary->type_updated == 0);
        return;
    }

    if (plan == Plan::Description)
    {
        CHECK(summary->type_updated == 1);   // 绿：无关字段不能被当成来源变化
        CHECK(fix02_stored_type(*db, symbol_id) == "Resistor");
        const core::Symbol after = lib.read(symbol_id);
        CHECK(after.description() == "race-desc");      // 新元数据保留
        CHECK(after.datasheet() == "https://example.invalid/race");
    }
    else if (plan == Plan::Properties)
    {
        CHECK(summary->type_updated == 1);   // 绿：不能比较整 properties_json
        CHECK(fix02_stored_type(*db, symbol_id) == "Resistor");
        const core::Symbol after = lib.read(symbol_id);
        const classifier::MatchSample before_sample = classifier::MatchSample::from_symbol(before);
        const classifier::MatchSample after_sample = classifier::MatchSample::from_symbol(after);
        CHECK(after_sample.name == before_sample.name);          // 四字段完全相等
        CHECK(after_sample.value == before_sample.value);
        CHECK(after_sample.model == before_sample.model);
        CHECK(after_sample.reference == before_sample.reference);
        REQUIRE(after.property("unrelated_key").has_value());
        CHECK(*after.property("unrelated_key") == "race-value");
    }
    else
    {
        CHECK(summary->type_updated == 0);   // 绿：原值 CAS 继续有效
        CHECK(fix02_stored_type(*db, symbol_id) == known_type);
        const core::Symbol after = lib.read(symbol_id);
        CHECK(after.description() == "race-known");     // 无关元数据保留
        const classifier::MatchSample before_sample = classifier::MatchSample::from_symbol(before);
        const classifier::MatchSample after_sample = classifier::MatchSample::from_symbol(after);
        CHECK(after_sample.name == before_sample.name);          // 四输入不变
        CHECK(after_sample.value == before_sample.value);
        CHECK(after_sample.model == before_sample.model);
        CHECK(after_sample.reference == before_sample.reference);
    }
}

// ------------------------------------------------------------
// B06：真 NULL 来源竞态与 optional Reference 变更
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a true NULL type and an optional reference change stay protected")
{
    SUBCASE("true SQL NULL survives a reference change")
    {
        U04B1Library lib(*db, matching_store, temp.path(), "RaceNull");
        const std::string symbol_id =
            lib.insert(u04b1_part("U04B1RaceNull", "R", std::string("R?"), "v1", "m1", "Unknown"));
        // 真 NULL（不是空串）：默认构造的 empty 不能代替 SQL NULL
        u04b1_raw_exec(*db,
                       "UPDATE symbols SET component_type = NULL WHERE id = '" + symbol_id + "'");
        REQUIRE(u04b1_raw_column(*db, symbol_id, "component_type").is_null);

        const core::Symbol before = lib.read(symbol_id);
        REQUIRE(before.component_type_id.empty());   // repo 读回 empty，不规范化成 Unknown
        REQUIRE(before.reference_prefix() == "R");
        REQUIRE(before.property("reference").has_value());
        REQUIRE(*before.property("reference") == "R?");
        const auto decision =
            matching_store.current()->evaluate(classifier::MatchSample::from_symbol(before));
        REQUIRE(decision.status == classifier::MatchStatus::Matched);
        REQUIRE(decision.type_id == "Resistor");

        // worker 只改真实 Reference 属性（prefix 保持 R），类型保持真 NULL
        U04B1SourceRace race(*db, db->path(),
                             "UPDATE symbols SET properties_json = json_set(properties_json, "
                             "'$.reference', 'U') WHERE id = '" + symbol_id + "'");
        services::ClassificationService service(db.get(), &matching_store);
        auto summary = service.classify_all();
        if (!summary)
            MESSAGE("classify_all failed: ", summary.error().format_message());
        race.finish_join();
        REQUIRE_FALSE(race.callback_error);
        REQUIRE(race.worker_started);
        REQUIRE(race.begin_seen);
        REQUIRE_FALSE(race.worker_timeout);
        REQUIRE_FALSE(race.handshake_timeout);
        if (!race.worker_ok)
            MESSAGE("second connection failed: ", race.worker_error);
        REQUIRE(race.worker_ok);

        const core::Symbol after = lib.read(symbol_id);
        REQUIRE(after.property("reference").has_value());
        CHECK(*after.property("reference") == "U");     // 来源确实变成 U
        CHECK(after.reference_prefix() == "R");         // 只改 actual reference 属性
        CHECK(u04b1_raw_column(*db, symbol_id, "component_type").is_null);   // 红：仍须是真 NULL
        REQUIRE(summary.has_value());
        CHECK(summary->type_updated == 0);   // 红：当前把 NULL 补成 Resistor
    }

    SUBCASE("missing reference becomes explicit empty")
    {
        // 初始靠 name 合法命中 Resistor，prefix R 让旧路径可写；Reference 初始缺失
        u04b1_store_config(matching_store, matching_config,
                           nlohmann::json::array({u04a_rule("b1-race-optional", "name",
                                                            "^U04B1RaceOptional$", "Resistor")}));
        U04B1Library lib(*db, matching_store, temp.path(), "RaceOptional");
        const std::string symbol_id =
            lib.insert(u04b1_part("U04B1RaceOptional", "R", std::nullopt, "v1", "m1", "Unknown"));

        const core::Symbol before = lib.read(symbol_id);
        REQUIRE(before.component_type_id == "Unknown");
        REQUIRE_FALSE(before.property("reference").has_value());   // optional 前值：缺失
        REQUIRE(before.reference_prefix() == "R");
        const auto decision =
            matching_store.current()->evaluate(classifier::MatchSample::from_symbol(before));
        REQUIRE(decision.status == classifier::MatchStatus::Matched);
        REQUIRE(decision.type_id == "Resistor");

        U04B1SourceRace race(*db, db->path(),
                             "UPDATE symbols SET properties_json = json_set(properties_json, "
                             "'$.reference', '') WHERE id = '" + symbol_id + "'");
        services::ClassificationService service(db.get(), &matching_store);
        auto summary = service.classify_all();
        if (!summary)
            MESSAGE("classify_all failed: ", summary.error().format_message());
        race.finish_join();
        REQUIRE_FALSE(race.callback_error);
        REQUIRE(race.worker_started);
        REQUIRE(race.begin_seen);
        REQUIRE_FALSE(race.worker_timeout);
        REQUIRE_FALSE(race.handshake_timeout);
        if (!race.worker_ok)
            MESSAGE("second connection failed: ", race.worker_error);
        REQUIRE(race.worker_ok);

        const core::Symbol after = lib.read(symbol_id);
        REQUIRE(after.property("reference").has_value());          // optional 后值：存在
        CHECK(after.property("reference")->empty());               // 且是显式空串
        CHECK(after.name() == "U04B1RaceOptional");                // 其他三输入未变
        CHECK(after.default_value() == "v1");
        CHECK(after.mpn() == "m1");
        CHECK(after.reference_prefix() == "R");
        CHECK(after.component_type_id == "Unknown");               // 红：当前写 Resistor
        REQUIRE(summary.has_value());
        CHECK(summary->type_updated == 0);                         // 红：当前 1
    }
}

// ------------------------------------------------------------
// B07：force 入口 —— 坏配置必须在清空 mtime 快照之前被拒（红），健康 force 仍真实重导（绿对照）
//
// 只走真实入口 api::ImportManager::import_all(Options)：Options{force=true} 先
// DELETE imported_files 再 run_now。判别证据是「不在启用目录里的停用来源快照」——
// 启用目标被错误重导后 mtime 会被重新记回，只有停用来源能暴露整表被清过。
// 全部临时 ComponentLibraries/ConfigStore，不读本机 KiCad 或用户库。
// ------------------------------------------------------------

namespace
{

/// 已导入文件快照的完整副本（真实仓储读）—— force 前后逐项比对的基准
std::unordered_map<std::string, int64_t> u04b1_imported_map(storage::Database& db)
{
    auto session = db.session();
    auto all = storage::ImportedFilesRepository(session).all();
    if (!all)
        MESSAGE("imported_files read failed: ", all.error().format_message());
    REQUIRE(all.has_value());
    return *all;
}

/// 按文件名（路径末段）在快照里定位唯一键；缺失或同名多键都是夹具失败
std::string u04b1_snapshot_key_for(const std::unordered_map<std::string, int64_t>& map,
                                   const std::string& filename)
{
    std::string found;
    for (const auto& [key, value] : map)
    {
        (void)value;
        if (fs::path(key).filename() != filename)
            continue;
        if (!found.empty())
        {
            MESSAGE("duplicate snapshot key for ", filename, ": ", found, " and ", key);
            REQUIRE(found.empty());
        }
        found = key;
    }
    if (found.empty())
        MESSAGE("no snapshot key for ", filename);
    REQUIRE_FALSE(found.empty());
    return found;
}

/// 两份快照逐项相等：键集合与取值都要一致（只比条数会漏掉「清表后重建」）
void u04b1_check_snapshots_equal(const std::unordered_map<std::string, int64_t>& expected,
                                 const std::unordered_map<std::string, int64_t>& actual)
{
    CHECK(actual.size() == expected.size());
    for (const auto& [key, value] : expected)
    {
        const auto it = actual.find(key);
        const bool present = it != actual.end();
        if (!present)
            MESSAGE("snapshot lost: ", key);
        CHECK(present);
        if (present)
            CHECK(it->second == value);
    }
}

/// 读回一条符号行（缺行是夹具/前提失败）
core::Symbol u04b1_symbol_row(storage::Database& db, const std::string& symbol_id)
{
    auto session = db.session();
    auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
    if (!row)
        MESSAGE("symbol read failed: ", row.error().format_message());
    REQUIRE(row.has_value());
    return *row;
}

}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a bad config force import preserves cached source snapshots")
{
    group();   // 唯一启用的临时库：symbol_path 就是临时 symbols 目录
    util::ConfigStore config(temp.path() / "config.json");
    api::ImportManager manager(*db, &config, &matching_store);

    // 健康首导：真实来源进库、mtime 记账；保存 UUID、库数、字段与源字节作为基准
    auto first = manager.import_all({});
    MESSAGE("healthy first import: symbols=", first.symbols, " skipped=", first.skipped);
    REQUIRE(first.symbols == 1);
    std::string symbol_id;
    {
        auto session = db->session();
        auto syms = storage::SymbolRepository(session).find_all();
        REQUIRE(syms.has_value());
        REQUIRE(syms->size() == 1);
        symbol_id = syms->front().id();
    }
    REQUIRE(u04b1_imported_files(*db) == 1);
    const core::Symbol symbol_before = u04b1_symbol_row(*db, symbol_id);
    const std::string source_bytes = read(file);
    const int libraries_before = count_rows(*db, "libraries");
    const int symbols_before = count_rows(*db, "symbols");

    // 一条启用目录之外的停用来源：合法 .kicad_sym，用真实仓储记固定 mtime 123。
    // 没有任何启用库指向它，所以被清掉之后不会在本轮被重新导入写回 ——
    // 这让「整表被 DELETE」无法被启用目标的重导掩盖。
    const fs::path parked = temp.path() / "parked" / "Inactive.kicad_sym";
    write(parked, R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1Parked" (property "Reference" "U") (property "Value" "parked"))))KICAD");
    const std::string parked_bytes = read(parked);
    {
        auto session = db->session();
        auto recorded = storage::ImportedFilesRepository(session).upsert(parked.string(), 123);
        if (!recorded)
            MESSAGE("parked snapshot upsert failed: ", recorded.error().format_message());
        REQUIRE(recorded.has_value());
    }
    REQUIRE(u04b1_imported_files(*db) == 2);

    const std::unordered_map<std::string, int64_t> snapshots_before = u04b1_imported_map(*db);
    REQUIRE(snapshots_before.size() == 2);
    const std::string active_key = u04b1_snapshot_key_for(snapshots_before, "test.kicad_sym");
    const std::string parked_key = u04b1_snapshot_key_for(snapshots_before, "Inactive.kicad_sym");
    REQUIRE(snapshots_before.at(parked_key) == 123);   // 停用来源的固定值

    // 坏匹配配置：真实 load 失败、last-valid 仍可展示、业务领取被拒
    write(matching_config, "{ this is not a matching config");
    REQUIRE_FALSE(matching_store.load(matching_config.string()).has_value());
    REQUIRE_FALSE(matching_store.load_error().empty());
    REQUIRE(matching_store.current() != nullptr);
    REQUIRE_FALSE(matching_store.acquire_business_snapshot().has_value());

    SUBCASE("force with a bad config changes nothing at all")
    {
        auto forced = manager.import_all({.force = true});
        MESSAGE("bad force: symbols=", forced.symbols, " footprints=", forced.footprints,
                " skipped=", forced.skipped);
        CHECK(forced.symbols == 0);   // 红：坏配置下任何导入都必须先被拒绝

        // 快照逐项保留（含停用来源）：清表之后启用目标的重导写不回这条
        const auto snapshots_after = u04b1_imported_map(*db);
        u04b1_check_snapshots_equal(snapshots_before, snapshots_after);
        const auto parked_after = snapshots_after.find(parked_key);
        if (parked_after != snapshots_after.end())
            CHECK(parked_after->second == 123);
        const bool active_present = snapshots_after.find(active_key) != snapshots_after.end();
        if (!active_present)
            MESSAGE("active snapshot lost: ", active_key);
        CHECK(active_present);

        // DB 行、库数与源文件字节一个都不许动
        const core::Symbol symbol_after = u04b1_symbol_row(*db, symbol_id);
        CHECK(symbol_after.id() == symbol_before.id());
        CHECK(symbol_after.name() == symbol_before.name());
        CHECK(symbol_after.footprint() == symbol_before.footprint());
        CHECK(symbol_after.default_value() == symbol_before.default_value());
        CHECK(symbol_after.component_type_id == symbol_before.component_type_id);
        CHECK(symbol_after.library_id() == symbol_before.library_id());
        CHECK(count_rows(*db, "symbols") == symbols_before);
        CHECK(count_rows(*db, "libraries") == libraries_before);
        CHECK(read(file) == source_bytes);
        CHECK(read(parked) == parked_bytes);
    }

    SUBCASE("after reload the same source still skips on its cached mtime")
    {
        // 本子用例自己制造「坏 force 已经发生」的前提链：每次 SUBCASE 都是独立
        // fixture，不借用上一组的污染状态冒充干净前提。
        auto forced = manager.import_all({.force = true});
        MESSAGE("bad force before reload: symbols=", forced.symbols, " skipped=", forced.skipped);
        CHECK(forced.symbols == 0);
        const auto after_force = u04b1_imported_map(*db);
        const bool parked_kept = after_force.find(parked_key) != after_force.end();
        if (!parked_kept)
            MESSAGE("parked snapshot lost after the rejected force: ", parked_key);
        CHECK(parked_kept);   // 被拒的 force 不得消费旧快照

        // 修复配置 reload 后：同一来源、不改 mtime，非 force 必须按缓存跳过
        write(matching_config, classifier::default_matching_config_json());
        REQUIRE(matching_store.load(matching_config.string()).has_value());
        REQUIRE(matching_store.acquire_business_snapshot().has_value());
        auto resumed = manager.import_all({});
        MESSAGE("reload rerun: symbols=", resumed.symbols, " skipped=", resumed.skipped);
        CHECK(resumed.symbols == 0);   // 红：坏 force 若吃掉快照，这里会重导
        CHECK(resumed.skipped == 1);
        CHECK(u04b1_count_symbols(*db, "name = 'R'") == 1);
        CHECK(read(file) == source_bytes);
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a healthy force import reimports unchanged sources under the same identity")
{
    group();
    util::ConfigStore config(temp.path() / "config.json");
    api::ImportManager manager(*db, &config, &matching_store);

    auto first = manager.import_all({});
    MESSAGE("healthy first import: symbols=", first.symbols, " skipped=", first.skipped);
    REQUIRE(first.symbols == 1);
    std::string symbol_id;
    {
        auto session = db->session();
        auto syms = storage::SymbolRepository(session).find_all();
        REQUIRE(syms.has_value());
        REQUIRE(syms->size() == 1);
        symbol_id = syms->front().id();
    }
    REQUIRE(u04b1_imported_files(*db) == 1);
    const core::Symbol symbol_before = u04b1_symbol_row(*db, symbol_id);
    const std::string source_bytes = read(file);
    const int libraries_before = count_rows(*db, "libraries");

    // 只改 DB 的 footprint：源文件与 mtime 都不动
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        core::Symbol changed = *row;
        changed.set_footprint("DB-only");
        auto updated = storage::SymbolRepository(session).update(changed);
        if (!updated)
            MESSAGE("DB-only update failed: ", updated.error().format_message());
        REQUIRE(updated.has_value());
    }

    // 非 force 对照：mtime 已缓存 -> 不解析、不写回，DB-only 保持
    auto rerun = manager.import_all({});
    MESSAGE("non-force rerun: symbols=", rerun.symbols, " skipped=", rerun.skipped);
    CHECK(rerun.symbols == 0);
    CHECK(rerun.skipped == 1);
    CHECK(u04b1_symbol_row(*db, symbol_id).footprint() == "DB-only");

    // force：忽略启用目标的已缓存 mtime，重新导出来源 -> 同 UUID、字段回源文件值。
    // 停用来源快照在健康 force 下允许保留也允许被清（force 语义只要求对启用目标
    // 忽略 mtime，不要求清无关来源全表），本用例不作断言。
    auto forced = manager.import_all({.force = true});
    MESSAGE("healthy force: symbols=", forced.symbols, " skipped=", forced.skipped);
    CHECK(forced.symbols == 1);
    const core::Symbol after = u04b1_symbol_row(*db, symbol_id);
    CHECK(after.id() == symbol_before.id());                    // UUID 保留
    CHECK(after.library_id() == symbol_before.library_id());    // 来源身份保留
    CHECK(after.name() == symbol_before.name());
    CHECK(after.footprint() == "Pkg:Old");                      // 源文件引用重新生效
    CHECK(count_rows(*db, "symbols") == 1);                     // 不产生重复行
    CHECK(count_rows(*db, "libraries") == libraries_before);    // 不新增库行
    CHECK(read(file) == source_bytes);                          // 未开 KF ID 写回，源字节不变
}

// ============================================================
// B08：共享 preview/apply 接口与结构化报告（B2 直接复用）
//   preview_all(draft = {}, detail_limit = 100) -> util::Result<Summary>
//   apply_all(expected_revision = {}) -> util::Result<ApplyReport>
// 两者与 classify_all 共用同一判定内核与同一批写回路径。
// ============================================================

TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 preview reports decisions without writing and caps only the details")
{
    u04b1_register_types({"custom:selector"});
    // 明确 revision：预览与执行报告都要带上这次使用的保存配置版本
    write(matching_config,
          u04a_config(nlohmann::json::array({u04a_rule("b8-name", "name", "^B8HIT",
                                                       "custom:selector")}),
                      nlohmann::json::array(), 7));
    REQUIRE(matching_store.load(matching_config.string()).has_value());

    U04B1Library lib(*db, matching_store, temp.path(), "Preview");
    const std::string hit =
        lib.insert(u04b1_part("B8HIT_PART", "U", std::string("U"), "", "", "Unknown"));
    const std::string plain =
        lib.insert(u04b1_part("B8PLAIN", "U", std::string("U"), "", "", "Unknown"));
    const std::string known =
        lib.insert(u04b1_part("B8KNOWN", "U", std::string("U"), "", "", "OpAmp"));

    services::ClassificationService service(db.get(), &matching_store);
    auto preview = service.preview_all();
    if (!preview)
        MESSAGE("preview failed: ", preview.error().format_message());
    REQUIRE(preview.has_value());
    CHECK(preview->total == 3);              // 计数覆盖全部行
    CHECK(preview->matched == 1);            // 只有 B8HIT_PART 命中配置
    CHECK(preview->protected_count == 1);    // OpAmp 行受保护
    // 业务分区：受保护行不进 eligible 状态计数，六状态之和 + protected == total
    CHECK(preview->unmatched == 1);          // 红：当前先把全部行 evaluate 再独立计保护
    CHECK(preview->protected_count + preview->matched + preview->unmatched +
              preview->conflicts + preview->input_too_long + preview->evaluation_limited +
              preview->evaluation_errors ==
          preview->total);
    CHECK(preview->revision == 7);
    CHECK(preview->type_updated == 0);       // 预览不写
    CHECK(preview->results.size() == 3);
    CHECK(fix02_stored_type(*db, hit) == "Unknown");    // 纯读：一条类型都没写
    CHECK(fix02_stored_type(*db, plain) == "Unknown");
    CHECK(fix02_stored_type(*db, known) == "OpAmp");
    {
        auto session = db->session();
        auto snapshots = storage::ImportedFilesRepository(session).all();
        REQUIRE(snapshots.has_value());
        CHECK(snapshots->empty());           // 也不消费任何来源快照
    }

    // 明细上限只截 results，计数仍覆盖全部行（扫描不为上限提前停止）
    auto capped = service.preview_all({}, 1);
    REQUIRE(capped.has_value());
    CHECK(capped->results.size() == 1);
    CHECK(capped->total == 3);
    CHECK(capped->matched == 1);
    CHECK(capped->unmatched == 1);   // 截断明细不影响分区计数（受保护行仍不算 unmatched）

    // draft：调用方用真实核心严格编译的候选就是预览依据（尚未保存的配置）
    auto draft = u04a_snapshot(u04a_config(
        nlohmann::json::array({u04a_rule("b8-draft", "name", "^B8PLAIN$", "custom:selector")}),
        nlohmann::json::array(), 99));
    auto drafted = service.preview_all(draft);
    REQUIRE(drafted.has_value());
    CHECK(drafted->revision == 99);
    CHECK(drafted->matched == 1);
    CHECK(fix02_stored_type(*db, plain) == "Unknown");   // draft 预览同样只读
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 apply rejects a stale revision and reports the committed part")
{
    u04b1_register_types({"custom:selector"});

    SUBCASE("stale revision is rejected before any write")
    {
        write(matching_config,
              u04a_config(nlohmann::json::array({u04a_rule("b8-apply", "name", "^B8APPLY",
                                                           "custom:selector")}),
                          nlohmann::json::array(), 3));
        REQUIRE(matching_store.load(matching_config.string()).has_value());
        U04B1Library lib(*db, matching_store, temp.path(), "StaleRevision");
        const std::string row =
            lib.insert(u04b1_part("B8APPLY_ONE", "U", std::string("U"), "", "", "Unknown"));

        services::ClassificationService service(db.get(), &matching_store);
        auto stale = service.apply_all(2);
        if (stale)
            MESSAGE("stale apply unexpectedly succeeded: type_updated=", stale->summary.type_updated);
        CHECK_FALSE(stale.has_value());
        CHECK(fix02_stored_type(*db, row) == "Unknown");   // 任何写入之前就失败

        auto applied = service.apply_all(3);
        REQUIRE(applied.has_value());
        CHECK_FALSE(applied->error.has_value());
        CHECK(applied->summary.revision == 3);
        CHECK(applied->summary.type_updated == 1);
        CHECK(fix02_stored_type(*db, row) == "custom:selector");
    }

    SUBCASE("the failing current batch reports zero committed and rolls back")
    {
        write(matching_config,
              u04a_config(nlohmann::json::array({u04a_rule("b8-apply", "name", "^B8APPLY",
                                                           "custom:selector")}),
                          nlohmann::json::array(), 3));
        REQUIRE(matching_store.load(matching_config.string()).has_value());
        U04B1Library lib(*db, matching_store, temp.path(), "RolledBatch");
        const std::string first =
            lib.insert(u04b1_part("B8APPLY_A", "U", std::string("U"), "", "", "Unknown"));
        const std::string second =
            lib.insert(u04b1_part("B8APPLY_B", "U", std::string("U"), "", "", "Unknown"));
        u04b1_raw_exec(*db,
                       "CREATE TRIGGER b8_apply_block BEFORE UPDATE ON symbols WHEN NEW.id = '" +
                           second + "' BEGIN SELECT RAISE(ABORT, 'b8 apply blocked'); END");

        services::ClassificationService service(db.get(), &matching_store);
        auto rolled = service.apply_all({});
        REQUIRE(rolled.has_value());
        REQUIRE(rolled->error.has_value());
        MESSAGE("apply error: ", rolled->error->format_message());
        CHECK(rolled->error->format_message().find("0 committed") != std::string::npos);
        CHECK(rolled->summary.type_updated == 0);
        CHECK(fix02_stored_type(*db, first) == "Unknown");    // 批内先写成功的那条也回滚
        CHECK(fix02_stored_type(*db, second) == "Unknown");
        u04b1_raw_exec(*db, "DROP TRIGGER b8_apply_block");
    }

    SUBCASE("the 501st row fails after 500 committed")
    {
        write(matching_config,
              u04a_config(nlohmann::json::array({u04a_rule("b8-apply", "name", "^B8APPLY",
                                                           "custom:selector")}),
                          nlohmann::json::array(), 3));
        REQUIRE(matching_store.load(matching_config.string()).has_value());
        U04B1Library lib(*db, matching_store, temp.path(), "PartialBatch");
        for (int index = 1; index <= 500; ++index)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "B8APPLY_%03d", index);
            lib.insert(u04b1_part(name, "U", std::string("U"), "", "", "Unknown"));
        }
        const std::string last =
            lib.insert(u04b1_part("B8APPLY_ZLAST", "U", std::string("U"), "", "", "Unknown"));
        u04b1_raw_exec(*db,
                       "CREATE TRIGGER b8_apply_last BEFORE UPDATE ON symbols WHEN NEW.id = '" +
                           last + "' BEGIN SELECT RAISE(ABORT, 'b8 apply last blocked'); END");

        services::ClassificationService service(db.get(), &matching_store);
        auto partial = service.apply_all({});
        REQUIRE(partial.has_value());
        REQUIRE(partial->error.has_value());
        MESSAGE("partial apply error: ", partial->error->format_message());
        CHECK(partial->error->format_message().find("500 committed") != std::string::npos);
        CHECK(partial->summary.type_updated == 500);          // 只计已提交的批
        // 写计划事实 vs 已提交批的真实写结果：501 条都在计划里，只有已提交的 500 条发布
        CHECK(partial->summary.would_update == 501);
        CHECK(partial->summary.unchanged == 0);
        CHECK(partial->summary.rolled_back == 1);             // 当前失败批只剩这一条候选
        CHECK(partial->summary.not_attempted == 0);           // 后面没有别的批
        CHECK(partial->summary.deleted == 0);
        CHECK(partial->summary.source_changed == 0);
        CHECK(partial->summary.cas_skipped == 0);
        CHECK(partial->summary.results.size() == 100);        // 明细上限仍是 100，第 501 条看不到
        int published_updated = 0;
        for (const auto& result : partial->summary.results)
            if (result.write_status == services::ClassificationService::WriteStatus::Updated)
                ++published_updated;
        CHECK(published_updated == 100);   // 明细只展示已提交批的 Updated，不展示未提交的写
        CHECK(u04b1_count_symbols(*db, "component_type = 'custom:selector'") == 500);
        CHECK(fix02_stored_type(*db, last) == "Unknown");     // 失败行原样
        u04b1_raw_exec(*db, "DROP TRIGGER b8_apply_last");
    }
}

// ------------------------------------------------------------
// B2-D1：类型删除必须保护匹配配置引用（当前/禁用条目）与仍被持有的旧快照。
// 回归事实：api::remove_component_type 已消费 TypeMatchingStore::ReferenceGuard
// （type_mutation.h），配置引用/旧快照租约会真实挡住持久化删除。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture,
                  "U04-B2 deletion: current and disabled rule or exception references block persistent deletion")
{
    auto& reg = core::TypeRegistry::instance();
    const fs::path type_file = temp.path() / "u04b2_delete_types.json";
    reg.bind_component_types_path(type_file.string());

    enum class RefKind
    {
        Rule,
        Exception,
    };
    RefKind kind = RefKind::Rule;
    bool enabled = true;
    SUBCASE("enabled rule")
    {
        kind = RefKind::Rule;
        enabled = true;
    }
    SUBCASE("disabled rule")
    {
        kind = RefKind::Rule;
        enabled = false;
    }
    SUBCASE("enabled exact exception")
    {
        kind = RefKind::Exception;
        enabled = true;
    }
    SUBCASE("disabled exact exception")
    {
        kind = RefKind::Exception;
        enabled = false;
    }

    // 完整注册表之上注册 custom 类型（id 留空由 Registry 生成），持久化到临时类型文件
    core::TypeEntry entry;
    entry.name = "U04B2Doomed";
    auto added = reg.add_component_type_persistent(entry);
    if (!added)
        MESSAGE("add_component_type_persistent failed: ", added.error().format_message());
    REQUIRE(added.has_value());
    const std::string type_id = *added;

    // 前提：DB 层零符号引用 —— 之后拒绝若发生，只能来自匹配配置引用
    {
        auto session = db->session();
        auto symbols = storage::SymbolRepository(session).count_by_component_type(type_id);
        if (!symbols)
            MESSAGE("count failed: ", symbols.error().format_message());
        REQUIRE(symbols.has_value());
        REQUIRE(*symbols == 0);
    }

    // 真实 save：只有一条 rule/exact 引用该类型，enabled 与否按 SUBCASE，
    // 不混入其他引用，避免掩盖 disabled 分支
    nlohmann::json rules = nlohmann::json::array();
    nlohmann::json exceptions = nlohmann::json::array();
    if (kind == RefKind::Rule)
        rules.push_back(u04a_rule("u04b2-only-ref", "reference", "^B2DOOMED-", type_id, 0, enabled));
    else
        exceptions.push_back(
            u04a_exception("u04b2-only-ref", "name", "B2DOOMED-KEY", type_id, enabled));
    auto saved = matching_store.save(u04a_config(std::move(rules), std::move(exceptions), 0), 0);
    if (!saved)
        MESSAGE("save failed: ", saved.error().format_message());
    REQUIRE(saved.has_value());

    const std::string type_bytes_before = read(type_file);
    const std::string config_bytes_before = read(matching_config);
    const std::string registry_json_before = fix04_types_json(reg.component_types());

    // A guard 先证明引用存在（disabled 也算）；删除必须在 guard 释放后进行，
    // 否则持配置锁调删除入口会自锁
    {
        auto guard = matching_store.acquire_reference_guard();
        if (!guard)
            MESSAGE("guard failed: ", guard.error().format_message());
        REQUIRE(guard.has_value());
        REQUIRE(guard->references(type_id));
    }

    auto attempt = api::remove_component_type(*db, type_id, &matching_store);
    if (!attempt)
        MESSAGE("remove failed: ", attempt.error().format_message());
    REQUIRE(attempt.has_value());
    CHECK(attempt->removed == false);   // 配置引用（含 disabled）挡住删除
    CHECK(attempt->references == 0);    // DB 层确实零引用，拒绝理由只能来自配置引用
    CHECK(attempt->matching_referenced);  // 拒删来自 guard 发现的配置引用
    CHECK_FALSE(attempt->reason.empty());

    // 类型文件、注册表、匹配配置一个都不能动
    CHECK(reg.find_component(type_id).has_value());
    CHECK(fix04_types_json(reg.component_types()) == registry_json_before);
    CHECK(read(type_file) == type_bytes_before);
    CHECK(read(matching_config) == config_bytes_before);
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B2 deletion: an old snapshot lease blocks deletion until released")
{
    auto& reg = core::TypeRegistry::instance();
    const fs::path type_file = temp.path() / "u04b2_lease_types.json";
    reg.bind_component_types_path(type_file.string());

    core::TypeEntry entry;
    entry.name = "U04B2Leased";
    auto added = reg.add_component_type_persistent(entry);
    if (!added)
        MESSAGE("add_component_type_persistent failed: ", added.error().format_message());
    REQUIRE(added.has_value());
    const std::string type_id = *added;

    {
        auto session = db->session();
        auto symbols = storage::SymbolRepository(session).count_by_component_type(type_id);
        REQUIRE(symbols.has_value());
        REQUIRE(*symbols == 0);
    }

    // 安装引用该类型的真实配置，再领一份旧快照持有
    auto installed = matching_store.save(
        u04a_config(
            nlohmann::json::array({u04a_rule("u04b2-lease", "name", "^B2LEASE-", type_id)}),
            nlohmann::json::array(), 0),
        0);
    if (!installed)
        MESSAGE("save failed: ", installed.error().format_message());
    REQUIRE(installed.has_value());
    auto old = matching_store.current();
    REQUIRE(old != nullptr);

    // 真实 save 移除引用（candidate revision 1 与 expected 1 一致）：
    // current 不再引用，但 old 仍持旧快照
    auto removed_ref = matching_store.save(
        u04a_config(
            nlohmann::json::array({u04a_rule("u04b2-plain", "reference", "^R[0-9?]*$", "Resistor")}),
            nlohmann::json::array(), 1),
        1);
    if (!removed_ref)
        MESSAGE("save without ref failed: ", removed_ref.error().format_message());
    REQUIRE(removed_ref.has_value());
    REQUIRE(matching_store.current() != nullptr);
    {
        const auto current_ids = matching_store.current()->referenced_type_ids();
        CHECK(std::find(current_ids.begin(), current_ids.end(), type_id) == current_ids.end());
    }

    const std::string type_bytes_before = read(type_file);
    const std::string config_bytes_before = read(matching_config);
    const std::string registry_json_at_block = fix04_types_json(reg.component_types());

    // A guard：旧快照仍被持有 → 引用必须仍受保护
    {
        auto guard = matching_store.acquire_reference_guard();
        if (!guard)
            MESSAGE("guard failed: ", guard.error().format_message());
        REQUIRE(guard.has_value());
        REQUIRE(guard->references(type_id));
    }

    auto blocked = api::remove_component_type(*db, type_id, &matching_store);
    if (!blocked)
        MESSAGE("remove failed: ", blocked.error().format_message());
    REQUIRE(blocked.has_value());
    CHECK(blocked->removed == false);   // 旧快照仍被持有 → 挡住删除
    CHECK(blocked->references == 0);    // current 已不引用，DB 也零引用
    CHECK(blocked->matching_referenced);  // 拒删来自 guard 发现的旧快照租约
    CHECK_FALSE(blocked->reason.empty());

    // 被拒后、释放租约前：类型必须原样存活（注册表 + 类型文件字节级不变），
    // 匹配配置原文也不能被删除入口动过
    CHECK(reg.find_component(type_id).has_value());
    CHECK(fix04_types_json(reg.component_types()) == registry_json_at_block);
    CHECK(read(type_file) == type_bytes_before);
    CHECK(read(matching_config) == config_bytes_before);

    // 释放旧快照（save 回执是 revision 数值不持快照；没有别的 shared_ptr 存旧快照）
    old.reset();

    auto retry = api::remove_component_type(*db, type_id, &matching_store);
    if (!retry.has_value())
        MESSAGE("retry after lease release: ", retry.error().format_message());
    CHECK(retry.has_value());   // 释放后必须成功，unexpected 即失败，不能只记日志
    if (retry.has_value())
    {
        CHECK(retry->removed);
        CHECK(retry->references == 0);
        CHECK_FALSE(retry->matching_referenced);  // 成功路径没有配置/租约引用
    }

    // 终态解析真实类型文件：目标 id 精确不在 types 数组，
    // 其余类型与 Registry 当前完整列表一一对应（不能拿 bytes != before 冒充持久删除）
    CHECK_FALSE(reg.find_component(type_id).has_value());
    CHECK(read(type_file) != type_bytes_before);
    const nlohmann::json file_doc = nlohmann::json::parse(read(type_file));
    REQUIRE(file_doc.contains("types"));
    REQUIRE(file_doc["types"].is_array());
    std::vector<std::string> file_ids;
    for (const auto& item : file_doc["types"])
    {
        REQUIRE(item.contains("id"));
        file_ids.push_back(item["id"].get<std::string>());
    }
    CHECK(std::find(file_ids.begin(), file_ids.end(), type_id) == file_ids.end());
    std::vector<std::string> registry_ids;
    for (const auto& entry : reg.component_types())
        registry_ids.push_back(entry.id);
    std::sort(file_ids.begin(), file_ids.end());
    std::sort(registry_ids.begin(), registry_ids.end());
    CHECK(file_ids == registry_ids);
    // 匹配配置原文一个字节都不能被删除入口改过
    CHECK(read(matching_config) == config_bytes_before);
}

// ------------------------------------------------------------
// B2-D2：删除保护的健康、独立性与交错控制。
//  * 坏/未 load 的匹配配置必须挡住元件删除（不能当「0 引用」放行），
//    封装类型删除不依赖匹配配置健康度；
//  * 健康且无引用的 custom 类型必须仍可真实删除；
//  * 保存一条引用与删除该类型必须被同一把配置锁串行化。
// ------------------------------------------------------------

namespace
{

/// 保存线程的存在性查询闸门：只有武装过它的线程（保存工作线程）会被截停，
/// 主线程的 load/重编译与删除线程的 guard 查询直接放行。
thread_local ReadGate* u04b2_existence_gate = nullptr;

/// 工作线程范围内武装本线程的存在性查询闸门；退出作用域（含异常展开）一定清掉。
struct U04B2ScopedExistenceGate
{
    explicit U04B2ScopedExistenceGate(ReadGate& gate) { u04b2_existence_gate = &gate; }
    ~U04B2ScopedExistenceGate() { u04b2_existence_gate = nullptr; }
    U04B2ScopedExistenceGate(const U04B2ScopedExistenceGate&) = delete;
    U04B2ScopedExistenceGate& operator=(const U04B2ScopedExistenceGate&) = delete;
};

/// 一次性启动通知：删除线程在调用真实 API 前置位，主线程看到后立即放行保存闸门。
/// 通知只证明线程已到调用前，不证明已取得 guard。
struct U04B2StartEvent
{
    void notify()
    {
        std::lock_guard lock(mutex_);
        started_ = true;
        condition_.notify_all();
    }
    bool wait(std::chrono::milliseconds limit)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, limit, [this] { return started_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool started_ = false;
};

/// 解析持久化类型文件里的全部 id：持久删除的终态用 id 集合核对，
/// 不能拿 bytes != before 冒充删除成功。
std::vector<std::string> u04b2_file_type_ids(const fs::path& file)
{
    const nlohmann::json doc = nlohmann::json::parse(read(file));
    REQUIRE(doc.contains("types"));
    REQUIRE(doc["types"].is_array());
    std::vector<std::string> ids;
    for (const auto& item : doc["types"])
    {
        REQUIRE(item.contains("id"));
        ids.push_back(item["id"].get<std::string>());
    }
    return ids;
}

std::vector<std::string> u04b2_registry_type_ids(const std::vector<core::TypeEntry>& entries)
{
    std::vector<std::string> ids;
    for (const auto& entry : entries)
        ids.push_back(entry.id);
    return ids;
}

}  // namespace

TEST_CASE_FIXTURE(LibraryFixture,
                  "U04-B2 deletion: unhealthy and unloaded config blocks component deletion but "
                  "leaves package deletion independent")
{
    auto& reg = core::TypeRegistry::instance();
    const fs::path comp_file = temp.path() / "u04b2_sick_comp_types.json";
    const fs::path pkg_file = temp.path() / "u04b2_sick_pkg_types.json";
    reg.bind_component_types_path(comp_file.string());
    reg.bind_package_types_path(pkg_file.string());

    core::TypeEntry comp_entry;
    comp_entry.name = "U04B2SickComp";
    auto comp_added = reg.add_component_type_persistent(comp_entry);
    if (!comp_added)
        MESSAGE("add component failed: ", comp_added.error().format_message());
    REQUIRE(comp_added.has_value());
    const std::string comp_id = *comp_added;

    core::TypeEntry pkg_entry;
    pkg_entry.name = "U04B2SickPkg";
    auto pkg_added = reg.add_package_type_persistent(pkg_entry);
    if (!pkg_added)
        MESSAGE("add package failed: ", pkg_added.error().format_message());
    REQUIRE(pkg_added.has_value());
    const std::string pkg_id = *pkg_added;

    // 前提：目标真实在案（不能用失效路径/不存在的 type 产生假绿）、DB 层零符号引用
    REQUIRE(reg.find_component(comp_id).has_value());
    REQUIRE(reg.find_package(pkg_id).has_value());
    {
        auto session = db->session();
        auto symbols = storage::SymbolRepository(session).count_by_component_type(comp_id);
        REQUIRE(symbols.has_value());
        REQUIRE(*symbols == 0);
    }

    // 新的局部 store（不用 fixture 的健康通道）：
    //  先健康 load、再坏 JSON load —— load_error 未清除；或从未 load —— 无有效配置。
    classifier::TypeMatchingStore sick_store;
    const fs::path good_config = temp.path() / "u04b2_sick_good_matching.json";
    const fs::path bad_config = temp.path() / "u04b2_sick_bad_matching.json";
    SUBCASE("bad JSON load after a healthy one")
    {
        write(good_config, u04a_config(nlohmann::json::array(), nlohmann::json::array(), 0));
        REQUIRE(sick_store.load(good_config.string()).has_value());
        write(bad_config, "{\"revision\": 1, \"rules\": ");
        const auto bad_load = sick_store.load(bad_config.string());
        if (bad_load)
            MESSAGE("bad JSON load unexpectedly succeeded");
        REQUIRE_FALSE(bad_load.has_value());
        CHECK_FALSE(sick_store.load_error().empty());
    }
    SUBCASE("never-loaded local store")
    {
        CHECK_FALSE(sick_store.has_snapshot());
    }

    std::string good_bytes;
    std::string bad_bytes;
    if (fs::exists(good_config))
        good_bytes = read(good_config);
    if (fs::exists(bad_config))
        bad_bytes = read(bad_config);
    const std::string comp_bytes_before = read(comp_file);
    const std::string pkg_bytes_before = read(pkg_file);
    const std::string comp_json_before = fix04_types_json(reg.component_types());
    const std::string pkg_json_before = fix04_types_json(reg.package_types());

    // 坏/未 load 配置不能被当成「0 引用」放行删除：真实入口必须 Result error 且理由指明匹配配置
    auto blocked = api::remove_component_type(*db, comp_id, &sick_store);
    if (blocked)
        MESSAGE("unexpected success with unhealthy matching config");
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error().format_message().find("matching") != std::string::npos);

    // 元件侧一切原样：注册表、类型文件、DB 计数、匹配配置原文（含坏配置文件）
    CHECK(reg.find_component(comp_id).has_value());
    CHECK(fix04_types_json(reg.component_types()) == comp_json_before);
    CHECK(read(comp_file) == comp_bytes_before);
    if (!good_bytes.empty())
        CHECK(read(good_config) == good_bytes);
    if (!bad_bytes.empty())
        CHECK(read(bad_config) == bad_bytes);
    {
        auto session = db->session();
        auto symbols = storage::SymbolRepository(session).count_by_component_type(comp_id);
        REQUIRE(symbols.has_value());
        CHECK(*symbols == 0);
    }

    // 同一个坏状态下，封装类型删除不依赖匹配配置健康度：真实成功并持久移除
    auto pkg_removed = api::remove_package_type(*db, pkg_id);
    if (!pkg_removed)
        MESSAGE("remove package failed: ", pkg_removed.error().format_message());
    REQUIRE(pkg_removed.has_value());
    CHECK(pkg_removed->removed);
    CHECK(pkg_removed->references == 0);
    CHECK_FALSE(pkg_removed->matching_referenced);

    CHECK_FALSE(reg.find_package(pkg_id).has_value());
    CHECK(read(pkg_file) != pkg_bytes_before);
    auto pkg_file_ids = u04b2_file_type_ids(pkg_file);
    CHECK(std::find(pkg_file_ids.begin(), pkg_file_ids.end(), pkg_id) == pkg_file_ids.end());
    auto pkg_registry_ids = u04b2_registry_type_ids(reg.package_types());
    std::sort(pkg_file_ids.begin(), pkg_file_ids.end());
    std::sort(pkg_registry_ids.begin(), pkg_registry_ids.end());
    CHECK(pkg_file_ids == pkg_registry_ids);

    // 封装删除同样不碰元件侧与匹配配置
    CHECK(reg.find_component(comp_id).has_value());
    CHECK(read(comp_file) == comp_bytes_before);
    if (!good_bytes.empty())
        CHECK(read(good_config) == good_bytes);
    if (!bad_bytes.empty())
        CHECK(read(bad_config) == bad_bytes);
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B2 deletion: healthy unreferenced custom types remain deletable")
{
    auto& reg = core::TypeRegistry::instance();
    const fs::path comp_file = temp.path() / "u04b2_free_comp_types.json";
    const fs::path pkg_file = temp.path() / "u04b2_free_pkg_types.json";
    reg.bind_component_types_path(comp_file.string());
    reg.bind_package_types_path(pkg_file.string());

    core::TypeEntry comp_entry;
    comp_entry.name = "U04B2FreeComp";
    auto comp_added = reg.add_component_type_persistent(comp_entry);
    if (!comp_added)
        MESSAGE("add component failed: ", comp_added.error().format_message());
    REQUIRE(comp_added.has_value());
    const std::string comp_id = *comp_added;

    core::TypeEntry pkg_entry;
    pkg_entry.name = "U04B2FreePkg";
    auto pkg_added = reg.add_package_type_persistent(pkg_entry);
    if (!pkg_added)
        MESSAGE("add package failed: ", pkg_added.error().format_message());
    REQUIRE(pkg_added.has_value());
    const std::string pkg_id = *pkg_added;

    // 前提：当前配置健康且不含 custom 引用（guard 直接确认，无引用目标的旧租约）、DB 零符号引用
    {
        auto guard = matching_store.acquire_reference_guard();
        if (!guard)
            MESSAGE("guard failed: ", guard.error().format_message());
        REQUIRE(guard.has_value());
        CHECK_FALSE(guard->references(comp_id));
        CHECK_FALSE(guard->references(pkg_id));
    }
    {
        auto session = db->session();
        auto symbols = storage::SymbolRepository(session).count_by_component_type(comp_id);
        REQUIRE(symbols.has_value());
        REQUIRE(*symbols == 0);
    }

    const std::string comp_bytes_before = read(comp_file);
    const std::string config_bytes_before = read(matching_config);

    auto comp_removed = api::remove_component_type(*db, comp_id, &matching_store);
    if (!comp_removed)
        MESSAGE("remove component failed: ", comp_removed.error().format_message());
    REQUIRE(comp_removed.has_value());
    CHECK(comp_removed->removed);
    CHECK(comp_removed->references == 0);
    CHECK_FALSE(comp_removed->matching_referenced);   // 成功路径没有配置/租约引用

    CHECK_FALSE(reg.find_component(comp_id).has_value());
    CHECK(read(comp_file) != comp_bytes_before);
    auto comp_file_ids = u04b2_file_type_ids(comp_file);
    CHECK(std::find(comp_file_ids.begin(), comp_file_ids.end(), comp_id) == comp_file_ids.end());
    auto comp_registry_ids = u04b2_registry_type_ids(reg.component_types());
    std::sort(comp_file_ids.begin(), comp_file_ids.end());
    std::sort(comp_registry_ids.begin(), comp_registry_ids.end());
    CHECK(comp_file_ids == comp_registry_ids);

    auto pkg_removed = api::remove_package_type(*db, pkg_id);
    if (!pkg_removed)
        MESSAGE("remove package failed: ", pkg_removed.error().format_message());
    REQUIRE(pkg_removed.has_value());
    CHECK(pkg_removed->removed);
    CHECK(pkg_removed->references == 0);
    CHECK_FALSE(pkg_removed->matching_referenced);

    CHECK_FALSE(reg.find_package(pkg_id).has_value());
    auto pkg_file_ids = u04b2_file_type_ids(pkg_file);
    CHECK(std::find(pkg_file_ids.begin(), pkg_file_ids.end(), pkg_id) == pkg_file_ids.end());
    auto pkg_registry_ids = u04b2_registry_type_ids(reg.package_types());
    std::sort(pkg_file_ids.begin(), pkg_file_ids.end());
    std::sort(pkg_registry_ids.begin(), pkg_registry_ids.end());
    CHECK(pkg_file_ids == pkg_registry_ids);

    // 健康删除路径不能动匹配配置原文
    CHECK(read(matching_config) == config_bytes_before);
}

TEST_CASE_FIXTURE(LibraryFixture,
                  "U04-B2 deletion: saving a reference and deleting the type is serialized")
{
    auto& reg = core::TypeRegistry::instance();
    const fs::path type_file = temp.path() / "u04b2_race_types.json";
    reg.bind_component_types_path(type_file.string());

    core::TypeEntry entry;
    entry.name = "U04B2Raced";
    auto added = reg.add_component_type_persistent(entry);
    if (!added)
        MESSAGE("add failed: ", added.error().format_message());
    REQUIRE(added.has_value());
    const std::string type_id = *added;
    REQUIRE(reg.find_component(type_id).has_value());

    // DB 层零符号引用：之后删除拒绝若发生，只能来自新保存的匹配引用
    {
        auto session = db->session();
        auto symbols = storage::SymbolRepository(session).count_by_component_type(type_id);
        REQUIRE(symbols.has_value());
        REQUIRE(*symbols == 0);
    }

    const std::string type_bytes_before = read(type_file);
    const std::string registry_json_before = fix04_types_json(reg.component_types());

    // 局部 race_store：存在性查询先真查真实 Registry 拿到结果，之后才按闸门暂停
    //（暂停时只持保存自己的 store mutex，不持 Registry 锁）。只拦武装线程对 target 的首查。
    ReadGate existence_gate;
    constexpr auto kWatchdog = std::chrono::milliseconds(10000);
    std::atomic<int> target_calls{0};
    std::atomic<bool> target_paused{false};
    classifier::TypeMatchingStore race_store(
        [&](const std::string& queried) -> bool
        {
            const bool found = reg.find_component(queried).has_value();
            if (queried == type_id)
            {
                ++target_calls;
                bool expect_unpaused = false;
                if (u04b2_existence_gate != nullptr && found &&
                    target_paused.compare_exchange_strong(expect_unpaused, true))
                    existence_gate.pause_after_read();
            }
            return found;
        });

    // 初始配置为空：load 阶段不查 target、不碰闸门
    const fs::path race_config = temp.path() / "u04b2_race_matching.json";
    write(race_config, u04a_config(nlohmann::json::array(), nlohmann::json::array(), 0));
    REQUIRE(race_store.load(race_config.string()).has_value());
    const auto installed = race_store.current();
    REQUIRE(installed != nullptr);
    const std::uint64_t expected = installed->revision();

    // 候选：唯一一条引用 target 的规则、无 exceptions —— 这次 save 对 target 恰好查询一次
    const std::string candidate = u04a_config(
        nlohmann::json::array({u04a_rule("u04b2-race", "reference", "^RACE-", type_id)}),
        nlohmann::json::array(), expected);

    // 拥有两个工作线程的 RAII 所有者：任何 REQUIRE 失败/异常展开路径都先放行闸门再 join，
    // 正确实现不会被清理顺序挂住。声明顺序保证它先于 race_store 依赖销毁。
    struct RaceThreads
    {
        RaceThreads(ReadGate& gate, classifier::TypeMatchingStore& store, std::string candidate,
                    std::uint64_t expected)
            : gate(gate), store(store), candidate(std::move(candidate)), expected(expected),
              save_thread(
                  [this]
                  {
                      U04B2ScopedExistenceGate armed(this->gate);
                      try
                      {
                          save_outcome.emplace(this->store.save(this->candidate, this->expected));
                      }
                      catch (const std::exception& thrown)
                      {
                          save_outcome.emplace(std::unexpect,
                                               util::Error::make<util::Error::Kind::ServiceError>(
                                                   std::string("save threw: ") + thrown.what()));
                      }
                      catch (...)
                      {
                          save_outcome.emplace(std::unexpect,
                                               util::Error::make<util::Error::Kind::ServiceError>(
                                                   "save threw an unknown exception"));
                      }
                  })
        {
        }

        void start_delete(storage::Database& db, const std::string& id)
        {
            delete_thread = std::thread(
                [this, &db, &id]
                {
                    try
                    {
                        // 只是「即将调用真实 API」的通知，不证明已取得 guard
                        deletion_started.notify();
                        delete_outcome.emplace(api::remove_component_type(db, id, &store));
                    }
                    catch (const std::exception& thrown)
                    {
                        delete_outcome.emplace(std::unexpect,
                                               util::Error::make<util::Error::Kind::ServiceError>(
                                                   std::string("delete threw: ") + thrown.what()));
                    }
                    catch (...)
                    {
                        delete_outcome.emplace(std::unexpect,
                                               util::Error::make<util::Error::Kind::ServiceError>(
                                                   "delete threw an unknown exception"));
                    }
                });
        }

        void finish()
        {
            gate.release();
            if (save_thread.joinable())
                save_thread.join();
            if (delete_thread.joinable())
                delete_thread.join();
        }
        ~RaceThreads() { finish(); }

        ReadGate& gate;
        classifier::TypeMatchingStore& store;
        std::string candidate;
        std::uint64_t expected;
        U04B2StartEvent deletion_started;
        std::optional<util::Result<std::uint64_t>> save_outcome;  // 声明顺序：早于线程成员
        std::optional<util::Result<api::RemovalOutcome>> delete_outcome;
        std::thread save_thread;
        std::thread delete_thread;
    };

    RaceThreads race(existence_gate, race_store, candidate, expected);

    // 前提：保存已持配置锁、target 的真实存在性查询已返回 true 并在此暂停
    const bool save_paused = existence_gate.wait_until_paused(kWatchdog);
    if (!save_paused)
        MESSAGE("save thread never reached the gated existence query");
    REQUIRE(save_paused);

    // 删除线程通知「即将调用真实 API」后，主线程立即放行保存 ——
    // 不等删除完成才放行（正确实现会等 guard，等删除完成会互相等待而死锁）
    race.start_delete(*db, type_id);
    REQUIRE(race.deletion_started.wait(kWatchdog));
    race.finish();

    // 夹具证据：target 恰好被存在性查询一次且查得 true 后暂停（次数不是产品 API 契约）
    CHECK(target_calls.load() == 1);
    CHECK(target_paused.load());

    // 保存成功并推进 revision：新引用在删除判定之前已真实发布
    REQUIRE(race.save_outcome.has_value());
    if (!race.save_outcome->has_value())
        MESSAGE("save failed: ", race.save_outcome->error().format_message());
    REQUIRE(race.save_outcome->has_value());
    CHECK(**race.save_outcome == expected + 1);
    {
        const auto snapshot = race_store.current();
        REQUIRE(snapshot != nullptr);
        CHECK(snapshot->revision() == expected + 1);
        const auto& ids = snapshot->referenced_type_ids();
        CHECK(std::find(ids.begin(), ids.end(), type_id) != ids.end());
    }

    // 删除被 guard 拒绝：业务引用拒绝（removed=false），不接受 Result 错误伪装
    REQUIRE(race.delete_outcome.has_value());
    if (!race.delete_outcome->has_value())
        MESSAGE("delete errored: ", race.delete_outcome->error().format_message());
    CHECK(race.delete_outcome->has_value());
    if (race.delete_outcome->has_value())
    {
        const auto& outcome = **race.delete_outcome;
        CHECK(outcome.removed == false);
        CHECK(outcome.references == 0);      // DB 层确实零引用
        CHECK(outcome.matching_referenced);  // 拒删来自 guard 发现的新配置引用
        CHECK_FALSE(outcome.reason.empty());
    }

    // 类型与类型文件原样保留，DB 仍零引用
    CHECK(reg.find_component(type_id).has_value());
    CHECK(fix04_types_json(reg.component_types()) == registry_json_before);
    CHECK(read(type_file) == type_bytes_before);
    {
        auto session = db->session();
        auto symbols = storage::SymbolRepository(session).count_by_component_type(type_id);
        REQUIRE(symbols.has_value());
        CHECK(*symbols == 0);
    }

    // 两线程结束后，用默认真实 Registry 查询重新编译磁盘配置：有效且确实引用 target
    const std::string config_after = read(race_config);
    auto recompiled = classifier::compile_matching_config(config_after);
    if (!recompiled)
        MESSAGE("recompile failed: ", recompiled.error().format_message());
    REQUIRE(recompiled.has_value());
    CHECK((*recompiled)->revision() == expected + 1);
    {
        const auto& disk_ids = (*recompiled)->referenced_type_ids();
        CHECK(std::find(disk_ids.begin(), disk_ids.end(), type_id) != disk_ids.end());
    }
}

// ------------------------------------------------------------
// B09：普通导入的错误/告警通道与重试语义
// ------------------------------------------------------------

TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 ordinary import reports discarded decisions as warnings without writing")
{
    std::string stem;
    std::string symbol_name;
    std::string value_text;
    std::string rule_id;
    std::string status_name;
    nlohmann::json rules = nlohmann::json::array();

    SUBCASE("evaluation limit")
    {
        stem = "WarnLimit";
        symbol_name = "U04B1WarnLimit";
        value_text = u04b1_limit_subject();
        rule_id = "b1-warn-limit-heavy";
        status_name = "evaluation_limit";
        rules = nlohmann::json::array(
            {u04a_rule("b1-warn-limit-first", "value", "^a+!$", "Capacitor", 0),
             u04a_rule(rule_id, "value", kU04B1LimitPattern, "Resistor", 100)});
    }
    SUBCASE("engine error")
    {
        stem = "WarnError";
        symbol_name = "U04B1WarnError";
        value_text = "x";
        rule_id = "b1-warn-error-recursive";
        status_name = "evaluation_error";
        rules = nlohmann::json::array(
            {u04a_rule("b1-warn-error-first", "value", "^x$", "Capacitor", 0),
             u04a_rule(rule_id, "value", kU04B1ErrorPattern, "Resistor", 100)});
    }

    u04b1_store_config(matching_store, matching_config, rules);
    U04B1Library lib(*db, matching_store, temp.path(), stem);
    write(lib.target, "(kicad_symbol_lib (version 20231120)\n  (symbol \"" + symbol_name +
                          "\" (property \"Reference\" \"R?\") (property \"Value\" \"" +
                          value_text + "\")))\n");

    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    auto imported = pipe | services::symbols_from{lib.symbol_dir.string(), ""} | services::execute;
    MESSAGE("warning import: symbols=", imported.symbols, " errors=", imported.errors.size(),
            " warnings=", imported.warnings.size(),
            " limited=", imported.evaluation_limited, " engine_errors=", imported.evaluation_errors);
    REQUIRE(imported.symbols == 1);
    CHECK(imported.errors.empty());   // 超限/异常不是操作级失败
    if (stem == "WarnLimit")
    {
        CHECK(imported.evaluation_limited == 1);   // 独立计数
        CHECK(imported.evaluation_errors == 0);
    }
    else
    {
        CHECK(imported.evaluation_limited == 0);
        CHECK(imported.evaluation_errors == 1);
    }
    REQUIRE_FALSE(imported.warnings.empty());     // 不能只有「成功无提示」的外观
    const std::string warning = imported.warnings.front();
    INFO("warning: ", warning);
    CHECK(warning.find(rule_id) != std::string::npos);        // 规则 reason 带出来了
    CHECK(warning.find(status_name) != std::string::npos);    // 状态可解释
    // 作废的决策不写类型：新行仍是未分类占位
    const std::string symbol_id = lib.find_id(symbol_name);
    CHECK(fix02_stored_type(*db, symbol_id) == "Unknown");
    CHECK(u04b1_count_symbols(*db, "component_type = 'Resistor'") == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a failed library import reports the error and retries on the unchanged source")
{
    auto dir = temp.path() / "b8_failed_import";
    write(dir / "Failed.kicad_sym",
          R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "B8Fail" (property "Reference" "R") (property "Value" "v1"))))KICAD");
    const std::string bytes = read(dir / "Failed.kicad_sym");

    u04b1_raw_exec(*db,
                   "CREATE TRIGGER b8_reject_insert BEFORE INSERT ON symbols "
                   "WHEN NEW.name = 'B8Fail' BEGIN SELECT RAISE(ABORT, 'b8 insert blocked'); END");

    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        return pipe | services::symbols_from{dir.string(), ""} | services::execute;
    };

    auto failed = run();
    MESSAGE("failed import: symbols=", failed.symbols, " skipped=", failed.skipped,
            " failed=", failed.errors.size());
    CHECK(failed.symbols == 0);
    CHECK_FALSE(failed.ok());
    CHECK(failed.skipped == 0);                            // imp_failed 不能当 skipped
    REQUIRE_FALSE(failed.errors.empty());
    CHECK(failed.errors.front().find("b8 insert blocked") != std::string::npos);
    CHECK(u04b1_count_symbols(*db, "name = 'B8Fail'") == 0);
    CHECK(u04b1_imported_files(*db) == 0);                 // 回滚过的库不消费来源快照

    // 修好故障后同一份未改动的源必须能重试（mtime 与源字节都不动）
    u04b1_raw_exec(*db, "DROP TRIGGER b8_reject_insert");
    auto retried = run();
    MESSAGE("retry import: symbols=", retried.symbols, " skipped=", retried.skipped);
    CHECK(retried.ok());
    CHECK(retried.symbols == 1);
    CHECK(u04b1_count_symbols(*db, "name = 'B8Fail'") == 1);
    CHECK(read(dir / "Failed.kicad_sym") == bytes);
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_name("B8Fail");
        REQUIRE(row.has_value());
        CHECK(row->component_type_id == "Resistor");       // 真实 Reference R 命中默认配置
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 the footprint phase also counts and classifies symbol libraries")
{
    auto dir = temp.path() / "b8_fp_phase_symbols";
    write(dir / "Mixed.kicad_sym",
          R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "B8Mixed" (property "Reference" "R") (property "Value" "v1"))))KICAD");

    // 只有 footprints_from：目录里的符号库仍要走健康门、被导入并计数
    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    auto r = pipe | services::footprints_from{dir.string()} | services::execute;
    MESSAGE("footprint phase: symbols=", r.symbols, " footprints=", r.footprints, " skipped=",
            r.skipped);
    CHECK(r.symbols == 1);        // Phase 2 的符号计数不能漏
    CHECK(r.footprints == 0);
    CHECK(r.ok());
    CHECK(r.matched == 1);
    CHECK(u04b1_count_symbols(*db, "name = 'B8Mixed'") == 1);
    CHECK(fix02_stored_type(*db, u04b1_symbol_id(*db, "B8Mixed")) == "Resistor");
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 bridges and the manager surface a bad matching config")
{
    U04B1Library lib(*db, matching_store, temp.path(), "Bridges");
    write(lib.target,
          R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "U04B1Bridge" (property "Reference" "R") (property "Value" "b1"))))KICAD");
    write(matching_config, "{ broken");
    REQUIRE_FALSE(matching_store.load(matching_config.string()).has_value());
    REQUIRE_FALSE(matching_store.acquire_business_snapshot().has_value());

    services::LibraryService bridge(db.get(), &matching_store);
    auto symbols = bridge.import_symbol_library(lib.symbol_dir);
    if (symbols)
        MESSAGE("symbol bridge unexpectedly succeeded: symbols=", *symbols);
    CHECK_FALSE(symbols.has_value());
    auto directory = bridge.import_directory(lib.symbol_dir);
    if (directory)
        MESSAGE("directory bridge unexpectedly succeeded: symbols=", directory->symbols);
    CHECK_FALSE(directory.has_value());
    // 纯 3D 模型不做元件类型匹配：models 桥的健康耦合断言不放在这里，
    // 已分离到 "U04-B1 model only imports do not require component matching rules"。
    CHECK(u04b1_count_symbols(*db, "name = 'U04B1Bridge'") == 0);

    api::ImportManager manager(*db, nullptr, &matching_store);
    auto managed = manager.import_directory(lib.symbol_dir.string());
    CHECK_FALSE(managed.ok());
    REQUIRE_FALSE(managed.errors.empty());
    CHECK(managed.errors.front().find("not healthy") != std::string::npos);
    CHECK(managed.symbols == 0);

    // 目录不存在同样是真实错误，不是「没有可导入的文件」。
    // 前提：先把配置恢复成健康状态（execute 的健康门先于目录检查，这是正确顺序，
    // 不能为了迁就夹具把目录检查挪到健康门之前）。
    write(matching_config, classifier::default_matching_config_json());
    REQUIRE(matching_store.load(matching_config.string()).has_value());
    REQUIRE(matching_store.acquire_business_snapshot().has_value());
    auto missing = manager.import_directory((temp.path() / "no_such_dir").string());
    CHECK_FALSE(missing.ok());
    REQUIRE_FALSE(missing.errors.empty());
    CHECK(missing.errors.front().find("not found") != std::string::npos);
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 background import logs a real failure instead of claiming completion")
{
    group();   // 一个真实启用的临时库：后台线程确实有活要干
    util::ConfigStore config(temp.path() / "config.json");
    api::ImportManager manager(*db, &config, &matching_store);

    write(matching_config, "{ broken");
    REQUIRE_FALSE(matching_store.load(matching_config.string()).has_value());
    REQUIRE_FALSE(matching_store.acquire_business_snapshot().has_value());

    // 线程安全的日志捕获：读方在 join（stop_async）之后才读
    std::mutex log_mutex;
    std::vector<std::string> logs;
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [&log_mutex, &logs](const spdlog::details::log_msg& message)
        {
            std::lock_guard lock(log_mutex);
            logs.emplace_back(message.payload.data(), message.payload.size());
        });
    auto logger = std::make_shared<spdlog::logger>("b1-capture", sink);
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);

    manager.start_async();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (manager.is_running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_FALSE(manager.is_running());    // 有界完成，不是挂住
    manager.stop_async();                 // join：之后读日志有 happens-before

    bool claimed_done = false;
    bool reported_failure = false;
    {
        std::lock_guard lock(log_mutex);
        for (const auto& line : logs)
        {
            if (line.find("Auto-import done") != std::string::npos) claimed_done = true;
            if (line.find("matching config is not healthy") != std::string::npos)
                reported_failure = true;
        }
    }
    // 读完立刻摘掉捕获 sink：后续析构/日志不再触碰这块局部缓冲
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(
        "b1-capture-off", std::make_shared<spdlog::sinks::null_sink_mt>()));
    CHECK_FALSE(claimed_done);            // 坏配置不许输出完成
    CHECK(reported_failure);              // 实际错误消息必须在日志里
    CHECK(manager.orchestrator()->import_seq() == 0);   // 一行都没提交 → 不发布完成信号
    CHECK(u04b1_count_symbols(*db, "name = 'R'") == 0);  // 也没有偷偷写入
}

// ============================================================
// U04-B1 审查后的 tests-only 阶段：先证明新边界，不提前返工产品
//
// 只调用现有真实入口（ClassificationService / ImportPipeline / LibraryService /
// ImportManager）+ 真实 SQLite authorizer / trigger，不引用尚未存在的 DTO 字段。
// 全部临时目录与临时 DB；authorizer 回调只返回码并给计数器加一，不执行 SQL、
// 不断言、不抛异常；装/卸都走 RAII，REQUIRE 早退或异常也一定摘掉。
// ============================================================

namespace
{

/// SQL 字面量：单引号按 SQL 规则转义（不用 JSON 转义冒充 SQL 引用）
std::string u04b1_sql_literal(const std::string& text)
{
    std::string out = "'";
    for (const char c : text)
    {
        if (c == '\'')
            out += "''";
        else
            out += c;
    }
    out += "'";
    return out;
}

/// 真实 SQLite authorizer 的装/卸 RAII：借连接装 hook 后立刻释放 session，
/// 析构时无论正常结束、REQUIRE 早退还是异常都把回调摘掉。
struct U04B1Authorizer
{
    using Callback = int (*)(void*, int, const char*, const char*, const char*, const char*);

    sqlite3* handle = nullptr;
    std::atomic<int> hits{0};

    U04B1Authorizer(storage::Database& db, Callback callback)
    {
        auto session = db.session();   // 只用这条连接装 hook，随即释放
        handle = session.handle();
        REQUIRE(sqlite3_set_authorizer(handle, callback, &hits) == SQLITE_OK);
    }
    /// 显式摘钩（幂等）：需要「先 join 后台线程、再摘钩」时由调用方提前摘掉，
    /// 析构仍会兜底；hits 是普通计数器，摘钩后读依旧安全。
    void remove()
    {
        if (handle != nullptr)
        {
            sqlite3_set_authorizer(handle, nullptr, nullptr);
            handle = nullptr;
        }
    }
    ~U04B1Authorizer() { remove(); }
    U04B1Authorizer(const U04B1Authorizer&) = delete;
    U04B1Authorizer& operator=(const U04B1Authorizer&) = delete;
    U04B1Authorizer(U04B1Authorizer&&) = delete;
    U04B1Authorizer& operator=(U04B1Authorizer&&) = delete;
};

/// 只拒显式 BEGIN；COMMIT / ROLLBACK 与全部读写一律放行
int u04b1_deny_begin(void* ctx, int action, const char* arg1, const char*, const char*, const char*)
{
    if (action != SQLITE_TRANSACTION || arg1 == nullptr)
        return SQLITE_OK;
    if (std::string_view(arg1) != "BEGIN")
        return SQLITE_OK;
    static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    return SQLITE_DENY;
}

/// 只拒显式 COMMIT；ROLLBACK 必须放行，让真实 DbSession 析构完成回滚
int u04b1_deny_commit(void* ctx, int action, const char* arg1, const char*, const char*, const char*)
{
    if (action != SQLITE_TRANSACTION || arg1 == nullptr)
        return SQLITE_OK;
    if (std::string_view(arg1) != "COMMIT")
        return SQLITE_OK;
    static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    return SQLITE_DENY;
}

/// deny-once：只拒第一次显式 ROLLBACK（SQLITE_TRANSACTION，arg1 = "ROLLBACK"），
/// 随后放行所有 ROLLBACK，让真实 DbSession 析构的重试能清理事务。
/// callback 只做原子计数 + 返回码：无 SQL、无断言、不抛异常。
/// hits 语义：1 = 显式 ROLLBACK 被拒；2 = 析构重试被放行（事务已清理）。
int u04b1_deny_first_rollback(void* ctx, int action, const char* arg1, const char*,
                              const char*, const char*)
{
    if (action != SQLITE_TRANSACTION || arg1 == nullptr)
        return SQLITE_OK;
    if (std::string_view(arg1) != "ROLLBACK")
        return SQLITE_OK;
    auto* hits = static_cast<std::atomic<int>*>(ctx);
    if (hits->fetch_add(1, std::memory_order_relaxed) != 0)
        return SQLITE_OK;   // 第二次起放行：析构重试路径必须能清理
    return SQLITE_DENY;
}

/// 只拒 SAVEPOINT 的 ROLLBACK 控制（SQLITE_SAVEPOINT 动作）；
/// SAVEPOINT / RELEASE 与真事务 BEGIN/COMMIT/ROLLBACK 一律放行。
/// 出处：SQLite 官方 src/build.c 的 sqlite3Savepoint，az = {"BEGIN", "RELEASE", "ROLLBACK"}
/// —— ROLLBACK TO x 的 arg1 是 "ROLLBACK"，不是 SQL 文本 "ROLLBACK TO"；
/// 封装批的 SAVEPOINT 控制走 SQLITE_SAVEPOINT，不会错误计入 COMMIT 次数
int u04b1_deny_savepoint_rollback(void* ctx, int action, const char* arg1, const char*,
                                  const char*, const char*)
{
    if (action != SQLITE_SAVEPOINT || arg1 == nullptr)
        return SQLITE_OK;
    if (std::string_view(arg1) != "ROLLBACK")
        return SQLITE_OK;
    static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    return SQLITE_DENY;
}

/// 只拒 component_libraries 的读（启用库清单读不出来），其余读/写/事务全放行
int u04b1_deny_component_libraries_read(void* ctx, int action, const char* arg1, const char*,
                                        const char*, const char*)
{
    if (action != SQLITE_READ || arg1 == nullptr)
        return SQLITE_OK;
    if (std::string_view(arg1) != "component_libraries")
        return SQLITE_OK;
    static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
    return SQLITE_DENY;
}

/// U04-B1 探针专用窄 helper：同一条连接上的双计数 hook —— sp_hits 独立拒
/// SAVEPOINT 的 ROLLBACK 控制，txn_hits 只拒第一次显式 ROLLBACK（析构重试放行）。
/// 两个计数互不影响，不复用/不改旧 helper 语义；callback 只做原子计数 +
/// 返回码：无 SQL、无断言、不抛异常；装/卸走 U04B1Authorizer 同一 RAII 模式。
struct U04B1DualDenyHook
{
    std::atomic<int> sp_hits{0};
    std::atomic<int> txn_hits{0};
    sqlite3* handle = nullptr;

    static int trampoline(void* ctx, int action, const char* arg1, const char*,
                          const char*, const char*)
    {
        auto* self = static_cast<U04B1DualDenyHook*>(ctx);
        if (action == SQLITE_SAVEPOINT)
        {
            if (arg1 != nullptr && std::string_view(arg1) == "ROLLBACK")
            {
                self->sp_hits.fetch_add(1, std::memory_order_relaxed);
                return SQLITE_DENY;
            }
            return SQLITE_OK;
        }
        if (action == SQLITE_TRANSACTION && arg1 != nullptr
            && std::string_view(arg1) == "ROLLBACK")
        {
            // 只拒第一次显式 ROLLBACK；DbSession 析构重试必须放行以清理事务
            return self->txn_hits.fetch_add(1, std::memory_order_relaxed) == 0
                       ? SQLITE_DENY
                       : SQLITE_OK;
        }
        return SQLITE_OK;
    }

    explicit U04B1DualDenyHook(storage::Database& db)
    {
        auto session = db.session();   // 只用这条连接装 hook，随即释放
        handle = session.handle();
        REQUIRE(sqlite3_set_authorizer(handle, &trampoline, this) == SQLITE_OK);
    }
    /// 显式摘钩（幂等）：业务调用结束后、任何 DB 检查之前由调用方摘掉，
    /// 析构全路径兜底
    void remove()
    {
        if (handle != nullptr)
        {
            sqlite3_set_authorizer(handle, nullptr, nullptr);
            handle = nullptr;
        }
    }
    ~U04B1DualDenyHook() { remove(); }
    U04B1DualDenyHook(const U04B1DualDenyHook&) = delete;
    U04B1DualDenyHook& operator=(const U04B1DualDenyHook&) = delete;
    U04B1DualDenyHook(U04B1DualDenyHook&&) = delete;
    U04B1DualDenyHook& operator=(U04B1DualDenyHook&&) = delete;
};

/// 在 preview 明细里按 symbol_id 取结果（缺失返回 nullptr，由调用方判夹具失败）
const services::ClassificationService::Result* u04b1_detail(
    const services::ClassificationService::Summary& summary, const std::string& symbol_id)
{
    for (const auto& item : summary.results)
        if (item.symbol_id == symbol_id)
            return &item;
    return nullptr;
}

/// 六个 eligible 决策状态之和（protected 是独立轴，不在这六个里面）
int u04b1_eligible_states(const services::ClassificationService::Summary& summary)
{
    return summary.matched + summary.unmatched + summary.conflicts + summary.input_too_long
           + summary.evaluation_limited + summary.evaluation_errors;
}

}  // namespace

// ------------------------------------------------------------
// C01：编译好的 draft 不能绕过 DB 预览的健康门
// draft 只替换求值快照，不替代「已保存配置健康」这一业务前提
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a compiled draft cannot bypass the database preview health gate")
{
    U04B1Library lib(*db, matching_store, temp.path(), "DraftGate");
    const std::string row = lib.insert(u04b1_part("U04B1DraftGate", "U", std::string("U")));

    // 真实核心严格编译的候选（未保存）：对这个来源结论唯一
    const auto draft = u04a_snapshot(u04a_config(
        nlohmann::json::array({u04a_rule("b1-draft-gate", "name", "^U04B1DraftGate$", "Resistor")}),
        nlohmann::json::array(), 42));
    const core::Symbol before = lib.read(row);
    const auto draft_decision = draft->evaluate(classifier::MatchSample::from_symbol(before));
    if (draft_decision.status != classifier::MatchStatus::Matched)
        MESSAGE("draft premise: ", classifier::match_status_name(draft_decision.status), " ",
                draft_decision.reason);
    REQUIRE(draft_decision.status == classifier::MatchStatus::Matched);   // 前提：候选合法
    REQUIRE(draft_decision.type_id == "Resistor");

    const int symbols_before = count_rows(*db, "symbols");
    const auto snapshots_before = u04b1_imported_map(*db);

    SUBCASE("a bad saved config still blocks a draft preview")
    {
        write(matching_config, "{ this is not a matching config");
        REQUIRE_FALSE(matching_store.load(matching_config.string()).has_value());
        REQUIRE(matching_store.current() != nullptr);   // last-valid 仍可用于展示
        REQUIRE_FALSE(matching_store.acquire_business_snapshot().has_value());

        services::ClassificationService service(db.get(), &matching_store);
        auto preview = service.preview_all(draft);
        if (preview)
            MESSAGE("bad-store draft preview unexpectedly succeeded: total=", preview->total,
                    " matched=", preview->matched, " type_updated=", preview->type_updated);
        CHECK_FALSE(preview.has_value());   // 红：draft 非空就整段跳过业务健康领取
        CHECK(fix02_stored_type(*db, row) == "Unknown");   // 预览不写类型
        CHECK(count_rows(*db, "symbols") == symbols_before);
        u04b1_check_snapshots_equal(snapshots_before, u04b1_imported_map(*db));
    }

    SUBCASE("a never loaded store rejects a draft preview")
    {
        // 显式局部 store：不 seed、不读 singleton、不借 fixture 的 last-valid
        classifier::TypeMatchingStore fresh;
        REQUIRE_FALSE(fresh.acquire_business_snapshot().has_value());

        services::ClassificationService service(db.get(), &fresh);
        auto preview = service.preview_all(draft);
        if (preview)
            MESSAGE("unloaded-store draft preview unexpectedly succeeded: total=",
                    preview->total);
        CHECK_FALSE(preview.has_value());   // 红：未 load 的 store 同样必须拒绝
        CHECK(fix02_stored_type(*db, row) == "Unknown");
        CHECK(count_rows(*db, "symbols") == symbols_before);
        u04b1_check_snapshots_equal(snapshots_before, u04b1_imported_map(*db));
    }
}

// ------------------------------------------------------------
// C02：protected 与 eligible 决策形成分区（受保护行不进六状态计数）
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 protected rows are outside the eligible matching decision counts")
{
    SUBCASE("known rows are protected and stay outside the eligible counts")
    {
        write(matching_config,
              u04a_config(
                  nlohmann::json::array({u04a_rule("b1-partition", "reference", "^U$", "OpAmp")}),
                  nlohmann::json::array(), 11));
        REQUIRE(matching_store.load(matching_config.string()).has_value());

        U04B1Library lib(*db, matching_store, temp.path(), "Partition");
        const std::string known =
            lib.insert(u04b1_part("U04B1PartKnown", "U", std::string("U"), "", "", "OpAmp"));
        const std::string matched =
            lib.insert(u04b1_part("U04B1PartMatched", "U", std::string("U")));
        const std::string unmatched =
            lib.insert(u04b1_part("U04B1PartUnmatched", "U", std::string("R?")));

        // 前提：同一配置下核心对 known 行也会给出 OpAmp 候选 —— 服务必须保护它，
        // 而不是把它算成 eligible 候选
        const auto known_decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(lib.read(known)));
        REQUIRE(known_decision.status == classifier::MatchStatus::Matched);
        REQUIRE(known_decision.type_id == "OpAmp");
        const auto unmatched_decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(lib.read(unmatched)));
        REQUIRE(unmatched_decision.status == classifier::MatchStatus::Unmatched);

        services::ClassificationService service(db.get(), &matching_store);
        auto preview = service.preview_all();
        if (!preview)
            MESSAGE("preview failed: ", preview.error().format_message());
        REQUIRE(preview.has_value());
        MESSAGE("partition: total=", preview->total, " protected=", preview->protected_count,
                " matched=", preview->matched, " unmatched=", preview->unmatched,
                " limited=", preview->evaluation_limited);

        CHECK(preview->total == 3);
        CHECK(preview->protected_count == 1);   // 红：当前先求值全行、再独立累加保护计数
        CHECK(preview->matched == 1);
        CHECK(preview->unmatched == 1);         // 红：当前 protected 行也被算成 unmatched
        CHECK(preview->conflicts == 0);
        CHECK(preview->input_too_long == 0);
        CHECK(preview->evaluation_limited == 0);
        CHECK(preview->evaluation_errors == 0);
        CHECK(preview->protected_count + u04b1_eligible_states(*preview) == preview->total);
        CHECK(preview->type_updated == 0);      // 预览不写
        CHECK(fix02_stored_type(*db, known) == "OpAmp");
        CHECK(fix02_stored_type(*db, matched) == "Unknown");
        CHECK(fix02_stored_type(*db, unmatched) == "Unknown");

        const auto* known_detail = u04b1_detail(*preview, known);
        REQUIRE(known_detail != nullptr);
        CHECK(known_detail->status == "protected");   // 红：当前直接是核心状态名
        CHECK(known_detail->type_id == "OpAmp");      // 保护 = 显示原值，不是候选
        const auto* matched_detail = u04b1_detail(*preview, matched);
        REQUIRE(matched_detail != nullptr);
        CHECK(matched_detail->status == "matched");
        const auto* unmatched_detail = u04b1_detail(*preview, unmatched);
        REQUIRE(unmatched_detail != nullptr);
        CHECK(unmatched_detail->status == "unmatched");
    }

    SUBCASE("a known row with a limit-triggering value is not evaluated")
    {
        write(matching_config,
              u04a_config(nlohmann::json::array(
                              {u04a_rule("b1-partition-first", "value", "^a+!$", "Capacitor", 0),
                               u04a_rule("b1-partition-heavy", "value", kU04B1LimitPattern,
                                         "Resistor", 100)}),
                          nlohmann::json::array(), 12));
        REQUIRE(matching_store.load(matching_config.string()).has_value());

        U04B1Library lib(*db, matching_store, temp.path(), "PartitionLimit");
        const std::string known = lib.insert(u04b1_part("U04B1LimitKnown", "U", std::string("U"),
                                                        u04b1_limit_subject(), "", "OpAmp"));

        // 前提：核心单独求值这条输入确实超限（不是 unmatched，也不是引擎报错）
        const auto decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(lib.read(known)));
        if (decision.status != classifier::MatchStatus::EvaluationLimit)
            MESSAGE("limit premise: ", classifier::match_status_name(decision.status), " ",
                    decision.reason);
        REQUIRE(decision.status == classifier::MatchStatus::EvaluationLimit);

        services::ClassificationService service(db.get(), &matching_store);
        auto preview = service.preview_all();
        REQUIRE(preview.has_value());
        CHECK(preview->total == 1);
        CHECK(preview->protected_count == 1);
        CHECK(preview->evaluation_limited == 0);   // 红：known 行不该跑无意义的重正则
        CHECK(fix02_stored_type(*db, known) == "OpAmp");
    }
}

// ------------------------------------------------------------
// C03：纯模型导入不做元件类型匹配 —— 坏 / 未 load 的元件规则不能阻止它
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 model only imports do not require component matching rules")
{
    auto break_saved_config = [&]
    {
        write(matching_config, "{ this is not a matching config");
        REQUIRE_FALSE(matching_store.load(matching_config.string()).has_value());
        REQUIRE_FALSE(matching_store.acquire_business_snapshot().has_value());
    };

    SUBCASE("a bad saved config does not block a scan of real .step files")
    {
        const fs::path models_dir = temp.path() / "b1_models_bad";
        write(models_dir / "Part.step", "ISO-10303-21;\nEND-ISO-10303-21;\n");
        const std::string bytes = read(models_dir / "Part.step");
        break_saved_config();

        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto r = pipe | services::models_from{models_dir.string()} | services::execute;
        MESSAGE("bad-store model scan: models=", r.models_3d, " errors=", r.errors.size(),
                " symbols=", r.symbols, " matched=", r.matched);
        CHECK(r.ok());            // 红：pending_models 也被当成健康门前提
        CHECK(r.errors.empty());
        CHECK(r.models_3d == 1);
        CHECK(r.matched == 0);
        CHECK(r.unmatched == 0);
        CHECK(r.conflicts == 0);
        CHECK(r.input_too_long == 0);
        CHECK(r.evaluation_limited == 0);
        CHECK(r.evaluation_errors == 0);
        CHECK(count_rows(*db, "symbols") == 0);
        CHECK(count_rows(*db, "component_libraries") == 0);

        // 仓储恰一行、路径正确、源文件 mtime 已记账。
        // 这些是被测产品的后置条件：产品失败时只报红，不用 REQUIRE 终止 case
        // （否则同 case 的 fresh / bridge 两个 SUBCASE 根本跑不到）；解引用一律 guard。
        {
            auto session = db->session();
            auto all = storage::Model3DRepository(session).find_all();
            if (!all)
                MESSAGE("model repository read failed: ", all.error().format_message());
            CHECK(all.has_value());
            if (all.has_value())
            {
                CHECK(all->size() == 1);
                if (!all->empty())
                    CHECK(fs::weakly_canonical(fs::path(all->front().file_path())) ==
                          fs::weakly_canonical(models_dir / "Part.step"));
            }
        }
        const auto snapshots = u04b1_imported_map(*db);
        CHECK(snapshots.size() == 1);
        // Part.step 恰一条快照、mtime 已记账：与原来同强度，缺键/重复键只报红不 REQUIRE
        int snapshot_count = 0;
        int64_t snapshot_mtime = -1;
        for (const auto& [key, value] : snapshots)
            if (fs::path(key).filename() == "Part.step")
            {
                ++snapshot_count;
                snapshot_mtime = value;
            }
        CHECK(snapshot_count == 1);
        CHECK(snapshot_mtime > 0);
        CHECK(read(models_dir / "Part.step") == bytes);   // 源字节不被改写
    }

    SUBCASE("a never loaded store does not block a scan of real .step files")
    {
        const fs::path models_dir = temp.path() / "b1_models_fresh";
        write(models_dir / "Part.step", "STEP-data");
        classifier::TypeMatchingStore fresh;   // 显式局部：不 seed
        REQUIRE_FALSE(fresh.acquire_business_snapshot().has_value());

        services::ImportPipeline pipe(*db, nullptr, false, &fresh);
        auto r = pipe | services::models_from{models_dir.string()} | services::execute;
        MESSAGE("fresh-store model scan: models=", r.models_3d, " errors=", r.errors.size());
        CHECK(r.ok());   // 红：未 load 的元件规则不该拦住纯模型
        CHECK(r.models_3d == 1);
        CHECK(count_rows(*db, "models_3d") == 1);
        CHECK(u04b1_imported_files(*db) == 1);
    }

    SUBCASE("the model bridge reports the count for an independent directory")
    {
        const fs::path models_dir = temp.path() / "b1_models_bridge";
        write(models_dir / "Bridge.step", "STEP-data");
        break_saved_config();

        services::LibraryService bridge(db.get(), &matching_store);
        auto scanned = bridge.scan_3d_models(models_dir);
        if (!scanned)
            MESSAGE("model bridge failed: ", scanned.error().format_message());
        CHECK(scanned.has_value());   // 红：整数桥把健康失败当 unexpected
        if (scanned.has_value())
            CHECK(*scanned == 1);
        CHECK(count_rows(*db, "models_3d") == 1);
    }
}

// ------------------------------------------------------------
// 附件 1：完整失败数量不能被 100 条明细上限截断
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 import failure counts survive the 100 message cap")
{
    SUBCASE("101 rejected libraries keep the full error count")
    {
        auto dir = temp.path() / "b9_cap";
        for (int index = 0; index <= 100; ++index)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "B9Fail_%03d", index);
            write(dir / (std::string(name) + ".kicad_sym"),
                  std::string("(kicad_symbol_lib (version 20231120)\n  (symbol \"") + name
                      + "\" (property \"Reference\" \"R\") (property \"Value\" \"v\")))\n");
        }
        write(dir / "B9Good.kicad_sym",
              "(kicad_symbol_lib (version 20231120)\n  (symbol \"B9Good\" (property \"Reference\" "
              "\"R\") (property \"Value\" \"v\")))\n");

        u04b1_raw_exec(*db,
                       "CREATE TRIGGER b9_reject_many BEFORE INSERT ON symbols "
                       "WHEN NEW.name LIKE 'B9Fail_%' "
                       "BEGIN SELECT RAISE(ABORT, 'b9 rejected library'); END");

        services::LibraryService libraries(db.get(), &matching_store);
        auto imported = libraries.import_directory(dir);
        if (!imported)
            MESSAGE("capped import failed: ", imported.error().format_message());
        REQUIRE(imported.has_value());   // 前提：1 个成功库已提交 → 部分成功形状成立
        MESSAGE("capped import: symbols=", imported->symbols, " errors=", imported->errors,
                " messages=", imported->messages.size(), " skipped=", imported->skipped);
        CHECK(imported->symbols == 1);
        CHECK(imported->errors == 101);   // 红：当前用 errors.size()，被明细上限截成 100
        CHECK(imported->failed_files == 101);   // 101 个没有提交的失败库文件，不是错误事件数
        CHECK(imported->messages.size() == 100);   // 明细仍受 100 条上限
        CHECK(imported->warnings == 0);            // 被拒库不产生告警：完整告警数就是 0
        CHECK_FALSE(imported->ok());
        CHECK(u04b1_count_symbols(*db, "name LIKE 'B9Fail_%'") == 0);
        CHECK(u04b1_count_symbols(*db, "name = 'B9Good'") == 1);
        CHECK(u04b1_imported_files(*db) == 1);
        u04b1_raw_exec(*db, "DROP TRIGGER b9_reject_many");
    }

    SUBCASE("101 limited inputs keep the decision count while details stay capped")
    {
        auto dir = temp.path() / "b9_limit";
        std::string text = "(kicad_symbol_lib (version 20231120)\n";
        for (int index = 0; index < 101; ++index)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "B9Limit_%03d", index);
            text += std::string("  (symbol \"") + name
                    + "\" (property \"Reference\" \"R\") (property \"Value\" \""
                    + u04b1_limit_subject() + "\"))\n";
        }
        text += ")\n";
        write(dir / "B9Limit.kicad_sym", text);

        u04b1_store_config(matching_store, matching_config,
                           nlohmann::json::array(
                               {u04a_rule("b9-limit-first", "value", "^a+!$", "Capacitor", 0),
                                u04a_rule("b9-limit-heavy", "value", kU04B1LimitPattern,
                                          "Resistor", 100)}));

        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto r = pipe | services::symbols_from{dir.string(), ""} | services::execute;
        MESSAGE("limit batch: symbols=", r.symbols, " limited=", r.evaluation_limited,
                " warnings=", r.warnings.size(), " errors=", r.errors.size());
        CHECK(r.symbols == 1);
        CHECK(r.ok());
        CHECK(r.errors.empty());
        CHECK(r.evaluation_limited == 101);   // 决策计数不受明细上限影响（绿对照）
        CHECK(r.evaluation_errors == 0);
        CHECK(r.warnings.size() == 100);      // 明细仍然受限
        CHECK(r.warning_count == 101);        // 完整告警事件数不受明细上限影响
        CHECK(r.error_count == 0);            // 求值受限不是错误
        CHECK(r.failed_files == 0);           // 库已提交：没有失败文件
        CHECK(u04b1_count_symbols(*db, "component_type = 'Unknown'") == 101);
        CHECK(u04b1_count_symbols(*db, "component_type = 'Resistor'") == 0);
    }

    SUBCASE("mixed errors and warnings keep both full counts in the merged messages")
    {
        auto dir = temp.path() / "b9_mixed";
        // 告警库：3 个求值受限的符号（真实 warning 事件）
        std::string text = "(kicad_symbol_lib (version 20231120)\n";
        for (int index = 0; index < 3; ++index)
        {
            text += std::string("  (symbol \"B9Warn_0") + std::to_string(index)
                    + "\" (property \"Reference\" \"R\") (property \"Value\" \""
                    + u04b1_limit_subject() + "\"))\n";
        }
        text += ")\n";
        write(dir / "B9Warn.kicad_sym", text);
        // 错误库：唯一符号的 INSERT 被拒 → 整库事务回滚
        write(dir / "B9Fail.kicad_sym",
              "(kicad_symbol_lib (version 20231120)\n  (symbol \"B9FailSym\" (property "
              "\"Reference\" \"R\") (property \"Value\" \"v\")))\n");
        u04b1_raw_exec(*db,
                       "CREATE TRIGGER b9_mixed_reject BEFORE INSERT ON symbols "
                       "WHEN NEW.name = 'B9FailSym' "
                       "BEGIN SELECT RAISE(ABORT, 'b9 mixed library rejected'); END");

        u04b1_store_config(matching_store, matching_config,
                           nlohmann::json::array(
                               {u04a_rule("b9-mix-first", "value", "^a+!$", "Capacitor", 0),
                                u04a_rule("b9-mix-heavy", "value", kU04B1LimitPattern,
                                          "Resistor", 100)}));

        services::LibraryService libraries(db.get(), &matching_store);
        auto imported = libraries.import_directory(dir);
        REQUIRE(imported.has_value());   // 告警库已提交 → 部分成功形状成立
        MESSAGE("mixed import: symbols=", imported->symbols, " errors=", imported->errors,
                " warnings=", imported->warnings, " failed_files=", imported->failed_files,
                " messages=", imported->messages.size());
        CHECK(imported->symbols == 1);
        CHECK(imported->errors == 1);        // 完整错误事件数
        CHECK(imported->warnings == 3);      // 完整告警事件数
        CHECK(imported->failed_files == 1);  // 回滚的失败库文件
        CHECK_FALSE(imported->ok());
        CHECK(imported->messages.size() == 4);           // 1 错误 + 3 告警全部保留
        CHECK(imported->messages.front().find("b9 mixed library rejected")
              != std::string::npos);                     // 错误在告警之前
        CHECK(u04b1_count_symbols(*db, "name = 'B9FailSym'") == 0);
        CHECK(u04b1_count_symbols(*db, "name LIKE 'B9Warn_%'") == 3);
        u04b1_raw_exec(*db, "DROP TRIGGER b9_mixed_reject");
    }
}

// ------------------------------------------------------------
// 附件 1c：真实跨 merged cap 的混合场景 —— 一个 101 求值受限符号的库（告警，
// 整库提交）+ 一个独立被 trigger 拒绝的库（错误，整库回滚），同目录一次
// 真实 LibraryService.import_directory：完整计数与 100 条明细上限并存，错误在前
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 mixed import across the merged cap keeps full counts with the error first")
{
    auto dir = temp.path() / "b9_cap_mixed";
    // 告警库：101 个求值受限符号（真实 warning 事件），整库可提交
    std::string text = "(kicad_symbol_lib (version 20231120)\n";
    for (int index = 0; index < 101; ++index)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "B9MixCap_%03d", index);
        text += std::string("  (symbol \"") + name
                + "\" (property \"Reference\" \"R\") (property \"Value\" \""
                + u04b1_limit_subject() + "\"))\n";
    }
    text += ")\n";
    write(dir / "B9MixLimit.kicad_sym", text);
    // 错误库：独立文件，唯一符号的 INSERT 被拒 → 整库事务回滚
    write(dir / "B9MixFail.kicad_sym",
          "(kicad_symbol_lib (version 20231120)\n  (symbol \"B9MixFailSym\" (property "
          "\"Reference\" \"R\") (property \"Value\" \"v\")))\n");

    u04b1_raw_exec(*db,
                   "CREATE TRIGGER b9_cap_mixed_reject BEFORE INSERT ON symbols "
                   "WHEN NEW.name = 'B9MixFailSym' "
                   "BEGIN SELECT RAISE(ABORT, 'b9 mixed cap library rejected'); END");

    // 与已验证的限额配置同形状：第一规则会先命中、再因超限作废，重规则触发求值受限
    u04b1_store_config(matching_store, matching_config,
                       nlohmann::json::array(
                           {u04a_rule("b9-capmix-first", "value", "^a+!$", "Capacitor", 0),
                            u04a_rule("b9-capmix-heavy", "value", kU04B1LimitPattern,
                                      "Resistor", 100)}));

    services::LibraryService libraries(db.get(), &matching_store);
    auto imported = libraries.import_directory(dir);
    if (!imported)
        MESSAGE("mixed cap import failed: ", imported.error().format_message());
    REQUIRE(imported.has_value());   // 前提：告警库已提交 → 部分成功形状成立
    MESSAGE("mixed cap import: symbols=", imported->symbols, " errors=", imported->errors,
            " warnings=", imported->warnings, " failed_files=", imported->failed_files,
            " messages=", imported->messages.size());
    CHECK(imported->symbols == 1);               // 告警库提交
    CHECK(imported->errors == 1);                // 完整错误事件数
    CHECK(imported->warnings == 101);            // 完整告警事件数，不受明细上限影响
    CHECK(imported->failed_files == 1);          // 回滚的失败库文件
    CHECK_FALSE(imported->ok());
    CHECK(imported->messages.size() == 100);     // merged cap：1 错误 + 前 99 条告警
    REQUIRE_FALSE(imported->messages.empty());   // front 的非空 guard：无明细不得解引用
    CHECK(imported->messages.front().find("b9 mixed cap library rejected")
          != std::string::npos);                 // 错误明细在前
    CHECK(u04b1_count_symbols(*db, "name LIKE 'B9MixCap_%'") == 101);
    CHECK(u04b1_count_symbols(*db, "component_type = 'Unknown'") == 101);
    CHECK(u04b1_count_symbols(*db, "name = 'B9MixFailSym'") == 0);   // 拒绝库未提交
    CHECK(u04b1_imported_files(*db) == 1);       // 只有告警库的 mtime 快照
    {
        const auto snapshots = u04b1_imported_map(*db);
        int fail_snapshots = 0;
        for (const auto& [key, value] : snapshots)
        {
            (void)value;
            if (fs::path(key).filename() == "B9MixFail.kicad_sym")
                ++fail_snapshots;
        }
        CHECK(fail_snapshots == 0);   // 拒绝库没有 mtime 快照
    }
    u04b1_raw_exec(*db, "DROP TRIGGER b9_cap_mixed_reject");
}

// ------------------------------------------------------------
// 附件 2：关联阶段 BEGIN 失败必须可见，并且可以去故障重试
// 无任何 pending 步骤：唯一显式 BEGIN 就是 Phase 4 关联事务
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 link begin failure is an import error and can retry")
{
    const core::Symbol sym = symbol();   // 真实符号行：Footprint = Pkg:Old
    const std::string symbol_id = sym.id();
    const core::Footprint fp = footprint_in("Pkg.pretty", "Old", true, true);
    const std::string source_before = read(file);
    REQUIRE(count_rows(*db, "symbol_footprint_links") == 0);

    {
        U04B1Authorizer hook(*db, &u04b1_deny_begin);
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto r = pipe | services::execute;
        MESSAGE("denied begin: errors=", r.errors.size(), " symbols=", r.symbols);
        // 无 pending 步骤：Phase 4 关联事务是唯一显式 BEGIN（隐式事务不通知 authorizer）
        REQUIRE(hook.hits.load() == 1);   // 前提：关联事务真的尝试过一次 BEGIN
        CHECK_FALSE(r.ok());             // 红：当前 begin 失败只 log，result 仍是 ok
        CHECK_FALSE(r.errors.empty());
        if (!r.errors.empty())
        {
            MESSAGE("link begin error: ", r.errors.front());
            CHECK(r.errors.front().find("link phase") != std::string::npos);
            CHECK(r.errors.front().find("BEGIN") != std::string::npos);
        }
    }   // RAII 摘钩

    // 失败这一轮关系原样、源字节与符号行都没动
    CHECK(count_rows(*db, "symbol_footprint_links") == 0);
    CHECK(u04b1_symbol_row(*db, symbol_id).footprint() == "Pkg:Old");
    CHECK(read(file) == source_before);

    // 去故障后同一未改动来源重试：唯一关系真正建立
    services::ImportPipeline retry(*db, nullptr, false, &matching_store);
    auto after = retry | services::execute;
    MESSAGE("link retry: errors=", after.errors.size());
    CHECK(after.ok());
    CHECK(count_rows(*db, "symbol_footprint_links") == 1);
    {
        auto session = db->session();
        auto linked = storage::RelationshipRepository(session).find_footprint_for_symbol(symbol_id);
        REQUIRE(linked.has_value());
        REQUIRE(linked->has_value());
        CHECK(**linked == fp.id());
    }
}

// ------------------------------------------------------------
// 附件 3：FP 事务 COMMIT 失败不能报已导入的模型数量，也不能消费快照
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 footprint commit failure discards model counts and snapshots")
{
    auto dir = temp.path() / "b9_commit";
    write(dir / "Part.step", "STEP-data-v1");
    write(dir / "Part.kicad_mod", "(footprint \"Part\" (layer \"F.Cu\") (model \"Part.step\"))");
    const std::string fp_bytes = read(dir / "Part.kicad_mod");
    const std::string step_bytes = read(dir / "Part.step");

    // 前提：真实 parser 确实解析出这条模型引用（不是无效格式让模型路径没被执行）
    auto parsed = parser::FootprintParser::parse(dir / "Part.kicad_mod");
    if (!parsed)
        MESSAGE("footprint parse failed: ", parsed.error().format_message());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->models_3d().size() == 1);
    REQUIRE_FALSE(parsed->models_3d().front().path.empty());

    CHECK(count_rows(*db, "footprints") == 0);
    CHECK(count_rows(*db, "models_3d") == 0);
    CHECK(count_rows(*db, "footprint_model_links") == 0);
    CHECK(u04b1_imported_files(*db) == 0);

    {
        U04B1Authorizer hook(*db, &u04b1_deny_commit);
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto r = pipe | services::footprints_from{dir.string()} | services::execute;
        MESSAGE("denied commit: footprints=", r.footprints, " models=", r.models_3d,
                " errors=", r.errors.size());
        // 一个封装目录 = 一个事务：批内只有第 1109 行的 COMMIT 是显式提交
        // （deferred mtime/模型子写共用同一 session，不产生第二个控制事务）
        REQUIRE(hook.hits.load() == 1);   // 前提：封装批的 COMMIT 真的被拒过一次
        CHECK_FALSE(r.ok());             // 红：错误必须可见
        CHECK_FALSE(r.errors.empty());
        if (!r.errors.empty())
        {
            MESSAGE("fp commit error: ", r.errors.front());
            CHECK(r.errors.front().find("commit") != std::string::npos);
        }
        CHECK(r.footprints == 0);
        CHECK(r.models_3d == 0);   // 红：COMMIT 前自增的 model_ref_imported_ 被当成已导入
    }

    // 事务整体回滚：四张表 0、两个源文件字节不变
    CHECK(count_rows(*db, "footprints") == 0);
    CHECK(count_rows(*db, "models_3d") == 0);
    CHECK(count_rows(*db, "footprint_model_links") == 0);
    CHECK(u04b1_imported_files(*db) == 0);
    CHECK(read(dir / "Part.kicad_mod") == fp_bytes);
    CHECK(read(dir / "Part.step") == step_bytes);

    // 去故障后同一未改动来源重试：1 FP / 1 模型 / 1 关系 / 两个来源快照
    services::ImportPipeline retry(*db, nullptr, false, &matching_store);
    auto after = retry | services::footprints_from{dir.string()} | services::execute;
    MESSAGE("fp retry: footprints=", after.footprints, " models=", after.models_3d,
            " errors=", after.errors.size());
    CHECK(after.ok());
    CHECK(after.footprints == 1);
    CHECK(after.models_3d == 1);
    CHECK(count_rows(*db, "footprints") == 1);
    CHECK(count_rows(*db, "models_3d") == 1);
    CHECK(count_rows(*db, "footprint_model_links") == 1);
    CHECK(u04b1_imported_files(*db) == 2);
    CHECK(read(dir / "Part.kicad_mod") == fp_bytes);
    CHECK(read(dir / "Part.step") == step_bytes);
}

// ------------------------------------------------------------
// 附件 4：FP 的 model / 关系 / mtime 子写失败不能被吞掉并消费来源
// 三个 SUBCASE 各装一个真实触发器（BEFORE INSERT，路径按 SQL 字面量转义）
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 footprint subordinate write failures roll back and retry")
{
    auto dir = temp.path() / "b9_sub";
    write(dir / "Part.step", "STEP-data");
    write(dir / "Part.kicad_mod", "(footprint \"Part\" (layer \"F.Cu\") (model \"Part.step\"))");
    const std::string fp_bytes = read(dir / "Part.kicad_mod");
    const std::string step_bytes = read(dir / "Part.step");
    const std::string fp_path = (dir / "Part.kicad_mod").string();

    // 前提：真实 parser 解析出这条模型引用
    auto parsed = parser::FootprintParser::parse(dir / "Part.kicad_mod");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->models_3d().size() == 1);

    auto scenario = [&](const std::string& trigger_name, const std::string& trigger_sql,
                        const std::string& expected_text)
    {
        u04b1_raw_exec(*db, trigger_sql);
        {
            services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
            auto r = pipe | services::footprints_from{dir.string()} | services::execute;
            MESSAGE("blocked subordinate write (", trigger_name, "): footprints=", r.footprints,
                    " models=", r.models_3d, " errors=", r.errors.size());
            CHECK_FALSE(r.ok());   // 红：当前子写失败被吞掉，Result 仍是 ok
            CHECK(r.footprints == 0);
            CHECK(r.models_3d == 0);
            bool mentioned = false;
            for (const auto& message : r.errors)
                if (message.find(expected_text) != std::string::npos)
                    mentioned = true;
            CHECK(mentioned);      // 失败必须出现在 errors 里
        }
        // 失败文件的封装 / 模型 / 关系 / 两个 mtime 都不提交，源字节不变
        CHECK(count_rows(*db, "footprints") == 0);
        CHECK(count_rows(*db, "models_3d") == 0);
        CHECK(count_rows(*db, "footprint_model_links") == 0);
        CHECK(u04b1_imported_files(*db) == 0);
        CHECK(read(dir / "Part.kicad_mod") == fp_bytes);
        CHECK(read(dir / "Part.step") == step_bytes);

        // 去故障后同一未改动来源的第一次重试必须真正提交
        u04b1_raw_exec(*db, "DROP TRIGGER " + trigger_name);
        services::ImportPipeline retry(*db, nullptr, false, &matching_store);
        auto after = retry | services::footprints_from{dir.string()} | services::execute;
        MESSAGE("retry after ", trigger_name, ": footprints=", after.footprints, " models=",
                after.models_3d, " errors=", after.errors.size());
        CHECK(after.ok());
        CHECK(after.footprints == 1);
        CHECK(after.models_3d == 1);
        CHECK(count_rows(*db, "footprints") == 1);
        CHECK(count_rows(*db, "models_3d") == 1);
        CHECK(count_rows(*db, "footprint_model_links") == 1);
        CHECK(u04b1_imported_files(*db) == 2);   // 封装 + 模型两个来源
        CHECK(read(dir / "Part.kicad_mod") == fp_bytes);
        CHECK(read(dir / "Part.step") == step_bytes);
    };

    SUBCASE("the footprint mtime insert is blocked")
    {
        scenario("b9_mtime_abort",
                 "CREATE TRIGGER b9_mtime_abort BEFORE INSERT ON imported_files "
                 "WHEN NEW.file_path = "
                     + u04b1_sql_literal(fp_path)
                     + " BEGIN SELECT RAISE(ABORT, 'b9 footprint mtime blocked'); END",
                 "b9 footprint mtime blocked");
    }

    SUBCASE("the model insert is blocked")
    {
        scenario("b9_model_abort",
                 "CREATE TRIGGER b9_model_abort BEFORE INSERT ON models_3d "
                 "BEGIN SELECT RAISE(ABORT, 'b9 model insert blocked'); END",
                 "b9 model insert blocked");
    }

    SUBCASE("the model link insert is blocked")
    {
        scenario("b9_link_abort",
                 "CREATE TRIGGER b9_link_abort BEFORE INSERT ON footprint_model_links "
                 "BEGIN SELECT RAISE(ABORT, 'b9 model link blocked'); END",
                 "b9 model link blocked");
    }
}

// ------------------------------------------------------------
// C04：被拒的模型 UPDATE 必须报错，且不能消费来源快照
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a rejected model update is reported and does not consume its source snapshot")
{
    auto dir = temp.path() / "b9_model_update";
    write(dir / "Part.step", "STEP-data-v1");
    const std::string changed_bytes = "STEP-data-v2 (different body)";

    // 健康首导：真实 model-only 导入一行 + 一份 mtime 快照
    services::ImportPipeline first(*db, nullptr, false, &matching_store);
    auto initial = first | services::models_from{dir.string()} | services::execute;
    MESSAGE("model first import: models=", initial.models_3d, " errors=", initial.errors.size());
    REQUIRE(initial.ok());
    REQUIRE(initial.models_3d == 1);
    REQUIRE(u04b1_imported_files(*db) == 1);

    std::string model_id;
    core::Model3D before;
    {
        auto session = db->session();
        auto all = storage::Model3DRepository(session).find_all();
        REQUIRE(all.has_value());
        REQUIRE(all->size() == 1);
        before = all->front();
        model_id = before.id();
    }
    const auto snapshots_before = u04b1_imported_map(*db);
    REQUIRE(snapshots_before.size() == 1);
    const std::string step_key = u04b1_snapshot_key_for(snapshots_before, "Part.step");
    const int64_t mtime_before = snapshots_before.at(step_key);

    // 源内容改变 + mtime 推进 2 秒（不 sleep），只挡这一行的 UPDATE
    write(dir / "Part.step", changed_bytes);
    fs::last_write_time(dir / "Part.step",
                        fs::last_write_time(dir / "Part.step") + std::chrono::seconds(2));
    u04b1_raw_exec(*db,
                   "CREATE TRIGGER b9_model_update_block BEFORE UPDATE ON models_3d "
                   "WHEN NEW.id = '" + model_id +
                       "' BEGIN SELECT RAISE(ABORT, 'review model update blocked'); END");

    services::ImportPipeline second(*db, nullptr, false, &matching_store);
    auto rejected = second | services::models_from{dir.string()} | services::execute;
    MESSAGE("rejected model update: models=", rejected.models_3d, " errors=",
            rejected.errors.size());
    CHECK_FALSE(rejected.ok());        // 红：repo.update 的 Result 被忽略，入口报成功
    CHECK(rejected.models_3d == 0);    // 红：失败不能计成已导入
    CHECK_FALSE(rejected.errors.empty());
    for (const auto& message : rejected.errors)
        MESSAGE("rejected update message: ", message);
    {
        bool update_reason_visible = false;
        for (const auto& message : rejected.errors)
            if (message.find("review model update blocked") != std::string::npos)
                update_reason_visible = true;
        CHECK(update_reason_visible);   // 具体触发器文本必须可见，不是笼统失败
    }
    CHECK(rejected.failed_files == 1);   // 真实来源文件失败计入 failed_files
    CHECK(rejected.error_count == 1);
    {
        auto session = db->session();
        auto row = storage::Model3DRepository(session).find_by_id(model_id);
        REQUIRE(row.has_value());
        CHECK(row->file_path() == before.file_path());
        CHECK(row->format() == before.format());
        CHECK(row->description() == before.description());
        CHECK(row->source() == before.source());
        CHECK(storage::Model3DRepository(session).count() == 1);
    }
    // 快照完整不变（失败不能消费来源），源字节也没被宿主改写
    const auto snapshots_after = u04b1_imported_map(*db);
    u04b1_check_snapshots_equal(snapshots_before, snapshots_after);
    const std::string after_key = u04b1_snapshot_key_for(snapshots_after, "Part.step");
    CHECK(snapshots_after.at(after_key) == mtime_before);
    CHECK(read(dir / "Part.step") == changed_bytes);

    // 去故障、mtime 不再变：同一来源的第一次重试真正更新并推进快照
    u04b1_raw_exec(*db, "DROP TRIGGER b9_model_update_block");
    services::ImportPipeline retry(*db, nullptr, false, &matching_store);
    auto retried = retry | services::models_from{dir.string()} | services::execute;
    MESSAGE("model update retry: models=", retried.models_3d, " errors=",
            retried.errors.size());
    CHECK(retried.ok());
    CHECK(retried.models_3d == 1);
    CHECK(count_rows(*db, "models_3d") == 1);
    const auto snapshots_final = u04b1_imported_map(*db);
    const std::string final_key = u04b1_snapshot_key_for(snapshots_final, "Part.step");
    CHECK(snapshots_final.at(final_key) > mtime_before);
    CHECK(read(dir / "Part.step") == changed_bytes);
}

// ------------------------------------------------------------
// C05：启用库读取失败不能被当成成功导入（同步入口 + 后台信号）
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a failed enabled library read is not a successful import")
{
    group();   // 一条真实启用库：读取失败必须是操作级错误，不能退化成空 pipeline
    util::ConfigStore config(temp.path() / "config.json");
    api::ImportManager manager(*db, &config, &matching_store);
    const int libraries_before = count_rows(*db, "component_libraries");

    SUBCASE("the synchronous import reports the read failure")
    {
        services::ImportPipeline::Result result;
        int hits = 0;
        {
            U04B1Authorizer hook(*db, &u04b1_deny_component_libraries_read);
            result = manager.import_all({});
            hits = hook.hits.load();   // 命中数在钩子作用域内取好，摘钩后不再依赖 hook
        }   // RAII 摘钩：下面的 rows / 快照查询不再被 deny-read 波及（否则恒返回 -1）
        MESSAGE("denied enabled-library read: errors=", result.errors.size(),
                " symbols=", result.symbols, " hits=", hits,
                " seq=", manager.orchestrator()->import_seq());
        REQUIRE(hits > 0);             // 前提：启用库读取真的被拒过
        CHECK_FALSE(result.ok());      // 红：读失败后仍执行空 pipeline 并报 ok
        CHECK_FALSE(result.errors.empty());
        CHECK(result.symbols == 0);
        // 一行都没提交 → 完成信号与数据版本不能推进
        CHECK(manager.orchestrator()->import_seq() == 0);
        CHECK(count_rows(*db, "symbols") == 0);
        CHECK(u04b1_imported_files(*db) == 0);
        CHECK(count_rows(*db, "component_libraries") == libraries_before);
    }

    SUBCASE("the background import does not claim success")
    {
        // 局部日志缓冲：捕获 sink 按引用持有它们。声明在最前 → 最后销毁，
        // 且下面的收尾顺序保证 default logger 与后台线程都先不碰这块缓冲。
        std::mutex log_mutex;
        std::vector<std::string> logs;

        auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&log_mutex, &logs](const spdlog::details::log_msg& message)
            {
                // 只拷贝 payload：不执行 SQL、不断言、不把异常丢给后台线程
                try
                {
                    std::lock_guard lock(log_mutex);
                    logs.emplace_back(message.payload.data(), message.payload.size());
                }
                catch (...)
                {
                }
            });
        auto capture = std::make_shared<spdlog::logger>("b1-enabled-capture", sink);
        capture->set_level(spdlog::level::trace);
        auto previous_logger = spdlog::default_logger();   // 夹具装的 null-sink logger
        spdlog::set_default_logger(capture);

        U04B1Authorizer hook(*db, &u04b1_deny_component_libraries_read);

        // 有界等待结束；REQUIRE 早退或异常也一样按序收尾：
        // 先停后台并 join → 再摘 authorizer → 最后恢复 logger（缓冲由外层次序兜底）
        struct HooksAndLoggerCleanup
        {
            api::ImportManager& manager;
            U04B1Authorizer& hook;
            std::shared_ptr<spdlog::logger> restore;
            ~HooksAndLoggerCleanup()
            {
                manager.stop_async();
                hook.remove();
                spdlog::set_default_logger(std::move(restore));
            }
        };
        bool finished_in_time = false;
        {
            HooksAndLoggerCleanup cleanup{manager, hook, previous_logger};
            manager.start_async();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (manager.is_running() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            finished_in_time = !manager.is_running();
        }   // 到这里：后台已 join、deny-read 已摘、捕获 logger 已卸下

        const int hits = hook.hits.load();   // 清理之后的读：无并发写者
        CHECK(finished_in_time);             // 有界完成，不是挂住
        REQUIRE(hits > 0);                   // 前提：启用库读取真的被拒过（不假报业务红）

        bool claimed_done = false;
        bool reported_failure = false;
        {
            std::lock_guard lock(log_mutex);
            for (const auto& line : logs)
            {
                if (line.find("Auto-import done") != std::string::npos) claimed_done = true;
                if (line.find("cannot read enabled libraries") != std::string::npos)
                    reported_failure = true;
            }
        }
        CHECK(reported_failure);   // 真实错误信息必须出现在日志里（绿）
        CHECK_FALSE(claimed_done); // 红：读失败不许输出完成
        CHECK(manager.orchestrator()->import_seq() == 0);   // 红：一行都没提交
        CHECK(count_rows(*db, "symbols") == 0);
        CHECK(u04b1_imported_files(*db) == 0);
    }
}

// ============================================================
// U04-B1 返工：同源事实 DTO、写过程状态与每批事务边界
//
// 只调用真实入口（ClassificationService 的 preview_all / apply_all /
// classify_symbol）与真实核心快照、真实 SymbolRepository；断言全部来自被测产品
// 返回的 DTO 与 DB 实际内容，不复制匹配算法、不读用户数据目录。
// ============================================================

namespace
{

/// 明细里的写状态名（B2 的稳定映射键）；明细指针由调用方先判空
std::string u04b1_write_status_name(const services::ClassificationService::Result& result)
{
    return services::ClassificationService::write_status_name(result.write_status);
}

/// 结构化依据必须是核心决策的逐字段原样复制（source/id/field/text/priority/type_id）
void u04b1_check_evidence(const classifier::Decision& decision,
                          const services::ClassificationService::Result& result)
{
    REQUIRE(result.structured_evidence.size() == decision.evidence.size());
    for (std::size_t index = 0; index < decision.evidence.size(); ++index)
    {
        CHECK(result.structured_evidence[index].source == decision.evidence[index].source);
        CHECK(result.structured_evidence[index].id == decision.evidence[index].id);
        CHECK(result.structured_evidence[index].field == decision.evidence[index].field);
        CHECK(result.structured_evidence[index].text == decision.evidence[index].text);
        CHECK(result.structured_evidence[index].priority == decision.evidence[index].priority);
        CHECK(result.structured_evidence[index].type_id == decision.evidence[index].type_id);
    }
}

}  // namespace

// ------------------------------------------------------------
// R1-01：报告里的 raw / sample / candidate / 结构化依据来自同一次读快照
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 the report carries the same-snapshot raw type, sample and evidence")
{
    u04b1_register_types({"custom:selector"});
    u04b1_store_config(
        matching_store, matching_config,
        nlohmann::json::array({u04a_rule("b1-rich-empty-ref", "reference", "^$", "TestPoint")}),
        nlohmann::json::array(
            {u04a_exception("b1-rich-exact-value", "value", " 10kµF ", "custom:selector")}));

    U04B1Library lib(*db, matching_store, temp.path(), "RichReport");
    // 原空白与 Unicode 原样保留：不 trim、不改大小写，model 是 MPN 而不是 3D 模型
    const std::string rich = lib.insert(
        u04b1_part("U04B1RichPart", "U", std::string(" R? "), " 10kµF ", "MPN-Ω-1", "Unknown"));
    // Reference 缺失（没有这个字段）与显式空串是两回事
    const std::string missing =
        lib.insert(u04b1_part("U04B1RichNoRef", "U", std::nullopt, "v1", "m1", "Unknown"));
    const std::string empty_ref =
        lib.insert(u04b1_part("U04B1RichEmptyRef", "U", std::string(), "v2", "m2", "Unknown"));

    const core::Symbol rich_row = lib.read(rich);
    REQUIRE(rich_row.component_type_id == "Unknown");
    const classifier::Decision rich_decision =
        matching_store.current()->evaluate(classifier::MatchSample::from_symbol(rich_row));
    if (rich_decision.status != classifier::MatchStatus::Matched)
        MESSAGE("rich premise: ", classifier::match_status_name(rich_decision.status), " ",
                rich_decision.reason);
    REQUIRE(rich_decision.status == classifier::MatchStatus::Matched);
    REQUIRE(rich_decision.type_id == "custom:selector");

    const core::Symbol missing_row = lib.read(missing);
    REQUIRE_FALSE(missing_row.property("reference").has_value());
    const core::Symbol empty_row = lib.read(empty_ref);
    REQUIRE(empty_row.property("reference").has_value());
    REQUIRE(empty_row.property("reference")->empty());
    const classifier::Decision empty_decision =
        matching_store.current()->evaluate(classifier::MatchSample::from_symbol(empty_row));
    REQUIRE(empty_decision.status == classifier::MatchStatus::Matched);
    REQUIRE(empty_decision.type_id == "TestPoint");

    const int symbols_before = count_rows(*db, "symbols");
    const auto snapshots_before = u04b1_imported_map(*db);

    services::ClassificationService service(db.get(), &matching_store);
    auto preview = service.preview_all();
    if (!preview)
        MESSAGE("preview failed: ", preview.error().format_message());
    REQUIRE(preview.has_value());
    MESSAGE("rich preview: total=", preview->total, " matched=", preview->matched,
            " would_update=", preview->would_update, " unchanged=", preview->unchanged);

    CHECK(preview->total == 3);
    CHECK(preview->protected_count == 0);
    CHECK(preview->matched == 2);
    CHECK(preview->unmatched == 1);
    CHECK(preview->conflicts == 0);
    CHECK(preview->input_too_long == 0);
    CHECK(preview->evaluation_limited == 0);
    CHECK(preview->evaluation_errors == 0);
    CHECK(preview->would_update == 2);
    CHECK(preview->unchanged == 0);
    CHECK(preview->type_updated == 0);                      // 预览不写
    CHECK(preview->deleted == 0);
    CHECK(preview->rolled_back == 0);
    CHECK(preview->not_attempted == 0);
    CHECK(preview->revision == matching_store.current()->revision());

    const auto* rich_detail = u04b1_detail(*preview, rich);
    REQUIRE(rich_detail != nullptr);
    CHECK(rich_detail->raw_type_id == "Unknown");   // 仓储原值，不是展示值
    CHECK(rich_detail->sample.name == rich_row.name());
    REQUIRE(rich_detail->sample.value.has_value());
    CHECK(*rich_detail->sample.value == " 10kµF ");         // 空白与 Unicode 原样
    REQUIRE(rich_detail->sample.model.has_value());
    CHECK(*rich_detail->sample.model == "MPN-Ω-1");         // model 是 MPN
    REQUIRE(rich_detail->sample.reference.has_value());
    CHECK(*rich_detail->sample.reference == " R? ");
    CHECK(rich_detail->candidate_type_id == "custom:selector");
    CHECK(rich_detail->type_id == "custom:selector");
    CHECK_FALSE(rich_detail->is_protected);
    CHECK(rich_detail->would_update);
    CHECK(rich_detail->status == "matched");
    CHECK(rich_detail->write_status == services::ClassificationService::WriteStatus::ReadOnly);
    CHECK(u04b1_write_status_name(*rich_detail) == "read_only");
    CHECK_FALSE(rich_detail->write_reason.empty());
    u04b1_check_evidence(rich_decision, *rich_detail);
    // 旧文本摘要继续兼容：条目 id 与类型都在，但不带可能超长的字段原文
    REQUIRE(rich_detail->evidence.size() == 1);
    CHECK(rich_detail->evidence.front().find("b1-rich-exact-value") != std::string::npos);
    CHECK(rich_detail->evidence.front().find("custom:selector") != std::string::npos);
    CHECK(rich_detail->evidence.front().find(" 10kµF ") == std::string::npos);

    const auto* missing_detail = u04b1_detail(*preview, missing);
    REQUIRE(missing_detail != nullptr);
    CHECK_FALSE(missing_detail->sample.reference.has_value());   // 缺失 = 没有这个字段
    CHECK(missing_detail->status == "unmatched");
    CHECK(missing_detail->candidate_type_id.empty());
    CHECK(missing_detail->structured_evidence.empty());
    CHECK_FALSE(missing_detail->would_update);
    CHECK(missing_detail->type_id == "Unknown");

    const auto* empty_detail = u04b1_detail(*preview, empty_ref);
    REQUIRE(empty_detail != nullptr);
    REQUIRE(empty_detail->sample.reference.has_value());         // 显式空串：字段存在
    CHECK(empty_detail->sample.reference->empty());
    CHECK(empty_detail->status == "matched");
    CHECK(empty_detail->candidate_type_id == "TestPoint");
    CHECK(empty_detail->would_update);

    CHECK(count_rows(*db, "symbols") == symbols_before);
    CHECK(fix02_stored_type(*db, rich) == "Unknown");
    CHECK(fix02_stored_type(*db, missing) == "Unknown");
    CHECK(fix02_stored_type(*db, empty_ref) == "Unknown");
    u04b1_check_snapshots_equal(snapshots_before, u04b1_imported_map(*db));
}

// ------------------------------------------------------------
// R1-02：保护行与丢弃的决策都不进写计划
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 protected rows and discarded decisions stay outside the write plan")
{
    SUBCASE("known identities are protected without a candidate or evidence")
    {
        u04b1_register_types({"custom:selector"});
        u04b1_store_config(matching_store, matching_config,
                           nlohmann::json::array({u04a_rule("b1-prot-name", "name", "^U04B1Prot",
                                                            "Resistor")}));
        U04B1Library lib(*db, matching_store, temp.path(), "ProtectedRows");
        const std::string builtin =
            lib.insert(u04b1_part("U04B1ProtBuiltin", "U", std::string("U"), "", "", "OpAmp"));
        const std::string custom = lib.insert(
            u04b1_part("U04B1ProtCustom", "U", std::string("U"), "", "", "custom:selector"));
        const std::string raw =
            lib.insert(u04b1_part("U04B1ProtRaw", "U", std::string("U"), "", "", "LegacyRawId"));

        // 前提：同一配置下核心对这些已知行也会给出 Resistor 候选 —— 服务必须保护原值
        const auto builtin_decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(lib.read(builtin)));
        REQUIRE(builtin_decision.status == classifier::MatchStatus::Matched);
        REQUIRE(builtin_decision.type_id == "Resistor");

        services::ClassificationService service(db.get(), &matching_store);
        auto preview = service.preview_all();
        if (!preview)
            MESSAGE("preview failed: ", preview.error().format_message());
        REQUIRE(preview.has_value());
        MESSAGE("protected preview: total=", preview->total, " protected=", preview->protected_count,
                " matched=", preview->matched, " unmatched=", preview->unmatched);

        CHECK(preview->total == 3);
        CHECK(preview->protected_count == 3);
        CHECK(preview->matched == 0);          // 保护行不进 eligible 计数
        CHECK(preview->unmatched == 0);
        CHECK(preview->conflicts == 0);
        CHECK(preview->evaluation_limited == 0);
        CHECK(preview->would_update == 0);
        CHECK(preview->unchanged == 0);
        CHECK(preview->type_updated == 0);
        CHECK(preview->protected_count + u04b1_eligible_states(*preview) == preview->total);

        const auto check_protected = [&](const std::string& id, const std::string& expected_type)
        {
            const auto* detail = u04b1_detail(*preview, id);
            REQUIRE(detail != nullptr);
            CHECK(detail->is_protected);
            CHECK(detail->status == "protected");
            CHECK(detail->raw_type_id == expected_type);
            CHECK(detail->type_id == expected_type);       // 展示原值，不是候选
            CHECK(detail->candidate_type_id.empty());      // 保护行没有候选
            CHECK(detail->structured_evidence.empty());    // 也没有伪造的命中依据
            CHECK(detail->evidence.empty());
            CHECK_FALSE(detail->would_update);
            CHECK(detail->write_status == services::ClassificationService::WriteStatus::ReadOnly);
        };
        check_protected(builtin, "OpAmp");
        check_protected(custom, "custom:selector");
        check_protected(raw, "LegacyRawId");
        CHECK(fix02_stored_type(*db, builtin) == "OpAmp");
        CHECK(fix02_stored_type(*db, custom) == "custom:selector");
        CHECK(fix02_stored_type(*db, raw) == "LegacyRawId");

        // 单符号路径与批量同一内核：同样是 protected、同样没有候选
        auto single = service.classify_symbol(builtin);
        REQUIRE(single.has_value());
        REQUIRE(single->size() >= 1);
        CHECK(single->front().is_protected);
        CHECK(single->front().status == "protected");
        CHECK(single->front().candidate_type_id.empty());
        CHECK(single->front().type_id == "OpAmp");
        CHECK(single->front().write_status == services::ClassificationService::WriteStatus::ReadOnly);
    }

    SUBCASE("a known row with a limit-triggering value is not evaluated")
    {
        u04b1_store_config(
            matching_store, matching_config,
            nlohmann::json::array(
                {u04a_rule("b1-prot-limit-first", "value", "^a+!$", "Capacitor", 0),
                 u04a_rule("b1-prot-limit-heavy", "value", kU04B1LimitPattern, "Resistor", 100)}));
        U04B1Library lib(*db, matching_store, temp.path(), "ProtectedLimit");
        const std::string known = lib.insert(u04b1_part("U04B1ProtLimit", "U", std::string("U"),
                                                        u04b1_limit_subject(), "", "OpAmp"));

        // 前提：核心单独求值这条输入确实超限 —— 保护行却不会跑它
        const auto decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(lib.read(known)));
        if (decision.status != classifier::MatchStatus::EvaluationLimit)
            MESSAGE("limit premise: ", classifier::match_status_name(decision.status));
        REQUIRE(decision.status == classifier::MatchStatus::EvaluationLimit);

        services::ClassificationService service(db.get(), &matching_store);
        auto preview = service.preview_all();
        REQUIRE(preview.has_value());
        CHECK(preview->total == 1);
        CHECK(preview->protected_count == 1);
        CHECK(preview->matched == 0);
        CHECK(preview->evaluation_limited == 0);   // 保护行不跑无意义的 regex
        CHECK(preview->would_update == 0);
        const auto* detail = u04b1_detail(*preview, known);
        REQUIRE(detail != nullptr);
        CHECK(detail->status == "protected");
        REQUIRE(detail->sample.value.has_value());
        CHECK(*detail->sample.value == u04b1_limit_subject());   // 事实原样，只是不求值
        CHECK(fix02_stored_type(*db, known) == "OpAmp");
    }

    SUBCASE("discarded decisions keep no candidate and no write")
    {
        u04b1_store_config(
            matching_store, matching_config,
            nlohmann::json::array(
                {u04a_rule("b1-disc-conflict-a", "name", "^U04B1DiscConflict$", "Resistor", 0),
                 u04a_rule("b1-disc-conflict-b", "name", "^U04B1DiscConflict$", "Capacitor", 0),
                 u04a_rule("b1-disc-toolong", "name", "^U04B1DiscTooLong$", "Capacitor"),
                 u04a_rule("b1-disc-limit-first", "value", "^a+!$", "Capacitor", 0),
                 u04a_rule("b1-disc-limit-heavy", "value", kU04B1LimitPattern, "Resistor", 100)}));

        U04B1Library lib(*db, matching_store, temp.path(), "Discarded");
        const std::string conflict =
            lib.insert(u04b1_part("U04B1DiscConflict", "U", std::string("U")));
        const std::string too_long = lib.insert(
            u04b1_part("U04B1DiscTooLong", "U", std::string("U"), std::string(5000, 'x')));
        const std::string limited = lib.insert(
            u04b1_part("U04B1DiscLimit", "U", std::string("U"), u04b1_limit_subject()));

        // 前提：三种输入在核心里的状态就是 conflict / input_too_long / evaluation_limit
        const auto conflict_decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(lib.read(conflict)));
        REQUIRE(conflict_decision.status == classifier::MatchStatus::Conflict);
        const auto long_decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(lib.read(too_long)));
        REQUIRE(long_decision.status == classifier::MatchStatus::InputTooLong);
        const auto limit_decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(lib.read(limited)));
        REQUIRE(limit_decision.status == classifier::MatchStatus::EvaluationLimit);

        const int symbols_before = count_rows(*db, "symbols");

        services::ClassificationService service(db.get(), &matching_store);
        auto preview = service.preview_all();
        REQUIRE(preview.has_value());
        MESSAGE("discarded preview: conflicts=", preview->conflicts,
                " too_long=", preview->input_too_long, " limited=", preview->evaluation_limited);
        CHECK(preview->total == 3);
        CHECK(preview->protected_count == 0);
        CHECK(preview->matched == 0);
        CHECK(preview->conflicts == 1);
        CHECK(preview->input_too_long == 1);
        CHECK(preview->evaluation_limited == 1);
        CHECK(preview->evaluation_errors == 0);
        CHECK(preview->would_update == 0);
        CHECK(preview->unchanged == 0);
        CHECK(preview->type_updated == 0);
        CHECK(preview->protected_count + u04b1_eligible_states(*preview) == preview->total);

        const auto* conflict_detail = u04b1_detail(*preview, conflict);
        REQUIRE(conflict_detail != nullptr);
        CHECK(conflict_detail->status == "conflict");
        CHECK(conflict_detail->candidate_type_id.empty());   // 冲突不替调用方选一个
        CHECK_FALSE(conflict_detail->would_update);
        u04b1_check_evidence(conflict_decision, *conflict_detail);   // 依据本身仍原样保留

        const auto* long_detail = u04b1_detail(*preview, too_long);
        REQUIRE(long_detail != nullptr);
        CHECK(long_detail->status == "input_too_long");
        CHECK(long_detail->candidate_type_id.empty());
        CHECK(long_detail->structured_evidence.empty());
        CHECK_FALSE(long_detail->would_update);

        const auto* limit_detail = u04b1_detail(*preview, limited);
        REQUIRE(limit_detail != nullptr);
        CHECK(limit_detail->status == "evaluation_limit");
        CHECK(limit_detail->candidate_type_id.empty());
        CHECK(limit_detail->structured_evidence.empty());
        CHECK_FALSE(limit_detail->would_update);

        for (const std::string& id : {conflict, too_long, limited})
            CHECK(fix02_stored_type(*db, id) == "Unknown");   // 丢弃的决策一条都不写
        CHECK(count_rows(*db, "symbols") == symbols_before);
    }

    SUBCASE("a matched candidate equal to the stored type is unchanged and never written")
    {
        u04b1_store_config(matching_store, matching_config,
                           nlohmann::json::array({u04a_rule("b1-same-unknown", "name",
                                                            "^U04B1SameUnknown$", "Unknown")}));
        U04B1Library lib(*db, matching_store, temp.path(), "SameUnknown");
        const std::string same =
            lib.insert(u04b1_part("U04B1SameUnknown", "U", std::string("U")));

        // 前提：核心确实 Matched，候选就是字面 Unknown（等于原值）
        const auto decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(lib.read(same)));
        REQUIRE(decision.status == classifier::MatchStatus::Matched);
        REQUIRE(decision.type_id == "Unknown");

        services::ClassificationService service(db.get(), &matching_store);
        auto preview = service.preview_all();
        REQUIRE(preview.has_value());
        CHECK(preview->matched == 1);
        CHECK(preview->would_update == 0);   // 候选就是原值：不排入写计划
        CHECK(preview->unchanged == 1);
        CHECK(preview->type_updated == 0);
        const auto* detail = u04b1_detail(*preview, same);
        REQUIRE(detail != nullptr);
        CHECK(detail->status == "matched");
        CHECK(detail->raw_type_id == "Unknown");
        CHECK(detail->candidate_type_id == "Unknown");
        CHECK(detail->type_id == "Unknown");
        CHECK_FALSE(detail->would_update);
        CHECK(fix02_stored_type(*db, same) == "Unknown");

        auto applied = service.apply_all({});
        REQUIRE(applied.has_value());
        REQUIRE_FALSE(applied->error.has_value());
        CHECK(applied->summary.type_updated == 0);
        CHECK(applied->summary.would_update == 0);
        CHECK(applied->summary.unchanged == 1);
        CHECK(applied->summary.rolled_back == 0);
        CHECK(applied->summary.not_attempted == 0);
        const auto* applied_detail = u04b1_detail(applied->summary, same);
        REQUIRE(applied_detail != nullptr);
        CHECK(applied_detail->write_status ==
              services::ClassificationService::WriteStatus::Unchanged);
        CHECK(u04b1_write_status_name(*applied_detail) == "unchanged");
        CHECK(fix02_stored_type(*db, same) == "Unknown");   // 不做 type 写
    }
}

// ------------------------------------------------------------
// R1-03：apply 报告真实写过程状态（提交批 / 来源变化 / CAS 跳过 / 行已删除）
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 apply publishes the real write status of every candidate")
{
    u04b1_store_config(matching_store, matching_config,
                       nlohmann::json::array({u04a_rule("b1-ws-name", "name", "^U04B1WS",
                                                        "Resistor")}));

    SUBCASE("a committed batch publishes updated")
    {
        U04B1Library lib(*db, matching_store, temp.path(), "WriteUpdated");
        const std::string first = lib.insert(u04b1_part("U04B1WSFirst", "U", std::string("U")));
        const std::string second = lib.insert(u04b1_part("U04B1WSSecond", "U", std::string("U")));

        services::ClassificationService service(db.get(), &matching_store);
        auto applied = service.apply_all({});
        if (!applied)
            MESSAGE("apply failed: ", applied.error().format_message());
        REQUIRE(applied.has_value());
        REQUIRE_FALSE(applied->error.has_value());
        CHECK(applied->summary.type_updated == 2);
        CHECK(applied->summary.would_update == 2);
        CHECK(applied->summary.unchanged == 0);
        CHECK(applied->summary.source_changed == 0);
        CHECK(applied->summary.cas_skipped == 0);
        CHECK(applied->summary.deleted == 0);
        CHECK(applied->summary.rolled_back == 0);
        CHECK(applied->summary.not_attempted == 0);
        for (const std::string& id : {first, second})
        {
            const auto* detail = u04b1_detail(applied->summary, id);
            REQUIRE(detail != nullptr);
            CHECK(detail->status == "matched");
            CHECK(detail->candidate_type_id == "Resistor");
            CHECK(detail->write_status == services::ClassificationService::WriteStatus::Updated);
            CHECK(u04b1_write_status_name(*detail) == "updated");
            CHECK_FALSE(detail->write_reason.empty());
        }
        CHECK(fix02_stored_type(*db, first) == "Resistor");
        CHECK(fix02_stored_type(*db, second) == "Resistor");
    }

    SUBCASE("a concurrent source change is reported as source_changed")
    {
        U04B1Library lib(*db, matching_store, temp.path(), "WriteSourceRace");
        const std::string row =
            lib.insert(u04b1_part("U04B1WSSource", "U", std::string("U"), "v1", "m1"));
        const core::Symbol before = lib.read(row);
        REQUIRE(before.component_type_id == "Unknown");
        REQUIRE(before.default_value() == "v1");
        const auto decision = matching_store.current()->evaluate(
            classifier::MatchSample::from_symbol(before));
        REQUIRE(decision.status == classifier::MatchStatus::Matched);
        REQUIRE(decision.type_id == "Resistor");

        // 真实 TRACE + 独立第二连接：来源更新发生在写事务 BEGIN 期间，回调只做有界握手
        U04B1SourceRace race(*db, db->path(),
                             "UPDATE symbols SET default_value = 'v2' WHERE id = " +
                                 u04b1_sql_literal(row));
        services::ClassificationService service(db.get(), &matching_store);
        auto applied = service.apply_all({});
        race.finish_join();
        REQUIRE_FALSE(race.callback_error);
        REQUIRE(race.worker_started);
        REQUIRE(race.begin_seen);
        REQUIRE_FALSE(race.worker_timeout);
        REQUIRE_FALSE(race.handshake_timeout);
        if (!race.worker_ok)
            MESSAGE("second connection failed: ", race.worker_error);
        REQUIRE(race.worker_ok);
        if (!applied)
            MESSAGE("apply failed: ", applied.error().format_message());
        REQUIRE(applied.has_value());
        REQUIRE_FALSE(applied->error.has_value());

        CHECK(applied->summary.type_updated == 0);
        CHECK(applied->summary.source_changed == 1);
        CHECK(applied->summary.cas_skipped == 0);
        CHECK(applied->summary.deleted == 0);
        CHECK(applied->summary.would_update == 1);   // 计划事实不随后来的来源改写

        const auto* detail = u04b1_detail(applied->summary, row);
        REQUIRE(detail != nullptr);
        CHECK(detail->write_status ==
              services::ClassificationService::WriteStatus::SourceChanged);
        CHECK(u04b1_write_status_name(*detail) == "source_changed");
        // 报告保留决定时的同源事实：sample/raw/candidate 都不被后来读出的来源改写
        CHECK(detail->raw_type_id == "Unknown");
        REQUIRE(detail->sample.value.has_value());
        CHECK(*detail->sample.value == "v1");
        CHECK(detail->candidate_type_id == "Resistor");
        CHECK(detail->status == "matched");
        u04b1_check_evidence(decision, *detail);

        // DB 的来源确实变了，类型保持原值（这一批没有写）
        const core::Symbol after = lib.read(row);
        CHECK(after.default_value() == "v2");
        CHECK(after.component_type_id == "Unknown");
    }

    SUBCASE("a concurrent type change is reported as cas_skipped")
    {
        U04B1Library lib(*db, matching_store, temp.path(), "WriteCasRace");
        const std::string row = lib.insert(u04b1_part("U04B1WSCas", "U", std::string("U")));
        REQUIRE(lib.read(row).component_type_id == "Unknown");

        // 只改类型列：四个来源字段没变，CAS 的期望原值已经对不上
        U04B1SourceRace race(*db, db->path(),
                             "UPDATE symbols SET component_type = 'Capacitor' WHERE id = " +
                                 u04b1_sql_literal(row));
        services::ClassificationService service(db.get(), &matching_store);
        auto applied = service.apply_all({});
        race.finish_join();
        REQUIRE_FALSE(race.callback_error);
        REQUIRE(race.worker_started);
        REQUIRE(race.begin_seen);
        REQUIRE_FALSE(race.worker_timeout);
        REQUIRE_FALSE(race.handshake_timeout);
        if (!race.worker_ok)
            MESSAGE("second connection failed: ", race.worker_error);
        REQUIRE(race.worker_ok);
        if (!applied)
            MESSAGE("apply failed: ", applied.error().format_message());
        REQUIRE(applied.has_value());
        REQUIRE_FALSE(applied->error.has_value());

        CHECK(applied->summary.type_updated == 0);
        CHECK(applied->summary.cas_skipped == 1);
        CHECK(applied->summary.source_changed == 0);
        CHECK(applied->summary.deleted == 0);

        const auto* detail = u04b1_detail(applied->summary, row);
        REQUIRE(detail != nullptr);
        CHECK(detail->write_status == services::ClassificationService::WriteStatus::CasSkipped);
        CHECK(u04b1_write_status_name(*detail) == "cas_skipped");
        CHECK(detail->raw_type_id == "Unknown");         // 决定时的原值
        CHECK(detail->candidate_type_id == "Resistor");
        CHECK(fix02_stored_type(*db, row) == "Capacitor");   // 并发写的值保留，没有被覆盖
    }

    SUBCASE("a concurrently deleted row is reported as deleted")
    {
        U04B1Library lib(*db, matching_store, temp.path(), "WriteDeletedRace");
        const std::string row = lib.insert(u04b1_part("U04B1WSDeleted", "U", std::string("U")));

        U04B1SourceRace race(*db, db->path(),
                             "DELETE FROM symbols WHERE id = " + u04b1_sql_literal(row));
        services::ClassificationService service(db.get(), &matching_store);
        auto applied = service.apply_all({});
        race.finish_join();
        REQUIRE_FALSE(race.callback_error);
        REQUIRE(race.worker_started);
        REQUIRE(race.begin_seen);
        REQUIRE_FALSE(race.worker_timeout);
        REQUIRE_FALSE(race.handshake_timeout);
        if (!race.worker_ok)
            MESSAGE("second connection failed: ", race.worker_error);
        REQUIRE(race.worker_ok);
        if (!applied)
            MESSAGE("apply failed: ", applied.error().format_message());
        REQUIRE(applied.has_value());
        REQUIRE_FALSE(applied->error.has_value());

        // 删除行是跳过，不是错误：整批照常提交，只是这一条不算写
        CHECK(applied->summary.type_updated == 0);
        CHECK(applied->summary.deleted == 1);
        CHECK(applied->summary.source_changed == 0);
        CHECK(applied->summary.cas_skipped == 0);
        const auto* detail = u04b1_detail(applied->summary, row);
        REQUIRE(detail != nullptr);
        CHECK(detail->write_status == services::ClassificationService::WriteStatus::Deleted);
        CHECK(u04b1_write_status_name(*detail) == "deleted");
        CHECK(u04b1_count_symbols(*db, "id = " + u04b1_sql_literal(row)) == 0);
    }
}

// ------------------------------------------------------------
// R1-04：批边界 —— 当前批失败回滚、后续批未开始、BEGIN 被拒不算回滚
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a failed batch rolls back its candidates and leaves the rest not attempted")
{
    u04b1_store_config(matching_store, matching_config,
                       nlohmann::json::array({u04a_rule("b1-batch-name", "name", "^U04B1Batch",
                                                        "Resistor")}));
    U04B1Library lib(*db, matching_store, temp.path(), "BatchRollback");
    std::string first;
    for (int index = 1; index <= 500; ++index)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "U04B1Batch%03d", index);
        const std::string id = lib.insert(u04b1_part(name, "U", std::string("U")));
        if (index == 1)
            first = id;
    }
    // 第 501 条落在第二批：第一批失败后它必须一条都没被尝试
    const std::string late = lib.insert(u04b1_part("U04B1BatchLate", "U", std::string("U")));
    u04b1_raw_exec(*db, "CREATE TRIGGER b1_r1_batch_block BEFORE UPDATE ON symbols WHEN NEW.id = '" +
                            first + "' BEGIN SELECT RAISE(ABORT, 'b1 batch blocked'); END");

    services::ClassificationService service(db.get(), &matching_store);
    auto failed = service.apply_all({});
    REQUIRE(failed.has_value());
    REQUIRE(failed->error.has_value());
    MESSAGE("batch rollback error: ", failed->error->format_message());
    CHECK(failed->error->format_message().find("0 committed") != std::string::npos);
    CHECK(failed->summary.total == 501);
    CHECK(failed->summary.would_update == 501);
    CHECK(failed->summary.type_updated == 0);
    CHECK(failed->summary.rolled_back == 500);    // 当前失败批的全部候选
    CHECK(failed->summary.not_attempted == 1);    // 后面那一批从未开始
    CHECK(failed->summary.deleted == 0);
    CHECK(failed->summary.source_changed == 0);
    CHECK(failed->summary.cas_skipped == 0);

    // 明细上限仍是 100：能看到的候选全是 RolledBack，绝不展示 Updated
    REQUIRE(failed->summary.results.size() == 100);
    for (const auto& result : failed->summary.results)
    {
        CHECK(result.write_status == services::ClassificationService::WriteStatus::RolledBack);
        CHECK(u04b1_write_status_name(result) == "rolled_back");
        CHECK(result.would_update);
        CHECK(result.candidate_type_id == "Resistor");   // 计划事实仍然如实
    }
    CHECK(u04b1_count_symbols(*db, "component_type = 'Resistor'") == 0);   // 整批真回滚
    CHECK(fix02_stored_type(*db, first) == "Unknown");
    CHECK(fix02_stored_type(*db, late) == "Unknown");
    u04b1_raw_exec(*db, "DROP TRIGGER b1_r1_batch_block");
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a refused batch transaction is not attempted instead of rolled back")
{
    u04b1_store_config(matching_store, matching_config,
                       nlohmann::json::array({u04a_rule("b1-begin-name", "name", "^U04B1Begin",
                                                        "Resistor")}));
    U04B1Library lib(*db, matching_store, temp.path(), "BeginRefused");
    const std::string first = lib.insert(u04b1_part("U04B1BeginOne", "U", std::string("U")));
    const std::string second = lib.insert(u04b1_part("U04B1BeginTwo", "U", std::string("U")));
    const std::string third = lib.insert(u04b1_part("U04B1BeginThree", "U", std::string("U")));

    services::ClassificationService service(db.get(), &matching_store);
    {
        U04B1Authorizer hook(*db, &u04b1_deny_begin);
        auto refused = service.apply_all({});
        REQUIRE(refused.has_value());
        REQUIRE(refused->error.has_value());
        MESSAGE("refused begin error: ", refused->error->format_message());
        CHECK(hook.hits.load() == 1);   // 第一批 BEGIN 被拒后立即停止，没有继续尝试
        CHECK(refused->summary.would_update == 3);
        CHECK(refused->summary.type_updated == 0);
        CHECK(refused->summary.rolled_back == 0);      // 没有事务被开启，谈不上回滚
        CHECK(refused->summary.not_attempted == 3);    // 本批与后续候选都没开始
        CHECK(refused->summary.deleted == 0);
        for (const std::string& id : {first, second, third})
        {
            const auto* detail = u04b1_detail(refused->summary, id);
            REQUIRE(detail != nullptr);
            CHECK(detail->write_status ==
                  services::ClassificationService::WriteStatus::NotAttempted);
            CHECK(u04b1_write_status_name(*detail) == "not_attempted");
            CHECK_FALSE(detail->write_reason.empty());
        }
        CHECK(u04b1_count_symbols(*db, "component_type = 'Resistor'") == 0);
    }   // RAII 摘钩

    // 去故障重试：同一条判定内核与写回路径把三条都真的写上
    auto retried = service.apply_all({});
    REQUIRE(retried.has_value());
    REQUIRE_FALSE(retried->error.has_value());
    CHECK(retried->summary.type_updated == 3);
    CHECK(retried->summary.rolled_back == 0);
    CHECK(retried->summary.not_attempted == 0);
    CHECK(fix02_stored_type(*db, first) == "Resistor");
    CHECK(fix02_stored_type(*db, second) == "Resistor");
    CHECK(fix02_stored_type(*db, third) == "Resistor");
}

// ------------------------------------------------------------
// R1-05：明细上限 100 不影响计数（预览同样汇报真实的写计划）
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 the default detail cap keeps the full decision counts")
{
    u04b1_store_config(matching_store, matching_config,
                       nlohmann::json::array({u04a_rule("b1-cap-name", "name", "^U04B1Cap",
                                                        "Resistor")}));
    U04B1Library lib(*db, matching_store, temp.path(), "DetailCap");
    for (int index = 1; index <= 105; ++index)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "U04B1Cap%03d", index);
        lib.insert(u04b1_part(name, "U", std::string("U")));
    }

    services::ClassificationService service(db.get(), &matching_store);
    auto preview = service.preview_all();
    REQUIRE(preview.has_value());
    MESSAGE("cap preview: total=", preview->total, " matched=", preview->matched,
            " results=", preview->results.size(), " would_update=", preview->would_update);
    CHECK(preview->total == 105);
    CHECK(preview->matched == 105);
    CHECK(preview->would_update == 105);
    CHECK(preview->unchanged == 0);
    CHECK(preview->type_updated == 0);          // 预览不写库
    CHECK(preview->results.size() == 100);      // 明细上限仍是 100
    for (const auto& result : preview->results)
    {
        CHECK(result.status == "matched");
        CHECK(result.would_update);
        CHECK(result.is_protected == false);
        CHECK(result.write_status == services::ClassificationService::WriteStatus::ReadOnly);
    }
    CHECK(u04b1_count_symbols(*db, "component_type = 'Resistor'") == 0);
}

// ------------------------------------------------------------
// B9-06：SAVEPOINT 局部回滚 —— 两个封装一个成功一个失败，
// 共享模型去重，失败文件无半行/无 mtime，成功文件可提交
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 savepoint keeps a failed footprint out of the committed batch and dedups the shared model")
{
    auto dir = temp.path() / "b9_partial";
    write(dir / "shared.step", "SHARED-STEP");
    write(dir / "Good.kicad_mod",
          "(footprint \"Good\" (layer \"F.Cu\") (model \"shared.step\"))");
    write(dir / "Bad.kicad_mod",
          "(footprint \"Bad\" (layer \"F.Cu\") (model \"shared.step\"))");
    const std::string bad_bytes = read(dir / "Bad.kicad_mod");

    u04b1_raw_exec(*db,
                   "CREATE TRIGGER b9_partial_bad BEFORE INSERT ON footprints "
                   "WHEN NEW.name = 'Bad' "
                   "BEGIN SELECT RAISE(ABORT, 'b9 partial bad footprint'); END");

    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    auto r = pipe | services::footprints_from{dir.string()} | services::execute;
    MESSAGE("partial batch: footprints=", r.footprints, " models=", r.models_3d,
            " failed_files=", r.failed_files, " error_count=", r.error_count);
    CHECK_FALSE(r.ok());
    CHECK(r.footprints == 1);        // 成功文件已提交
    CHECK(r.models_3d == 1);         // 共享模型只创建一次
    CHECK(r.failed_files == 1);      // 失败文件如实验告
    CHECK(r.error_count == 1);
    {
        auto session = db->session();
        auto good = storage::FootprintRepository(session).find_by_name("Good");
        REQUIRE(good.has_value());
        auto links = storage::RelationshipRepository(session)
                         .find_models_for_footprint(good->id());
        REQUIRE(links.has_value());
        REQUIRE(links->size() == 1);
    }
    CHECK(count_rows(*db, "footprints") == 1);            // Bad 没有半行
    CHECK(count_rows(*db, "models_3d") == 1);             // 共享模型去重
    CHECK(count_rows(*db, "footprint_model_links") == 1);
    CHECK(u04b1_imported_files(*db) == 2);                // Good + shared.step
    {
        const auto snapshots = u04b1_imported_map(*db);
        bool bad_recorded = false;
        for (const auto& [key, value] : snapshots)
        {
            (void)value;
            if (fs::path(key).filename() == "Bad.kicad_mod")
                bad_recorded = true;
        }
        CHECK_FALSE(bad_recorded);   // 失败文件没有 mtime 快照
    }
    CHECK(read(dir / "Bad.kicad_mod") == bad_bytes);

    // 去故障后同一来源重试：Bad 真正提交，共享模型不重复创建也不重复计数
    u04b1_raw_exec(*db, "DROP TRIGGER b9_partial_bad");
    services::ImportPipeline retry(*db, nullptr, false, &matching_store);
    auto after = retry | services::footprints_from{dir.string()} | services::execute;
    MESSAGE("partial retry: footprints=", after.footprints, " models=", after.models_3d,
            " errors=", after.errors.size());
    CHECK(after.ok());
    CHECK(after.footprints == 1);
    CHECK(after.models_3d == 0);     // 共享模型未变，不重复计数
    CHECK(count_rows(*db, "footprints") == 2);
    CHECK(count_rows(*db, "footprint_model_links") == 2);
    CHECK(count_rows(*db, "models_3d") == 1);
    CHECK(u04b1_imported_files(*db) == 3);
}

// ------------------------------------------------------------
// B9-07：整批 COMMIT 被拒 —— 两个文件计数 0，failed_files == 2，
// 解除故障后同源第一次重试整批提交
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a rejected whole-batch commit reports two failed files with zero counts")
{
    auto dir = temp.path() / "b9_commit2";
    write(dir / "Left.kicad_mod", "(footprint \"Left\" (layer \"F.Cu\"))");
    write(dir / "Right.kicad_mod", "(footprint \"Right\" (layer \"F.Cu\"))");

    {
        U04B1Authorizer hook(*db, &u04b1_deny_commit);
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto r = pipe | services::footprints_from{dir.string()} | services::execute;
        MESSAGE("two-file commit denied: footprints=", r.footprints,
                " failed_files=", r.failed_files, " error_count=", r.error_count,
                " hits=", hook.hits.load());
        REQUIRE(hook.hits.load() == 1);   // 前提：整批唯一的显式 COMMIT 真的被拒过
        CHECK_FALSE(r.ok());
        CHECK(r.footprints == 0);        // 整批回滚：一条都不能算已导入
        CHECK(r.models_3d == 0);
        CHECK(r.failed_files == 2);      // 两个没有提交的来源文件
        CHECK(r.error_count == 1);       // 一个 COMMIT 错误事件，不按文件数虚增
        CHECK_FALSE(r.errors.empty());
        if (!r.errors.empty())
            CHECK(r.errors.front().find("commit") != std::string::npos);
    }
    CHECK(count_rows(*db, "footprints") == 0);
    CHECK(u04b1_imported_files(*db) == 0);

    // 解除故障：同源第一次重试整批提交
    services::ImportPipeline retry(*db, nullptr, false, &matching_store);
    auto after = retry | services::footprints_from{dir.string()} | services::execute;
    MESSAGE("whole-batch retry: footprints=", after.footprints, " errors=", after.errors.size());
    CHECK(after.ok());
    CHECK(after.footprints == 2);
    CHECK(count_rows(*db, "footprints") == 2);
    CHECK(u04b1_imported_files(*db) == 2);
}

// ------------------------------------------------------------
// B9-08：模型扫描的 INSERT / 快照写失败 —— 错误可见、不消费 mtime、
// 同内容同 mtime 的下一次运行可重试成功
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a blocked model insert or snapshot write keeps the scan failure visible and retryable")
{
    auto dir = temp.path() / "b9_model_scan";
    write(dir / "Fresh.step", "STEP-fresh");
    const std::string bytes = read(dir / "Fresh.step");
    const std::string step_path = (dir / "Fresh.step").string();

    auto scenario = [&](const std::string& trigger_name, const std::string& trigger_sql,
                        const std::string& expected_text)
    {
        u04b1_raw_exec(*db, trigger_sql);
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto r = pipe | services::models_from{dir.string()} | services::execute;
        MESSAGE("blocked model scan (", trigger_name, "): models=", r.models_3d,
                " failed_files=", r.failed_files, " error_count=", r.error_count);
        CHECK_FALSE(r.ok());
        CHECK(r.models_3d == 0);         // 失败不能计成已导入
        CHECK(r.failed_files == 1);      // 真实来源文件失败
        CHECK(r.error_count == 1);
        bool mentioned = false;
        for (const auto& message : r.errors)
            if (message.find(expected_text) != std::string::npos)
                mentioned = true;
        CHECK(mentioned);                // 具体触发器文本可见
        CHECK(count_rows(*db, "models_3d") == 0);
        CHECK(u04b1_imported_files(*db) == 0);   // mtime 未被消费
        CHECK(read(dir / "Fresh.step") == bytes);

        u04b1_raw_exec(*db, "DROP TRIGGER " + trigger_name);
        services::ImportPipeline retry(*db, nullptr, false, &matching_store);
        auto after = retry | services::models_from{dir.string()} | services::execute;
        MESSAGE("model scan retry (", trigger_name, "): models=", after.models_3d,
                " errors=", after.errors.size());
        CHECK(after.ok());
        CHECK(after.models_3d == 1);
        CHECK(count_rows(*db, "models_3d") == 1);
        CHECK(u04b1_imported_files(*db) == 1);
    };

    SUBCASE("the model row insert is blocked")
    {
        scenario("b9_model_insert_block",
                 "CREATE TRIGGER b9_model_insert_block BEFORE INSERT ON models_3d "
                 "BEGIN SELECT RAISE(ABORT, 'b9 model scan insert blocked'); END",
                 "b9 model scan insert blocked");
    }

    SUBCASE("the model snapshot write is blocked")
    {
        scenario("b9_model_mtime_block",
                 "CREATE TRIGGER b9_model_mtime_block BEFORE INSERT ON imported_files "
                 "WHEN NEW.file_path = " + u04b1_sql_literal(step_path) +
                     " BEGIN SELECT RAISE(ABORT, 'b9 model snapshot blocked'); END",
                 "b9 model snapshot blocked");
    }
}

// ------------------------------------------------------------
// B9-09：SAVEPOINT 控制失败 —— 子写原因与控制错误两个真实事件都在，
// 整批终止（不假继续），failed_files 如实，去故障后同源重试可提交
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a savepoint control failure keeps both the write reason and the control error")
{
    auto dir = temp.path() / "b9_spctl";
    write(dir / "Solo.kicad_mod", "(footprint \"Solo\" (layer \"F.Cu\"))");
    const std::string bytes = read(dir / "Solo.kicad_mod");
    u04b1_raw_exec(*db,
                   "CREATE TRIGGER b9_spctl_block BEFORE INSERT ON footprints "
                   "WHEN NEW.name = 'Solo' "
                   "BEGIN SELECT RAISE(ABORT, 'b9 savepoint sub blocked'); END");

    {
        U04B1Authorizer hook(*db, &u04b1_deny_savepoint_rollback);
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto r = pipe | services::footprints_from{dir.string()} | services::execute;
        MESSAGE("savepoint control denied: error_count=", r.error_count,
                " failed_files=", r.failed_files, " footprints=", r.footprints,
                " hits=", hook.hits.load());
        REQUIRE(hook.hits.load() == 1);   // 前提：SAVEPOINT 的 ROLLBACK 控制真的被拒过一次；
                                          // 夹具故障未触达时在此明确失败，不假报业务红
        CHECK_FALSE(r.ok());
        CHECK(r.footprints == 0);       // 整批终止，不假继续
        CHECK(r.error_count == 2);      // 写原因 + 控制原因两个真实事件
        CHECK(r.failed_files == 1);     // 整批回滚：该文件没有提交
        bool write_reason = false;
        bool control_reason = false;
        for (const auto& message : r.errors)
        {
            if (message.find("b9 savepoint sub blocked") != std::string::npos)
                write_reason = true;
            if (message.find("footprint import rolled back") != std::string::npos)
                control_reason = true;
        }
        CHECK(write_reason);       // 原子写原因没有被控制失败挤掉
        CHECK(control_reason);     // 控制错误事件也在
    }
    CHECK(count_rows(*db, "footprints") == 0);
    CHECK(u04b1_imported_files(*db) == 0);
    CHECK(read(dir / "Solo.kicad_mod") == bytes);

    // 去故障：同源第一次重试真正提交
    u04b1_raw_exec(*db, "DROP TRIGGER b9_spctl_block");
    services::ImportPipeline retry(*db, nullptr, false, &matching_store);
    auto after = retry | services::footprints_from{dir.string()} | services::execute;
    MESSAGE("savepoint retry: footprints=", after.footprints, " errors=", after.errors.size());
    CHECK(after.ok());
    CHECK(after.footprints == 1);
    CHECK(count_rows(*db, "footprints") == 1);
    CHECK(u04b1_imported_files(*db) == 1);
}

// ------------------------------------------------------------
// B9-10：共享模型 —— 同批两个健康 FP 指向同一个真实 Shared.step。
// 批内文件次序由解析完成序决定（不是文件名序），下面所有断言只依赖最终行为。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 two footprints share one committed model in a single batch")
{
    auto dir = temp.path() / "b9_share_ok";
    write(dir / "Shared.step", "SHARED-STEP-OK");
    write(dir / "First.kicad_mod",
          "(footprint \"First\" (layer \"F.Cu\") (model \"Shared.step\"))");
    write(dir / "Second.kicad_mod",
          "(footprint \"Second\" (layer \"F.Cu\") (model \"Shared.step\"))");
    const std::string first_bytes = read(dir / "First.kicad_mod");
    const std::string second_bytes = read(dir / "Second.kicad_mod");
    const std::string step_bytes = read(dir / "Shared.step");

    // 前提：两个文件的真实 parser 都解析出同一条 Shared.step 引用
    for (const char* name : {"First.kicad_mod", "Second.kicad_mod"})
    {
        auto parsed = parser::FootprintParser::parse(dir / name);
        if (!parsed)
            MESSAGE(name, " parse failed: ", parsed.error().format_message());
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->models_3d().size() == 1);
        REQUIRE_FALSE(parsed->models_3d().front().path.empty());
    }

    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    auto r = pipe | services::footprints_from{dir.string()} | services::execute;
    MESSAGE("shared ok batch: footprints=", r.footprints, " models=", r.models_3d,
            " errors=", r.errors.size());
    REQUIRE(r.ok());
    CHECK(r.footprints == 2);
    CHECK(r.models_3d == 1);          // 共享模型只创建一次，无论谁先处理
    CHECK(count_rows(*db, "footprints") == 2);
    CHECK(count_rows(*db, "models_3d") == 1);
    CHECK(count_rows(*db, "footprint_model_links") == 2);
    CHECK(u04b1_imported_files(*db) == 3);   // 两个封装 + Shared.step

    // 两个 FP 都链到同一个真实模型行，模型路径就是 Shared.step
    std::string shared_model_id;
    {
        auto session = db->session();
        for (const char* name : {"First", "Second"})
        {
            auto fp = storage::FootprintRepository(session).find_by_name(name);
            REQUIRE(fp.has_value());
            auto links = storage::RelationshipRepository(session)
                             .find_models_for_footprint(fp->id());
            REQUIRE(links.has_value());
            REQUIRE(links->size() == 1);
            auto model = storage::Model3DRepository(session).find_by_id(links->front());
            REQUIRE(model.has_value());
            CHECK(fs::weakly_canonical(fs::path(model->file_path())) ==
                  fs::weakly_canonical(dir / "Shared.step"));
            if (shared_model_id.empty())
                shared_model_id = model->id();
            CHECK(model->id() == shared_model_id);
        }
    }
    // 三个来源快照各恰一份（不靠键序，按文件名计数）
    const auto snapshots = u04b1_imported_map(*db);
    CHECK(snapshots.size() == 3);
    for (const char* name : {"First.kicad_mod", "Second.kicad_mod", "Shared.step"})
    {
        int count = 0;
        for (const auto& [key, value] : snapshots)
        {
            (void)value;
            if (fs::path(key).filename() == name)
                ++count;
        }
        CHECK(count == 1);
    }
    // 首次真实提交，源字节全部不变
    CHECK(read(dir / "First.kicad_mod") == first_bytes);
    CHECK(read(dir / "Second.kicad_mod") == second_bytes);
    CHECK(read(dir / "Shared.step") == step_bytes);
}

// ------------------------------------------------------------
// B9-11：关系阶段的晚失败 —— FP 插入与模型动作都放行，只有
// footprint_model_links 的 INSERT 被真实 trigger 拒（用 footprints.name 定位）。
// 早失败（BEFORE INSERT ON footprints）不能冒充这种暂存后的晚失败。
// 批内文件次序不定：所有断言按最终行为，两种次序都必须成立。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 a late link rejection keeps the healthy file and the shared model")
{
    auto dir = temp.path() / "b9_share_late";
    write(dir / "Shared.step", "SHARED-STEP-LATE");
    write(dir / "Good.kicad_mod",
          "(footprint \"Good\" (layer \"F.Cu\") (model \"Shared.step\"))");
    write(dir / "LateBad.kicad_mod",
          "(footprint \"LateBad\" (layer \"F.Cu\") (model \"Shared.step\"))");
    const std::string good_bytes = read(dir / "Good.kicad_mod");
    const std::string bad_bytes = read(dir / "LateBad.kicad_mod");
    const std::string step_bytes = read(dir / "Shared.step");

    // 故障只挂在关系阶段：不拦 footprints 的 INSERT，真实 trigger 按
    // footprints.name 关联定位 LateBad 的关系行
    u04b1_raw_exec(*db,
                   "CREATE TRIGGER b9_share_late_block BEFORE INSERT ON footprint_model_links "
                   "WHEN EXISTS (SELECT 1 FROM footprints "
                   "WHERE id = NEW.footprint_id AND name = 'LateBad') "
                   "BEGIN SELECT RAISE(ABORT, 'b9 late link blocked'); END");

    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    auto r = pipe | services::footprints_from{dir.string()} | services::execute;
    MESSAGE("late link batch: footprints=", r.footprints, " models=", r.models_3d,
            " failed_files=", r.failed_files, " error_count=", r.error_count);
    CHECK_FALSE(r.ok());
    CHECK(r.error_count == 1);
    CHECK(r.failed_files == 1);       // LateBad 所在文件没有提交
    CHECK(r.footprints == 1);         // 只有 Good 提交
    CHECK(r.models_3d == 1);          // 共享模型由成功文件真实提交（两种次序同值）
    bool link_reason = false;
    for (const auto& message : r.errors)
        if (message.find("b9 late link blocked") != std::string::npos)
            link_reason = true;
    CHECK(link_reason);               // 关系阶段的具体触发器文本必须可见

    CHECK(count_rows(*db, "footprints") == 1);            // LateBad 没有半行
    CHECK(count_rows(*db, "models_3d") == 1);             // 共享模型恰一行
    CHECK(count_rows(*db, "footprint_model_links") == 1); // 只有 Good 的关系
    CHECK(u04b1_imported_files(*db) == 2);                // Good + Shared.step
    {
        auto session = db->session();
        auto good = storage::FootprintRepository(session).find_by_name("Good");
        REQUIRE(good.has_value());
        auto bad = storage::FootprintRepository(session).find_by_name("LateBad");
        CHECK_FALSE(bad.has_value());   // 被 rollback 的 FP 不可查
        auto links = storage::RelationshipRepository(session)
                         .find_models_for_footprint(good->id());
        REQUIRE(links.has_value());
        CHECK(links->size() == 1);
    }
    // LateBad 的封装快照没有提交，Shared.step 的快照在
    {
        const auto snapshots = u04b1_imported_map(*db);
        int bad_snapshots = 0;
        int step_snapshots = 0;
        for (const auto& [key, value] : snapshots)
        {
            (void)value;
            if (fs::path(key).filename() == "LateBad.kicad_mod")
                ++bad_snapshots;
            if (fs::path(key).filename() == "Shared.step")
                ++step_snapshots;
        }
        CHECK(bad_snapshots == 0);
        CHECK(step_snapshots == 1);
    }
    CHECK(read(dir / "Good.kicad_mod") == good_bytes);
    CHECK(read(dir / "LateBad.kicad_mod") == bad_bytes);
    CHECK(read(dir / "Shared.step") == step_bytes);

    // 去故障后同一来源的第一次重试：LateBad 真正提交，共享模型不重复
    u04b1_raw_exec(*db, "DROP TRIGGER b9_share_late_block");
    services::ImportPipeline retry(*db, nullptr, false, &matching_store);
    auto after = retry | services::footprints_from{dir.string()} | services::execute;
    MESSAGE("late link retry: footprints=", after.footprints, " models=", after.models_3d,
            " errors=", after.errors.size());
    CHECK(after.ok());
    CHECK(after.footprints == 1);
    CHECK(after.models_3d == 0);      // Shared.step 未变：不重复创建也不重复计数
    CHECK(count_rows(*db, "footprints") == 2);
    CHECK(count_rows(*db, "models_3d") == 1);
    CHECK(count_rows(*db, "footprint_model_links") == 2);
    CHECK(u04b1_imported_files(*db) == 3);
}

// ------------------------------------------------------------
// B9-12 回归：新模型事务的显式 ROLLBACK 失败必须成为第二个错误事件。
// 产品已修：模型 scan 的 rollback 结果照实上报，不再 (void) 丢弃；
// 本回归锁定 insert 原因与 rollback 控制原因都必须独立可见。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture,
                  "U04-B1 a denied explicit model rollback is a second error and the connection recovers")
{
    auto dir = temp.path() / "b9_model_txn";
    write(dir / "Solo.step", "STEP-TXN");
    const std::string bytes = read(dir / "Solo.step");
    CHECK(count_rows(*db, "models_3d") == 0);
    CHECK(u04b1_imported_files(*db) == 0);

    u04b1_raw_exec(*db,
                   "CREATE TRIGGER b9_txn_model_block BEFORE INSERT ON models_3d "
                   "BEGIN SELECT RAISE(ABORT, 'b9 txn model insert blocked'); END");

    {
        U04B1Authorizer hook(*db, &u04b1_deny_first_rollback);
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto r = pipe | services::models_from{dir.string()} | services::execute;
        hook.remove();   // 业务调用结束后先摘钩，再做任何 DB 检查
        MESSAGE("denied explicit rollback: error_count=", r.error_count,
                " failed_files=", r.failed_files, " models=", r.models_3d,
                " rollback_hits=", hook.hits.load());
        // 前提：第一次显式 ROLLBACK 被拒（hit 1），DbSession 析构的重试被放行（hit 2）
        REQUIRE(hook.hits.load() == 2);
        CHECK_FALSE(r.ok());
        CHECK(r.models_3d == 0);
        CHECK(r.failed_files == 1);
        bool insert_reason = false;
        for (const auto& message : r.errors)
            if (message.find("b9 txn model insert blocked") != std::string::npos)
                insert_reason = true;
        CHECK(insert_reason);           // insert 原因必须可见
        bool rollback_reason = false;   // 回归：rollback 控制原因必须独立可见
        for (const auto& message : r.errors)
            if (message.find("ROLLBACK") != std::string::npos
                && message.find("not authorized") != std::string::npos)
                rollback_reason = true;
        CHECK(rollback_reason);         // 真实 DbSession 文本："ROLLBACK: not authorized"
        // 回归：显式 rollback 的控制原因不再被 (void) 丢弃，error_count == 2
        CHECK(r.error_count == 2);
        // 连接已恢复自动提交：模型与快照都没有提交，源字节不变
        CHECK(count_rows(*db, "models_3d") == 0);
        CHECK(u04b1_imported_files(*db) == 0);
        {
            auto session = db->session();
            CHECK(sqlite3_get_autocommit(session.handle()) == 1);
        }
        CHECK(read(dir / "Solo.step") == bytes);
    }

    // 去故障后同 mtime 的第一次重试真正提交
    u04b1_raw_exec(*db, "DROP TRIGGER b9_txn_model_block");
    services::ImportPipeline retry(*db, nullptr, false, &matching_store);
    auto after = retry | services::models_from{dir.string()} | services::execute;
    MESSAGE("model txn retry: models=", after.models_3d, " errors=", after.errors.size());
    CHECK(after.ok());
    CHECK(after.models_3d == 1);
    CHECK(count_rows(*db, "models_3d") == 1);
    CHECK(u04b1_imported_files(*db) == 1);
}

// ------------------------------------------------------------
// U04-B1 探针 1：未变化模型扫描的 skipped 只留在扫描内部，不得透传进旧
// Result.skipped。旧口径只有符号/封装 skipped，模型 skip 只进内部日志。
// 回归事实：execute 曾真实新增 r.skipped += scan.skipped，第二次全
// unchanged 的重扫把旧计数污染成 1（U04-B1 红）。修复后该透传已删除，
// 本测试守护旧口径不被再次破坏。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 unchanged model scanning keeps the legacy skipped counter")
{
    auto dir = temp.path() / "u04b1_model_skip";
    write(dir / "Model.step", "STEP-SKIP");
    const std::string bytes = read(dir / "Model.step");
    CHECK(count_rows(*db, "models_3d") == 0);
    CHECK(u04b1_imported_files(*db) == 0);

    // 第一次：真实导入 1 个模型 + 1 条来源快照
    services::ImportPipeline first(*db, nullptr, false, &matching_store);
    auto r1 = first | services::models_from{dir.string()} | services::execute;
    MESSAGE("first model scan: models=", r1.models_3d, " skipped=", r1.skipped,
            " errors=", r1.errors.size());
    REQUIRE(r1.ok());
    REQUIRE(r1.models_3d == 1);
    REQUIRE(r1.skipped == 0);
    CHECK(count_rows(*db, "models_3d") == 1);
    CHECK(u04b1_imported_files(*db) == 1);

    // 第二个新 pipeline、同文件同 mtime：整目录 unchanged，只走内部 skip 日志
    services::ImportPipeline second(*db, nullptr, false, &matching_store);
    auto r2 = second | services::models_from{dir.string()} | services::execute;
    MESSAGE("unchanged rescan: models=", r2.models_3d, " skipped=", r2.skipped,
            " error_count=", r2.error_count, " failed_files=", r2.failed_files);
    CHECK(r2.ok());
    CHECK(r2.models_3d == 0);
    CHECK(r2.error_count == 0);
    CHECK(r2.failed_files == 0);
    CHECK(r2.skipped == 0);
    // DB / 快照 / 源字节都不动
    CHECK(count_rows(*db, "models_3d") == 1);
    CHECK(u04b1_imported_files(*db) == 1);
    CHECK(read(dir / "Model.step") == bytes);
}

// ------------------------------------------------------------
// U04-B1 探针 2：封装批的外层 ROLLBACK 失败必须成为第三个错误事件。
// 链条：子写被 trigger 拒（原因 1）→ ROLLBACK TO SAVEPOINT 被拒
// （原因 2，真实文本含 "ROLLBACK TO SAVEPOINT" + "not authorized"）→
// 外层显式 ROLLBACK 被拒（原因 3）。回归事实：pipeline.cpp 曾用 (void)
// session.rollback() 把原因 3 丢弃、只报 2（U04-B1 红）；修复后三个原因
// 各一个事件。析构重试放行后连接必须恢复自动提交，且不能永久拒所有 rollback。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture,
                  "U04-B1 a denied outer footprint rollback preserves all three causes")
{
    auto dir = temp.path() / "u04b1_fp_outer_rollback";
    write(dir / "Solo.kicad_mod", "(footprint \"Solo\")");
    const std::string bytes = read(dir / "Solo.kicad_mod");
    CHECK(count_rows(*db, "footprints") == 0);
    CHECK(u04b1_imported_files(*db) == 0);

    u04b1_raw_exec(*db,
                   "CREATE TRIGGER u04b1_fp_child_block BEFORE INSERT ON footprints "
                   "BEGIN SELECT RAISE(ABORT, 'u04-b1 fp child insert blocked'); END");

    {
        U04B1DualDenyHook hook(*db);
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        auto r = pipe | services::footprints_from{dir.string()} | services::execute;
        hook.remove();   // 业务调用结束后先摘钩，再做任何 DB 检查
        MESSAGE("outer fp rollback: error_count=", r.error_count,
                " failed_files=", r.failed_files, " footprints=", r.footprints,
                " sp_hits=", hook.sp_hits.load(), " txn_hits=", hook.txn_hits.load());
        // 前提：SAVEPOINT 回滚控制被拒 1 次；第一次显式 ROLLBACK 被拒，
        // DbSession 析构重试被放行（事务已清理）
        REQUIRE(hook.sp_hits.load() == 1);
        REQUIRE(hook.txn_hits.load() == 2);
        CHECK_FALSE(r.ok());
        CHECK(r.footprints == 0);
        CHECK(r.failed_files == 1);
        bool child_reason = false;
        bool savepoint_reason = false;
        bool outer_reason = false;
        for (const auto& message : r.errors)
        {
            if (message.find("u04-b1 fp child insert blocked") != std::string::npos)
                child_reason = true;
            if (message.find("ROLLBACK TO SAVEPOINT") != std::string::npos
                && message.find("not authorized") != std::string::npos)
                savepoint_reason = true;
            // 外层原因排除 SAVEPOINT 控制：DbSession 的显式 ROLLBACK 文本
            // 是 "ROLLBACK: not authorized"，不含 "ROLLBACK TO SAVEPOINT"
            if (message.find("ROLLBACK") != std::string::npos
                && message.find("not authorized") != std::string::npos
                && message.find("ROLLBACK TO SAVEPOINT") == std::string::npos)
                outer_reason = true;
        }
        CHECK(child_reason);      // 原因 1：trigger 拒绝的子写
        CHECK(savepoint_reason);  // 原因 2：SAVEPOINT 回滚控制失败
        // 回归事实：外层真实 ROLLBACK 的控制原因曾被 (void) 丢弃、error_count
        // 只有 2（U04-B1 红）；修复后原因 3 独立成事件，error_count==3
        CHECK(outer_reason);      // 原因 3：外层显式 ROLLBACK 控制失败
        CHECK(r.error_count == 3);
        // 连接已恢复自动提交：封装与快照都没有提交，源字节不变
        CHECK(count_rows(*db, "footprints") == 0);
        CHECK(u04b1_imported_files(*db) == 0);
        {
            auto session = db->session();
            CHECK(sqlite3_get_autocommit(session.handle()) == 1);
        }
        CHECK(read(dir / "Solo.kicad_mod") == bytes);
    }

    // 不能永久拒所有 rollback：去故障后同 source/mtime 的首次重试正常提交
    u04b1_raw_exec(*db, "DROP TRIGGER u04b1_fp_child_block");
    services::ImportPipeline retry(*db, nullptr, false, &matching_store);
    auto after = retry | services::footprints_from{dir.string()} | services::execute;
    MESSAGE("fp outer retry: footprints=", after.footprints, " errors=", after.errors.size());
    CHECK(after.ok());
    CHECK(after.footprints == 1);
    CHECK(count_rows(*db, "footprints") == 1);
    CHECK(u04b1_imported_files(*db) == 1);
}

// ------------------------------------------------------------
// U04-B1 探针 3：KF 写回后的快照刷新失败，rollback 控制原因不得丢失。
// 链条：符号库事务照常提交（symbols=1，原快照走 INSERT 放行），KF 宿主
// 写回真实发生（源出现 KF 属性），之后 record_file 的第二次 upsert 走
// conflict UPDATE 才被拒（原因 1，指定 trigger 文本）。回归事实：该路径
// 曾用 (void) session.rollback() 把真实 ROLLBACK 控制原因丢弃、只报 1
// （U04-B1 红）；修复后快照原因与 ROLLBACK 控制原因各一个事件，已提交
// 的来源不算失败。
// ------------------------------------------------------------
TEST_CASE_FIXTURE(LibraryFixture, "U04-B1 post-KF snapshot rollback failure preserves both errors")
{
    REQUIRE(read(file).find("Kicad_Forge_ID") == std::string::npos);   // 前提：源起初无 KF 属性
    auto owner = group(false, false);   // 未锁定用户库：KF 写回合法
    const std::string path_literal = u04b1_sql_literal(file.string());

    u04b1_raw_exec(*db,
                   "CREATE TRIGGER u04b1_kf_snapshot_block BEFORE UPDATE ON imported_files "
                   "WHEN NEW.file_path = " + path_literal + " "
                   "BEGIN SELECT RAISE(ABORT, 'u04-b1 kf snapshot refresh blocked'); END");

    std::string kf_id;
    core::Uuid symbol_id{};
    {
        U04B1Authorizer hook(*db, &u04b1_deny_first_rollback);
        auto r = import(true, owner);   // 真实 opt-in 写回路径
        hook.remove();   // 安全先摘钩，再做任何 DB / 文件检查
        MESSAGE("post-KF snapshot: symbols=", r.symbols, " error_count=", r.error_count,
                " failed_files=", r.failed_files, " rollback_hits=", hook.hits.load());
        // 前提：patch 确实发生（源已有 KF 属性且 parser 认得出非空 ID）；
        // rollback 只拒一次，析构重试放行
        REQUIRE(hook.hits.load() == 2);
        REQUIRE(read(file).find("Kicad_Forge_ID") != std::string::npos);
        auto patched = parser::SymbolLibParser::parse(file);
        REQUIRE(patched.has_value());
        REQUIRE(patched->items.size() == 1);
        REQUIRE_FALSE(patched->items.front().Kicad_Forge_ID().empty());
        kf_id = patched->items.front().Kicad_Forge_ID();
        {
            auto session = db->session();
            auto rows = storage::SymbolRepository(session).find_all();
            REQUIRE(rows.has_value());
            REQUIRE(rows->size() == 1);
            REQUIRE(rows->front().Kicad_Forge_ID() == kf_id);
            symbol_id = rows->front().id();   // 捕获真实 DB 主键，供 force retry 比对
        }

        // 符号库已提交：原 imported 快照在 INSERT 路径放行
        CHECK(r.symbols == 1);
        CHECK(u04b1_count_symbols(*db, "name = 'R'") == 1);
        CHECK(u04b1_imported_files(*db) == 1);
        // 已提交的来源不错误计为未提交
        CHECK(r.failed_files == 0);
        bool snapshot_reason = false;
        bool rollback_reason = false;
        for (const auto& message : r.errors)
        {
            if (message.find("u04-b1 kf snapshot refresh blocked") != std::string::npos)
                snapshot_reason = true;
            if (message.find("ROLLBACK") != std::string::npos
                && message.find("not authorized") != std::string::npos)
                rollback_reason = true;
        }
        CHECK(snapshot_reason);   // 原因 1：快照刷新被拒
        // 回归事实：record_file 的 (void) rollback 曾丢弃控制原因、error_count
        // 只有 1（U04-B1 红）；修复后原因 2 独立成事件，error_count==2
        CHECK(rollback_reason);   // 原因 2：该路径真实 ROLLBACK 控制失败
        CHECK(r.error_count == 2);
        {
            auto session = db->session();
            CHECK(sqlite3_get_autocommit(session.handle()) == 1);
        }
    }

    // 不能永久拒 rollback，也不假称源未改：去 trigger 后 force 重导第一次正常，
    // UUID / 文件字节 / 快照行数都不增
    u04b1_raw_exec(*db, "DROP TRIGGER u04b1_kf_snapshot_block");
    const std::string patched_bytes = read(file);
    services::ImportPipeline retry(*db, nullptr, false, &matching_store, /*force_reimport=*/true);
    auto after = retry | services::symbols_from{file.parent_path().string(), owner}
               | services::execute;
    MESSAGE("post-KF force retry: symbols=", after.symbols, " errors=", after.errors.size());
    CHECK(after.ok());
    {
        auto session = db->session();
        auto rows = storage::SymbolRepository(session).find_all();
        REQUIRE(rows.has_value());
        REQUIRE(rows->size() == 1);
        CHECK(rows->front().id() == symbol_id);           // UUID 稳定：重导不换主键
        CHECK(rows->front().Kicad_Forge_ID() == kf_id);   // KF ID 同时保持
    }
    CHECK(u04b1_count_symbols(*db, "name = 'R'") == 1);
    CHECK(u04b1_imported_files(*db) == 1);
    CHECK(read(file) == patched_bytes);   // 已写回的 KF patch 不被重导破坏
}

TEST_CASE_FIXTURE(LibraryFixture,
                  "U04-B2 staged: matching failures keep Unknown and are recorded as warnings")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto exercise = [&](MatchStatus expected_status, const nlohmann::json& rules,
                        const std::string& value, const std::string& mpn)
    {
        // 第一条 name 规则本身可命中；完整样本的异常必须作废该候选。
        core::Symbol name_only;
        name_only.set_name("STUB_C934");
        const auto first_rule = u04a_snapshot(u04a_config(nlohmann::json::array({rules.front()})));
        REQUIRE(first_rule->evaluate(MatchSample::from_symbol(name_only)).status
                == MatchStatus::Matched);
        REQUIRE(matching_store.save(u04a_config(rules), 0).has_value());
        REQUIRE(matching_store.acquire_business_snapshot().has_value());

        core::Symbol expected;
        expected.set_name("STUB_C934");
        expected.set_default_value(value);
        expected.set_mpn(mpn);
        expected.set_property("reference", "U");
        const auto decision = matching_store.current()->evaluate(MatchSample::from_symbol(expected));
        REQUIRE(decision.status == expected_status);
        REQUIRE(decision.type_id.empty());
        REQUIRE_FALSE(decision.reason.empty());
        if (expected_status == MatchStatus::Conflict)
            REQUIRE(decision.evidence.size() == 2);
        else
            REQUIRE(decision.evidence.empty());

        std::string script = LCSC01_STUB_PLUGIN_PY;
        const std::string fp_needle = "(property \"Footprint\" \"old:fp\")";
        REQUIRE(count_occurrences(script, fp_needle) == 1);
        const std::string properties = fp_needle + " (property \"Value\" \"" + value
                                       + "\") (property \"MPN\" \"" + mpn + "\")";
        script.replace(script.find(fp_needle), fp_needle.size(), properties);
        REQUIRE(count_occurrences(script, "%s") == 2);
        REQUIRE(count_occurrences(script, "% (name, base)") == 1);

        const auto root = temp.path() / "u04b2warnings";
        const auto sym_dir = root / "symbols";
        const auto fp_dir = root / "footprints";
        fs::create_directories(sym_dir);
        fs::create_directories(fp_dir);
        const auto package_id = lcsc01_register_package(
            *db, "U04B2Warnings", sym_dir.string(), fp_dir.string());
        const auto scan = temp.path() / "plugins_u04b2warnings";
        write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
        write(scan / "stub" / "plugin.py", script);
        plugin::PluginManager manager({scan});
        manager.discover();
        api::StagedImporter importer(*db, manager, &matching_store);

        const auto request = lcsc01_request(package_id, "C934", "u04b2-warning-first");
        auto result = importer.run(request);
        if (!result)
            MESSAGE("staged warning import failed: ", result.error().format_message());
        REQUIRE(result.has_value());
        CHECK(result->value("ok", false));
        CHECK_FALSE(result->value("noop", false));
        REQUIRE(result->contains("warnings"));
        REQUIRE((*result)["warnings"].is_array());
        REQUIRE_FALSE((*result)["warnings"].empty());
        bool warning_has_status_and_reason = false;
        for (const auto& warning : (*result)["warnings"])
        {
            REQUIRE(warning.is_string());
            const auto message = warning.get<std::string>();
            if (message.find(classifier::match_status_name(expected_status)) != std::string::npos
                && message.find(decision.reason) != std::string::npos)
                warning_has_status_and_reason = true;
        }
        CHECK(warning_has_status_and_reason);

        const std::string symbol_id = result->value("symbol_id", std::string(""));
        const std::string footprint_id = result->value("footprint_id", std::string(""));
        REQUIRE_FALSE(symbol_id.empty());
        REQUIRE_FALSE(footprint_id.empty());
        const auto symbol_file = sym_dir / "LCSC.kicad_sym";
        const auto footprint_file = fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod";
        REQUIRE(fs::exists(symbol_file));
        REQUIRE(fs::exists(footprint_file));
        const std::string symbol_bytes = read(symbol_file);
        const std::string footprint_bytes = read(footprint_file);
        CHECK(symbol_bytes.find(value) != std::string::npos);
        CHECK(symbol_bytes.find(mpn) != std::string::npos);
        std::string symbol_library_id;
        auto check_symbol = [&]
        {
            auto session = db->session();
            auto library = storage::LibraryRepository(session).find_by_source_key(
                storage::existing_source_key(symbol_file));
            REQUIRE(library.has_value());
            CHECK(library->component_library_id == package_id);
            auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
            REQUIRE(row.has_value());
            CHECK(row->library_id() == library->id);
            if (symbol_library_id.empty())
                symbol_library_id = library->id;
            CHECK(row->library_id() == symbol_library_id);
            CHECK(row->name() == "STUB_C934");
            CHECK(row->default_value() == value);
            CHECK(row->mpn() == mpn);
            CHECK(row->component_type_id == "Unknown");
            const auto reference = row->property("reference");
            REQUIRE(reference.has_value());
            CHECK(*reference == "U");
            CHECK(row->footprint() == "LCSC:STUBFP");
            auto link = storage::RelationshipRepository(session).find_footprint_for_symbol(symbol_id);
            REQUIRE(link.has_value());
            REQUIRE(link->has_value());
            CHECK(**link == footprint_id);
            REQUIRE(storage::FootprintRepository(session).find_by_id(footprint_id).has_value());
        };
        check_symbol();
        REQUIRE(count_rows(*db, "symbol_footprint_links") == 1);
        REQUIRE(count_rows(*db, "import_sources") == 1);
        REQUIRE(count_rows(*db, "import_operations") == 1);
        CHECK(audit_scalar_text(*db,
                               "SELECT target_library_id FROM import_sources "
                               "WHERE external_id = 'C934'") == symbol_library_id);
        CHECK(audit_scalar_text(*db,
                               "SELECT symbol_id FROM import_sources "
                               "WHERE external_id = 'C934'") == symbol_id);
        const std::string source_digest = audit_scalar_text(
            *db, "SELECT content_digest FROM import_sources WHERE external_id = 'C934'");
        REQUIRE_FALSE(source_digest.empty());
        const std::string operation_digest = audit_scalar_text(
            *db, "SELECT request_digest FROM import_operations WHERE id = 'u04b2-warning-first'");
        REQUIRE_FALSE(operation_digest.empty());
        const std::string recorded_json = audit_scalar_text(
            *db, "SELECT result_json FROM import_operations WHERE id = 'u04b2-warning-first'");
        REQUIRE_FALSE(recorded_json.empty());
        CHECK(nlohmann::json::parse(recorded_json) == *result);
        const int libraries_before = count_rows(*db, "libraries");
        const int symbols_before = count_rows(*db, "symbols");
        const int footprints_before = count_rows(*db, "footprints");
        REQUIRE(libraries_before >= 0);
        REQUIRE(symbols_before >= 0);
        REQUIRE(footprints_before >= 0);

        write(scan / "stub" / "plugin.py",
              "raise RuntimeError(\"warning replay must not run the plugin\")\n");
        auto replay = importer.run(request);
        if (!replay)
            MESSAGE("staged warning replay failed: ", replay.error().format_message());
        REQUIRE(replay.has_value());
        CHECK(replay->value("replayed", false));
        nlohmann::json replay_core = *replay;
        replay_core.erase("replayed");
        CHECK(replay_core == *result);
        CHECK(read(symbol_file) == symbol_bytes);
        CHECK(read(footprint_file) == footprint_bytes);
        check_symbol();
        CHECK(count_rows(*db, "libraries") == libraries_before);
        CHECK(count_rows(*db, "symbols") == symbols_before);
        CHECK(count_rows(*db, "footprints") == footprints_before);
        CHECK(count_rows(*db, "symbol_footprint_links") == 1);
        CHECK(count_rows(*db, "import_sources") == 1);
        CHECK(count_rows(*db, "import_operations") == 1);
        CHECK(audit_scalar_text(*db,
                               "SELECT content_digest FROM import_sources "
                               "WHERE external_id = 'C934'") == source_digest);
        CHECK(audit_scalar_text(*db,
                               "SELECT request_digest FROM import_operations "
                               "WHERE id = 'u04b2-warning-first'") == operation_digest);
        CHECK(audit_scalar_text(*db,
                               "SELECT result_json FROM import_operations "
                               "WHERE id = 'u04b2-warning-first'") == recorded_json);
    };

    SUBCASE("conflict")
    {
        exercise(MatchStatus::Conflict,
                 nlohmann::json::array({
                     u04a_rule("u04b2-conflict-first", "name", "^STUB_C934$", "Resistor", 0),
                     u04a_rule("u04b2-conflict-second", "name", "^STUB_C934$", "Capacitor", 0)}),
                 "U04S_VALUE_v1", "U04S_MPN_v1");
    }
    SUBCASE("input-too-long")
    {
        exercise(MatchStatus::InputTooLong,
                 nlohmann::json::array({
                     u04a_rule("u04b2-long-name", "name", "^STUB_C934$", "Capacitor")}),
                 std::string(classifier::kMaxSourceFieldBytes + 1, 'a'), "U04S_MPN_v1");
    }
    SUBCASE("evaluation-limit")
    {
        exercise(MatchStatus::EvaluationLimit,
                 nlohmann::json::array({
                     u04a_rule("u04b2-limit-name", "name", "^STUB_C934$", "Capacitor", 0),
                     u04a_rule("u04b2-limit-model", "model",
                               "(*NO_AUTO_POSSESS)(*NO_START_OPT)^(a+)+$", "Resistor", 100)}),
                 "U04S_VALUE_v1", std::string(30, 'a') + "!");
    }
    SUBCASE("evaluation-error")
    {
        exercise(MatchStatus::EvaluationError,
                 nlohmann::json::array({
                     u04a_rule("u04b2-error-name", "name", "^STUB_C934$", "Capacitor", 0),
                     u04a_rule("u04b2-error-model", "model",
                               "(*NO_START_OPT)(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|b))(?&A)",
                               "Resistor", 100)}),
                 "U04S_VALUE_v1", "x");
    }
}

TEST_CASE_FIXTURE(LibraryFixture,
                  "U04-B2 staged: unchanged source is not reclassified after rules change")
{
    if (!audit_python_available())
    {
        WARN("python not available — staged import test skipped");
        return;
    }

    auto& registry = core::TypeRegistry::instance();
    registry.bind_component_types_path((temp.path() / "component_types.json").string());
    auto added = registry.add_component_type_persistent(
        core::TypeEntry{"", "U04B2Noop", "", "", ""});
    REQUIRE(added.has_value());
    const std::string custom = *added;
    REQUIRE(matching_store.save(u04a_config(nlohmann::json::array()), 0).has_value());

    const auto root = temp.path() / "u04b2noop";
    const auto sym_dir = root / "symbols";
    const auto fp_dir = root / "footprints";
    fs::create_directories(sym_dir);
    fs::create_directories(fp_dir);
    const auto package_id = lcsc01_register_package(
        *db, "U04B2Noop", sym_dir.string(), fp_dir.string());
    const auto scan = temp.path() / "plugins_u04b2noop";
    write(scan / "stub" / "manifest.json", STUB_PLUGIN_MANIFEST);
    write(scan / "stub" / "plugin.py", LCSC01_STUB_PLUGIN_PY);
    plugin::PluginManager manager({scan});
    manager.discover();
    api::StagedImporter importer(*db, manager, &matching_store);

    auto first = importer.run(lcsc01_request(package_id, "C935", "u04b2-noop-first"));
    if (!first)
        MESSAGE("first staged no-op import failed: ", first.error().format_message());
    REQUIRE(first.has_value());
    CHECK(first->value("ok", false));
    CHECK_FALSE(first->value("noop", false));
    REQUIRE(first->contains("warnings"));
    REQUIRE((*first)["warnings"].is_array());
    CHECK((*first)["warnings"].empty());
    const std::string symbol_id = first->value("symbol_id", std::string(""));
    const std::string footprint_id = first->value("footprint_id", std::string(""));
    REQUIRE_FALSE(symbol_id.empty());
    REQUIRE_FALSE(footprint_id.empty());
    const auto symbol_file = sym_dir / "LCSC.kicad_sym";
    const auto footprint_file = fp_dir / "LCSC.pretty" / "STUBFP.kicad_mod";
    REQUIRE(fs::exists(symbol_file));
    REQUIRE(fs::exists(footprint_file));
    const std::string symbol_bytes = read(symbol_file);
    const std::string footprint_bytes = read(footprint_file);
    std::optional<core::Symbol> original;
    std::string symbol_library_id;
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        original = *row;
        REQUIRE(original->component_type_id == "Unknown");
        auto library = storage::LibraryRepository(session).find_by_source_key(
            storage::existing_source_key(symbol_file));
        REQUIRE(library.has_value());
        CHECK(library->component_library_id == package_id);
        symbol_library_id = library->id;
        CHECK(original->library_id() == symbol_library_id);
        auto link = storage::RelationshipRepository(session).find_footprint_for_symbol(symbol_id);
        REQUIRE(link.has_value());
        REQUIRE(link->has_value());
        CHECK(**link == footprint_id);
    }
    REQUIRE(count_rows(*db, "symbol_footprint_links") == 1);
    REQUIRE(count_rows(*db, "import_sources") == 1);
    REQUIRE(count_rows(*db, "import_operations") == 1);
    const std::string source_digest = audit_scalar_text(
        *db, "SELECT content_digest FROM import_sources WHERE external_id = 'C935'");
    REQUIRE_FALSE(source_digest.empty());
    const std::string first_json = audit_scalar_text(
        *db, "SELECT result_json FROM import_operations WHERE id = 'u04b2-noop-first'");
    REQUIRE_FALSE(first_json.empty());
    CHECK(nlohmann::json::parse(first_json) == *first);
    const int libraries_before = count_rows(*db, "libraries");
    const int symbols_before = count_rows(*db, "symbols");
    const int footprints_before = count_rows(*db, "footprints");

    const auto revision = matching_store.current()->revision();
    REQUIRE(revision == 1);
    auto installed = matching_store.save(
        u04a_config(nlohmann::json::array({
                        u04a_rule("u04b2-noop-name", "name", "^STUB_C935$", custom)}),
                    nlohmann::json::array(), revision),
        revision);
    if (!installed)
        MESSAGE("no-op rule install failed: ", installed.error().format_message());
    REQUIRE(installed.has_value());
    CHECK(*installed == 2);
    const auto decision = matching_store.current()->evaluate(MatchSample::from_symbol(*original));
    REQUIRE(decision.status == MatchStatus::Matched);
    REQUIRE(decision.type_id == custom);

    // 来源产物完全相同，新 operation 只能记 no-op；规则改变不重分类。
    auto second = importer.run(lcsc01_request(package_id, "C935", "u04b2-noop-second"));
    if (!second)
        MESSAGE("second staged no-op import failed: ", second.error().format_message());
    REQUIRE(second.has_value());
    CHECK(second->value("ok", false));
    CHECK(second->value("noop", false));
    CHECK_FALSE(second->value("replayed", false));
    CHECK(second->value("symbol_id", std::string("")) == symbol_id);
    CHECK(second->value("footprint_id", std::string("")) == footprint_id);
    REQUIRE(second->contains("warnings"));
    REQUIRE((*second)["warnings"].is_array());
    CHECK((*second)["warnings"].empty());
    CHECK(read(symbol_file) == symbol_bytes);
    CHECK(read(footprint_file) == footprint_bytes);
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        REQUIRE(row.has_value());
        CHECK(row->id() == original->id());
        CHECK(row->component_type_id == "Unknown");
        CHECK(row->library_id() == symbol_library_id);
        CHECK(row->name() == original->name());
        CHECK(row->default_value() == original->default_value());
        CHECK(row->mpn() == original->mpn());
        CHECK(row->property("reference") == original->property("reference"));
        CHECK(row->footprint() == original->footprint());
        auto link = storage::RelationshipRepository(session).find_footprint_for_symbol(symbol_id);
        REQUIRE(link.has_value());
        REQUIRE(link->has_value());
        CHECK(**link == footprint_id);
    }
    CHECK(count_rows(*db, "libraries") == libraries_before);
    CHECK(count_rows(*db, "symbols") == symbols_before);
    CHECK(count_rows(*db, "footprints") == footprints_before);
    CHECK(count_rows(*db, "symbol_footprint_links") == 1);
    CHECK(count_rows(*db, "import_sources") == 1);
    CHECK(count_rows(*db, "import_operations") == 2);
    CHECK(audit_scalar_text(*db,
                           "SELECT content_digest FROM import_sources "
                           "WHERE external_id = 'C935'") == source_digest);
    CHECK(audit_scalar_text(*db,
                           "SELECT target_library_id FROM import_sources "
                           "WHERE external_id = 'C935'") == symbol_library_id);
    CHECK(audit_scalar_text(*db,
                           "SELECT symbol_id FROM import_sources "
                           "WHERE external_id = 'C935'") == symbol_id);
    CHECK(audit_scalar_text(*db,
                           "SELECT result_json FROM import_operations "
                           "WHERE id = 'u04b2-noop-first'") == first_json);
    const std::string second_json = audit_scalar_text(
        *db, "SELECT result_json FROM import_operations WHERE id = 'u04b2-noop-second'");
    REQUIRE_FALSE(second_json.empty());
    CHECK(nlohmann::json::parse(second_json) == *second);
}

namespace
{
// 逐列保留真实仓储行（包括 SQL NULL），供只读/仅类型写回的持久化断言使用。
nlohmann::json u04b2_api_row(storage::Database& db, const std::string& id)
{
    auto session = db.session();
    sqlite3_stmt* raw = nullptr;
    const int prepared = sqlite3_prepare_v2(
        session.handle(), "SELECT * FROM symbols WHERE id = ?1", -1, &raw, nullptr);
    U04B1Stmt stmt(raw, &sqlite3_finalize);
    REQUIRE(prepared == SQLITE_OK);
    REQUIRE(stmt != nullptr);
    REQUIRE(sqlite3_bind_text(stmt.get(), 1, id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt.get()) == SQLITE_ROW);
    auto out = nlohmann::json::object();
    for (int column = 0; column < sqlite3_column_count(stmt.get()); ++column)
    {
        const char* name = sqlite3_column_name(stmt.get(), column);
        if (sqlite3_column_type(stmt.get(), column) == SQLITE_NULL)
            out[name] = nullptr;
        else
        {
            const auto* text = sqlite3_column_text(stmt.get(), column);
            out[name] = std::string(reinterpret_cast<const char*>(text),
                                    sqlite3_column_bytes(stmt.get(), column));
        }
    }
    return out;
}

void u04b2_api_error(const nlohmann::json& result)
{
    REQUIRE(result.is_object());
    REQUIRE(result.contains("ok"));
    CHECK(result.at("ok") == false);
    REQUIRE(result.contains("error"));
    REQUIRE(result.at("error").is_string());
    CHECK_FALSE(result.at("error").get<std::string>().empty());
}

const nlohmann::json* u04b2_api_detail(const nlohmann::json& report, const std::string& id)
{
    REQUIRE(report.contains("details"));
    REQUIRE(report.at("details").is_array());
    for (const auto& detail : report.at("details"))
        if (detail.value("symbol_id", std::string()) == id)
            return &detail;
    return nullptr;
}
}  // namespace

TEST_CASE_FIXTURE(LibraryFixture, "U04-B2 API GET exposes healthy first-bad and last-valid config state")
{
    api::TypeMatchingContext context(*db, matching_store, matching_config.string());
    const std::string healthy_bytes = read(matching_config);
    const auto healthy = api::type_matching_get(context);
    REQUIRE(healthy.value("ok", false));
    CHECK(healthy.at("has_snapshot") == true);
    CHECK(healthy.at("load_error") == "");
    CHECK(healthy.at("revision") == 0);
    CHECK(healthy.at("rules") == nlohmann::json::parse(healthy_bytes).at("rules"));

    SUBCASE("first bad load has no published config")
    {
        classifier::TypeMatchingStore fresh;
        const auto path = temp.path() / "first_bad_matching.json";
        write(path, "{ first load is broken");
        REQUIRE_FALSE(fresh.load(path.string()).has_value());
        api::TypeMatchingContext first_bad(*db, fresh, path.string());
        const auto result = api::type_matching_get(first_bad);
        CHECK(result.at("ok") == false);
        CHECK(result.at("has_snapshot") == false);
        REQUIRE(result.at("load_error").is_string());
        CHECK_FALSE(result.at("load_error").get<std::string>().empty());
        CHECK_FALSE(result.contains("rules"));
        CHECK_FALSE(result.contains("revision"));
        CHECK(read(path) == "{ first load is broken");
    }
    SUBCASE("failed load exposes the last valid config with an error")
    {
        write(matching_config, "{ saved config is broken");
        REQUIRE_FALSE(matching_store.load(matching_config.string()).has_value());
        const auto result = api::type_matching_get(context);
        CHECK(result.at("ok") == false);
        CHECK(result.at("has_snapshot") == true);
        CHECK(result.at("revision") == healthy.at("revision"));
        CHECK(result.at("rules") == healthy.at("rules"));
        CHECK(result.at("exceptions") == healthy.at("exceptions"));
        CHECK_FALSE(result.at("load_error").get<std::string>().empty());
        CHECK(read(matching_config) == "{ saved config is broken");
    }
    CHECK(count_rows(*db, "symbols") == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B2 API save validates requests and persists config without classifying rows")
{
    U04B1Library lib(*db, matching_store, temp.path(), "ApiSave");
    const std::string id = lib.insert(u04b1_part("API_SAVE", "U", std::string(" R? "),
                                               " 10k ", "MPN-1"));
    const auto row_before = u04b2_api_row(*db, id);
    const auto snapshots_before = u04b1_imported_map(*db);
    const std::string source_before = read(lib.target);
    const std::string bytes_before = read(matching_config);
    const auto config = nlohmann::json::parse(u04a_config(nlohmann::json::array({
        u04a_rule("api-save-name", "name", "^API_SAVE$", "Resistor")})));
    api::TypeMatchingContext context(*db, matching_store, matching_config.string());
    const auto valid = nlohmann::json{{"config", config}, {"expected_revision", 0}};

    auto rejects_without_writing = [&](const nlohmann::json& request)
    {
        const auto result = api::type_matching_save(context, request);
        u04b2_api_error(result);
        CHECK(read(matching_config) == bytes_before);
        CHECK(matching_store.current()->revision() == 0);
        CHECK(u04b2_api_row(*db, id) == row_before);
    };
    for (const auto& root : nlohmann::json::array({nullptr, true, 4, "text", nlohmann::json::array()}))
        rejects_without_writing(root);
    rejects_without_writing(nlohmann::json::object());
    rejects_without_writing({{"config", config}});
    rejects_without_writing({{"expected_revision", 0}});
    auto unknown_key = valid;
    unknown_key["path"] = (temp.path() / "unapproved.json").string();
    rejects_without_writing(unknown_key);
    for (const auto& bad_revision : nlohmann::json::array(
             {-1, 0.5, true, classifier::kMaxSafeRevision + 1, nullptr, "0"}))
    {
        auto request = valid;
        request["expected_revision"] = bad_revision;
        rejects_without_writing(request);
    }
    auto stale = valid;
    stale["expected_revision"] = 1;
    rejects_without_writing(stale);
    for (const auto& bad_config : nlohmann::json::array(
             {nullptr, true, "{}", nlohmann::json::array()}))
    {
        auto request = valid;
        request["config"] = bad_config;
        rejects_without_writing(request);
    }
    for (const auto& invalid_rule : nlohmann::json::array({
             u04a_rule("api-bad-regex", "name", "(", "Resistor"),
             u04a_rule("api-bad-type", "name", "^API_SAVE$", "custom:not-registered")}))
    {
        auto request = valid;
        request["config"]["rules"] = nlohmann::json::array({invalid_rule});
        rejects_without_writing(request);
    }
    auto bad_config_revision = valid;
    bad_config_revision["config"]["revision"] = 1;
    rejects_without_writing(bad_config_revision);

    for (const auto* text : {"{", "[]", "null", "true", "1"})
        CHECK_FALSE(api::type_matching_parse_request_body(text).has_value());
    auto parsed = api::type_matching_parse_request_body(valid.dump());
    REQUIRE(parsed.has_value());
    const auto saved = api::type_matching_save(context, *parsed);
    REQUIRE(saved.value("ok", false));
    CHECK(saved.at("revision") == 1);
    CHECK(read(matching_config) != bytes_before);
    CHECK(nlohmann::json::parse(read(matching_config)).at("revision") == 1);
    const auto decision = matching_store.current()->evaluate(MatchSample::from_symbol(lib.read(id)));
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Resistor");
    CHECK(u04b2_api_row(*db, id) == row_before);
    CHECK(read(lib.target) == source_before);
    u04b1_check_snapshots_equal(snapshots_before, u04b1_imported_map(*db));
    const std::string saved_bytes = read(matching_config);
    u04b2_api_error(api::type_matching_save(context, valid));
    CHECK(read(matching_config) == saved_bytes);
    CHECK(u04b2_api_row(*db, id) == row_before);
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B2 API reload uses only its configured path and recovers a first bad load")
{
    classifier::TypeMatchingStore fresh;
    const auto configured = temp.path() / "reload" / "configured.json";
    const auto other = temp.path() / "reload" / "other.json";
    write(configured, "{ configured load is broken");
    const std::string good = u04a_config(nlohmann::json::array({
        u04a_rule("api-reloaded", "name", "^RESTORED$", "Resistor")}),
        nlohmann::json::array(), 9);
    write(other, good);
    api::TypeMatchingContext context(*db, fresh, configured.string());
    const auto first = api::type_matching_reload(context, nlohmann::json::object());
    u04b2_api_error(first);
    CHECK(first.at("has_snapshot") == false);
    CHECK_FALSE(first.at("load_error").get<std::string>().empty());
    for (const auto& body : nlohmann::json::array({
             nullptr, false, nlohmann::json::array(),
             nlohmann::json{{"path", other.string()}}, nlohmann::json{{"config", nlohmann::json::object()}}}))
    {
        u04b2_api_error(api::type_matching_reload(context, body));
        CHECK_FALSE(fresh.has_snapshot());
        CHECK(read(configured) == "{ configured load is broken");
        CHECK(read(other) == good);
    }
    write(configured, good);
    const auto restored = api::type_matching_reload(context, nlohmann::json::object());
    REQUIRE(restored.value("ok", false));
    CHECK(restored.at("has_snapshot") == true);
    CHECK(restored.at("load_error") == "");
    CHECK(restored.at("revision") == 9);
    CHECK(fresh.path() == configured.string());
    MatchSample sample;
    sample.name = "RESTORED";
    const auto decision = fresh.current()->evaluate(sample);
    REQUIRE(decision.status == MatchStatus::Matched);
    CHECK(decision.type_id == "Resistor");
    CHECK(read(configured) == good);
    CHECK(read(other) == good);
    CHECK(count_rows(*db, "symbols") == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B2 API sample preview preserves optional fields and drafts stay pure")
{
    u04b1_register_types({"custom:api-sample"});
    const auto draft = nlohmann::json::parse(u04a_config(
        nlohmann::json::array({u04a_rule("api-empty-reference", "reference", "^$", "TestPoint")}),
        nlohmann::json::array({u04a_exception("api-exact-value", "value", " 10kµF ",
                                             "custom:api-sample")})));
    write(matching_config, draft.dump());
    REQUIRE(matching_store.load(matching_config.string()).has_value());
    U04B1Library lib(*db, matching_store, temp.path(), "ApiSample");
    const std::string id = lib.insert(u04b1_part("API_DB_ROW", "R", std::string()));
    const auto row_before = u04b2_api_row(*db, id);
    const std::string file_before = read(lib.target);
    const std::string config_before = read(matching_config);
    const auto snapshots_before = u04b1_imported_map(*db);
    api::TypeMatchingContext context(*db, matching_store, matching_config.string());

    const auto empty = api::type_matching_preview(context, {{"sample", nlohmann::json::object()}});
    REQUIRE(empty.value("ok", false));
    CHECK(empty.at("mode") == "sample");
    CHECK(empty.at("source") == "saved");
    CHECK(empty.at("sample") == nlohmann::json::object());
    CHECK(empty.at("status") == "unmatched");
    CHECK_FALSE(empty.contains("summary"));
    CHECK_FALSE(empty.contains("details"));
    const auto explicit_empty = api::type_matching_preview(context, {{"sample", {{"reference", ""}}}});
    REQUIRE(explicit_empty.value("ok", false));
    CHECK(explicit_empty.at("status") == "matched");
    CHECK(explicit_empty.at("type_id") == "TestPoint");
    CHECK(explicit_empty.at("sample").at("reference") == "");
    REQUIRE(explicit_empty.at("structured_evidence").size() == 1);
    CHECK(explicit_empty.at("structured_evidence").front().at("text") == "");
    CHECK(explicit_empty.at("structured_evidence").front().at("field") == "reference");
    const auto exact = api::type_matching_preview(context, {{"sample", {{"value", " 10kµF "}}}});
    REQUIRE(exact.value("ok", false));
    CHECK(exact.at("status") == "matched");
    CHECK(exact.at("type_id") == "custom:api-sample");
    CHECK(exact.at("sample").at("value") == " 10kµF ");
    CHECK(exact.at("structured_evidence") == nlohmann::json::array({
        nlohmann::json{{"source", "exception"}, {"id", "api-exact-value"},
                       {"field", "value"}, {"text", " 10kµF "}, {"priority", 0},
                       {"type_id", "custom:api-sample"}}}));
    const auto trimmed = api::type_matching_preview(context, {{"sample", {{"value", "10kµF"}}}});
    REQUIRE(trimmed.value("ok", false));
    CHECK(trimmed.at("status") == "unmatched");
    for (const auto* field : {"name", "value", "model", "reference"})
        for (const auto& invalid : nlohmann::json::array({nullptr, 7, false}))
            u04b2_api_error(api::type_matching_preview(context,
                {{"sample", nlohmann::json{{field, invalid}}}}));
    for (const auto& invalid : nlohmann::json::array({nullptr, 7, nlohmann::json::array()}))
        u04b2_api_error(api::type_matching_preview(context, {{"sample", invalid}}));
    u04b2_api_error(api::type_matching_preview(context, {{"sample", {{"unexpected", "R"}}}}));
    CHECK(read(matching_config) == config_before);
    CHECK(u04b2_api_row(*db, id) == row_before);

    const std::string bad = "{ saved matching config is broken";
    write(matching_config, bad);
    REQUIRE_FALSE(matching_store.load(matching_config.string()).has_value());
    u04b2_api_error(api::type_matching_preview(context, {{"sample", nlohmann::json::object()}}));
    u04b2_api_error(api::type_matching_preview(context, nlohmann::json::object()));
    u04b2_api_error(api::type_matching_preview(context, {{"config", draft}}));
    const auto diagnostic = api::type_matching_preview(context,
        {{"config", draft}, {"sample", {{"value", " 10kµF "}}}});
    REQUIRE(diagnostic.value("ok", false));
    CHECK(diagnostic.at("mode") == "sample");
    CHECK(diagnostic.at("source") == "draft");
    CHECK(diagnostic.at("saved_healthy") == false);
    CHECK_FALSE(diagnostic.at("load_error").get<std::string>().empty());
    CHECK(diagnostic.at("status") == "matched");
    CHECK(diagnostic.at("type_id") == "custom:api-sample");
    CHECK(diagnostic.at("structured_evidence") == exact.at("structured_evidence"));
    CHECK(read(matching_config) == bad);
    CHECK(u04b2_api_row(*db, id) == row_before);
    CHECK(read(lib.target) == file_before);
    u04b1_check_snapshots_equal(snapshots_before, u04b1_imported_map(*db));
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B2 API database preview reports full counts and caps only its details")
{
    u04b1_register_types({"custom:api-kept", "custom:api-rich"});
    u04b1_store_config(matching_store, matching_config,
        nlohmann::json::array({u04a_rule("api-preview-name", "name", "^API_MATCH", "Resistor", 17)}),
        nlohmann::json::array({u04a_exception("api-preview-exact", "value", " 10kµF ",
                                             "custom:api-rich")}));
    U04B1Library lib(*db, matching_store, temp.path(), "ApiPreview");
    const std::string builtin = lib.insert(u04b1_part(
        "API_000_Builtin", "U", std::string("U"), "v", "m", "Capacitor"));
    const std::string custom = lib.insert(u04b1_part(
        "API_001_Custom", "U", std::string("U"), "v", "m", "custom:api-kept"));
    const std::string legacy = lib.insert(u04b1_part(
        "API_002_Legacy", "U", std::string("U"), "v", "m", "LegacyApiType"));
    const std::string rich = lib.insert(u04b1_part(
        "API_003_Rich", "U", std::string(" R? "), " 10kµF ", "MPN-Ω-1"));
    const std::string unmatched = lib.insert(u04b1_part(
        "API_004_Unmatched", "R", std::nullopt, "other", "other"));
    std::vector<std::string> matched_ids;
    for (int index = 1; index <= 105; ++index)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "API_MATCH_%03d", index);
        matched_ids.push_back(lib.insert(u04b1_part(name, "U", std::string("U"))));
    }
    const auto rich_before = u04b2_api_row(*db, rich);
    const auto first_before = u04b2_api_row(*db, matched_ids.front());
    const std::string config_before = read(matching_config);
    const std::string file_before = read(lib.target);
    const auto snapshots_before = u04b1_imported_map(*db);
    api::TypeMatchingContext context(*db, matching_store, matching_config.string());
    const auto report = api::type_matching_preview(context, nlohmann::json::object());
    REQUIRE(report.value("ok", false));
    CHECK(report.at("mode") == "database");
    CHECK(report.at("source") == "saved");
    const nlohmann::json expected_summary{
        {"total", 110}, {"revision", 0}, {"protected_count", 3}, {"matched", 106},
        {"unmatched", 1}, {"conflicts", 0}, {"input_too_long", 0},
        {"evaluation_limited", 0}, {"evaluation_errors", 0}, {"would_update", 106},
        {"unchanged", 0}, {"type_updated", 0}, {"source_changed", 0}, {"cas_skipped", 0},
        {"deleted", 0}, {"rolled_back", 0}, {"not_attempted", 0}};
    CHECK(report.at("summary") == expected_summary);
    REQUIRE(report.at("details").size() == 100);
    for (const auto& id : {builtin, custom, legacy})
    {
        const auto* detail = u04b2_api_detail(report, id);
        REQUIRE(detail != nullptr);
        CHECK(detail->at("status") == "protected");
        CHECK(detail->at("is_protected") == true);
        CHECK(detail->at("would_update") == false);
        CHECK(detail->at("candidate_type_id") == "");
        CHECK(detail->at("raw_type_id") == lib.read(id).component_type_id);
        CHECK(detail->at("write_status") == "read_only");
    }
    const auto* rich_detail = u04b2_api_detail(report, rich);
    REQUIRE(rich_detail != nullptr);
    CHECK(rich_detail->at("raw_type_id") == "Unknown");
    CHECK(rich_detail->at("sample") == nlohmann::json({
        {"name", "API_003_Rich"}, {"reference", " R? "},
        {"value", " 10kµF "}, {"model", "MPN-Ω-1"}}));
    CHECK(rich_detail->at("type_id") == "custom:api-rich");
    CHECK(rich_detail->at("candidate_type_id") == "custom:api-rich");
    CHECK(rich_detail->at("would_update") == true);
    CHECK(rich_detail->at("structured_evidence") == nlohmann::json::array({
        nlohmann::json{{"source", "exception"}, {"id", "api-preview-exact"},
                       {"field", "value"}, {"text", " 10kµF "}, {"priority", 0},
                       {"type_id", "custom:api-rich"}}}));
    const auto* rule_detail = u04b2_api_detail(report, matched_ids.front());
    REQUIRE(rule_detail != nullptr);
    CHECK(rule_detail->at("structured_evidence") == nlohmann::json::array({
        nlohmann::json{{"source", "rule"}, {"id", "api-preview-name"},
                       {"field", "name"}, {"text", "API_MATCH_001"}, {"priority", 17},
                       {"type_id", "Resistor"}}}));
    const auto* unmatched_detail = u04b2_api_detail(report, unmatched);
    REQUIRE(unmatched_detail != nullptr);
    CHECK(unmatched_detail->at("status") == "unmatched");
    CHECK_FALSE(unmatched_detail->at("sample").contains("reference"));
    for (const auto& detail : report.at("details"))
    {
        CHECK(detail.at("write_status") == "read_only");
        REQUIRE(detail.at("write_reason").is_string());
        CHECK_FALSE(detail.at("write_reason").get<std::string>().empty());
    }

    auto draft = nlohmann::json::parse(config_before);
    draft["revision"] = 23;
    draft["rules"].front()["type_id"] = "Capacitor";
    const auto drafted = api::type_matching_preview(context, {{"config", draft}});
    REQUIRE(drafted.value("ok", false));
    CHECK(drafted.at("source") == "draft");
    CHECK(drafted.at("summary").at("revision") == 23);
    CHECK(drafted.at("summary").at("matched") == 106);
    CHECK(drafted.at("summary").at("would_update") == 106);
    CHECK(drafted.at("summary").at("type_updated") == 0);
    const auto* drafted_rule = u04b2_api_detail(drafted, matched_ids.front());
    REQUIRE(drafted_rule != nullptr);
    CHECK(drafted_rule->at("candidate_type_id") == "Capacitor");
    CHECK(read(matching_config) == config_before);
    CHECK(matching_store.current()->revision() == 0);
    CHECK(u04b2_api_row(*db, rich) == rich_before);
    CHECK(u04b2_api_row(*db, matched_ids.front()) == first_before);
    CHECK(u04b1_count_symbols(*db, "component_type = 'Unknown'") == 107);
    CHECK(read(lib.target) == file_before);
    u04b1_check_snapshots_equal(snapshots_before, u04b1_imported_map(*db));
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B2 API apply uses saved revision and changes only eligible type columns")
{
    u04b1_register_types({"custom:api-kept", "custom:api-target"});
    u04b1_store_config(matching_store, matching_config,
        nlohmann::json::array({u04a_rule("api-apply-name", "name", "^API_APPLY", "custom:api-target")}));
    U04B1Library lib(*db, matching_store, temp.path(), "ApiApply");
    const std::vector<std::string> original_types{
        "Resistor", "custom:api-kept", "LegacyApiKept", "Unknown", ""};
    std::vector<std::string> ids;
    std::vector<nlohmann::json> rows_before;
    for (std::size_t index = 0; index < original_types.size(); ++index)
    {
        auto part = u04b1_part("API_APPLY_" + std::to_string(index), "Q", std::string(" U? "),
                              " raw value ", "MPN-Ω", original_types[index]);
        part.set_footprint("Pkg:Preserved");
        part.set_property("value_range", "preserved range");
        part.set_property("source_marker", "verbatim");
        ids.push_back(lib.insert(part));
        rows_before.push_back(u04b2_api_row(*db, ids.back()));
    }
    const auto snapshots_before = u04b1_imported_map(*db);
    const std::string file_before = read(lib.target);
    const std::string config_before = read(matching_config);
    api::TypeMatchingContext context(*db, matching_store, matching_config.string());
    const auto revision = matching_store.current()->revision();

    for (const auto& request : nlohmann::json::array({
             nlohmann::json::object(), nlohmann::json{{"expected_revision", revision + 1}},
             nlohmann::json{{"expected_revision", revision},
                            {"config", nlohmann::json::parse(config_before)}},
             nlohmann::json{{"expected_revision", revision}, {"sample", nlohmann::json::object()}}}))
    {
        u04b2_api_error(api::type_matching_apply(context, request));
        for (std::size_t index = 0; index < ids.size(); ++index)
            CHECK(u04b2_api_row(*db, ids[index]) == rows_before[index]);
    }

    bool legacy_api = false;
    SUBCASE("saved apply") {}
    SUBCASE("legacy classify") { legacy_api = true; }
    const auto report = legacy_api ? api::type_matching_classify(context)
                                   : api::type_matching_apply(context, {{"expected_revision", revision}});
    REQUIRE(report.value("ok", false));
    CHECK(report.at("source") == "saved");
    CHECK(report.at("summary").at("revision") == revision);
    CHECK(report.at("summary").at("total") == 5);
    CHECK(report.at("summary").at("protected_count") == 3);
    CHECK(report.at("summary").at("matched") == 2);
    CHECK(report.at("summary").at("would_update") == 2);
    CHECK(report.at("summary").at("type_updated") == 2);
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        auto after = u04b2_api_row(*db, ids[index]);
        auto before = rows_before[index];
        CHECK(after.at("component_type") == (index < 3 ? original_types[index] : "custom:api-target"));
        after.erase("component_type");
        before.erase("component_type");
        CHECK(after == before);
        const auto* detail = u04b2_api_detail(report, ids[index]);
        REQUIRE(detail != nullptr);
        CHECK(detail->at("raw_type_id") == original_types[index]);
        CHECK(detail->at("sample").at("reference") == " U? ");
        CHECK(detail->at("sample").at("value") == " raw value ");
        CHECK(detail->at("sample").at("model") == "MPN-Ω");
        CHECK(detail->at("write_status") == (index < 3 ? "not_eligible" : "updated"));
        CHECK(detail->at("is_protected") == (index < 3));
        CHECK(detail->at("would_update") == (index >= 3));
    }
    if (legacy_api)
    {
        CHECK(report.at("total") == 5);
        CHECK(report.at("matched") == 2);
        CHECK(report.at("type_updated") == 2);
        CHECK(report.at("results") == report.at("details"));
        for (const auto& result : report.at("results"))
            for (const auto* key : {"name", "library", "type_id", "type", "confidence"})
                CHECK(result.contains(key));
    }
    CHECK(read(matching_config) == config_before);
    CHECK(read(lib.target) == file_before);
    u04b1_check_snapshots_equal(snapshots_before, u04b1_imported_map(*db));
}

TEST_CASE_FIXTURE(LibraryFixture, "U04-B2 API apply and legacy classify retain a real partial batch report")
{
    u04b1_store_config(matching_store, matching_config,
        nlohmann::json::array({u04a_rule("api-partial-name", "name", "^API_PARTIAL_", "Resistor")}));
    U04B1Library lib(*db, matching_store, temp.path(), "ApiPartial");
    std::vector<std::string> ids;
    for (int index = 1; index <= 1001; ++index)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "API_PARTIAL_%04d", index);
        ids.push_back(lib.insert(u04b1_part(name, "U", std::string(" U? "), " raw ", "MPN-1")));
    }
    REQUIRE(ids.size() == 1001);
    const auto first_before = u04b2_api_row(*db, ids.front());
    const auto failed_before = u04b2_api_row(*db, ids[500]);
    const auto last_before = u04b2_api_row(*db, ids.back());
    const std::string config_before = read(matching_config);
    const std::string file_before = read(lib.target);
    const auto snapshots_before = u04b1_imported_map(*db);
    u04b1_raw_exec(*db,
        "CREATE TRIGGER api_partial_block BEFORE UPDATE ON symbols WHEN NEW.id = "
        + u04b1_sql_literal(ids[500])
        + " BEGIN SELECT RAISE(ABORT, 'api partial second batch blocked'); END");
    api::TypeMatchingContext context(*db, matching_store, matching_config.string());
    bool legacy_api = false;
    SUBCASE("saved apply") {}
    SUBCASE("legacy classify") { legacy_api = true; }
    const auto report = legacy_api ? api::type_matching_classify(context)
        : api::type_matching_apply(context, {{"expected_revision", matching_store.current()->revision()}});
    u04b2_api_error(report);
    CHECK(report.at("error").get<std::string>().find("500 committed") != std::string::npos);
    CHECK(report.at("error").get<std::string>().find("api partial second batch blocked") != std::string::npos);
    CHECK(report.at("source") == "saved");
    const nlohmann::json expected_summary{
        {"total", 1001}, {"revision", 0}, {"protected_count", 0}, {"matched", 1001},
        {"unmatched", 0}, {"conflicts", 0}, {"input_too_long", 0},
        {"evaluation_limited", 0}, {"evaluation_errors", 0}, {"would_update", 1001},
        {"unchanged", 0}, {"type_updated", 500}, {"source_changed", 0}, {"cas_skipped", 0},
        {"deleted", 0}, {"rolled_back", 500}, {"not_attempted", 1}};
    CHECK(report.at("summary") == expected_summary);
    REQUIRE(report.at("details").size() == 100);
    for (const auto& detail : report.at("details"))
    {
        CHECK(detail.at("status") == "matched");
        CHECK(detail.at("raw_type_id") == "Unknown");
        CHECK(detail.at("candidate_type_id") == "Resistor");
        CHECK(detail.at("would_update") == true);
        CHECK(detail.at("write_status") == "updated");
        CHECK_FALSE(detail.at("write_reason").get<std::string>().empty());
    }
    if (legacy_api)
    {
        CHECK(report.at("total") == 1001);
        CHECK(report.at("matched") == 1001);
        CHECK(report.at("type_updated") == 500);
        CHECK(report.at("results") == report.at("details"));
        for (const auto& result : report.at("results"))
            for (const auto* key : {"name", "library", "type_id", "type", "confidence"})
                CHECK(result.contains(key));
    }
    CHECK(u04b1_count_symbols(*db, "component_type = 'Resistor'") == 500);
    CHECK(u04b1_count_symbols(*db, "component_type = 'Unknown'") == 501);
    CHECK(fix02_stored_type(*db, ids.front()) == "Resistor");
    CHECK(fix02_stored_type(*db, ids[500]) == "Unknown");
    CHECK(fix02_stored_type(*db, ids.back()) == "Unknown");
    auto first_after = u04b2_api_row(*db, ids.front());
    auto first_source_before = first_before;
    first_after.erase("component_type");
    first_source_before.erase("component_type");
    CHECK(first_after == first_source_before);
    CHECK(u04b2_api_row(*db, ids[500]) == failed_before);
    CHECK(u04b2_api_row(*db, ids.back()) == last_before);
    CHECK(read(matching_config) == config_before);
    CHECK(read(lib.target) == file_before);
    u04b1_check_snapshots_equal(snapshots_before, u04b1_imported_map(*db));
    u04b1_raw_exec(*db, "DROP TRIGGER api_partial_block");
}

TEST_CASE_FIXTURE(LibraryFixture, "U08PIPE UTF8 single symbol file preserves source identity ownership and incremental snapshots")
{
    const auto directory = temp.path() / fs::path(std::u8string(u8"中文符号目录"));
    const auto selected = directory / fs::path(std::u8string(u8"开关元件.kicad_sym"));
    const auto neighbor = directory / fs::path(std::u8string(u8"不应导入.kicad_sym"));
    const auto selected_utf8 = services::path_to_utf8(selected);
    write(selected, source);
    write(neighbor, "(kicad_symbol_lib (version 20231120) "
                    "(symbol \"UnicodeNeighbor\" (property \"Reference\" \"U\")))");
    const bool native_opened = std::ifstream(selected, std::ios::binary).is_open();
    const bool narrow_opened = std::ifstream(selected.string(), std::ios::binary).is_open();
    INFO("UTF8 native path open=", native_opened, "; narrow path open=", narrow_opened,
         "; path string round trip=", fs::path(selected.string()) == selected);
    REQUIRE(native_opened);
    std::string owner;
    {
        auto session = db->session();
        storage::ComponentLibraryRepository::ComponentLibrary package;
        package.name = "UTF8SingleFile";
        package.symbol_path = selected_utf8;
        const auto inserted = storage::ComponentLibraryRepository(session).insert(package);
        REQUIRE(inserted.has_value());
        owner = inserted->id;
    }
    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        return pipe | services::symbols_from{selected_utf8, owner} | services::execute;
    };
    std::vector<std::string> parse_messages;
    const auto first = [&]
    {
        test_support::RestoreDefaultLogger restore_logger;
        auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&](const spdlog::details::log_msg& message)
            {
                if (message.level >= spdlog::level::warn)
                    parse_messages.emplace_back(message.payload.data(), message.payload.size());
            });
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("u08-utf8-import", sink));
        return run();
    }();
    for (const auto& message : parse_messages) MESSAGE(message);
    for (const auto& error : first.errors) MESSAGE(error);
    REQUIRE(first.ok());
    REQUIRE(first.symbols == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'R'") == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols WHERE name = 'UnicodeNeighbor'") == 0);
    CHECK(audit_scalar_text(*db, "SELECT file_path FROM libraries") == selected_utf8);
    std::string library_id;
    std::string symbol_id;
    {
        auto session = db->session();
        const auto libraries = storage::LibraryRepository(session).find_all();
        REQUIRE(libraries.has_value());
        REQUIRE(libraries->size() == 1);
        const auto& library = libraries->front();
        library_id = library.id;
        CHECK(library.component_library_id == owner);
        CHECK(library.name == services::path_to_utf8(selected.stem()));
        CHECK(library.file_path == selected);
        CHECK(fs::equivalent(library.file_path, selected));
        const auto canonical_utf8 = fs::weakly_canonical(selected).generic_u8string();
        std::string expected_key(canonical_utf8.begin(), canonical_utf8.end());
#ifdef _WIN32
        for (char& byte : expected_key)
            if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte - 'A' + 'a');
#endif
        CHECK(library.source_key == expected_key);
        CHECK(library.source_key == storage::existing_source_key(selected));
        const auto stored = storage::SymbolRepository(session).find_by_name("R");
        REQUIRE(stored.has_value());
        symbol_id = stored->id();
        CHECK(stored->library_id() == library_id);
        storage::ImportedFilesRepository snapshots(session);
        const auto snapshot = snapshots.find_mtime(selected_utf8);
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->has_value());
        const auto all = snapshots.all();
        REQUIRE(all.has_value());
        REQUIRE(all->size() == 1);
        CHECK(all->contains(selected_utf8));
        CHECK_FALSE(all->contains(services::path_to_utf8(neighbor)));
    }
    const auto second = run();
    REQUIRE(second.ok());
    CHECK(second.symbols == 0);
    CHECK(second.skipped == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM libraries") == 1);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM imported_files") == 1);
    auto session = db->session();
    const auto stored = storage::SymbolRepository(session).find_by_id(symbol_id);
    REQUIRE(stored.has_value());
    CHECK(stored->library_id() == library_id);
}

TEST_CASE_FIXTURE(LibraryFixture, "U08PIPE library counts use only grouped library ids and leave empty libraries at zero")
{
    const auto sym = symbol();
    std::string empty_id;
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = "Empty";
        meta.file_path = temp.path() / "Empty.kicad_sym";
        const auto inserted = storage::LibraryRepository(session).insert(meta);
        REQUIRE(inserted.has_value());
        empty_id = inserted->id;
    }
    U04B1Authorizer hook(*db,
        [](void*, int action, const char* table, const char* column, const char*, const char*)
        {
            return action == SQLITE_READ && table && column
                       && std::string_view(table) == "symbols"
                       && std::string_view(column) == "properties_json" ? SQLITE_DENY : SQLITE_OK;
        });
    auto session = db->session();
    const auto counts = storage::SymbolRepository(session).count_by_library();
    REQUIRE(counts.has_value());
    REQUIRE(counts->contains(sym.library_id()));
    CHECK(counts->at(sym.library_id()) == 1);
    const auto empty = counts->find(empty_id);
    CHECK((empty == counts->end() ? 0 : empty->second) == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "U08PIPE incorrect confidence is repaired while manual relationships stay unchanged")
{
    const auto sym = symbol();
    const auto fp = footprint_in("Pkg.pretty", "Old", true, true);
    {
        auto session = db->session();
        REQUIRE(storage::RelationshipRepository(session)
                    .link_symbol_to_footprint(sym.id(), fp.id(), "imported", 0.5).has_value());
    }
    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    CHECK((pipe | services::execute).ok());
    {
        auto session = db->session();
        const auto links = storage::RelationshipRepository(session).find_symbol_link_rows(sym.id());
        REQUIRE(links.has_value());
        REQUIRE(links->size() == 1);
        CHECK(links->front().confidence == 1.0);
        REQUIRE(storage::RelationshipRepository(session)
                    .link_symbol_to_footprint(sym.id(), fp.id(), "explicit", 0.6).has_value());
    }
    REQUIRE(db->execute("CREATE TRIGGER u08_manual_no_delete BEFORE DELETE ON symbol_footprint_links "
                        "BEGIN SELECT RAISE(ABORT, 'manual link must stay'); END").has_value());
    REQUIRE(db->execute("CREATE TRIGGER u08_manual_no_insert BEFORE INSERT ON symbol_footprint_links "
                        "BEGIN SELECT RAISE(ABORT, 'manual link must stay'); END").has_value());
    services::ImportPipeline retry(*db, nullptr, false, &matching_store);
    CHECK((retry | services::execute).ok());
    auto session = db->session();
    const auto links = storage::RelationshipRepository(session).find_symbol_link_rows(sym.id());
    REQUIRE(links.has_value());
    REQUIRE(links->size() == 1);
    CHECK(links->front().confidence == 0.6);
    CHECK(links->front().link_type == "explicit");
}

TEST_CASE_FIXTURE(LibraryFixture, "U08PIPE missing or wrong symbol root reports failure without linking")
{
    SUBCASE("missing directory") { file = temp.path() / "DoesNotExist"; }
    SUBCASE("wrong regular file")
    {
        file = temp.path() / "Wrong.txt";
        write(file, "not a symbol library");
    }
    bool linking = false;
    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    pipe | services::symbols_from{file.string(), ""}
         | services::progress{[&](const std::string& phase, int, int) { linking |= phase == "linking"; }};
    const auto result = pipe | services::execute;
    CHECK_FALSE(result.ok());
    CHECK(result.error_count == 1);
    CHECK_FALSE(linking);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM imported_files") == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "U08PIPE iterator initialization errors remain visible after a directory disappears")
{
    const auto directory = temp.path() / "Vanishing";
    const auto part = directory / "Part.kicad_sym";
    write(part, source);
    int scans = 0;
    bool removed = false;
    services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
    pipe | services::symbols_from{directory.string(), ""}
         | services::progress{[&](const std::string& phase, int completed, int)
           {
               if (phase != "scanning" || completed != 0 || ++scans != 2) return;
               // 删除两个已知的合成路径，让预检成功、iterator 初始化真实失败。
               removed = fs::remove(part) && fs::remove(directory);
           }};
    const auto result = pipe | services::execute;
    REQUIRE(removed);
    CHECK_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    CHECK(result.errors.front().find("cannot scan import directory") != std::string::npos);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM symbols") == 0);
    CHECK(audit_scalar_int(*db, "SELECT COUNT(*) FROM imported_files") == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "import progress reports parsed and processed source files before completion")
{
    write(file.parent_path() / "second.kicad_sym",
          R"((kicad_symbol_lib (version 20231120)
               (symbol "Q" (property "Reference" "Q"))
               (symbol "Q2" (property "Reference" "Q"))))");
    const auto fp_root = temp.path() / "footprints";
    write(fp_root / "Pkg.pretty" / "A.kicad_mod", "(footprint \"A\")");
    write(fp_root / "Pkg.pretty" / "B.kicad_mod", "(footprint \"B\")");
    const auto model_root = temp.path() / "models";
    write(model_root / "A.step", "test model");
    struct Event { std::string phase; int completed; int total; };
    std::vector<Event> events;
    bool parsed_before_write = false;
    bool footprints_parsed_before_write = false;
    auto on_progress = [&](const std::string& phase, int completed, int total)
    {
        events.push_back({phase, completed, total});
        if (phase == "symbols_parse" && completed == 1)
            parsed_before_write = count_rows(*db, "symbols") == 0;
        if (phase == "footprints_parse" && completed == 1)
            footprints_parsed_before_write = count_rows(*db, "footprints") == 0;
    };
    auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        return pipe | services::progress{on_progress}
                    | services::symbols_from{file.parent_path().string(), ""}
                    | services::footprints_from{fp_root.string()}
                    | services::models_from{model_root.string()} | services::execute;
    };
    auto saw = [&](std::string_view phase, int completed, int total)
    {
        return std::any_of(events.begin(), events.end(), [&](const auto& event)
        {
            return event.phase == phase && event.completed == completed && event.total == total;
        });
    };
    auto imported = run();
    REQUIRE(imported.ok());
    CHECK(imported.symbols == 2); // 两个库文件，数据库有三个元件条目
    CHECK(count_rows(*db, "symbols") == 3);
    CHECK(parsed_before_write);
    CHECK(footprints_parsed_before_write);
    CHECK(saw("scanning", 0, 0));
    CHECK(saw("symbols_parse", 1, 2));
    CHECK(saw("symbols_parse", 2, 2));
    CHECK(saw("symbols_import", 1, 2));
    CHECK(saw("symbols_import", 2, 2));
    CHECK(saw("footprints_parse", 1, 2));
    CHECK(saw("footprints_parse", 2, 2));
    CHECK(saw("footprints", 1, 2));
    CHECK(saw("footprints", 2, 2));
    CHECK(saw("models", 1, 1));
    CHECK(saw("linking", 0, 0));
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().phase == "done");

    events.clear();
    auto skipped = run();
    REQUIRE(skipped.ok());
    CHECK(skipped.symbols == 0);
    CHECK(skipped.footprints == 0);
    CHECK(skipped.models_3d == 0);
    CHECK(saw("symbols_parse", 2, 2));
    CHECK(saw("symbols_import", 2, 2));
    CHECK(saw("footprints_parse", 2, 2));
    CHECK(saw("footprints", 2, 2));
    CHECK(saw("models", 1, 1));
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().phase == "done");
}

TEST_CASE_FIXTURE(LibraryFixture, "import progress processed files do not become committed counts on rollback")
{
    write(file.parent_path() / "second.kicad_sym", source);
    const auto fp_root = temp.path() / "footprints";
    write(fp_root / "Pkg.pretty" / "A.kicad_mod", "(footprint \"A\")");
    write(fp_root / "Pkg.pretty" / "B.kicad_mod", "(footprint \"B\")");
    std::string terminal;
    int parsed = 0, written = 0, footprints = 0;
    services::ImportPipeline::Result result;
    {
        U04B1Authorizer deny_commit(*db, &u04b1_deny_commit);
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        pipe | services::progress{[&](const std::string& phase, int completed, int)
        {
            terminal = phase;
            if (phase == "symbols_parse") parsed = completed;
            if (phase == "symbols_import") written = completed;
            if (phase == "footprints") footprints = completed;
        }};
        result = pipe | services::symbols_from{file.parent_path().string(), ""}
                      | services::footprints_from{fp_root.string()} | services::execute;
    }
    CHECK(parsed == 2);
    CHECK(written == 2);
    CHECK(footprints == 2);
    CHECK(terminal == "failed");
    CHECK_FALSE(result.ok());
    CHECK(result.symbols == 0);
    CHECK(result.footprints == 0);
    CHECK(count_rows(*db, "symbols") == 0);
    CHECK(count_rows(*db, "footprints") == 0);
    CHECK(u04b1_imported_files(*db) == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "import progress cancellation stops before source writes")
{
    std::atomic<bool> cancel = false;
    std::string terminal;
    services::ImportPipeline pipe(*db, &cancel, false, &matching_store);
    pipe | services::progress{[&](const std::string& phase, int completed, int)
    {
        terminal = phase;
        if (phase == "symbols_parse" && completed == 1) cancel = true;
    }};
    auto result = pipe | services::symbols_from{file.parent_path().string(), ""}
                       | services::execute;
    CHECK(cancel.load());
    CHECK(terminal == "cancelled");
    CHECK(result.symbols == 0);
    CHECK(count_rows(*db, "symbols") == 0);
    CHECK(u04b1_imported_files(*db) == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "import progress synchronous snapshots remain readable while database is busy")
{
    group();
    api::ImportOrchestrator orchestrator(*db, nullptr, &matching_store);
    CHECK(orchestrator.progress_snapshot().phase == "idle");
    for (int iteration = 0; iteration < 2; ++iteration)
    {
        std::optional<services::ImportPipeline::Result> result;
        std::exception_ptr exception;
        std::thread worker;
        bool was_running = false;
        api::ImportOrchestrator::ProgressSnapshot snapshot;
        {
            // 真正持住同一 DbSession，让任务在首次读库时等待；读取进度不能借连接。
            auto held = db->session();
            worker = std::thread([&]
            {
                try { result = orchestrator.run_now(); }
                catch (...) { exception = std::current_exception(); }
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!orchestrator.is_running() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            was_running = orchestrator.is_running();
            snapshot = orchestrator.progress_snapshot();
        }
        worker.join(); // 断言前保证释放数据库锁且线程已收尾
        CHECK(was_running);
        CHECK(snapshot.phase == "scanning");
        CHECK(snapshot.completed == 0);
        CHECK(snapshot.total == 0);
        CHECK_FALSE(exception);
        REQUIRE(result.has_value());
        CHECK(result->ok());
        CHECK_FALSE(orchestrator.is_running());
        CHECK(orchestrator.progress_snapshot().phase == "done");
        CHECK(orchestrator.sym_count() == (iteration == 0 ? 1 : 0));
    }
    CHECK(count_rows(*db, "symbols") == 1);
    CHECK(orchestrator.import_seq() == 2);
}

TEST_CASE_FIXTURE(LibraryFixture, "import progress orchestrator has terminal states after cancellation and exceptions")
{
    const auto owner = group();
    api::ImportOrchestrator orchestrator(*db, nullptr, &matching_store);
    SUBCASE("background cancellation is observable without the database session")
    {
        std::thread stopper;
        bool cancellation_seen = false;
        api::ImportOrchestrator::ProgressSnapshot snapshot;
        {
            auto held = db->session();
            orchestrator.start();
            snapshot = orchestrator.progress_snapshot();
            stopper = std::thread([&] { orchestrator.stop(); });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!orchestrator.cancel_requested() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            cancellation_seen = orchestrator.cancel_requested();
        }
        stopper.join();
        CHECK(cancellation_seen);
        CHECK(snapshot.phase == "scanning");
        CHECK_FALSE(orchestrator.is_running());
        CHECK(orchestrator.progress_snapshot().phase == "cancelled");
        CHECK(count_rows(*db, "symbols") == 0);
    }
    SUBCASE("synchronous model file path is rejected without throwing")
    {
        {
            auto session = db->session();
            auto libraries = storage::ComponentLibraryRepository(session).find_all();
            REQUIRE(libraries.has_value());
            for (auto& library : *libraries)
            {
                if (library.id != owner) continue;
                library.symbol_path.clear();
                // 模型源要求目录：普通文件应在导入前返回错误，不应抛异常。
                library.model_3d_path = file.string();
                REQUIRE(storage::ComponentLibraryRepository(session).update(library).has_value());
            }
        }
        services::ImportPipeline::Result result;
        CHECK_NOTHROW(result = orchestrator.run_now());
        CHECK_FALSE(result.ok());
        CHECK(result.error_count == 1);
        CHECK_FALSE(orchestrator.is_running());
        CHECK(orchestrator.progress_snapshot().phase == "failed");
        CHECK(orchestrator.import_seq() == 0);
        CHECK(orchestrator.sym_count() == 0);
    }
    SUBCASE("background matching configuration failure is terminal")
    {
        write(matching_config, "{}");
        REQUIRE_FALSE(matching_store.load(matching_config.string()).has_value());
        orchestrator.start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (orchestrator.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        const bool finished = !orchestrator.is_running();
        orchestrator.stop();
        CHECK(finished);
        CHECK(orchestrator.progress_snapshot().phase == "failed");
        CHECK(orchestrator.import_seq() == 0);
    }
}

TEST_CASE_FIXTURE(LibraryFixture, "DERIVED import and reimport inherit metadata without sharing identity")
{
    const auto symbols = temp.path() / "derived_symbols";
    const auto target = symbols / "Derived.kicad_sym";
    const auto pretty = temp.path() / "Pkg.pretty";
    write(pretty / "A.kicad_mod", R"((footprint "A" (layer "F.Cu")))");
    write(pretty / "B.kicad_mod", R"((footprint "B" (layer "F.Cu")))");

    // 子符号故意排在基符号之前；重导只改基符号，子符号自身字节不变。
    const auto source_for = [](const std::string& footprint)
    {
        return std::string(R"KICAD((kicad_symbol_lib (version 20231120)
  (symbol "DerivedChild" (extends "DerivedBase")
    (property "Value" "ChildValue") (property "MPN" "ChildMPN"))
  (symbol "DerivedBase"
    (property "Reference" "R") (property "Value" "BaseValue")
    (property "Footprint" ")KICAD") + footprint + R"KICAD(")
    (property "Datasheet" "https://example.invalid/derived.pdf")
    (property "Description" "Inherited resistor")
    (property "CustomField" "Inherited custom value")
    (property "Kicad_Forge_ID" "KF-DERIVED-BASE")
    (property "Pre_Kicad_Forge_ID" "KF-DERIVED-BASE-PREVIOUS")
    (symbol "DerivedBase_1_1"
      (pin passive line (at 0 0 0) (length 2.54)
        (name "A") (number "1"))
      (pin passive line (at 5.08 0 180) (length 2.54)
        (name "B") (number "2"))
    )
  )
)
)KICAD";
    };
    write(target, source_for("Pkg:A"));

    const auto run = [&]
    {
        services::ImportPipeline pipe(*db, nullptr, false, &matching_store);
        pipe | services::symbols_from{symbols.string(), ""} |
            services::footprints_from{pretty.string()};
        return pipe | services::execute;
    };
    const auto load = [&](const std::string& name)
    {
        auto session = db->session();
        auto row = storage::SymbolRepository(session).find_by_name(name);
        if (!row)
            MESSAGE("derived symbol lookup failed: ", row.error().format_message());
        REQUIRE(row.has_value());
        return *row;
    };
    const auto footprint_id = [&](const std::string& name)
    {
        auto session = db->session();
        auto row = storage::FootprintRepository(session).find_by_name(name);
        REQUIRE(row.has_value());
        return row->id();
    };

    const auto first = run();
    REQUIRE(first.ok());
    REQUIRE(first.symbols == 1); // 计数单位是符号库文件，不是库内符号行。
    REQUIRE(first.footprints == 2);
    const auto base = load("DerivedBase");
    const auto before = load("DerivedChild");
    CHECK(before.id() != base.id());
    CHECK(before.reference_prefix() == "R");
    CHECK(before.property("reference") == std::optional<std::string>{"R"});
    CHECK(before.default_value() == "ChildValue");
    CHECK(before.mpn() == "ChildMPN");
    CHECK(before.footprint() == "Pkg:A");
    CHECK(before.datasheet() == "https://example.invalid/derived.pdf");
    CHECK(before.description() == "Inherited resistor");
    CHECK(before.property("CustomField") ==
          std::optional<std::string>{"Inherited custom value"});
    CHECK(before.component_type_id == "Resistor");
    REQUIRE(before.pins().size() == 2);
    CHECK(before.pins()[0].number == "1");
    CHECK(before.pins()[0].name == "A");
    CHECK(before.pins()[0].electrical_type == "passive");
    CHECK(before.pins()[1].number == "2");
    CHECK(base.Kicad_Forge_ID() == "KF-DERIVED-BASE");
    CHECK(before.Kicad_Forge_ID() == core::Symbol::compute_hash(before));
    CHECK(before.Kicad_Forge_ID() != base.Kicad_Forge_ID());
    CHECK(before.Pre_Kicad_Forge_ID().empty());
    auto links = audit_symbol_links(*db, before.id());
    REQUIRE(links.size() == 1);
    CHECK(links.front().footprint_id == footprint_id("A"));
    CHECK(links.front().link_type == "imported");

    const auto old_time = fs::last_write_time(target);
    write(target, source_for("Pkg:B"));
    fs::last_write_time(target, old_time + std::chrono::seconds(2));
    const auto second = run();
    REQUIRE(second.ok());
    REQUIRE(second.symbols == 1);
    const auto after = load("DerivedChild");
    CHECK(after.id() == before.id());
    CHECK(after.footprint() == "Pkg:B");
    CHECK(after.component_type_id == "Resistor");
    CHECK(after.default_value() == "ChildValue");
    CHECK(after.mpn() == "ChildMPN");
    CHECK(after.Kicad_Forge_ID() == core::Symbol::compute_hash(after));
    CHECK(after.Kicad_Forge_ID() != before.Kicad_Forge_ID());
    CHECK(after.Pre_Kicad_Forge_ID() == before.Kicad_Forge_ID());
    CHECK(after.Pre_Kicad_Forge_ID() != "KF-DERIVED-BASE-PREVIOUS");
    links = audit_symbol_links(*db, after.id());
    REQUIRE(links.size() == 1);
    CHECK(links.front().footprint_id == footprint_id("B"));
    CHECK(links.front().link_type == "imported");
}
