// Import pipeline benchmark — measures each phase to identify bottlenecks
#include "../src/core/model/symbol.h"
#include "../src/core/io/parser/symbol_lib_parser.h"
#include "../src/core/io/sexpr/token/tokenizer.h"
#include "../src/core/io/sexpr/dom/dom_builder.h"
#include "../src/classifier/rule_engine.h"
#include "../src/classifier/type_matching.h"
#include "../src/core/db/database.h"
#include "../src/core/repo/repositories.h"
#include "../src/interface/service/import/pipeline.h"

#include "support/test_environment.h"
#include <spdlog/sinks/null_sink.h>

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sqlite3.h>

using namespace kforge;
namespace fs = std::filesystem;

// Simple timer
struct Timer {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start = Clock::now();
    double ms() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
};

namespace {

// These fixtures and the database belong to this invocation. No production
// configuration, installed KiCad libraries, or existing user DB is consulted.
struct SyntheticFixture {
    test_support::TempDirectory temp;
    fs::path symbols = temp.path() / "symbols";
    fs::path footprints = temp.path() / "footprints";
    fs::path matching = temp.path() / "matching.json";

    SyntheticFixture(int files, int pins) {
        fs::create_directories(symbols);
        const auto pretty = footprints / "Synthetic.pretty";
        fs::create_directories(pretty);
        std::ofstream config(matching, std::ios::binary);
        config << R"({"schema_version":1,"revision":0,"rules":[],"exceptions":[]})";
        if (!config.good()) throw std::runtime_error("Cannot write isolated matching config");

        std::ofstream library(symbols / "Synthetic.kicad_sym", std::ios::binary);
        library << "(kicad_symbol_lib (version 20231120) (generator bench_import)\n";
        for (int index = 0; index < files; ++index) {
            const auto name = "Part_" + std::to_string(index);
            library << "(symbol \"" << name << "\"\n"
                    << " (property \"Reference\" \"U\")\n"
                    << " (property \"Value\" \"" << name << "\")\n"
                    << " (property \"Footprint\" \"Synthetic:" << name << "\")\n"
                    << " (property \"Description\" \"Synthetic import benchmark part\")\n"
                    << " (property \"MPN\" \"BENCH-" << index << "\")\n"
                    << " (symbol \"" << name << "_1_1\"\n";
            std::ofstream footprint(pretty / (name + ".kicad_mod"), std::ios::binary);
            footprint << "(footprint \"" << name << "\" (version 20240108)"
                      << " (generator bench_import) (layer \"F.Cu\") (attr smd)\n";
            for (int pin = 1; pin <= pins; ++pin) {
                library << "  (pin passive line (at " << pin << " 0 0) (length 2.54)"
                        << " (name \"P" << pin << "\" (effects (font (size 1 1))))"
                        << " (number \"" << pin << "\" (effects (font (size 1 1)))))\n";
                footprint << " (pad \"" << pin << "\" smd rect (at " << pin
                          << " 0) (size 0.8 1.2) (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n";
            }
            library << "))\n";
            footprint << ")\n";
            if (!footprint.good()) throw std::runtime_error("Cannot write synthetic footprint");
        }
        library << ")\n";
        if (!library.good()) throw std::runtime_error("Cannot write synthetic symbol library");
    }
};

struct SyntheticMetrics {
    using Clock = std::chrono::steady_clock;
    mutable std::mutex mutex;
    std::string phase = "setup";
    Clock::time_point changed = Clock::now();
    std::vector<std::pair<std::string, double>> phases;
    std::uint64_t link_inserts = 0, link_deletes = 0;
    std::uint64_t symbol_properties_prepare_reads = 0, link_symbol_properties_prepare_reads = 0;
    std::uint64_t symbol_properties_selects = 0, link_symbol_properties_selects = 0;

    void transition(const std::string& next) {
        std::lock_guard lock(mutex);
        if (next == phase) return;
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double, std::milli>(now - changed).count();
        auto previous = std::find_if(phases.begin(), phases.end(),
            [&](const auto& entry) { return entry.first == phase; });
        if (previous == phases.end()) phases.emplace_back(phase, elapsed);
        else previous->second += elapsed;
        phase = next;
        changed = now;
    }

