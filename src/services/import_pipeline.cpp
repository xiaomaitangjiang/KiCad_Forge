#include "classifier/rule_engine.h"
#include "core/footprint.h"
#include "core/symbol.h"
#include "correspondence/checker.h"
#include "parser/footprint_parser.h"
#include "parser/symbol_lib_parser.h"
#include "services/import_pipeline.h"
#include "storage/database.h"
#include "storage/repositories.h"
#include "util/logger.h"
#include "util/result.h"

#include <algorithm>
#include <filesystem>
#include <future>
#include <sqlite3.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace kforge::services
{

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
ImportPipeline& ImportPipeline::operator|(with_3d_linking /*unused*/)
{
    do_3d_linking_ = true;
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

    // Phase 1: Import symbols
    for (auto& s : pending_symbols_)
    {
        auto stats = import_directory(s.dir, s.comp_lib_id);
        r.symbols += stats.symbols;
        r.footprints += stats.footprints;
    }

    // Phase 2: Import footprints (separate from symbols to handle .pretty)
    for (auto& f : pending_footprints_)
    {
        auto stats = import_directory(f.dir);
        r.footprints += stats.footprints;
    }

    // Phase 3: Scan 3D models
    for (auto& m : pending_models_)
    {
        int count = scan_3d_models(m.dir);
        r.models_3d += count;
    }

    // Phase 4: Link 3D models to footprints
    if (do_3d_linking_)
    {
        r.linked_models = do_link_3d_models();
    }

    return r;
}

// ============================================================
// Import implementations (moved from LibraryService)
// ============================================================

int ImportPipeline::import_symbol_library(const std::string& path_str, const std::string& comp_lib_id)
{
    fs::path path(path_str);
    auto result = parser::SymbolLibParser::parse(path);
    if (!result)
    {
        return 0;
    }

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
        if (!lib_ins)
        {
            LOG_ERROR("Cannot create library");
            return 0;
        }
        lib_id = lib_ins->id;
    }

    storage::SymbolRepository repo(db_);
    std::unordered_set<std::string> seen;
    {
        auto existing = repo.find_by_library(lib_id);
        if (existing)
        {
            for (auto& s : *existing)
            {
                seen.insert(s.name());
            }
        }
    }

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    int imported = 0;
    for (auto& sym : result->items)
    {
        if (seen.contains(sym.name()))
        {
            continue;
        }
        seen.insert(sym.name());
        if (sym.component_type == core::ComponentType::Unknown)
        {
            sym.component_type = classifier::guess_type_from_name(sym.name());
        }
        sym.set_library_id(lib_id);
        auto ins = repo.insert(sym);
        if (ins)
        {
            imported++;
            try_link_symbol_footprint(ins->id(), ins->footprint());
        }
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return imported;
}

IMP_Result ImportPipeline::import_footprint(const std::string& path_str)
{
    fs::path path(path_str);
    auto result = parser::FootprintParser::parse(path);
    if (!result)
    {
        return imp_failed(util::error_formatter(result.error()));
    }

    storage::FootprintRepository repo(db_);
    auto existing = repo.find_by_name(result->name());
    if (existing)
    {
        return imp_skipped();
    }

    auto ins = repo.insert(*result);
    if (ins)
    {
        LOG_INFO("FP: {} -> {}", path.filename().string(), ins->name());

        // Link explicit 3D models — lazy-build model filename hash pool
        if (model_name_index_.empty())
        {
            storage::Model3DRepository mr(db_);
            if (auto models = mr.find_all())
            {
                for (auto& m : *models)
                {
                    model_name_index_[m.file_path().stem().string()] = m.id();
                }
            }
        }
        storage::RelationshipRepository rr(db_);
        for (auto& m : ins->models_3d())
        {
            if (m.path.empty())
                continue;
            auto slash = m.path.find_last_of("/\\");
            auto stem = (slash != std::string::npos)
                          ? m.path.substr(slash + 1, m.path.find_last_of('.') - slash - 1)
                          : m.path.substr(0, m.path.find_last_of('.'));
            auto it = model_name_index_.find(stem);
            if (it != model_name_index_.end())
                auto _ = rr.link_footprint_to_model(ins->id(), it->second, "explicit");
        }
        return imp_added();
    }
    return imp_failed("Insert failed");
}

ImportPipeline::ImportStats ImportPipeline::import_directory(const std::string& dir_str,
                                                             const std::string& comp_lib_id)
{
    ImportStats stats;
    fs::path dir(dir_str);
    if (!fs::exists(dir))
    {
        return stats;
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

    // Parallel parse symbol files (pure CPU, no DB)
    if (!sym_files.empty())
    {
        unsigned n_threads = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::future<int>> futures;
        futures.reserve(sym_files.size());
        for (auto& f : sym_files)
        {
            futures.push_back(std::async(std::launch::async,
                                         [this, f, &comp_lib_id]
                                         {
                                             return import_symbol_library(f, comp_lib_id);
                                         }));
        }
        for (auto& fut : futures)
        {
            int n = fut.get();
            if (n > 0)
            {
                stats.symbols += n;
            }
            else
            {
                stats.errors++;
            }
        }
    }

    // Serial .pretty and standalone .kicad_mod
    for (auto& pd : pretty_dirs)
    {
        int fp_count = 0;
        std::error_code ec2;
        for (const auto& pf : fs::directory_iterator(pd, ec2))
        {
            if (ec2)
            {
                break;
            }
            if (pf.is_regular_file() && pf.path().extension() == ".kicad_mod")
            {
                auto r = import_footprint(pf.path().string());
                if (r.is(ImportStatus::Added))
                {
                    stats.footprints++;
                    fp_count++;
                }
                else if (r.is(ImportStatus::Failed))
                {
                    stats.errors++;
                }
            }
        }
        LOG_INFO("FP DIR: {} -> {} footprints", pd, fp_count);
    }
    for (auto& mf : mod_files)
    {
        auto r = import_footprint(mf);
        if (r.is(ImportStatus::Added))
        {
            stats.footprints++;
        }
        else if (r.is(ImportStatus::Failed))
        {
            stats.errors++;
        }
    }

    LOG_INFO("DIR SCAN: {} -> {} symbols, {} footprints, {} errors", dir_str, stats.symbols,
             stats.footprints, stats.errors);
    return stats;
}

int ImportPipeline::scan_3d_models(const std::string& dir_str)
{
    fs::path dir(dir_str);
    if (!fs::exists(dir))
    {
        return 0;
    }

    storage::Model3DRepository repo(db_);
    int imported = 0;
    int skipped = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        auto ext = entry.path().extension().string();
        std::ranges::transform(ext, ext.begin(),
                               [](unsigned char c)
                               {
                                   return std::tolower(c);
                               });
        std::string format;
        if (ext == ".step" || ext == ".stp")
        {
            format = "step";
        }
        else if (ext == ".wrl")
        {
            format = "wrl";
        }
        else if (ext == ".iges" || ext == ".igs")
        {
            format = "iges";
        }
        else
        {
            continue;
        }
        auto existing = repo.find_by_path(entry.path().string());
        if (existing)
        {
            skipped++;
            continue;
        }
        core::Model3D m;
        m.set_file_path(entry.path());
        m.set_format(format);
        m.set_description(entry.path().stem().string());
        m.set_source("filesystem");
        auto ins = repo.insert(m);
        if (ins)
        {
            imported++;
        }
    }
    LOG_INFO("3D SCAN: {} -> {} imported, {} already in DB", dir_str, imported, skipped);
    return imported;
}

void ImportPipeline::try_link_symbol_footprint(const std::string& sym_id, const std::string& fp_ref)
{
    if (fp_ref.empty())
    {
        return;
    }
    // Strip library prefix: "Package_SO:SOIC-8" → "SOIC-8"
    auto colon = fp_ref.find(':');
    std::string short_name = (colon != std::string::npos) ? fp_ref.substr(colon + 1) : fp_ref;

    storage::FootprintRepository fr(db_);
    storage::RelationshipRepository rr(db_);
    auto fp = fr.find_by_name(short_name);
    if (!fp)
    {
        fp = fr.find_by_name(fp_ref);
    }
    if (fp)
    {
        auto _ = rr.link_symbol_to_footprint(sym_id, fp->id(), "imported", 1.0);
    }
}

int ImportPipeline::do_link_3d_models()
{
    storage::SymbolRepository sr(db_);
    storage::FootprintRepository fr(db_);
    storage::Model3DRepository mr(db_);
    storage::RelationshipRepository rr(db_);
    auto syms = sr.find_all();
    auto models = mr.find_all();
    if (!syms || !models)
    {
        return 0;
    }

    // Build model stem → id index (O(N))
    std::unordered_map<std::string, core::Uuid> model_index;
    for (auto& m : *models)
    {
        std::string stem = m.file_path().stem().string();
        if (!model_index.contains(stem))
        {
            model_index[stem] = m.id();
        }
    }

    int linked = 0;
    int skipped = 0;
    int count = 0;
    for (auto& sym : *syms)
    {
        if ((cancel_ != nullptr) && cancel_->load(std::memory_order_relaxed))
        {
            break;
        }
        if (sym.footprint().empty())
        {
            skipped++;
            continue;
        }
        auto colon = sym.footprint().find(':');
        std::string short_fp =
            (colon != std::string::npos) ? sym.footprint().substr(colon + 1) : sym.footprint();
        // Exact match
        core::Uuid model_id;
        auto it = model_index.find(short_fp);
        if (it != model_index.end())
        {
            model_id = it->second;
        }
        else
        {
            // Substring match
            for (auto& [stem, id] : model_index)
            {
                if (stem.contains(short_fp) || short_fp.contains(stem))
                {
                    model_id = id;
                    break;
                }
            }
        }
        if (model_id.empty())
        {
            continue;
        }

        core::Uuid fp_id;
        auto existing_fp = rr.find_footprint_for_symbol(sym.id());
        if (existing_fp && *existing_fp)
        {
            fp_id = **existing_fp;
        }
        else
        {
            auto existing = fr.find_by_name(short_fp);
            if (existing)
            {
                fp_id = existing->id();
            }
            else
            {
                core::Footprint fp;
                fp.set_name(short_fp);
                fp.set_description("Auto-created from symbol footprint_ref");
                auto ins = fr.insert(fp);
                if (!ins)
                {
                    continue;
                }
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
