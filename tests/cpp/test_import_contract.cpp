#include <doctest/doctest.h>

#include "classifier/type_matching.h"
#include "core/db/database.h"
#include "core/repo/repositories.h"
#include "interface/api/import_orchestrator.h"
#include "interface/service/import/source_path.h"
#include "../support/test_environment.h"

#include <chrono>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <thread>

using namespace kforge;
namespace fs = std::filesystem;

TEST_CASE("U08PATH source roles distinguish a symbol file from a footprint directory")
{
    test_support::TempDirectory temp;
    const auto file = temp.path() / "Device.KICAD_SYM";
    std::ofstream(file) << "(kicad_symbol_lib (version 20231120))";
    auto symbol = services::validate_library_source_path(file.string(), services::LibrarySourceKind::Symbols);
    REQUIRE(symbol.has_value());
    CHECK(symbol->single_file);
    CHECK(symbol->path == file);
    auto footprint = services::validate_library_source_path(file.string(), services::LibrarySourceKind::Footprints);
    REQUIRE_FALSE(footprint.has_value());
    CHECK(footprint.error().message().find("Symbol path") != std::string::npos);
    CHECK_NOTHROW(nlohmann::json({{"error", footprint.error().message()}}).dump());
    auto directory = services::validate_library_source_path(temp.path().string(), services::LibrarySourceKind::Footprints);
    REQUIRE(directory.has_value());
    CHECK_FALSE(directory->single_file);
}

TEST_CASE("U08PATH configuration permits future roots but imports require existing sources")
{
    test_support::TempDirectory temp;
    for (auto kind : {services::LibrarySourceKind::Symbols, services::LibrarySourceKind::Footprints,
                      services::LibrarySourceKind::Models})
    {
        const auto missing = temp.path() / "not-created";
        CHECK(services::validate_library_source_path(missing.string(), kind, true).has_value());
        CHECK_FALSE(services::validate_library_source_path(missing.string(), kind).has_value());
        CHECK_FALSE(fs::exists(missing));
        CHECK_FALSE(services::validate_library_source_path("", kind, true).has_value());
    }
    const auto future_symbol = temp.path() / "Future.kicad_sym";
    CHECK(services::validate_library_source_path(future_symbol.string(), services::LibrarySourceKind::Symbols, true).has_value());
    CHECK_FALSE(services::validate_library_source_path(future_symbol.string(), services::LibrarySourceKind::Footprints, true).has_value());
    const auto wrong = temp.path() / "text.txt";
    std::ofstream(wrong) << "not a library";
    CHECK_FALSE(services::validate_library_source_path(wrong.string(), services::LibrarySourceKind::Symbols).has_value());
    CHECK_FALSE(services::validate_library_source_path(wrong.string(), services::LibrarySourceKind::Footprints).has_value());
    CHECK_FALSE(services::validate_library_source_path(wrong.string(), services::LibrarySourceKind::Models).has_value());
    for (const auto* extension : {".STEP", ".stp", ".WRL", ".iges", ".IGS", ".kicad_sym", ".kicad_mod"})
    {
        const auto future_model = temp.path() / (std::string("Future") + extension);
        CHECK_FALSE(services::validate_library_source_path(future_model.string(),
                    services::LibrarySourceKind::Models, true).has_value());
    }
    const auto model_directory = temp.path() / "Directory.STEP";
    fs::create_directory(model_directory);
    CHECK(services::validate_library_source_path(model_directory.string(),
                services::LibrarySourceKind::Models).has_value());
}

TEST_CASE("U08ORCH concurrent manual import is rejected without waiting on the database")
{
    using namespace std::chrono_literals;
    test_support::TempDirectory temp;
    auto opened = storage::Database::open(temp.path() / "isolated.db");
    REQUIRE(opened.has_value());
    auto db = std::move(*opened);
    REQUIRE(db->execute("DELETE FROM component_libraries").has_value());
    classifier::TypeMatchingStore matching;
    api::ImportOrchestrator orchestrator(*db, nullptr, &matching);
    std::future<services::ImportPipeline::Result> first;
    std::future<services::ImportPipeline::Result> duplicate;
    bool ready = false;
    services::ImportPipeline::Result duplicate_result;
    {
        auto held = db->session();
        first = std::async(std::launch::async, [&] { return orchestrator.run_now(); });
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!orchestrator.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        CHECK(orchestrator.is_running());
        duplicate = std::async(std::launch::async, [&] { return orchestrator.run_now(); });
        ready = duplicate.wait_for(200ms) == std::future_status::ready;
        CHECK(ready);
        if (ready) duplicate_result = duplicate.get();
        // Release the held session before joining either future, even on failure.
    }
    CHECK(first.get().ok());
    if (!ready) duplicate_result = duplicate.get();
    CHECK_FALSE(duplicate_result.ok());
    CHECK(duplicate_result.error_count == 1);
    CHECK(orchestrator.import_seq() == 1);
    CHECK_FALSE(orchestrator.is_running());
}