    static void update(void* context, int operation, const char*, const char* table, sqlite3_int64) {
        auto& self = *static_cast<SyntheticMetrics*>(context);
        std::lock_guard lock(self.mutex);
        if (self.phase != "linking" || std::strcmp(table, "symbol_footprint_links") != 0) return;
        if (operation == SQLITE_INSERT) ++self.link_inserts;
        if (operation == SQLITE_DELETE) ++self.link_deletes;
    }
    static int authorize(void* context, int action, const char* table, const char* column,
                         const char*, const char*) {
        if (action != SQLITE_READ || !table || !column || std::strcmp(table, "symbols") != 0
            || std::strcmp(column, "properties_json") != 0) return SQLITE_OK;
        auto& self = *static_cast<SyntheticMetrics*>(context);
        std::lock_guard lock(self.mutex);
        ++self.symbol_properties_prepare_reads;
        if (self.phase == "linking") ++self.link_symbol_properties_prepare_reads;
        return SQLITE_OK;
    }
    static int trace(unsigned event, void* context, void* statement, void*) {
        if (event != SQLITE_TRACE_STMT) return 0;
        const char* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if (!sql) return 0;
        const std::string_view text(sql);
        // Repository SQL uses these uppercase SELECT/FROM keywords. This counts
        // executed statements, not returned rows or JSON deserializations.
        if (!text.starts_with("SELECT") || text.find("properties_json") == text.npos
            || text.find("FROM symbols") == text.npos) return 0;
        auto& self = *static_cast<SyntheticMetrics*>(context);
        std::lock_guard lock(self.mutex);
        ++self.symbol_properties_selects;
        if (self.phase == "linking") ++self.link_symbol_properties_selects;
        return 0;
    }
};

class SyntheticHooks {
public:
    SyntheticHooks(storage::Database& database, SyntheticMetrics& metrics) : db_(database) {
        auto session = db_.session();
        sqlite3_update_hook(session.handle(), SyntheticMetrics::update, &metrics);
        if (sqlite3_set_authorizer(session.handle(), SyntheticMetrics::authorize, &metrics) != SQLITE_OK)
            throw std::runtime_error("Cannot install benchmark SQLite authorizer");
        sqlite3_trace_v2(session.handle(), SQLITE_TRACE_STMT, SyntheticMetrics::trace, &metrics);
    }
    ~SyntheticHooks() {
        auto session = db_.session();
        sqlite3_trace_v2(session.handle(), 0, nullptr, nullptr);
        sqlite3_set_authorizer(session.handle(), nullptr, nullptr);
        sqlite3_update_hook(session.handle(), nullptr, nullptr);
    }
private:
    storage::Database& db_;
};

static std::int64_t synthetic_scalar(storage::Database& db, const char* sql) {
    auto session = db.session();
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(session.handle(), sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare benchmark count");
    const int rc = sqlite3_step(statement);
    const auto count = rc == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    if (count < 0) throw std::runtime_error("Cannot read benchmark count");
    return count;
}

static int synthetic_benchmark(int files, int pins) {
    printf("=== Synthetic Import Benchmark: files=%d, symbols=%d, pins_per_symbol=%d ===\n", files, files, pins);
    printf("Fixture generation and schema migration are excluded from timings.\n");
    test_support::RestoreDefaultLogger restore_logger;
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(
        "synthetic-bench", std::make_shared<spdlog::sinks::null_sink_mt>()));
    SyntheticFixture fixture(files, pins);
    classifier::TypeMatchingStore matching_store;
    auto loaded = matching_store.load(fixture.matching.string());
    if (!loaded) throw std::runtime_error(loaded.error().format_message());
    auto opened = storage::Database::open(fixture.temp.path() / "synthetic.db");
    if (!opened) throw std::runtime_error(opened.error().format_message());
    auto& db = **opened;
    // Database migration may seed installed KiCad *paths*. Remove that metadata
    // before importing; only the explicit temporary sources below are consumed.
    if (auto cleared = db.execute("DELETE FROM component_libraries"); !cleared)
        throw std::runtime_error(cleared.error().format_message());
    std::string component_id;
    {
        storage::ComponentLibraryRepository::ComponentLibrary component;
        component.name = "Synthetic benchmark";
        component.symbol_path = fixture.symbols.string();
        component.footprint_path = fixture.footprints.string();
        auto session = db.session();
        auto inserted = storage::ComponentLibraryRepository(session).insert(component);
        if (!inserted) throw std::runtime_error(inserted.error().format_message());
        component_id = inserted->id;
    }

    for (const char* run : {"cold", "warm"}) {
        SyntheticMetrics metrics;
        services::ImportPipeline::Result result;
        double total_ms = 0;
        {
            SyntheticHooks hooks(db, metrics);
            Timer timer;
            services::ImportPipeline pipeline(db, nullptr, false, &matching_store);
            result = pipeline | services::symbols_from{fixture.symbols.string(), component_id}
                | services::footprints_from{fixture.footprints.string()}
                | services::progress{[&](const std::string& phase, int, int) { metrics.transition(phase); }}
                | services::execute;
            total_ms = timer.ms();
            metrics.transition("finished");
        }
        printf("\nrun=%s total_ms=%.3f errors=%llu warnings=%llu imported_symbol_files=%d imported_footprints=%d skipped_files=%d\n",
            run, total_ms, static_cast<unsigned long long>(result.error_count),
            static_cast<unsigned long long>(result.warning_count), result.symbols, result.footprints, result.skipped);
        for (const auto& [phase, elapsed] : metrics.phases)
            printf("phase=%s elapsed_ms=%.3f\n", phase.c_str(), elapsed);
        printf("link_inserts=%llu link_deletes=%llu symbol_properties_prepare_reads=%llu link_symbol_properties_prepare_reads=%llu symbol_properties_selects=%llu link_symbol_properties_selects=%llu\n",
            static_cast<unsigned long long>(metrics.link_inserts), static_cast<unsigned long long>(metrics.link_deletes),
            static_cast<unsigned long long>(metrics.symbol_properties_prepare_reads), static_cast<unsigned long long>(metrics.link_symbol_properties_prepare_reads),
            static_cast<unsigned long long>(metrics.symbol_properties_selects), static_cast<unsigned long long>(metrics.link_symbol_properties_selects));
        const auto libraries = synthetic_scalar(db, "SELECT COUNT(*) FROM libraries");
        const auto symbols = synthetic_scalar(db, "SELECT COUNT(*) FROM symbols");
        const auto footprints = synthetic_scalar(db, "SELECT COUNT(*) FROM footprints");
        const auto links = synthetic_scalar(db, "SELECT COUNT(*) FROM symbol_footprint_links");
        const auto pin_count = synthetic_scalar(db, "SELECT COALESCE(SUM(pin_count),0) FROM symbols");
        const auto invalid_links = synthetic_scalar(db,
            "SELECT COUNT(*) FROM symbol_footprint_links l JOIN symbols s ON s.id=l.symbol_id "
            "JOIN footprints f ON f.id=l.footprint_id WHERE s.footprint_ref <> 'Synthetic:' || f.name");
        printf("libraries=%lld symbols=%lld footprints=%lld links=%lld pin_count=%lld invalid_links=%lld\n",
            static_cast<long long>(libraries), static_cast<long long>(symbols), static_cast<long long>(footprints),
            static_cast<long long>(links), static_cast<long long>(pin_count), static_cast<long long>(invalid_links));
        if (!result.ok() || libraries != 1 || symbols != files || footprints != files || links != files
            || pin_count != static_cast<std::int64_t>(files) * pins || invalid_links != 0) {
            for (const auto& error : result.errors) printf("ERROR: %s\n", error.c_str());
            throw std::runtime_error("Synthetic import failed correctness checks");
        }
    }
    printf("\nSQLite authorizer counts prepare-time column reads; trace counts executed SELECT statements, neither is a returned-row count.\n");
    return 0;
}

static int positive_argument(const char* option, const char* text, int maximum) {
    int value = 0;
    const std::string_view input(text);
    auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || value < 1 || value > maximum)
        throw std::runtime_error(std::string(option) + " must be between 1 and " + std::to_string(maximum));
    return value;
}

} // namespace

