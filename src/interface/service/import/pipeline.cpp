#include "classifier/rule_engine.h"
#include "core/model/footprint.h"
#include "core/model/symbol.h"
#include "correspondence/checker.h"
#include "core/io/parser/footprint_parser.h"
#include "core/io/parser/symbol_lib_parser.h"
#include "core/io/sexpr/path_util.h"
#include "core/io/sexpr/text_util.h"
#include "interface/service/import/pipeline.h"
#include "core/db/database.h"
#include "core/repo/repositories.h"
#include "util/logger.h"
#include "util/error.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <sqlite3.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace kforge::services
{

namespace
{
// Map a 3D model file extension to its format name ("" = not a model)
}  // namespace

// ============================================================
// operator| — accumulate steps
// ============================================================

ImportPipeline& ImportPipeline::operator|(symbols_from src)
{
    pending_symbols_.push_back(std::move(src));
    return *this;
}
ImportPipeline& ImportPipeline::operator|(footprints_from src)
{
    pending_footprints_.push_back(std::move(src));
    return *this;
}
ImportPipeline& ImportPipeline::operator|(models_from src)
{
    pending_models_.push_back(std::move(src));
    return *this;
}
ImportPipeline& ImportPipeline::operator|(progress p)
{
    progress_fn_ = std::move(p);
    return *this;
}

// ============================================================
// execute — run all accumulated steps in order
// ============================================================

ImportPipeline::Result ImportPipeline::operator|(execute_t /*unused*/)
{
    Result r;

    // Load per-file mtime snapshots once — file_unchanged()/record_file() then
    // hit memory instead of one SQLite query per file
    {
        storage::ImportedFilesRepository ifr(db_);
        if (auto snapshots = ifr.all())
        {
            file_mtimes_ = std::move(*snapshots);
        }
        else
        {
            file_mtimes_.clear();
        }
    }

    // 3D model roots for resolving ${KICADx_3DMODEL_DIR} references
    model_roots_.clear();
    for (auto& m : pending_models_)
    {
        model_roots_.push_back(m.dir);
    }
    model_ref_imported_ = 0;

    if (cancelled())
    {
        return r;
    }

    // Phase 1: Import symbols
    for (auto& s : pending_symbols_)
    {
        if (cancelled())
        {
            break;
        }
        if (auto sr = import_directory(s.dir, s.comp_lib_id); sr.is(ImportStatus::Added))
        {
            r.symbols += sr.info.symbols;
            r.footprints += sr.info.footprints;
            r.skipped += sr.info.skipped;
        }
    }

    // In-memory pools loaded once — replace per-file find_by_name / find_by_path
    // SQLite queries during the footprint phase
    {
        storage::FootprintRepository fr(db_);
        if (auto fps = fr.find_all())
        {
            for (auto& f : *fps)
            {
                fp_by_name_.emplace(f.name(), std::move(f));
            }
        }
        storage::Model3DRepository mr(db_);
        if (auto models = mr.find_all())
        {
            for (auto& m : *models)
            {
                models_by_path_.emplace(m.file_path().string(), std::move(m));
            }
        }
    }

    // Phase 2: Import footprints — resolve (model ...) reference paths against
    // the footprint dir + model roots, import the file (source="footprint_ref")
    // and link explicitly. No stem fallback.
    for (auto& f : pending_footprints_)
    {
        if (cancelled())
        {
            break;
        }
        if (auto sr = import_directory(f.dir); sr.is(ImportStatus::Added))
        {
            r.footprints += sr.info.footprints;
            r.skipped += sr.info.skipped;
        }
    }

    // Phase 3: Scan 3D model directories AFTER footprints — the in-memory
    // hash pool (models_by_path_) now contains the ref-imported models and skips them
    for (auto& m : pending_models_)
    {
        if (cancelled())
        {
            break;
        }
        int count = scan_3d_models(m.dir);
        r.models_3d += count;
    }

    // Models imported from explicit footprint refs during Phase 2
    r.models_3d += model_ref_imported_;

    // Phase 4: Link symbols → footprints (footprints now exist in DB).
    // Links only depend on the symbol/footprint sets — skip when nothing was
    // added this run. Memory-mapped: name index + existing links loaded once,
    // new links written in one transaction (idempotent — never re-links).
    if (!cancelled() && (r.symbols > 0 || r.footprints > 0))
    {
        std::unordered_map<std::string, core::Uuid> fp_id_by_name;
        fp_id_by_name.reserve(fp_by_name_.size());
        for (auto& [name, fp] : fp_by_name_)
        {
            fp_id_by_name[name] = fp.id();
        }

        storage::RelationshipRepository rr(db_);
        std::unordered_map<std::string, std::string> existing_links;
        if (auto links = rr.find_all_symbol_links())
        {
            existing_links = std::move(*links);
        }

        storage::SymbolRepository sym_repo(db_);
        if (auto syms = sym_repo.find_all())
        {
            sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
            for (auto& sym : *syms)
            {
                if (cancelled())
                {
                    break;
                }
                if (sym.footprint().empty() || existing_links.contains(sym.id()))
                {
                    continue;
                }
                // Strip library prefix: "Package_SO:SOIC-8" → "SOIC-8"
                auto colon = sym.footprint().find(':');
                std::string short_name = (colon != std::string::npos)
                                             ? sym.footprint().substr(colon + 1)
                                             : sym.footprint();
                auto it = fp_id_by_name.find(short_name);
                if (it == fp_id_by_name.end())
                {
                    it = fp_id_by_name.find(sym.footprint());
                }
                if (it != fp_id_by_name.end())
                {
                    auto _ = rr.link_symbol_to_footprint(sym.id(), it->second, "imported", 1.0);
                }
            }
            sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
        }
    }

    return r;
}

// ============================================================
// Import implementations 
// ============================================================

IMP_Result ImportPipeline::import_symbol_library(const std::string& path_str,
                                                 const std::string& comp_lib_id)
{
    auto lib = parse_symbol_library(path_str);
    if (!lib)
    {
        return imp_skipped();  // unchanged or parse error
    }
    return write_symbol_library(*lib, comp_lib_id);
}

std::optional<ImportPipeline::ParsedSymbolLib> ImportPipeline::parse_symbol_library(
    const std::string& path_str)
{
    fs::path path(path_str);

    // Per-file mtime snapshot — skip parsing entirely when unchanged
    if (file_unchanged(path))
    {
        return std::nullopt;
    }

    auto result = parser::SymbolLibParser::parse(path);
    if (!result)
    {
        kforge::util::log_warn{}("SYM PARSE FAIL: {} -> {}", path_str,
                                 util::error_formatter(result.error()));
        return std::nullopt;  // not recorded — retried next run
    }

    ParsedSymbolLib lib;
    lib.path = path;
    lib.lib_name = result->name.empty() ? path.stem().string() : result->name;
    lib.items = std::move(result->items);
    return lib;
}

IMP_Result ImportPipeline::write_symbol_library(ParsedSymbolLib& lib,
                                                const std::string& comp_lib_id)
{
    const fs::path& path = lib.path;
    kforge::util::log_info{}("IMPORT: {} -> {} items", path.string(), lib.items.size());

    storage::LibraryRepository lr(db_);
    std::string lib_id = lib.lib_name;
    {
        core::LibraryMeta meta;
        meta.name = lib_id;
        meta.file_path = path;
        meta.component_library_id = comp_lib_id;
        meta.description = "Imported from " + path.filename().string();
        auto lib_ins = lr.insert(meta);
        if (!lib_ins)
        {
            kforge::util::log_error{}("Cannot create library");
            return imp_failed("Cannot create library");
        }
        lib_id = lib_ins->id;
    }

    storage::SymbolRepository repo(db_);
    std::unordered_set<std::string> seen;
    std::unordered_map<std::string, std::string> id_by_name;  // name → existing row id
    {
        auto existing = repo.find_by_library(lib_id);
        if (existing)
        {
            for (auto& s : *existing)
            {
                seen.insert(s.name());
                id_by_name[s.name()] = s.id();
            }
        }
    }

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    int imported = 0;
    for (auto& sym : lib.items)
    {
        if (sym.component_type == core::ComponentType::Unknown)
        {
            sym.component_type = classifier::guess_type_from_name(sym.name());
        }
        sym.set_library_id(lib_id);

        if (seen.contains(sym.name()))
        {
            // File changed since last import — refresh the existing row
            sym.set_id(id_by_name[sym.name()]);
            if (repo.update(sym))
            {
                imported++;
            }
            continue;
        }
        seen.insert(sym.name());
        auto ins = repo.insert(sym);
        if (ins)
        {
            imported++;  // linking happens in Phase 4
        }
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);

    // Optional: patch Kicad_Forge_ID back into the .kicad_sym file. Off by
    // default — official KiCad libraries must never be modified unless the
    // user explicitly opts in (write_kf_id_to_file_). Best-effort: failures
    // are logged but never fail the import.
    if (write_kf_id_to_file_ && std::filesystem::exists(path))
    {
        std::unordered_map<std::string, std::string> name_to_kf;
        for (auto& s : lib.items)
        {
            auto kf = s.Kicad_Forge_ID();
            if (!kf.empty())
            {
                name_to_kf[s.name()] = kf;
            }
        }
        if (!name_to_kf.empty())
        {
            int patched = sexpr::patch_kf_id_to_file(path, name_to_kf);
            if (patched > 0)
            {
                kforge::util::log_info{}("Patched Kicad_Forge_ID into {} symbols in {}",
                                         patched, path.filename().string());
            }
        }
    }

    record_file(path);
    return imported > 0 ? imp_added() : imp_skipped();
}

IMP_Result ImportPipeline::import_footprint(const std::string& path_str)
{
    fs::path path(path_str);
    auto fp = parse_footprint_file(path);
    if (!fp)
    {
        return imp_skipped();  // unchanged or parse error
    }
    return write_footprint(path, *fp);
}

std::optional<core::Footprint> ImportPipeline::parse_footprint_file(const fs::path& path)
{
    // Per-file mtime snapshot — skip parsing entirely when unchanged
    if (file_unchanged(path))
    {
        return std::nullopt;
    }

    auto result = parser::FootprintParser::parse(path);
    if (!result)
    {
        return std::nullopt;  // not recorded — retried next run
    }
    return std::move(*result);
}

IMP_Result ImportPipeline::write_footprint(const fs::path& path, const core::Footprint& fp_in)
{
    core::Footprint fp = fp_in;  // mutable copy for id assignment
    storage::FootprintRepository repo(db_);
    std::string fp_id;
    auto it = fp_by_name_.find(fp.name());
    if (it != fp_by_name_.end())
    {
        // File changed since last import — refresh the existing row
        fp.set_id(it->second.id());
        if (!repo.update(fp))
        {
            return imp_failed("Update failed");
        }
        fp_id = it->second.id();

        // Clear old model links — the refs may have changed
        storage::RelationshipRepository rr(db_);
        if (auto olds = rr.find_models_for_footprint(fp_id))
        {
            for (auto& mid : *olds)
            {
                auto _ = rr.unlink_footprint_model(fp_id, mid);
            }
        }
        it->second = std::move(fp);  // keep the pool in sync
    }
    else
    {
        auto ins = repo.insert(fp);
        if (!ins)
        {
            return imp_failed("Insert failed");
        }
        fp_id = ins->id();
        fp_by_name_.emplace(ins->name(), std::move(*ins));
    }
    kforge::util::log_info{}("FP: {} -> {}", path.filename().string(), fp.name());

    // Link 3D models: resolve explicit reference paths and import the file
    // if it is not in the DB yet — no stem fallback (user requirement)
    int model_refs = 0;
    int model_linked = 0;
    storage::RelationshipRepository rr(db_);
    storage::Model3DRepository mrepo(db_);
    for (auto& m : fp.models_3d())
    {
        if (m.path.empty())
        {
            continue;
        }
        model_refs++;

        // Resolve the reference against the footprint dir + model roots
        auto resolved = resolve_model_path(m.path, path.parent_path());
        if (resolved.empty())
        {
            continue;  // unresolvable reference — stays unlinked
        }
        std::string model_id;
        auto mit = models_by_path_.find(resolved.string());
        if (mit != models_by_path_.end())
        {
            model_id = mit->second.id();
        }
        else
        {
            core::Model3D nm;
            nm.set_file_path(resolved);
            nm.set_format(sexpr::model_format_of(resolved.extension().string()));
            nm.set_description(resolved.stem().string());
            nm.set_source("footprint_ref");
            if (auto nins = mrepo.insert(nm); nins)
            {
                model_id = nins->id();
                model_ref_imported_++;
                record_file(resolved);  // snapshot so the scan phase skips it
                models_by_path_.emplace(resolved.string(), std::move(*nins));
            }
        }
        if (!model_id.empty())
        {
            auto _ = rr.link_footprint_to_model(fp_id, model_id, "explicit");
            model_linked++;
        }
    }
    if (model_refs > 0)
    {
        kforge::util::log_info{}("FP 3D: {} refs, {} linked for {}", model_refs, model_linked,
                                 fp.name());
    }
    record_file(path);
    return imp_added();
}

ImportPipeline::ImportDirResult ImportPipeline::import_directory(const std::string& dir_str,
                                                                  const std::string& comp_lib_id)
{
    ImportCounts counts;
    fs::path dir(dir_str);
    if (!fs::exists(dir))
    {
        return ImportDirResult::err(ImportStatus::Failed, counts);
    }

    // Collect files
    std::vector<std::string> sym_files;
    std::vector<std::string> mod_files;
    std::vector<std::string> pretty_dirs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (ec)
        {
            break;
        }
        if (cancelled())
        {
            break;
        }
        if (entry.is_directory() && entry.path().extension() == ".pretty")
        {
            pretty_dirs.push_back(entry.path().string());
        }
        else if (entry.is_regular_file())
        {
            auto ext = entry.path().extension().string();
            if (ext == ".kicad_sym")
            {
                sym_files.push_back(entry.path().string());
            }
            else if (ext == ".kicad_mod")
            {
                mod_files.push_back(entry.path().string());
            }
        }
    }

    // Symbols: bounded parallel parse (pure CPU, no DB access), then serial
    // transactional writes on the single connection — never touch sqlite3*
    // from more than one thread at a time
    if (!sym_files.empty())
    {
        const unsigned n_threads =
            std::min(std::max(1u, std::thread::hardware_concurrency()), 16u);
        std::vector<ParsedSymbolLib> parsed_libs;
        std::mutex parsed_mtx;
        for (size_t i = 0; i < sym_files.size() && !cancelled(); i += n_threads)
        {
            size_t end = std::min(i + n_threads, sym_files.size());
            std::vector<std::future<void>> futures;
            futures.reserve(end - i);
            for (size_t j = i; j < end; ++j)
            {
                futures.push_back(std::async(
                    std::launch::async,
                    [this, &parsed_libs, &parsed_mtx, &f = sym_files[j]]()
                    {
                        if (auto lib = parse_symbol_library(f))
                        {
                            std::lock_guard lock(parsed_mtx);
                            parsed_libs.push_back(std::move(*lib));
                        }
                    }));
            }
            for (auto& fut : futures)
            {
                fut.get();
            }
        }

        for (auto& lib : parsed_libs)
        {
            if (cancelled())
            {
                break;
            }
            if (write_symbol_library(lib, comp_lib_id).is(ImportStatus::Added))
            {
                counts.symbols++;
            }
            else
            {
                counts.skipped++;
            }
        }
    }

    // Footprints: parallel parse (no DB access), then serial transactional
    // write — one BEGIN/COMMIT per directory instead of per-file commits
    auto import_fp_files = [this](const std::vector<std::string>& files)
    {
        struct Pending
        {
            fs::path path;
            core::Footprint fp;
        };
        std::vector<Pending> pending;
        std::mutex pending_mtx;
        std::vector<std::future<void>> futures;
        futures.reserve(files.size());
        for (auto& f : files)
        {
            if (cancelled())
            {
                break;
            }
            futures.push_back(std::async(std::launch::async,
                                         [this, &pending, &pending_mtx, &f]()
                                         {
                                             if (auto fp = parse_footprint_file(fs::path(f)))
                                             {
                                                 std::lock_guard lock(pending_mtx);
                                                 pending.push_back({f, std::move(*fp)});
                                             }
                                         }));
        }
        for (auto& fut : futures)
        {
            fut.get();
        }

        int added = 0;
        int skipped = static_cast<int>(files.size()) - static_cast<int>(pending.size());
        if (!pending.empty())
        {
            sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
            for (auto& p : pending)
            {
                if (cancelled())
                {
                    break;
                }
                if (write_footprint(p.path, p.fp).is(ImportStatus::Added))
                {
                    added++;
                }
                else
                {
                    skipped++;  // write failure
                }
            }
            sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
        }
        return std::pair<int, int>{added, skipped};
    };

    for (auto& pd : pretty_dirs)
    {
        std::vector<std::string> files;
        std::error_code ec2;
        for (const auto& pf : fs::directory_iterator(pd, ec2))
        {
            if (ec2)
            {
                break;
            }
            if (pf.is_regular_file() && pf.path().extension() == ".kicad_mod")
            {
                files.push_back(pf.path().string());
            }
        }
        auto [added, skipped] = import_fp_files(files);
        counts.footprints += added;
        counts.skipped += skipped;
        kforge::util::log_info{}("FP DIR: {} -> {} footprints", pd, added);
    }
    {
        auto [added, skipped] = import_fp_files(mod_files);
        counts.footprints += added;
        counts.skipped += skipped;
    }

    kforge::util::log_info{}("DIR SCAN: {} -> {} symbols, {} footprints, {} skipped", dir_str,
                             counts.symbols, counts.footprints, counts.skipped);
    return ImportDirResult{ImportStatus::Added, counts};
}