TEST_CASE("U08ORCH identical symbol and footprint file fields cannot bypass validation")
{
    test_support::TempDirectory temp;
    const auto file = temp.path() / "Device.kicad_sym";
    std::ofstream(file) << "(kicad_symbol_lib (version 20231120) (symbol \"R\"))";
    auto opened = storage::Database::open(temp.path() / "isolated.db");
    REQUIRE(opened.has_value());
    auto db = std::move(*opened);
    REQUIRE(db->execute("DELETE FROM component_libraries").has_value());
    {
        auto session = db->session();
        storage::ComponentLibraryRepository::ComponentLibrary lib;
        lib.name = "Incorrect fields";
        lib.symbol_path = file.string();
        lib.footprint_path = file.string();
        REQUIRE(storage::ComponentLibraryRepository(session).insert(lib).has_value());
    }
    classifier::TypeMatchingStore matching;
    api::ImportOrchestrator orchestrator(*db, nullptr, &matching);
    auto result = orchestrator.run_now();
    CHECK_FALSE(result.ok());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].find("Footprint path") != std::string::npos);
    CHECK(result.errors[0].find(".kicad_sym") != std::string::npos);
    CHECK(orchestrator.import_seq() == 0);
    CHECK(orchestrator.progress_snapshot().phase == "failed");
    auto session = db->session();
    auto symbols = storage::SymbolRepository(session).find_all();
    REQUIRE(symbols.has_value());
    CHECK(symbols->empty());
}

namespace
{
struct ImportBoundaryFixture
{
    test_support::TempDirectory temp;
    classifier::TypeMatchingStore matching;
    std::unique_ptr<storage::Database> db;
    ImportBoundaryFixture()
    {
        const auto config = temp.path() / "matching.json";
        std::ofstream(config) << R"({"schema_version":1,"revision":0,"rules":[],"exceptions":[]})";
        REQUIRE(matching.load(config.string()).has_value());
        auto opened = storage::Database::open(temp.path() / "test.db");
        REQUIRE(opened.has_value());
        db = std::move(*opened);
        REQUIRE(db->execute("DELETE FROM component_libraries").has_value());
    }
    void write(const fs::path& path, std::string_view contents)
    {
        fs::create_directories(path.parent_path());
        std::ofstream out(path);
        out << contents;
        REQUIRE(out.good());
    }
    services::ImportPipeline pipeline()
    {
        return services::ImportPipeline(*db, nullptr, false, &matching);
    }
};
constexpr std::string_view boundary_symbol =
    R"((kicad_symbol_lib (version 20231120) (symbol "Part" (property "Reference" "R"))))";
}

TEST_CASE_FIXTURE(ImportBoundaryFixture, "U08EDGE nested sources and uppercase suffixes are imported once")
{
    const auto root = temp.path() / "sources";
    write(root / "nested" / "symbols" / "Device.KICAD_SYM", boundary_symbol);
    write(root / "nested" / "Pkg.PRETTY" / "Pad.KICAD_MOD", "(footprint \"Pad\")");
    auto pipe = pipeline();
    auto result = pipe | services::symbols_from{root.string(), ""} | services::execute;
    CHECK(result.ok());
    CHECK(result.symbols == 1);
    CHECK(result.footprints == 1);
    auto session = db->session();
    auto footprints = storage::FootprintRepository(session).find_all();
    REQUIRE(footprints.has_value());
    REQUIRE(footprints->size() == 1);
    CHECK(footprints->front().footprint_lib_name() == "Pkg");
    auto snapshots = storage::ImportedFilesRepository(session).all();
    REQUIRE(snapshots.has_value());
    CHECK(snapshots->size() == 2);
}

