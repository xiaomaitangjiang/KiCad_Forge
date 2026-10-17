#include "classifier/rule_engine.h"
#include "core/model/footprint.h"
#include "core/model/symbol.h"
#include "correspondence/checker.h"
#include "core/io/parser/footprint_parser.h"
#include "core/io/parser/symbol_lib_parser.h"
#include "core/io/sexpr/path_util.h"
#include "core/io/sexpr/text_util.h"
#include "core/io/source_mutation.h"
#include "core/model/type_registry.h"
#include "interface/service/import/pipeline.h"
#include "interface/service/library/source_policy.h"
#include "core/db/database.h"
#include "core/repo/repositories.h"
#include "util/logger.h"
#include "util/error.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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
        auto session = db_->session();
        storage::ImportedFilesRepository ifr(session);
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
        auto session = db_->session();
        storage::FootprintRepository fr(session);
        if (auto fps = fr.find_all())
        {
            for (auto& f : *fps)
            {
                auto key = f.source_key();
                if (key.empty())
                    key = storage::existing_source_key(f.library_path());
                if (key.empty())
                    continue;  // 无来源的旧行不参与按来源刷新
                fp_by_source_.emplace(key, std::move(f));
            }
        }
        storage::Model3DRepository mr(session);
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
        // 引用解析：先按 ".pretty 库名/封装名" 精确定位；
        // 无库名前缀时只在全库唯一的情况下自动链接 —— 歧义留给人工确认
        std::unordered_map<std::string, core::Uuid> fp_id_by_ref;
        std::unordered_map<std::string, core::Uuid> fp_id_by_unique_name;
        std::unordered_map<std::string, int> name_uses;
        fp_id_by_ref.reserve(fp_by_source_.size());
        for (auto& [key, fp] : fp_by_source_)
        {
            fp_id_by_ref[fp.footprint_lib_name() + "/" + fp.name()] = fp.id();
            name_uses[fp.name()]++;
            fp_id_by_unique_name[fp.name()] = fp.id();
        }
        for (auto& [name, uses] : name_uses)
        {
            if (uses > 1)
                fp_id_by_unique_name.erase(name);
        }

        auto session = db_->session();
        storage::RelationshipRepository rr(session);
        std::unordered_map<std::string, std::string> existing_links;
        if (auto links = rr.find_all_symbol_links())
        {
            existing_links = std::move(*links);
        }

        storage::SymbolRepository sym_repo(session);
        if (auto syms = sym_repo.find_all())
        {
            if (auto begun = session.begin(); !begun)
            {
                kforge::util::log_error{}("Import link phase: {}", begun.error().format_message());
                return r;
            }
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
                // "Package_SO:SOIC-8" → 库名 Package_SO、封装名 SOIC-8
                const auto& reference = sym.footprint();
                auto lib_name = storage::footprint_lib_name_of(reference);
                std::string short_name = lib_name.empty()
                                             ? reference
                                             : reference.substr(lib_name.size() + 1);
                core::Uuid target;
                if (!lib_name.empty())
                {
                    if (auto it = fp_id_by_ref.find(lib_name + "/" + short_name);
                        it != fp_id_by_ref.end())
                    {
                        target = it->second;
                    }
                }
                else if (auto it = fp_id_by_unique_name.find(short_name);
                         it != fp_id_by_unique_name.end())
                {
                    target = it->second;
                }
                if (!target.empty())
                {
                    auto _ = rr.link_symbol_to_footprint(sym.id(), target, "imported", 1.0);
                }
            }
            if (auto committed = session.commit(); !committed)
            {
                kforge::util::log_error{}("Import link phase: {}",
                                          committed.error().format_message());
            }
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

    int imported = 0;
    bool locked = false;
    {
        auto session = db_->session();
        storage::LibraryRepository lr(session);
        std::string lib_id = lib.lib_name;
        {
            core::LibraryMeta meta;
            meta.name = lib_id;
            meta.file_path = path;
            meta.component_library_id = comp_lib_id;
            meta.description = "Imported from " + path.filename().string();
            // 身份是来源路径：同一文件重复导入保留原 UUID，
            // 同名不同来源则各自成行
            auto lib_ins = lr.upsert_by_source_key(meta);
            if (!lib_ins)
            {
                kforge::util::log_error{}("Cannot register library {}: {}", path.string(),
                                          lib_ins.error().format_message());
                return imp_failed(lib_ins.error().format_message());
            }
            lib_id = lib_ins->id;
        }

        storage::SymbolRepository repo(session);
        std::unordered_set<std::string> seen;
        // 库里已有行：更新时只写文件里有依据的字段，库内元数据（类型 ID 等）
        // 从旧行带回 —— 文件里根本没有这些信息，覆盖就等于丢数据
        struct Existing
        {
            std::string id;
            std::string component_type_id;
            std::string package_type_id;
        };
        std::unordered_map<std::string, Existing> existing_by_name;
        {
            auto existing = repo.find_by_library(lib_id);
            if (existing)
            {
                for (auto& s : *existing)
                {
                    seen.insert(s.name());
                    existing_by_name[s.name()] =
                        Existing{s.id(), s.component_type_id, s.package_type_id};
                }
            }
        }

        if (auto begun = session.begin(); !begun)
        {
            return imp_failed(begun.error().format_message());
        }
        for (auto& sym : lib.items)
        {
            sym.set_library_id(lib_id);
            if (sym.Kicad_Forge_ID().empty())
                sym.set_Kicad_Forge_ID(core::Symbol::compute_hash(sym));

            if (seen.contains(sym.name()))
            {
                // File changed since last import — refresh the existing row.
                // 文件里没有类型信息：库内已有的分类结果（含认不出的 ID）原样带回
                const auto& previous = existing_by_name[sym.name()];
                sym.set_id(previous.id);
                if (sym.component_type_id.empty())
                    sym.component_type_id = previous.component_type_id;
                if (sym.package_type_id.empty())
                    sym.package_type_id = previous.package_type_id;
                if (repo.update(sym))
                {
                    imported++;
                }
                continue;
            }
            seen.insert(sym.name());
            // 新行才按名字猜一个内置类型；已有行上面的合并已经保证不覆盖
            if (sym.component_type_id.empty())
            {
                sym.component_type_id =
                    core::builtin_component_id(classifier::guess_type_from_name(sym.name()));
            }
            auto ins = repo.insert(sym);
            if (ins)
            {
                imported++;  // linking happens in Phase 4
            }
        }
        if (auto committed = session.commit(); !committed)
        {
            return imp_failed(committed.error().format_message());
        }

        // 开关只允许未锁定用户库写回；标准库始终只读。
        if (auto locked_result = source_is_locked(session, path, comp_lib_id); locked_result)
        {
            locked = *locked_result;
        }
        else
        {
            kforge::util::log_error{}("Lock check failed: {}",
                                      locked_result.error().format_message());
            return imp_failed(locked_result.error().format_message());
        }

        // 快照与符号行同一事务记账（A04）
        record_file(session, path);
    }

    if (write_kf_id_to_file_ && !locked && std::filesystem::exists(path))
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
            // 读 + 改在锁外算好，落盘交给统一变更服务（备份 + 恢复日志）
            std::string original;
            {
                std::ifstream in(path, std::ios::binary);
                original.assign(std::istreambuf_iterator<char>(in), {});
            }
            std::string patched = original;
            int changed = sexpr::patch_kf_id_content(patched, name_to_kf);
            if (changed > 0)
            {
                io::SourceMutationService mutations(*db_);
                auto written = mutations.apply(
                    {io::FileChange{.path = path,
                                    .content = std::move(patched),
                                    .expected_digest = io::content_digest(original)}},
                    [](storage::DbSession&) -> util::Result<void> { return {}; });
                if (written)
                {
                    record_file(path);  // 文件已改写，刷新快照避免下次重复解析
                }
                else
                {
                    kforge::util::log_error{}("KF ID write failed for {}: {}", path.string(),
                                              written.error().format_message());
                }
            }
        }
    }

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
    auto session = db_->session();
    if (auto begun = session.begin(); !begun)
    {
        return imp_failed(begun.error().format_message());
    }
    auto result = write_footprint(session, path, *fp);
    if (auto committed = session.commit(); !committed)
    {
        return imp_failed(committed.error().format_message());
    }
    return result;
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

