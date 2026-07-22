#include "services/library_service.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>

#include "storage/database.h"
#include "storage/repositories.h"
#include "parser/symbol_lib_parser.h"
#include "parser/footprint_parser.h"
#include "classifier/rule_engine.h"
#include "correspondence/checker.h"

namespace kforge::services {

LibraryService::LibraryService(storage::Database* db) : db_(db) {}

util::Result<int> LibraryService::import_symbol_library(const std::filesystem::path& path) {
    auto result = parser::SymbolLibParser::parse(path);
    if (!result) return std::unexpected(result.error());

    printf("IMPORT: %s -> %zu items\n", path.string().c_str(), result->items.size());

    storage::SymbolRepository repo(db_->handle());
    int imported = 0;

    for (auto& sym : result->items) {
        // Skip if already imported
        auto existing = repo.find_by_name(sym.name());
        if (existing) continue;

        // Infer type from name prefix
        if (sym.component_type == core::ComponentType::Unknown) {
            sym.component_type = classifier::guess_type_from_name(sym.name());
        }

        auto ins = repo.insert(sym);
        if (ins) {
            imported++;
            // Try to link to existing footprint
            try_link_symbol_footprint(*ins);
        }
    }
    return imported;
}

util::Result<int> LibraryService::import_footprint(const std::filesystem::path& path) {
    auto result = parser::FootprintParser::parse(path);
    if (!result) return std::unexpected(result.error());

    storage::FootprintRepository repo(db_->handle());
    auto ins = repo.insert(*result);
    return ins.has_value() ? 1 : 0;
}

util::Result<LibraryService::ImportStats> LibraryService::import_directory(
    const std::filesystem::path& dir) {
    ImportStats stats;
    if (!std::filesystem::exists(dir)) {
        return std::unexpected(util::Error::io("Directory not found: " + dir.string()));
    }

#ifdef _WIN32
    std::string pattern = dir.string();
    for (auto& c : pattern) if (c == '/') c = '\\';
    pattern += "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return std::unexpected(util::Error::io("Cannot list directory: " + dir.string()));
    }
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string full = dir.string() + "\\" + name;
        // Match .kicad_sym / .kicad_mod by checking filename suffix
        auto ends_with = [](const std::string& s, const std::string& suffix) {
            return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        bool is_sym = ends_with(name, ".kicad_sym");
        bool is_mod = ends_with(name, ".kicad_mod");
        stats.messages.push_back(name + (is_sym ? "=SYM" : is_mod ? "=MOD" : "=SKIP"));

        if (is_sym) {
            try {
                auto r = import_symbol_library(full);
                if (r) stats.symbols += *r;
                else { stats.errors++; stats.messages.push_back(name + ": IMPORT_FAIL " + r.error().message); }
            } catch (std::exception& e) {
                stats.errors++; stats.messages.push_back(name + ": EXCEPTION " + std::string(e.what()));
            }
        } else if (is_mod) {
            auto r = import_footprint(full);
            if (r) stats.footprints += *r;
            else { stats.errors++; stats.messages.push_back(name + ": " + r.error().message); }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    stats.messages.push_back("[Win32] scanned " + std::to_string(stats.symbols + stats.footprints + stats.errors) + " files in " + dir.string());
#else
    for (auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto& p = entry.path();
        auto ext = p.extension().string();
        if (ext == ".kicad_sym") {
            auto r = import_symbol_library(p);
            if (r) stats.symbols += *r;
            else { stats.errors++; stats.messages.push_back(p.filename().string() + ": " + r.error().message); }
        } else if (ext == ".kicad_mod") {
            auto r = import_footprint(p);
            if (r) stats.footprints += *r;
            else { stats.errors++; stats.messages.push_back(p.filename().string() + ": " + r.error().message); }
        }
    }
#endif
    return stats;
}

void LibraryService::try_link_symbol_footprint(const core::Symbol& sym) {
    if (sym.footprint().empty()) return;

    storage::FootprintRepository fp_repo(db_->handle());
    storage::RelationshipRepository rel_repo(db_->handle());

    auto fp = fp_repo.find_by_name(sym.footprint());
    if (fp) {
        auto _ = rel_repo.link_symbol_to_footprint(sym.id(), fp->id(), "imported", 1.0);
    }
}

int LibraryService::symbol_count() const {
    storage::SymbolRepository repo(db_->handle());
    return repo.count();
}

int LibraryService::footprint_count() const {
    storage::FootprintRepository repo(db_->handle());
    return repo.count();
}

int LibraryService::model_3d_count() const {
    storage::Model3DRepository repo(db_->handle());
    return repo.count();
}

util::Result<int> LibraryService::scan_3d_models(const std::filesystem::path& dir) {
    if (!std::filesystem::exists(dir)) {
        return std::unexpected(util::Error::io("Directory not found: " + dir.string()));
    }

    storage::Model3DRepository repo(db_->handle());
    int imported = 0;

    // Recursive walk using std::filesystem
    for (auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        std::string format;
        if (ext == ".step" || ext == ".stp") format = "step";
        else if (ext == ".wrl") format = "wrl";
        else if (ext == ".iges" || ext == ".igs") format = "iges";
        else continue;

        // Check if already registered
        auto existing = repo.find_by_path(entry.path().string());
        if (existing) continue;

        core::Model3D m;
        m.set_file_path(entry.path());
        m.set_format(format);
        m.set_description(entry.path().stem().string());
        m.set_source("filesystem");
        auto ins = repo.insert(m);
        if (ins) imported++;
    }
    printf("3D SCAN: %s -> %d models\n", dir.string().c_str(), imported);
    return imported;
}

util::Result<int> LibraryService::link_3d_models_to_symbols() {
    storage::SymbolRepository sr(db_->handle());
    storage::FootprintRepository fr(db_->handle());
    storage::Model3DRepository mr(db_->handle());
    storage::RelationshipRepository rr(db_->handle());

    auto syms = sr.find_all();
    auto models = mr.find_all();
    if (!syms || !models) return 0;

    int linked = 0;

    for (auto& sym : *syms) {
        if (sym.footprint().empty()) continue;

        std::string fp_name = sym.footprint();
        // Extract short footprint name (after ':')
        auto colon = fp_name.find(':');
        std::string short_fp = (colon != std::string::npos) ? fp_name.substr(colon + 1) : fp_name;

        for (auto& model : *models) {
            std::string model_stem = model.file_path().stem().string();

            // Heuristic match using Levenshtein similarity
            double fp_sim = correspondence::CorrespondenceChecker::name_similarity(short_fp, model_stem);
            double name_sim = correspondence::CorrespondenceChecker::name_similarity(sym.name(), model_stem);
            double best_sim = std::max(fp_sim, name_sim);
            bool match = (best_sim > 0.7) ||
                         model_stem.find(short_fp.substr(0, std::min<size_t>(15, short_fp.size()))) != std::string::npos;

            if (!match) continue;

            // Ensure a footprint record exists for this symbol
            core::Uuid fp_id;
            auto existing_fp = rr.find_footprint_for_symbol(sym.id());
            if (existing_fp && *existing_fp) {
                fp_id = **existing_fp;
            } else {
                // Create a stub footprint record
                core::Footprint fp;
                fp.set_name(short_fp);
                fp.set_description("Auto-created from symbol footprint_ref");
                auto ins = fr.insert(fp);
                if (!ins) continue;
                fp_id = ins->id();
                auto _ = rr.link_symbol_to_footprint(sym.id(), fp_id, "imported", 1.0);
            }

            // Link model to footprint
            auto _ = rr.link_footprint_to_model(fp_id, model.id(), "heuristic");
            linked++;
            break;  // One model per symbol
        }
    }

    printf("3D LINK: %d models linked to symbols\n", linked);
    return linked;
}

}  // namespace kforge::services
