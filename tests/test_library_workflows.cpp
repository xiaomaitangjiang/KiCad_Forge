#include <doctest/doctest.h>
#include "support/test_environment.h"
#include "core/db/database.h"
#include "core/repo/repositories.h"
#include "core/io/parser/symbol_lib_parser.h"
#include "interface/api/import_manager.h"
#include "interface/service/library/binding.h"
#include "interface/service/classification/service.h"
#include "interface/service/library/source_policy.h"
#include "core/io/sexpr/text_util.h"
#include <spdlog/sinks/null_sink.h>
#include <fstream>
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
        auto result = storage::ComponentLibraryRepository(db->handle()).insert(lib);
        REQUIRE(result.has_value());
        return result->id;
    }
    core::Symbol symbol(const std::string& owner = "")
    {
        core::LibraryMeta meta;
        meta.name = "test";
        meta.file_path = file;
        meta.component_library_id = owner;
        auto lib = storage::LibraryRepository(db->handle()).insert(meta);
        REQUIRE(lib.has_value());
        core::Symbol sym;
        sym.set_name("R");
        sym.set_library_id(lib->id);
        sym.set_footprint("Pkg:Old");
        auto result = storage::SymbolRepository(db->handle()).insert(sym);
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
        auto result = storage::FootprintRepository(db->handle()).insert(fp);
        REQUIRE(result.has_value());
        return *result;
    }
    auto import(bool write_ids = false, std::string owner = "")
    {
        services::ImportPipeline pipe(db->handle(), nullptr, write_ids);
        return pipe | services::symbols_from{file.parent_path().string(), owner} | services::execute;
    }
};
}

TEST_CASE_FIXTURE(LibraryFixture, "binding updates source DB reference and relationship together")
{
    auto sym = symbol();
    auto fp = footprint("New", "new.step");
    services::SymbolBindingManager manager(db->handle());
    auto assigned = manager.assign_footprint(sym.id(), fp.id());
    INFO((assigned ? "success" : assigned.error().format_message()));
    REQUIRE(assigned.has_value());
    auto parsed = parser::SymbolLibParser::parse(file);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 1);
    CHECK(parsed->items.front().footprint() == "Pkg:New");
    auto updated = storage::SymbolRepository(db->handle()).find_by_id(sym.id());
    REQUIRE(updated.has_value());
    CHECK(updated->footprint() == "Pkg:New");
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
    storage::FootprintRepository repo(db->handle());
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
        services::ImportPipeline pipe(db->handle());
        return pipe | services::footprints_from{directory.string()} | services::execute;
    };
    run();
    auto fp = storage::FootprintRepository(db->handle()).find_by_name("Part");
    REQUIRE(fp.has_value());
    CHECK(fp->library_path() == path.string());
    auto old_time = fs::last_write_time(path);
    write(path, R"((footprint "Part" (model "second.step")))");
    fs::last_write_time(path, old_time + std::chrono::seconds(2));
    run();
    auto links = storage::RelationshipRepository(db->handle()).find_models_for_footprint(fp->id());
    REQUIRE(links.has_value());
    REQUIRE(links->size() == 1);
    auto model = storage::Model3DRepository(db->handle()).find_by_id(links->front());
    REQUIRE(model.has_value());
    CHECK(model->file_path().filename() == "second.step");
}

TEST_CASE_FIXTURE(LibraryFixture, "binding failure preserves old link and source")
{
    auto sym = symbol();
    auto old = footprint("Old");
    auto next = footprint("New", "new.step");
    storage::RelationshipRepository relations(db->handle());
    REQUIRE(relations.link_symbol_to_footprint(sym.id(), old.id()).has_value());
    bool missing = false;
    SUBCASE("unknown footprint") { next.set_id("missing"); }
    SUBCASE("missing source") { fs::remove(file); missing = true; }
    SUBCASE("write failure") { fs::create_directory(fs::path(file.string() + ".tmp")); }
    SUBCASE("DB failure") {
        REQUIRE(db->execute("CREATE TRIGGER reject_link BEFORE INSERT ON symbol_footprint_links BEGIN SELECT RAISE(ABORT,'test failure'); END").has_value());
    }
    SUBCASE("commit failure") {
        sqlite3_commit_hook(db->handle(), [](void*) { return 1; }, nullptr);
    }
    SUBCASE("missing footprint source") { fs::remove(next.library_path()); }
    SUBCASE("malformed footprint source") { write(next.library_path(), "("); }
    services::SymbolBindingManager manager(db->handle());
    CHECK_FALSE(manager.assign_footprint(sym.id(), next.id()).has_value());
    CHECK(manager.get(sym.id()).fp_id == old.id());
    if (!missing) CHECK(read(file) == source);
    auto current = storage::SymbolRepository(db->handle()).find_by_id(sym.id());
    REQUIRE(current.has_value());
    CHECK(current->footprint() == "Pkg:Old");
    CHECK(storage::Model3DRepository(db->handle()).count() == 0);
}