IMP_Result ImportPipeline::write_footprint(const storage::DbSession& session, const fs::path& path,
                                           const core::Footprint& fp_in)
{
    core::Footprint fp = fp_in;  // mutable copy for id assignment
    fp.set_library_path(path.string());
    // 来源身份：规范路径 + .pretty 库名（同名不同目录靠它区分）
    fp.set_source_key(storage::existing_source_key(path));
    if (fp.footprint_lib_name().empty() && path.parent_path().extension() == ".pretty")
    {
        fp.set_footprint_lib_name(path.parent_path().stem().string());
    }
    storage::FootprintRepository repo(session);
    std::string fp_id;
    auto it = fp.source_key().empty() ? fp_by_source_.end() : fp_by_source_.find(fp.source_key());
    if (it != fp_by_source_.end())
    {
        // File changed since last import — refresh the existing row
        fp.set_id(it->second.id());
        // 文件里没有类型信息：旧行的分类结果原样带回
        if (fp.package_type_id.empty())
            fp.package_type_id = it->second.package_type_id;
        if (!repo.update(fp))
        {
            return imp_failed("Update failed");
        }
        fp_id = it->second.id();

        // Clear old model links — the refs may have changed
        storage::RelationshipRepository rr(session);
        if (auto olds = rr.find_models_for_footprint(fp_id))
        {
            for (auto& mid : *olds)
            {
                auto _ = rr.unlink_footprint_model(fp_id, mid);
            }
        }
        it->second = fp;  // model references are still needed below
    }
    else
    {
        auto ins = repo.insert(fp);
        if (!ins)
        {
            return imp_failed("Insert failed");
        }
        fp_id = ins->id();
        if (!ins->source_key().empty())
        {
            fp_by_source_.emplace(ins->source_key(), std::move(*ins));
        }
    }
    kforge::util::log_info{}("FP: {} -> {}", path.filename().string(), fp.name());

    // Link 3D models: resolve explicit reference paths and import the file
    // if it is not in the DB yet — no stem fallback (user requirement)
    int model_refs = 0;
    int model_linked = 0;
    storage::RelationshipRepository rr(session);
    storage::Model3DRepository mrepo(session);
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
                record_file(session, resolved);  // snapshot so the scan phase skips it
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
    record_file(session, path);
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

        counts.skipped += static_cast<int>(sym_files.size() - parsed_libs.size());
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
            auto session = db_->session();
            auto begun = session.begin();
            if (!begun)
            {
                kforge::util::log_error{}("FP import: {}", begun.error().format_message());
                skipped += static_cast<int>(pending.size());
                return std::pair<int, int>{added, skipped};
            }
            for (auto& p : pending)
            {
                if (cancelled())
                {
                    break;
                }
                if (write_footprint(session, p.path, p.fp).is(ImportStatus::Added))
                {
                    added++;
                }
                else
                {
                    skipped++;  // write failure
                }
            }
            if (auto committed = session.commit(); !committed)
            {
                kforge::util::log_error{}("FP import: {}", committed.error().format_message());
            }
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

    // 走目录期间不持数据库锁：只有确实要写库的那几个文件才开 session
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
            auto session = db_->session();
            storage::Model3DRepository repo(session);
            auto _ = repo.update(it->second);
            record_file(session, store_path);
            imported++;
            continue;
        }
        core::Model3D m;
        m.set_file_path(store_path);
        m.set_format(format);
        m.set_description(entry.path().stem().string());
        m.set_source("filesystem");
        auto session = db_->session();
        storage::Model3DRepository repo(session);
        auto ins = repo.insert(m);
        if (ins)
        {
            record_file(session, store_path);
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
                       std::chrono::file_clock::to_sys(ft).time_since_epoch())
                       .count();
    std::lock_guard lock(mtimes_mtx_);
    auto it = file_mtimes_.find(p.string());
    return it != file_mtimes_.end() && it->second == mtime_s;
}

void ImportPipeline::record_file(const fs::path& p)
{
    auto session = db_->session();
    record_file(session, p);
}

void ImportPipeline::record_file(const storage::DbSession& session, const fs::path& p)
{
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec)
    {
        return;  // unreadable mtime — don't record; the file will be re-parsed next run
    }
    auto mtime_s = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::file_clock::to_sys(ft).time_since_epoch())
                       .count();
    {
        std::lock_guard lock(mtimes_mtx_);
        file_mtimes_[p.string()] = mtime_s;
    }
    storage::ImportedFilesRepository ifr(session);
    auto _ = ifr.upsert(p.string(), mtime_s);
}

fs::path ImportPipeline::resolve_model_path(std::string_view ref, const fs::path& fp_dir) const
{
    // Shared implementation — see core/io/sexpr/path_util.h (binding manager
    // uses the same resolver when auto-linking models after a re-bind).
    return sexpr::resolve_model_path(ref, fp_dir, model_roots_);
}

}  // namespace kforge::services
