// Import pipeline benchmark — measures each phase to identify bottlenecks
#include "../src/core/symbol.h"
#include "../src/parser/symbol_lib_parser.h"
#include "../src/sexpr/tokenizer.h"
#include "../src/sexpr/dom_builder.h"
#include "../src/classifier/rule_engine.h"
#include "../src/storage/database.h"
#include "../src/storage/repositories.h"
#include "../src/services/import_pipeline.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
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

// Find a test file — use a medium-sized KiCad symbol library
static fs::path find_test_file() {
    // Try a large file first, fall back to Diode
    fs::path large = "D:/Program Files/KiCad/10.0/share/kicad/symbols/Converter_DCDC.kicad_sym";
    if (fs::exists(large)) return large;
    fs::path test = "D:/Program Files/KiCad/10.0/share/kicad/symbols/Diode.kicad_sym";
    if (fs::exists(test)) return test;
    return {};
}

int main() {
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
            printf("  PARSE ERROR: %s\n", root.error().message.c_str());
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
            printf("  PARSE ERROR: %s\n", result.error().message.c_str());
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
            printf("  Cannot open DB: %s\n", db_result.error().message.c_str());
        } else {
            auto& db = *db_result;
            storage::SymbolRepository repo(db->handle());

            // Pre-create the default library
            sqlite3_exec(db->handle(),
                "INSERT OR IGNORE INTO libraries(id,name,file_path) "
                "VALUES('default','Default','test.kicad_sym')",
                nullptr, nullptr, nullptr);

            auto result = parser::SymbolLibParser::parse(test_file);
            if (result) {
                // Warm-up: 1 insert
                if (!result->items.empty()) {
                    auto s = result->items[0];
                    s.set_library_id("default");
                    repo.insert(s);
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
    if (sym_count > 0) {
        printf("\n--- Phase 8: LibraryService::import_symbol_library ---\n");
        auto tmp_path = fs::temp_directory_path() / "kforge_bench2.db";
        std::error_code ec;
        fs::remove(tmp_path, ec);
        auto db_result = storage::Database::open(tmp_path);
        if (db_result) {
            auto& db = *db_result;
            Timer t;
            auto result = services::ImportPipeline(db->handle())
                | services::symbols_from{test_file.string(), "test"}
                | services::execute;
            printf("  imported %d symbols: %.1f ms\n", result.symbols, t.ms());
            fs::remove(tmp_path, ec);
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