int ImportPipeline::scan_3d_models(const std::string& dir_str)
{
    fs::path dir(dir_str);
    if (!fs::exists(dir))
    {
        return 0;
    }

    storage::Model3DRepository repo(db_);

    // Reuse the shared in-memory hash pool (models_by_path_) — it already
    // contains the ref-imported models from the footprint phase
    int imported = 0;
    int skipped = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir))
    {
        if (cancelled())
        {
            break;
        }
        if (!entry.is_regular_file())
        {
            continue;
        }
        std::string format = sexpr::model_format_of(entry.path().extension().string());
        if (format.empty())
        {
            continue;
        }

        // Canonicalize so lookups match paths resolved from footprint refs
        std::error_code ec;
        auto canon = fs::weakly_canonical(entry.path(), ec);
        auto store_path = ec ? fs::absolute(entry.path()) : canon;

        auto it = models_by_path_.find(store_path.string());
        if (it != models_by_path_.end())
        {
            // Unchanged since last import → skip
            if (file_unchanged(store_path))
            {
                skipped++;
                continue;
            }
            auto _ = repo.update(it->second);
            record_file(store_path);
            imported++;
            continue;
        }
        core::Model3D m;
        m.set_file_path(store_path);
        m.set_format(format);
        m.set_description(entry.path().stem().string());
        m.set_source("filesystem");
        auto ins = repo.insert(m);
        if (ins)
        {
            record_file(store_path);
            models_by_path_.emplace(store_path.string(), std::move(*ins));
            imported++;
        }
    }
    kforge::util::log_info{}("3D SCAN: {} -> {} imported, {} already in DB", dir_str, imported,
                              skipped);
    return imported;
}

