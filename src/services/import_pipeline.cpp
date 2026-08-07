#include "services/import_pipeline.h"

#include "classifier/rule_engine.h"
#include "correspondence/checker.h"
#include "core/symbol.h"
#include "core/footprint.h"
#include "parser/symbol_lib_parser.h"
#include "parser/footprint_parser.h"
#include "storage/database.h"
#include "storage/repositories.h"
#include "util/logger.h"

#include <algorithm>
#include <filesystem>
#include <sqlite3.h>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace kforge::services {

// ============================================================
// operator| — accumulate steps
// ============================================================

ImportPipeline& ImportPipeline::operator|(symbols_from src) {
    pending_symbols_.push_back(std::move(src));
    return *this;
}
ImportPipeline& ImportPipeline::operator|(footprints_from src) {
    pending_footprints_.push_back(std::move(src));
    return *this;
}
ImportPipeline& ImportPipeline::operator|(models_from src) {
    pending_models_.push_back(std::move(src));
    return *this;
}
ImportPipeline& ImportPipeline::operator|(with_auto_link) {
    do_auto_link_ = true;
    return *this;
}
ImportPipeline& ImportPipeline::operator|(with_3d_linking) {
    do_3d_linking_ = true;
    return *this;
}
ImportPipeline& ImportPipeline::operator|(progress p) {
    progress_fn_ = p;
    return *this;
}

// ============================================================
// execute — run all accumulated steps in order
// ============================================================

ImportPipeline::Result ImportPipeline::operator|(execute_t) {
    Result r;

    // Phase 1: Import symbols
    for (auto& s : pending_symbols_) {
        auto stats = import_directory(s.dir, s.comp_lib_id);
        r.symbols += stats.symbols;
        r.footprints += stats.footprints;
    }

    // Phase 2: Import footprints (separate from symbols to handle .pretty)
    for (auto& f : pending_footprints_) {
        auto stats = import_directory(f.dir);
        r.footprints += stats.footprints;
    }

    // Phase 3: Scan 3D models
    for (auto& m : pending_models_) {
        int count = scan_3d_models(m.dir);
        r.models_3d += count;
    }

    // Phase 4: Auto-link symbol→footprint (inline — CorrespondenceService needs Database*)
    if (do_auto_link_) {
        storage::SymbolRepository sr(db_);
        storage::FootprintRepository fr(db_);
        storage::RelationshipRepository rr(db_);
        auto syms = sr.find_all();
        auto fps = fr.find_all();
        if (syms && fps && !syms->empty() && !fps->empty()) {
            // Capped suggest_matches
            const int MAX_S = 2000, MAX_F = 1000;
            std::vector<core::Symbol> capped_s;
            std::vector<core::Footprint> capped_f;
            for (int i = 0; i < (int)syms->size() && i < MAX_S; i++) capped_s.push_back((*syms)[i]);
            for (int i = 0; i < (int)fps->size() && i < MAX_F; i++) capped_f.push_back((*fps)[i]);
            auto suggestions = correspondence::CorrespondenceChecker::suggest_matches(capped_s, capped_f);
            for (auto& sug : suggestions) {
                if (sug.score >= 0.7) {
                    auto linked = rr.link_symbol_to_footprint(
                        sug.symbol_id, sug.footprint_id, "heuristic", sug.score);
                    if (linked) r.linked_symbols++;
                }
            }
        }
        LOG_INFO("Auto-link: {} symbol-footprint links", r.linked_symbols);
    }

    // Phase 5: Link 3D models to footprints
    if (do_3d_linking_) {
        r.linked_models = do_link_3d_models();
    }

    return r;
}

// ============================================================
// Import implementations (moved from LibraryService)
// ============================================================