TEST_CASE_FIXTURE(LibraryFixture, "binding service blocks source writes to locked libraries")
{
    auto sym = symbol(group(true));
    auto fp = footprint("New");
    services::SymbolBindingManager manager(db->handle());
    CHECK_FALSE(manager.assign_footprint(sym.id(), fp.id()).has_value());
    CHECK(read(file) == source);
}

TEST_CASE_FIXTURE(LibraryFixture, "incremental import reports skipped files and preserves symbol identity")
{
    import();
    auto before = storage::SymbolRepository(db->handle()).find_all();
    REQUIRE(before.has_value());
    REQUIRE(before->size() == 1);
    auto result = import();
    CHECK(result.symbols == 0);
    CHECK(result.skipped == 1);
    auto after = storage::SymbolRepository(db->handle()).find_all();
    REQUIRE(after.has_value());
    REQUIRE(after->size() == 1);
    CHECK(after->front().id() == before->front().id());
    CHECK(read(file) == source);
}

TEST_CASE_FIXTURE(LibraryFixture, "manual force import reloads unchanged files after cancellation")
{
    group();
    util::ConfigStore config(temp.path() / "config.json");
    api::ImportManager manager(db->handle(), &config);
    manager.import_all({});
    auto syms = storage::SymbolRepository(db->handle()).find_all();
    REQUIRE(syms.has_value());
    REQUIRE(syms->size() == 1);
    auto changed = syms->front();
    changed.set_footprint("DB-only");
    REQUIRE(storage::SymbolRepository(db->handle()).update(changed).has_value());
    manager.stop_async();
    manager.import_all({.force = true});
    auto refreshed = storage::SymbolRepository(db->handle()).find_by_id(changed.id());
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
    auto rows = storage::SymbolRepository(db->handle()).find_all();
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
    api::ImportManager manager(db->handle(), nullptr);
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
    api::ImportManager manager(db->handle(), &config);
    manager.import_all({});
    CHECK(read(file).find("Kicad_Forge_ID") != std::string::npos);
}

TEST_CASE_FIXTURE(LibraryFixture, "cancelled pipeline does not write sources or consume import snapshots")
{
    std::atomic<bool> cancel = true;
    services::ImportPipeline cancelled(db->handle(), &cancel, true);
    auto result = cancelled | services::symbols_from{file.parent_path().string(), ""} | services::execute;
    CHECK(result.symbols == 0);
    CHECK(read(file) == source);
    auto snapshot = storage::ImportedFilesRepository(db->handle()).find_mtime(file.string());
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot->has_value());
    cancel = false;
    services::ImportPipeline resumed(db->handle(), &cancel);
    result = resumed | services::symbols_from{file.parent_path().string(), ""} | services::execute;
    CHECK(result.symbols == 1);
}

TEST_CASE_FIXTURE(LibraryFixture, "source protection checks complete path components")
{
    group(true);
    auto locked = services::source_is_locked(db->handle(), file);
    REQUIRE(locked.has_value());
    CHECK(*locked);
    auto sibling = temp.path() / "symbols-extra" / "other.kicad_sym";
    auto unlocked = services::source_is_locked(db->handle(), sibling);
    REQUIRE(unlocked.has_value());
    CHECK_FALSE(*unlocked);
    CHECK_FALSE(services::source_belongs_to(temp.path() / "other", file.parent_path().string()));
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