// Find a test file — use a medium-sized KiCad symbol library
static fs::path find_test_file() {
    // Try a large file first, fall back to Diode
    fs::path large = "D:/Program Files/KiCad/10.0/share/kicad/symbols/Converter_DCDC.kicad_sym";
    if (fs::exists(large)) return large;
    fs::path test = "D:/Program Files/KiCad/10.0/share/kicad/symbols/Diode.kicad_sym";
    if (fs::exists(test)) return test;
    return {};
}

int main(int argc, char** argv) {
    // Selecting synthetic mode happens before the legacy installed-library probe.
    // No argument prints usage so an accidental benchmark run cannot read user libraries.
    if (argc <= 1) {
        printf("Usage: bench_import --synthetic [--files 512] [--pins 16]\n"
               "       bench_import --local-kicad (explicitly reads installed KiCad libraries)\n");
        return 0;
    }
    if (std::string_view(argv[1]) == "--synthetic") {
        try {
            int files = 512, pins = 16;
            for (int index = 2; index < argc; ++index) {
                const std::string_view option(argv[index]);
                if (++index >= argc) throw std::runtime_error("Missing benchmark option value");
                if (option == "--files") files = positive_argument("--files", argv[index], 32768);
                else if (option == "--pins") pins = positive_argument("--pins", argv[index], 128);
                else throw std::runtime_error("Unknown synthetic benchmark option");
            }
            return synthetic_benchmark(files, pins);
        } catch (const std::exception& error) {
            fprintf(stderr, "Synthetic benchmark failed: %s\n", error.what());
            return 1;
        }
    }
    if (argc != 2 || std::string_view(argv[1]) != "--local-kicad") {
        fprintf(stderr, "Expected --synthetic or --local-kicad\n");
        return 2;
    }
    printf("=== Import Pipeline Benchmark ===\n\n");

    auto test_file = find_test_file();
    if (test_file.empty()) {
        printf("SKIP: no .kicad_sym file found (install KiCad or place one nearby)\n");
        return 0;
    }
    printf("File: %s\n", test_file.string().c_str());

    // ---- Phase 1: File I/O (read into string) ----
    printf("\n--- Phase 1: File I/O ---\n");
    {
        Timer t;
        std::ifstream f(test_file, std::ios::binary);
        std::stringstream buf;
        buf << f.rdbuf();
        std::string content = buf.str();
        f.close();
        printf("  read %zu bytes: %.1f ms\n", content.size(), t.ms());
    }

    // ---- Phase 2: Tokenization only ----
    printf("\n--- Phase 2: Tokenizer ---\n");
    {
        std::ifstream f(test_file, std::ios::binary);
        std::stringstream buf; buf << f.rdbuf();
        std::string content = buf.str();
        f.close();

        Timer t;
        sexpr::Tokenizer tz(content);
        int count = 0;
        while (true) {
            auto tok = tz.next();
            if (!tok || tok->type == sexpr::TokenType::Eof) break;
            count++;
        }
        printf("  %d tokens: %.1f ms\n", count, t.ms());
    }

    // ---- Phase 3: Full parse (Tokenizer + DomBuilder) ----
    printf("\n--- Phase 3: DomBuilder (full parse) ---\n");
    {
        std::ifstream f(test_file, std::ios::binary);
        std::stringstream buf; buf << f.rdbuf();
        std::string content = buf.str();
        f.close();

        Timer t;
        sexpr::DomBuilder builder;
        auto root = builder.build(content);
        if (!root) {
            printf("  PARSE ERROR: %s\n", root.error().message().c_str());
        } else {
            // Count nodes
            std::function<int(const sexpr::DomNode&)> count_nodes =
                [&](const sexpr::DomNode& n) {
                    int c = 1;
                    for (auto& ch : n.children()) c += count_nodes(*ch);
                    return c;
                };
            int nodes = 0;
            for (auto& ch : (*root)->children()) nodes += count_nodes(*ch);
            printf("  %d DOM nodes: %.1f ms\n", nodes, t.ms());
        }
    }

    // ---- Phase 4: Symbol-level parse (DomBuilder + parse_symbol) ----
    printf("\n--- Phase 4: SymbolLibParser::parse ---\n");
    int sym_count = 0;
    {
        Timer t;
        auto result = parser::SymbolLibParser::parse(test_file);
        if (!result) {
            printf("  PARSE ERROR: %s\n", result.error().message().c_str());
        } else {
            sym_count = (int)result->items.size();
            printf("  %d symbols: %.1f ms\n", sym_count, t.ms());
        }
    }

    // ---- Phase 5: Hash computation (XXH64) ----
    if (sym_count > 0) {
        printf("\n--- Phase 5: XXH64 hash ---\n");
        auto result = parser::SymbolLibParser::parse(test_file);
        if (result) {
            Timer t;
            for (auto& s : result->items) {
                core::Symbol::compute_hash(s);
            }
            printf("  %d hashes: %.1f ms (%.3f ms/sym)\n", sym_count, t.ms(),
                   t.ms() / sym_count);
        }
    }

    // ---- Phase 6: Classification (guess_type_from_name) ----
    if (sym_count > 0) {
        printf("\n--- Phase 6: Classification ---\n");
        auto result = parser::SymbolLibParser::parse(test_file);
        if (result) {
            Timer t;
            for (auto& s : result->items) {
                classifier::guess_type_from_name(s.name());
            }
            printf("  %d classifications: %.1f ms\n", sym_count, t.ms());
        }
    }

    // ---- Phase 7: DB insert (temp file SQLite) ----
    if (sym_count > 0) {
        printf("\n--- Phase 7: DB insert ---\n");
        auto tmp_path = fs::temp_directory_path() / "kforge_bench.db";
        std::error_code ec;
        fs::remove(tmp_path, ec);
        auto db_result = storage::Database::open(tmp_path);
        if (!db_result) {
            printf("  Cannot open DB: %s\n", db_result.error().message().c_str());
        } else {
            auto& db = *db_result;
            auto session = db->session();
            storage::SymbolRepository repo(session);

            // Pre-create the default library
            sqlite3_exec(session.handle(),
                "INSERT OR IGNORE INTO libraries(id,name,file_path) "
                "VALUES('default','Default','test.kicad_sym')",
                nullptr, nullptr, nullptr);

            auto result = parser::SymbolLibParser::parse(test_file);
            if (result) {
                // Warm-up: 1 insert
                if (!result->items.empty()) {
                    auto s = result->items[0];
                    s.set_library_id("default");
                    (void)repo.insert(s);  // warm-up, ignore result
                }

                Timer t;
                int inserted = 0;
                for (auto& s : result->items) {
                    auto sym = s;
                    sym.set_library_id("default");
                    auto ins = repo.insert(sym);
                    if (ins) inserted++;
                }
                printf("  %d inserts: %.1f ms (%.3f ms/insert)\n",
                       inserted, t.ms(), inserted > 0 ? t.ms() / inserted : 0);
            }
            fs::remove(tmp_path, ec);
        }
    }

    // ---- Phase 8: LibraryService full import pipeline ----
    // 隔离：本次运行专用的 TempDirectory + 独立 DB 文件 + 显式空规则局部 matcher，
    // 不再共享固定的 kforge_bench2.db 名称。基准仍然读取本机 KiCad 源库。
    if (sym_count > 0) {
        printf("\n--- Phase 8: LibraryService::import_symbol_library ---\n");
        test_support::TempDirectory temp;
        const auto config_path = temp.path() / "matching.json";
        const auto db_path = temp.path() / "bench_import.db";
        {
            std::ofstream config(config_path);
            config << R"({"schema_version":1,"revision":0,"rules":[],"exceptions":[]})";
        }
        classifier::TypeMatchingStore matching_store;
        const bool config_ok = matching_store.load(config_path.string()).has_value();
        auto db_result = storage::Database::open(db_path);
        if (config_ok && db_result) {
            Timer t;
            services::ImportPipeline pipe(**db_result, nullptr, false, &matching_store);
            auto result = pipe | services::symbols_from{test_file.string(), "test"} | services::execute;
            printf("  imported %d symbols: %.1f ms\n", result.symbols, t.ms());
        } else {
            printf("  SKIP: isolated bench config/db could not be prepared\n");
        }
    }

    // ---- Phase 9: Bulk file scan (largest files) ----
    {
        printf("\n--- Phase 9: Scan directory ---\n");
        fs::path dir = test_file.parent_path();
        Timer t;
        int file_count = 0;
        size_t total_bytes = 0;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".kicad_sym") {
                file_count++;
                total_bytes += entry.file_size();
            }
        }
        printf("  %d .kicad_sym files, %zu MB total: %.1f ms\n",
               file_count, total_bytes / (1024 * 1024), t.ms());
    }

    printf("\n=== Done ===\n");
    return 0;
}