int ImportPipeline::import_symbol_library(const std::string& path_str,
                                           const std::string& comp_lib_id) {
    fs::path path(path_str);
    auto result = parser::SymbolLibParser::parse(path);
    if (!result) return 0;

    LOG_INFO("IMPORT: {} -> {} items", path_str, result->items.size());

    storage::LibraryRepository lr(db_);
    std::string lib_id = result->name.empty() ? path.stem().string() : result->name;
    {
        core::LibraryMeta lib;
        lib.name = lib_id;
        lib.file_path = path;
        lib.component_library_id = comp_lib_id;
        lib.description = "Imported from " + path.filename().string();
        auto lib_ins = lr.insert(lib);
        if (!lib_ins) { LOG_ERROR("Cannot create library"); return 0; }
        lib_id = lib_ins->id;
    }

    storage::SymbolRepository repo(db_);
    std::unordered_set<std::string> seen;
    {
        auto existing = repo.find_by_library(lib_id);
        if (existing) for (auto& s : *existing) seen.insert(s.name());
    }

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    int imported = 0;
    for (auto& sym : result->items) {
        if (seen.count(sym.name())) continue;
        seen.insert(sym.name());
        if (sym.component_type == core::ComponentType::Unknown)
            sym.component_type = classifier::guess_type_from_name(sym.name());
        sym.set_library_id(lib_id);
        auto ins = repo.insert(sym);
        if (ins) {
            imported++;
            try_link_symbol_footprint(ins->id(), ins->footprint());
        }
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return imported;
}

int ImportPipeline::import_footprint(const std::string& path_str) {
    fs::path path(path_str);
    auto result = parser::FootprintParser::parse(path);
    if (!result) return 0;

    storage::FootprintRepository repo(db_);
    auto existing = repo.find_by_name(result->name());
    if (existing) return 0;  // dedup

    auto ins = repo.insert(*result);
    if (ins) {
        LOG_INFO("FP: {} -> {}", path.filename().string(), ins->name());
        return 1;
    }
    return 0;
}

ImportPipeline::ImportStats ImportPipeline::import_directory(
    const std::string& dir_str, const std::string& comp_lib_id) {
    ImportStats stats;
    fs::path dir(dir_str);
    if (!fs::exists(dir)) return stats;

#ifdef _WIN32
    std::string pattern = dir.string();
    for (auto& c : pattern) if (c == '/') c = '\\';
    pattern += "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return stats;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string full = dir.string() + "\\" + name;
        auto ends_with = [](const std::string& s, const std::string& suf) {
            return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
        };
        // .pretty folders (KiCad footprint libraries)
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && ends_with(name, ".pretty")) {
            int fp_count = 0;
            std::string pp = full + "\\*";
            WIN32_FIND_DATAA pfd;
            HANDLE ph = FindFirstFileA(pp.c_str(), &pfd);
            if (ph != INVALID_HANDLE_VALUE) {
                do {
                    std::string pname = pfd.cFileName;
                    if (pname == "." || pname == "..") continue;
                    if (pfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    if (ends_with(pname, ".kicad_mod")) {
                        auto r = import_footprint(full + "\\" + pname);
                        if (r) { stats.footprints += r; fp_count++; }
                        else stats.errors++;
                    }
                } while (FindNextFileA(ph, &pfd));
                FindClose(ph);
                LOG_INFO("FP DIR: {} -> {} footprints", full, fp_count);
            }
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        bool is_sym = ends_with(name, ".kicad_sym");
        bool is_mod = ends_with(name, ".kicad_mod");
        stats.messages.push_back(name);
        if (is_sym) {
            auto r = import_symbol_library(full, comp_lib_id);
            if (r > 0) stats.symbols += r; else stats.errors++;
        } else if (is_mod) {
            auto r = import_footprint(full);
            if (r) stats.footprints += r; else stats.errors++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
    LOG_INFO("DIR SCAN: {} -> {} symbols, {} footprints, {} errors",
             dir_str, stats.symbols, stats.footprints, stats.errors);
    return stats;
}

int ImportPipeline::scan_3d_models(const std::string& dir_str) {
    fs::path dir(dir_str);
    if (!fs::exists(dir)) return 0;

    storage::Model3DRepository repo(db_);
    int imported = 0, skipped = 0;
    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        std::string format;
        if (ext == ".step" || ext == ".stp") format = "step";
        else if (ext == ".wrl") format = "wrl";
        else if (ext == ".iges" || ext == ".igs") format = "iges";
        else continue;
        auto existing = repo.find_by_path(entry.path().string());
        if (existing) { skipped++; continue; }
        core::Model3D m;
        m.set_file_path(entry.path());
        m.set_format(format);
        m.set_description(entry.path().stem().string());
        m.set_source("filesystem");
        auto ins = repo.insert(m);
        if (ins) imported++;
    }
    LOG_INFO("3D SCAN: {} -> {} imported, {} already in DB", dir_str, imported, skipped);
    return imported;
}

void ImportPipeline::try_link_symbol_footprint(const std::string& sym_id, const std::string& fp_ref) {
    if (fp_ref.empty()) return;
    // Strip library prefix: "Package_SO:SOIC-8" → "SOIC-8"
    auto colon = fp_ref.find(':');
    std::string short_name = (colon != std::string::npos) ? fp_ref.substr(colon + 1) : fp_ref;

    storage::FootprintRepository fr(db_);
    storage::RelationshipRepository rr(db_);
    auto fp = fr.find_by_name(short_name);
    if (!fp) fp = fr.find_by_name(fp_ref);
    if (fp) {
        auto _ = rr.link_symbol_to_footprint(sym_id, fp->id(), "imported", 1.0);
    }
}

int ImportPipeline::do_link_3d_models() {
    storage::SymbolRepository sr(db_);
    storage::FootprintRepository fr(db_);
    storage::Model3DRepository mr(db_);
    storage::RelationshipRepository rr(db_);
    auto syms = sr.find_all();
    auto models = mr.find_all();
    if (!syms || !models) return 0;

    // Build model stem → id index (O(N))
    std::unordered_map<std::string, core::Uuid> model_index;
    for (auto& m : *models) {
        std::string stem = m.file_path().stem().string();
        if (!model_index.count(stem)) model_index[stem] = m.id();
    }

    int linked = 0, skipped = 0;
    for (auto& sym : *syms) {
        if (sym.footprint().empty()) { skipped++; continue; }
        auto colon = sym.footprint().find(':');
        std::string short_fp = (colon != std::string::npos) ? sym.footprint().substr(colon + 1)
                                                             : sym.footprint();
        // Exact match
        core::Uuid model_id;
        auto it = model_index.find(short_fp);
        if (it != model_index.end()) {
            model_id = it->second;
        } else {
            // Substring match
            for (auto& [stem, id] : model_index) {
                if (stem.find(short_fp) != std::string::npos ||
                    short_fp.find(stem) != std::string::npos) { model_id = id; break; }
            }
        }
        if (model_id.empty()) continue;

        core::Uuid fp_id;
        auto existing_fp = rr.find_footprint_for_symbol(sym.id());
        if (existing_fp && *existing_fp) {
            fp_id = **existing_fp;
        } else {
            auto existing = fr.find_by_name(short_fp);
            if (existing) {
                fp_id = existing->id();
            } else {
                core::Footprint fp;
                fp.set_name(short_fp);
                fp.set_description("Auto-created from symbol footprint_ref");
                auto ins = fr.insert(fp);
                if (!ins) continue;
                fp_id = ins->id();
            }
            auto _ = rr.link_symbol_to_footprint(sym.id(), fp_id, "imported", 1.0);
        }
        auto _ = rr.link_footprint_to_model(fp_id, model_id, "heuristic");
        linked++;
    }
    LOG_INFO("3D LINK: {} linked, {} skipped (no footprint_ref)", linked, skipped);
    return linked;
}

}  // namespace kforge::services
