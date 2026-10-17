#include <doctest/doctest.h>
#include "support/test_environment.h"
#include "core/db/database.h"
#include "core/db/source_key_migration.h"
#include "core/io/source_mutation.h"
#include "core/model/type_registry.h"
#include "core/repo/repositories.h"
#include "core/io/parser/symbol_lib_parser.h"
#include "interface/api/import_manager.h"
#include "interface/api/staged_import.h"
#include "plugin/plugin_manager.h"
#include "interface/service/library/binding.h"
#include "interface/service/library/service.h"
#include "interface/service/classification/service.h"
#include "interface/service/library/source_policy.h"
#include "core/io/sexpr/text_util.h"
#include <spdlog/sinks/null_sink.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <set>
#include <thread>
#include <sqlite3.h>

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

struct LibraryFixture
{
    test_support::TempDirectory temp;
    test_support::RestoreDefaultLogger restore;
    std::unique_ptr<storage::Database> db;
    fs::path file = temp.path() / "symbols" / "test.kicad_sym";
    LibraryFixture()
    {
        spdlog::set_default_logger(std::make_shared<spdlog::logger>(
            "data-test", std::make_shared<spdlog::sinks::null_sink_mt>()));
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
        services::ImportPipeline pipe(*db, nullptr, write_ids);
        return pipe | services::symbols_from{file.parent_path().string(), owner} | services::execute;
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
    CHECK(binding.fp_id == fp.id());
    CHECK(binding.model_name == "new.step");
    auto no_model = footprint("Bare");
    REQUIRE(manager.assign_footprint(sym.id(), no_model.id()).has_value());
    CHECK(manager.get(sym.id()).model_id.empty());
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
        services::ImportPipeline pipe(*db);
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
        // 目标只读：替换阶段失败（临时文件按操作 ID 命名，没法预先占位）
        fs::permissions(file, fs::perms::owner_read, fs::perm_options::replace);
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
    CHECK(manager.get(sym.id()).fp_id == old.id());
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
    api::ImportManager manager(*db, &config);
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
    services::ClassificationService service(db.get());
    auto result = service.classify_all();
    REQUIRE(result.has_value());
    CHECK(result->type_updated == 1);
    CHECK(read(file) == source);
}

TEST_CASE_FIXTURE(LibraryFixture, "async import can restart after its previous thread finished")
{
    group();
    api::ImportManager manager(*db, nullptr);
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
    api::ImportManager manager(*db, &config);
    manager.import_all({});
    CHECK(read(file).find("Kicad_Forge_ID") != std::string::npos);
}

TEST_CASE_FIXTURE(LibraryFixture, "cancelled pipeline does not write sources or consume import snapshots")
{
    std::atomic<bool> cancel = true;
    services::ImportPipeline cancelled(*db, &cancel, true);
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
    services::ImportPipeline resumed(*db, &cancel);
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

    services::ImportPipeline pipe(*db);
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

namespace
{
/// 用完把注册表恢复原状 —— 它是进程级单例，别把顺序留给后面的用例
struct RegistrySnapshot
{
    std::vector<core::TypeEntry> components = core::TypeRegistry::instance().component_types();
    std::vector<core::TypeEntry> packages = core::TypeRegistry::instance().package_types();
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
        core::TypeRegistry::instance().load_component_types_json(dump(components));
        core::TypeRegistry::instance().load_package_types_json(dump(packages));
    }
};
}  // namespace

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
        CHECK(repo.count_by_component_type(*added) == 1);
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

    auto plan = [](const std::string& path, const std::string& new_content)
    {
        nlohmann::json file;
        file["path"] = path;
        file["temp"] = path + ".tmp";
        file["backup"] = path + ".bak";
        file["existed"] = true;
        file["remove"] = false;
        file["old_digest"] = "";
        file["new_digest"] = io::content_digest(new_content);
        nlohmann::json root;
        root["operation"] = "test";
        root["files"] = nlohmann::json::array({file});
        return root.dump();
    };
    insert_mutation_log(*db, "m-prepared", "prepared", plan(restored.string(), "half-written"));
    insert_mutation_log(*db, "m-applied", "applied", plan(finalized.string(), "final"));
    insert_mutation_log(*db, "m-conflict", "applied", plan(conflicted.string(), "our-result"));

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
    CHECK(count_rows(*db, "file_mutations") == 0);  // 处理完清空
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

    services::LibraryService libraries(db.get());
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

    api::StagedImporter importer(*db, manager);
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

    api::StagedImporter importer(*db, manager);
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