bool ImportPipeline::file_unchanged(const fs::path& p)
{
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec)
    {
        return false;  // unreadable mtime → treat as changed (re-import)
    }
    auto mtime_s = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::clock_cast<std::chrono::system_clock>(ft).time_since_epoch())
                       .count();
    std::lock_guard lock(mtimes_mtx_);
    auto it = file_mtimes_.find(p.string());
    return it != file_mtimes_.end() && it->second == mtime_s;
}

void ImportPipeline::record_file(const fs::path& p)
{
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec)
    {
        return;  // unreadable mtime — don't record; the file will be re-parsed next run
    }
    auto mtime_s = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::clock_cast<std::chrono::system_clock>(ft).time_since_epoch())
                       .count();
    {
        std::lock_guard lock(mtimes_mtx_);
        file_mtimes_[p.string()] = mtime_s;
    }
    storage::ImportedFilesRepository ifr(db_);
    auto _ = ifr.upsert(p.string(), mtime_s);
}

fs::path ImportPipeline::resolve_model_path(std::string_view ref, const fs::path& fp_dir) const
{
    // Shared implementation — see core/io/sexpr/path_util.h (binding manager
    // uses the same resolver when auto-linking models after a re-bind).
    return sexpr::resolve_model_path(ref, fp_dir, model_roots_);
}

}  // namespace kforge::services