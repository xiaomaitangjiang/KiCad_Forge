#include "services/library_service.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <fstream>
#include <sstream>

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

    // Must have a target library — auto-create from file name for local imports
    storage::LibraryRepository lr(db_->handle());
    core::Uuid lib_id = sym_target_library_;
    if (lib_id.empty()) {
        std::string lib_name = result->name.empty() ? path.stem().string() : result->name;
        core::LibraryMeta lib;
        lib.name = lib_name;
        lib.file_path = path;
        lib.description = "Imported from " + path.filename().string();
        auto lib_ins = lr.insert(lib);
        lib_id = lib_ins ? lib_ins->id : "";
    }
    if (lib_id.empty()) { printf("No target library — skipping import\n"); return 0; }

    storage::SymbolRepository repo(db_->handle());
    int imported = 0;
    for (auto& sym : result->items) {
        // Dedup: check within THIS library only, not globally.
        // Same symbol name can exist in multiple libraries.
        auto in_lib = repo.find_by_library(lib_id);
        bool exists = false;
        if (in_lib) for (auto& s : *in_lib) if (s.name() == sym.name()) { exists = true; break; }
        if (exists) continue;
        if (sym.component_type == core::ComponentType::Unknown)
            sym.component_type = classifier::guess_type_from_name(sym.name());
        sym.set_library_id(lib_id);
        auto ins = repo.insert(sym);
        if (ins) { imported++; try_link_symbol_footprint(*ins); }
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
    if (!std::filesystem::exists(dir))
        return std::unexpected(util::Error::io("Directory not found: " + dir.string()));

#ifdef _WIN32
    std::string pattern = dir.string();
    for (auto& c : pattern) if (c == '/') c = '\\';
    pattern += "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return std::unexpected(util::Error::io("Cannot list directory: " + dir.string()));
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string full = dir.string() + "\\" + name;
        auto ends_with = [](const std::string& s, const std::string& suf) {
            return s.size() >= suf.size() && s.compare(s.size()-suf.size(), suf.size(), suf) == 0;
        };
        // Handle .pretty folders (KiCad footprint libraries)
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && ends_with(name, ".pretty")) {
            // Scan inside this .pretty folder for .kicad_mod files
            std::string pretty_pattern = full + "\\*";
            WIN32_FIND_DATAA pfd;
            HANDLE ph = FindFirstFileA(pretty_pattern.c_str(), &pfd);
            if (ph != INVALID_HANDLE_VALUE) {
                do {
                    std::string pname = pfd.cFileName;
                    if (pname == "." || pname == "..") continue;
                    if (pfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    if (ends_with(pname, ".kicad_mod")) {
                        std::string pfull = full + "\\" + pname;
                        stats.messages.push_back(name + "/" + pname + "=MOD");
                        auto r = import_footprint(pfull);
                        if (r) stats.footprints += *r;
                        else stats.errors++;
                    }
                } while (FindNextFileA(ph, &pfd));
                FindClose(ph);
            }
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        bool is_sym = ends_with(name, ".kicad_sym");
        bool is_mod = ends_with(name, ".kicad_mod");
        stats.messages.push_back(name + (is_sym ? "=SYM" : is_mod ? "=MOD" : "=SKIP"));
        if (is_sym) {
            try {
                auto r = import_symbol_library(full);
                if (r) stats.symbols += *r;
                else { stats.errors++; stats.messages.push_back(name + ": " + r.error().message); }
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
    stats.messages.push_back("[Win32] scanned " + std::to_string(stats.symbols + stats.footprints + stats.errors) + " files");
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
    if (fp) { auto _ = rel_repo.link_symbol_to_footprint(sym.id(), fp->id(), "imported", 1.0); }
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
    if (!std::filesystem::exists(dir))
        return std::unexpected(util::Error::io("Directory not found: " + dir.string()));
    storage::Model3DRepository repo(db_->handle());
    int imported = 0;
    for (auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        std::string format;
        if (ext == ".step" || ext == ".stp") format = "step";
        else if (ext == ".wrl") format = "wrl";
        else if (ext == ".iges" || ext == ".igs") format = "iges";
        else continue;
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
        auto colon = fp_name.find(':');
        std::string short_fp = (colon != std::string::npos) ? fp_name.substr(colon + 1) : fp_name;
        for (auto& model : *models) {
            std::string model_stem = model.file_path().stem().string();
            double fp_sim = correspondence::CorrespondenceChecker::name_similarity(short_fp, model_stem);
            double name_sim = correspondence::CorrespondenceChecker::name_similarity(sym.name(), model_stem);
            double best_sim = std::max(fp_sim, name_sim);
            bool match = (best_sim > 0.7) ||
                model_stem.find(short_fp.substr(0, std::min<size_t>(15, short_fp.size()))) != std::string::npos;
            if (!match) continue;
            core::Uuid fp_id;
            auto existing_fp = rr.find_footprint_for_symbol(sym.id());
            if (existing_fp && *existing_fp) {
                fp_id = **existing_fp;
            } else {
                core::Footprint fp; fp.set_name(short_fp);
                fp.set_description("Auto-created from symbol footprint_ref");
                auto ins = fr.insert(fp); if (!ins) continue;
                fp_id = ins->id();
                auto _ = rr.link_symbol_to_footprint(sym.id(), fp_id, "imported", 1.0);
            }
            auto _ = rr.link_footprint_to_model(fp_id, model.id(), "heuristic");
            linked++; break;
        }
    }
    printf("3D LINK: %d models linked to symbols\n", linked);
    return linked;
}

// ============================================================
// merge_into_library — KiCad v10 correct S-expression format
// ============================================================

// Helper: atom needs quoting only if it contains spaces or certain chars
static bool needs_quote(const std::string& s) {
    if (s.empty()) return true;
    for (char c : s) {
        if (c == ' ' || c == '"' || c == '(' || c == ')' || c == '\n' || c == '\t' || c == '\r')
            return true;
    }
    return false;
}

static std::string atom_or_string(const std::string& s) {
    if (needs_quote(s)) return "\"" + s + "\"";
    return s;
}

// Format a double with 2 decimal places (KiCad standard)
static std::string fmt2(double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// Write property in correct KiCad format (matching real .kicad_sym files)
static std::string write_property(const std::string& name, const std::string& val,
                                   double x, double y, bool hide) {
    std::string s = "\t\t\t(property \"" + name + "\" " + atom_or_string(val) + "\n"
                    "\t\t\t\t(at " + fmt2(x) + " " + fmt2(y) + " 0)\n";
    s += "\t\t\t\t(effects\n"
         "\t\t\t\t\t(font (size 1.27 1.27))\n";
    if (hide && name != "Reference" && name != "Value")
        s += "\t\t\t\t\t(hide yes)\n";
    s += "\t\t\t\t)\n"
         "\t\t\t)\n";
    return s;
}

util::Result<int> LibraryService::merge_into_library(
    const std::filesystem::path& source_sym,
    const std::filesystem::path& target_sym)
{
    // Parse source to get symbols
    auto src = parser::SymbolLibParser::parse(source_sym);
    if (!src || src->items.empty()) {
        printf("MERGE: no symbols in source\n");
        return 0;
    }

    // Read target file
    std::string target_text;
    if (std::filesystem::exists(target_sym)) {
        std::ifstream tf(target_sym, std::ios::binary);
        if (!tf.is_open())
            return std::unexpected(util::Error::io("Cannot open: " + target_sym.string()));
        std::stringstream buf; buf << tf.rdbuf();
        target_text = buf.str();
    }

    // Find where to insert — before the last ')'
    auto last_paren = target_text.find_last_of(')');
    if (last_paren == std::string::npos) {
        // Empty or invalid — create new header
        target_text = "(kicad_symbol_lib\n"
                      "  (version 20231120)\n"
                      "  (generator KiCad_Forge)\n"
                      ")\n";
        last_paren = target_text.find_last_of(')');
    }

    // Build symbol blocks matching real KiCad .kicad_sym format
    std::string insertion;
    for (auto& sym : src->items) {
        std::string name = sym.name();
        std::string val = sym.default_value().empty() ? name : sym.default_value();
        std::string ref = sym.reference_prefix().empty() ? "U" : sym.reference_prefix();

        insertion += "\n\t(symbol " + atom_or_string(name) + "\n";

        if (sym.pin_count() > 0)
            insertion += "\t\t(pin_names (offset 1.016))\n";
        insertion += "\t\t(exclude_from_sim no)\n";
        insertion += "\t\t(in_bom yes)\n";
        insertion += "\t\t(on_board yes)\n";

        // Properties — hide yes is INSIDE effects, not separate
        insertion += write_property("Reference", ref, 0, 7.62, false);
        insertion += write_property("Value", val, 0, 2.54, false);
        if (!sym.footprint().empty())
            insertion += write_property("Footprint", sym.footprint(), 0, -2.54, true);
        if (!sym.datasheet().empty())
            insertion += write_property("Datasheet", sym.datasheet(), 0, -7.62, true);
        if (!sym.description().empty())
            insertion += write_property("Description", sym.description(), 0, -12.7, true);
        if (!sym.mpn().empty())
            insertion += write_property("MPN", sym.mpn(), 0, -17.78, true);

        // Graphical sub-symbol
        insertion += "\t\t(symbol " + atom_or_string(name + "_0_1") + "\n"
                     "\t\t\t(pin_numbers hide)\n"
                     "\t\t\t(pin_names hide)\n"
                     "\t\t\t(rectangle (start -5.08 5.08) (end 5.08 -5.08)\n"
                     "\t\t\t\t(stroke (width 0.254) (type default))\n"
                     "\t\t\t\t(fill (type background))\n"
                     "\t\t\t)\n";

        // Pins — exact KiCad format: (pin "N" electrical_type graphic_style ...)
        int n = sym.pin_count();
        if (n == 0) n = 3;  // fallback: at least 3 pins for basic IC
        for (int i = 0; i < n; i++) {
            std::string pin_num, pin_name, etype;
            if (i < (int)sym.pins().size()) {
                auto& p = sym.pins()[i];
                pin_num = p.number;
                pin_name = p.name;
                etype = p.electrical_type;
            }
            if (pin_num.empty() || pin_num == "unspecified") pin_num = std::to_string(i + 1);
            if (pin_name.empty()) pin_name = pin_num;
            if (etype.empty()) etype = "passive";

            double py = 3.81 - i * 2.54;
            bool left = (i < n / 2);
            double px = left ? -7.62 : 7.62;
            double ang = left ? 0.0 : 180.0;
            // etype might have compound like "input line" — keep as-is
            if (etype.find(' ') == std::string::npos) etype += " line";

            insertion += "\t\t\t(pin " + etype + "\n"
                         "\t\t\t\t(at " + fmt2(px) + " " + fmt2(py) + " " + fmt2(ang) + ")\n"
                         "\t\t\t\t(length 2.54)\n"
                         "\t\t\t\t(name " + atom_or_string(pin_name) + "\n"
                         "\t\t\t\t\t(effects (font (size 1.27 1.27)))\n"
                         "\t\t\t\t)\n"
                         "\t\t\t\t(number " + atom_or_string(pin_num) + "\n"
                         "\t\t\t\t\t(effects (font (size 1.27 1.27)))\n"
                         "\t\t\t\t)\n"
                         "\t\t\t)\n";
        }

        insertion += "\t\t)\n";  // close sub-symbol
        insertion += "\t)\n";    // close symbol
    }

    // Insert before closing ')'
    target_text.insert(last_paren, insertion);

    // Write back
    std::ofstream out(target_sym, std::ios::binary);
    if (!out.is_open())
        return std::unexpected(util::Error::io("Cannot write: " + target_sym.string()));
    out << target_text;

    printf("MERGED: %zu symbols into %s\n", src->items.size(), target_sym.string().c_str());
    return (int)src->items.size();
}

}  // namespace kforge::services