TEST_CASE_FIXTURE(ImportBoundaryFixture, "U08EDGE parse failures are failures and same-mtime repair retries")
{
    const auto root = temp.path() / "sources";
    bool symbols = false;
    SUBCASE("symbol parse failure") { symbols = true; }
    SUBCASE("footprint parse failure") { symbols = false; }
    const auto bad = root / (symbols ? "Bad.kicad_sym" : "Bad.kicad_mod");
    write(bad, "(wrong_root)");
    const auto timestamp = fs::last_write_time(bad);
    write(root / (symbols ? "Good.kicad_sym" : "Good.kicad_mod"),
          symbols ? boundary_symbol : "(footprint \"Good\")");
    auto first_pipe = pipeline();
    auto first = first_pipe | services::symbols_from{root.string(), ""} | services::execute;
    CHECK_FALSE(first.ok());
    CHECK(first.error_count == 1);
    CHECK(first.failed_files == 1);
    CHECK(first.skipped == 0);
    CHECK((symbols ? first.symbols : first.footprints) == 1);
    REQUIRE(first.errors.size() == 1);
    CHECK(first.errors.front().find(bad.filename().string()) != std::string::npos);
    write(bad, symbols ? boundary_symbol : "(footprint \"Repaired\")");
    fs::last_write_time(bad, timestamp);
    auto retry_pipe = pipeline();
    auto retry = retry_pipe | services::symbols_from{root.string(), ""} | services::execute;
    CHECK(retry.ok());
    CHECK(retry.failed_files == 0);
    CHECK(retry.skipped == 1);
    CHECK((symbols ? retry.symbols : retry.footprints) == 1);
}

TEST_CASE_FIXTURE(ImportBoundaryFixture, "U08EDGE disappearing source is reported rather than skipped")
{
    const auto root = temp.path() / "sources";
    bool symbols = false;
    SUBCASE("symbol disappears after scan") { symbols = true; }
    SUBCASE("footprint disappears after scan") { symbols = false; }
    const auto file = root / (symbols ? "Gone.kicad_sym" : "Gone.kicad_mod");
    write(file, symbols ? boundary_symbol : "(footprint \"Gone\")");
    auto pipe = pipeline();
    pipe | services::symbols_from{root.string(), ""};
    pipe | services::progress{[&](const std::string& phase, int completed, int)
    {
        if (completed == 0 && phase == (symbols ? "symbols_parse" : "footprints_parse"))
            fs::remove(file);
    }};
    auto result = pipe | services::execute;
    CHECK_FALSE(result.ok());
    CHECK(result.failed_files == 1);
    CHECK(result.error_count == 1);
    CHECK(result.skipped == 0);
}

TEST_CASE_FIXTURE(ImportBoundaryFixture, "U08EDGE model file root is rejected before any symbol is committed")
{
    const auto symbols = temp.path() / "symbols";
    const auto model_file = temp.path() / "Part.STEP";
    write(symbols / "Part.kicad_sym", boundary_symbol);
    write(model_file, "model");
    auto pipe = pipeline();
    services::ImportPipeline::Result result;
    CHECK_NOTHROW(result = pipe | services::symbols_from{symbols.string(), ""}
                               | services::models_from{model_file.string()} | services::execute);
    CHECK_FALSE(result.ok());
    CHECK(result.error_count == 1);
    CHECK(result.symbols == 0);
    auto session = db->session();
    auto rows = storage::SymbolRepository(session).find_all();
    REQUIRE(rows.has_value());
    CHECK(rows->empty());
}

TEST_CASE_FIXTURE(ImportBoundaryFixture, "U08EDGE model scan disappearance preserves earlier committed counts")
{
    const auto symbols = temp.path() / "symbols";
    const auto models = temp.path() / "models";
    write(symbols / "Part.kicad_sym", boundary_symbol);
    write(models / "nested" / "Part.step", "model");
    bool model_scan = false;
    auto pipe = pipeline();
    pipe | services::symbols_from{symbols.string(), ""} | services::models_from{models.string()};
    pipe | services::progress{[&](const std::string& phase, int completed, int)
    {
        if (phase == "symbols_import" && completed == 1) model_scan = true;
        if (model_scan && phase == "scanning" && completed == 1)
            fs::remove_all(models);
    }};
    services::ImportPipeline::Result result;
    CHECK_NOTHROW(result = pipe | services::execute);
    CHECK_FALSE(result.ok());
    CHECK(result.error_count >= 1);
    CHECK(result.symbols == 1);
    CHECK(result.models_3d == 0);
}
