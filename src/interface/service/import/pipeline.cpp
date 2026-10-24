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
#include "interface/service/import/source_path.h"
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
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace kforge::services
{

namespace
{
/// 已有类型身份（内置 / custom / 未注册历史 ID）一律保护：只有 SQL NULL/空串
/// 与字面 "Unknown" 属于「还没有类型」，才允许被配置候选补齐。
bool is_known_type(const std::string& raw_type_id)
{
    return !raw_type_id.empty()
           && raw_type_id != core::builtin_component_id(core::ComponentType::Unknown);
}

std::string unknown_type_id()
{
    return core::builtin_component_id(core::ComponentType::Unknown);
}

/// 一次导入操作里允许保留的告警/错误明细条数（计数不受影响，不停止扫描）
void add_message(std::vector<std::string>& messages, std::string message)
{
    if (messages.size() < kMaxReportMessages)
        messages.push_back(std::move(message));
}

/// 错误/告警统一通道：完整计数永远在明细上限之外先加，再有限保存文本。
/// 任何错误都必须经这里记账，避免「只 push 了一条消息但计数没跟上」。
void report_error(ImportPipeline::ImportCounts& counts, std::string message)
{
    counts.error_count++;
    add_message(counts.errors, std::move(message));
}

void report_warning(ImportPipeline::ImportCounts& counts, std::string message)
{
    counts.warning_count++;
    add_message(counts.warnings, std::move(message));
}

void report_error(ImportPipeline::Result& result, std::string message)
{
    result.error_count++;
    add_message(result.errors, std::move(message));
}

/// 事务控制语句（SAVEPOINT / RELEASE / ROLLBACK TO）：失败必须让调用方终止整批，
/// 不能继续在状态不明的连接上假提交。直接走 SQLite 官方语句，不自建事务框架。
util::Result<void> exec_control(sqlite3* handle, const char* sql)
{
    if (sqlite3_exec(handle, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        return util::Result<void>(std::unexpected(util::Error::make<util::Error::Kind::DbError>(
            std::string(sql) + ": " + (handle != nullptr ? sqlite3_errmsg(handle)
                                                         : "no connection"))));
    return {};
}

/// 未匹配以外的「作废/被保护」决策要能解释：状态 + 规则 reason + 来源定位。
std::string decision_message(const std::string& where, const core::Symbol& symbol,
                             const classifier::Decision& decision)
{
    std::string message = where + " '" + symbol.name() + "': "
                          + classifier::match_status_name(decision.status);
    if (!decision.reason.empty())
        message += " (" + decision.reason + ")";
    return message;
}

/// 把一个符号条目的决策计入计数；返回是否属于「要写回」的候选。
void count_decision(ImportPipeline::ImportCounts& counts, const core::Symbol& symbol,
                    const std::string& where, const classifier::Decision& decision)
{
    using classifier::MatchStatus;
    switch (decision.status)
    {
        case MatchStatus::Matched: counts.matched++; break;
        case MatchStatus::Unmatched: counts.unmatched++; break;
        case MatchStatus::Conflict:
            counts.conflicts++;
            report_warning(counts, decision_message(where, symbol, decision));
            break;
        case MatchStatus::InputTooLong:
            counts.input_too_long++;
            report_warning(counts, decision_message(where, symbol, decision));
            break;
        case MatchStatus::EvaluationLimit:
            counts.evaluation_limited++;
            report_warning(counts, decision_message(where, symbol, decision));
            break;
        case MatchStatus::EvaluationError:
            counts.evaluation_errors++;
            report_warning(counts, decision_message(where, symbol, decision));
            break;
    }
}

/// 新行 / 旧行的类型合并：只有「真已知身份」被保护 —— 非空且不是字面 "Unknown"
/// （内置、custom、认不出的历史 raw ID 都算已知）。还没有类型（NULL/空串/Unknown）
/// 的旧行在 core 给出 Matched 候选时才补齐；其余状态（unmatched/conflict/limit/
/// error）保留旧原值。新行未匹配才落 "Unknown" 占位。
std::string merge_component_type(const std::string& previous_type_id,
                                 const classifier::Decision& decision, bool new_row)
{
    if (is_known_type(previous_type_id)) return previous_type_id;
    if (decision.matched() && !decision.type_id.empty()) return decision.type_id;
    if (previous_type_id.empty() && new_row) return unknown_type_id();
    return previous_type_id;   // 旧行的空/Unknown 原样保留，不规范化成别的值
}

/// 把一条来源的决策计数合并进聚合计数（明细上限同样适用，计数先加）
void merge_counts(ImportPipeline::ImportCounts& target, const ImportPipeline::ImportCounts& from)
{
    target.symbols += from.symbols;
    target.footprints += from.footprints;
    target.skipped += from.skipped;
    target.failed_files += from.failed_files;
    target.error_count += from.error_count;
    target.warning_count += from.warning_count;
    target.matched += from.matched;
    target.unmatched += from.unmatched;
    target.conflicts += from.conflicts;
    target.input_too_long += from.input_too_long;
    target.evaluation_limited += from.evaluation_limited;
    target.evaluation_errors += from.evaluation_errors;
    for (const auto& message : from.errors)
        add_message(target.errors, message);
    for (const auto& message : from.warnings)
        add_message(target.warnings, message);
}

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
    report_progress("scanning", 0, 0);
    try
    {
        auto result = execute_steps();
        report_progress(cancelled() ? "cancelled" : result.ok() ? "done" : "failed", 0, 0);
        return result;
    }
    catch (...)
    {
        report_progress("failed", 0, 0);
        throw;
    }
}

void ImportPipeline::report_progress(const std::string& phase, int completed, int total) const
{
    if (progress_fn_.fn)
        progress_fn_.fn(phase, completed, total);
}

ImportPipeline::Result ImportPipeline::execute_steps()
{
    Result r;

    // 取消的空操作保持原空返回：不领取快照、不消费任何来源
    if (cancelled())
    {
        return r;
    }

    // 配置错误先于任何快照读取/库行写入拒绝；不能把普通文件当空目录后
    // 继续重算整个数据库的关系。符号路径允许精确指向一个 .kicad_sym。
    for (const auto& source : pending_symbols_)
    {
        auto valid = validate_library_source_path(source.dir, LibrarySourceKind::Symbols);
        if (!valid) report_error(r, valid.error().format_message());
    }
    for (const auto& source : pending_footprints_)
    {
        auto valid = validate_library_source_path(source.dir, LibrarySourceKind::Footprints);
        if (!valid) report_error(r, valid.error().format_message());
    }
    for (const auto& source : pending_models_)
    {
        auto valid = validate_library_source_path(source.dir, LibrarySourceKind::Models);
        if (!valid) report_error(r, valid.error().format_message());
    }
    if (!r.ok()) return r;

    // 健康门：一次 execute 只领取一次已保存配置的业务快照，失败时在任何写入
    // （库行 / 符号 / 封装 / imported_files / 源文件改写）之前明确报错。
    // 只有可能读到符号的步骤（symbols_from / footprints_from —— 封装目录里同样
    // 可能有 .kicad_sym）才依赖元件类型配置；纯 models_from 不做类型决策，
    // 坏 / 未 load 的 store 不能拦住它。
    std::shared_ptr<const classifier::MatchingSnapshot> snapshot;
    if (!pending_symbols_.empty() || !pending_footprints_.empty())
    {
        auto acquired = matching_store_->acquire_business_snapshot();
        if (!acquired)
        {
            report_error(r, "import blocked: matching config is not healthy: "
                                + acquired.error().format_message());
            return r;
        }
        // lease 持有到所有阶段（含最终 commit/rollback）结束才释放
        snapshot = std::move(*acquired);
    }

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
            // 读不到快照就不是「没有已导入文件」：增量判断会失真，必须先报错
            report_error(r, "cannot read imported_files snapshots: "
                                + snapshots.error().format_message());
            return r;
        }
    }

    // 3D model roots for resolving ${KICADx_3DMODEL_DIR} references
    model_roots_.clear();
    for (auto& m : pending_models_)
    {
        model_roots_.push_back(m.dir);
    }
    model_ref_imported_ = 0;

    /// 一条来源的局部计数并进聚合结果：完整计数先并入，明细只保留前 100 条
    auto absorb_counts = [&r](const ImportCounts& info)
    {
        r.symbols += info.symbols;
        r.footprints += info.footprints;
        r.skipped += info.skipped;
        r.failed_files += info.failed_files;
        r.error_count += info.error_count;
        r.warning_count += info.warning_count;
        r.matched += info.matched;
        r.unmatched += info.unmatched;
        r.conflicts += info.conflicts;
        r.input_too_long += info.input_too_long;
        r.evaluation_limited += info.evaluation_limited;
        r.evaluation_errors += info.evaluation_errors;
        for (const auto& message : info.errors)
            add_message(r.errors, message);
        for (const auto& message : info.warnings)
            add_message(r.warnings, message);
    };
    auto absorb = [&absorb_counts](const ImportDirResult& sr) { absorb_counts(sr.info); };

    // Phase 1: Import symbols
    for (auto& s : pending_symbols_)
    {
        if (cancelled())
        {
            break;
        }
        absorb(import_directory(s.dir, s.comp_lib_id, snapshot.get()));
    }

    // In-memory pools loaded once — replace per-file find_by_name / find_by_path
    // SQLite queries during the footprint phase
    {
        const auto pool_wait_started = std::chrono::steady_clock::now();
        auto session = db_->session();
        const auto pool_wait = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pool_wait_started);
        kforge::util::log_debug{}("import: footprint pool session acquired after {} ms",
                                  pool_wait.count());
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
        // 该目录同样可能含 .kicad_sym：符号计数与错误一个都不能丢
        absorb(import_directory(f.dir, "", snapshot.get()));
    }

    // Phase 3: Scan 3D model directories AFTER footprints — the in-memory
    // hash pool (models_by_path_) now contains the ref-imported models and skips them
    for (auto& m : pending_models_)
    {
        if (cancelled())
        {
            break;
        }
        // 每个模型文件一个真实事务：insert/update 与快照一起提交才算成功；
        // 失败只影响那一条，错误与失败文件数照实并入报告
        auto scan = scan_3d_models(m.dir);
        r.models_3d += scan.imported;
        // 旧口径：Result.skipped 只数符号/封装；模型 scan 的 skipped 是私有
        // 计数，只进 scan 内部日志，不得透传污染旧计数（U04-B1 回归守护）
        absorb_counts(scan.counts);
    }

    // Models imported from explicit footprint refs during Phase 2
    r.models_3d += model_ref_imported_;

    // Phase 4: 关联重算 —— 源文件里的 Footprint 属性是唯一依据。
    // 候选按「来源库/封装名」与「裸封装名」两种键聚合，多行不能互相覆盖；
    // 只有唯一候选才自动关联，歧义/缺失一律不猜。
    //
    // 每次未取消的 execute 都要重新核对：上一轮若在写关系时失败，文件快照
    // 可能已经被消费（文件与 mtime 都没变），按「本轮是否导入过文件」来决定
    // 是否核对会让这类失败永远补不回来。核对本身是幂等的 —— 依据是库里
    // 已提交的符号引用与已加载的封装池，不会因为重复执行而漂移。
    if (!cancelled())
    {
        // 关联处理的是数据库行，没有可对应的来源文件总数，只发布阶段。
        report_progress("linking", 0, 0);
        std::unordered_map<std::string, std::vector<core::Uuid>> fp_ids_by_ref;
        std::unordered_map<std::string, std::vector<core::Uuid>> fp_ids_by_name;
        for (auto& [key, fp] : fp_by_source_)
        {
            fp_ids_by_ref[fp.footprint_lib_name() + "/" + fp.name()].push_back(fp.id());
            fp_ids_by_name[fp.name()].push_back(fp.id());
        }

        auto candidates_for = [&](const std::string& reference)
        {
            std::vector<core::Uuid> candidates;
            auto append = [&candidates](const std::vector<core::Uuid>& source)
            {
                for (const auto& id : source)
                {
                    if (std::find(candidates.begin(), candidates.end(), id) == candidates.end())
                        candidates.push_back(id);
                }
            };
            auto lib_name = storage::footprint_lib_name_of(reference);
            if (!lib_name.empty())
            {
                // 带库前缀的引用只在指定库里找；找不到就是不解析，
                // 不能退化成裸名字去别的库（或同名库）里猜一个
                std::string short_name = reference.substr(lib_name.size() + 1);
                if (auto it = fp_ids_by_ref.find(lib_name + "/" + short_name);
                    it != fp_ids_by_ref.end())
                {
                    append(it->second);
                }
            }
            else if (auto it = fp_ids_by_name.find(reference); it != fp_ids_by_name.end())
            {
                // 引用本来就没有库前缀，才按裸封装名找
                append(it->second);
            }
            return candidates;
        };

        auto session = db_->session();
        storage::RelationshipRepository rr(session);

        // 关系逐行读取：同一符号的多条关联不会被单值 map 覆盖
        std::unordered_map<std::string,
                           std::vector<storage::RelationshipRepository::SymbolLinkRow>>
            links_by_symbol;
        if (auto rows = rr.find_all_symbol_link_rows())
        {
            for (auto& row : *rows)
                links_by_symbol[row.symbol_id].push_back(std::move(row));
        }
        else
        {
            // 读不全现有关系就等于不知道哪些关联是人工绑的：拿空集合继续
            // 会把人工关系当成不存在而另加一条，必须原地停下
            kforge::util::log_error{}("Import link phase: {}",
                                      rows.error().format_message());
            report_error(r, "link phase: " + rows.error().format_message());
            return r;
        }

        struct PendingLinks
        {
            std::string symbol_id;
            std::vector<std::string> remove_ids;
            std::string add_id;
        };
        std::vector<PendingLinks> pending;

        storage::SymbolRepository sym_repo(session);
        auto syms = sym_repo.find_footprint_references();
        if (!syms)
        {
            kforge::util::log_error{}("Import link phase: {}", syms.error().format_message());
            report_error(r, "link phase: " + syms.error().format_message());
            return r;  // 符号读不全同样不能拿半截清单重算关系
        }
        else
        {
            for (auto& sym : *syms)
            {
                const std::string& reference = sym.footprint_ref;
                std::vector<core::Uuid> candidates =
                    reference.empty() ? std::vector<core::Uuid>{} : candidates_for(reference);
                // 空 / 不存在 / 歧义 → 不自动关联
                std::string resolved =
                    candidates.size() == 1 ? candidates.front() : std::string();

                PendingLinks item;
                item.symbol_id = sym.id;
                bool resolved_kept = false;
                if (auto it = links_by_symbol.find(sym.id); it != links_by_symbol.end())
                {
                    for (const auto& link : it->second)
                    {
                        bool keep = false;
                        if (link.link_type != "imported")
                        {
                            // 人工/插件关系：仍对应当前引用就保留精确 UUID，
                            // 即使同名候选不止一个。
                            keep = std::find(candidates.begin(), candidates.end(),
                                             link.footprint_id) != candidates.end();
                        }
                        else
                        {
                            // 正确的自动关系已经是最终状态：保留行身份与时间戳，
                            // 只修正目标、歧义或置信度确实变化的关系。
                            keep = !resolved.empty() && link.footprint_id == resolved
                                   && link.confidence == 1.0;
                        }
                        if (keep)
                        {
                            if (link.footprint_id == resolved)
                                resolved_kept = true;
                        }
                        else
                        {
                            item.remove_ids.push_back(link.footprint_id);
                        }
                    }
                }
                if (!resolved.empty() && !resolved_kept)
                    item.add_id = resolved;
                if (!item.remove_ids.empty() || !item.add_id.empty())
                    pending.push_back(std::move(item));
            }
        }

        if (!pending.empty() && !cancelled())
        {
            if (auto begun = session.begin(); !begun)
            {
                // 事务根本开不起来 = 这一轮一条关系都重算不了：必须是可见的错误，
                // 不能只写日志后返回一个「看起来成功」的结果（后台会误报 done）
                kforge::util::log_error{}("Import link phase: {}", begun.error().format_message());
                report_error(r, "link phase: " + begun.error().format_message());
                return r;
            }
            bool failed = false;
            for (auto& item : pending)
            {
                for (const auto& fp_id : item.remove_ids)
                {
                    if (auto unlinked = rr.unlink_symbol_footprint(item.symbol_id, fp_id);
                        !unlinked)
                    {
                        kforge::util::log_error{}("Import link phase: {}",
                                                  unlinked.error().format_message());
                        report_error(r, "link phase: " + unlinked.error().format_message());
                        failed = true;
                        break;
                    }
                }
                if (failed)
                    break;
                if (!item.add_id.empty())
                {
                    if (auto linked =
                            rr.link_symbol_to_footprint(item.symbol_id, item.add_id,
                                                        "imported", 1.0);
                        !linked)
                    {
                        kforge::util::log_error{}("Import link phase: {}",
                                                  linked.error().format_message());
                        report_error(r, "link phase: " + linked.error().format_message());
                        failed = true;
                        break;
                    }
                }
            }
            if (failed)
            {
                (void) session.rollback();  // 失败保留原关系，不提交半截
            }
            else if (cancelled())
            {
                (void) session.rollback();  // 取消同样不能提交半截关系
                kforge::util::log_warn{}("Import link phase: cancelled, links left unchanged");
            }
            else if (auto committed = session.commit(); !committed)
            {
                kforge::util::log_error{}("Import link phase: {}",
                                          committed.error().format_message());
                report_error(r, "link phase commit: " + committed.error().format_message());
            }
        }
    }

    return r;
}

// ============================================================
// Import implementations 
// ============================================================

IMP_Result ImportPipeline::import_symbol_library(const std::string& path_str,
                                                 const std::string& comp_lib_id,
                                                 const classifier::MatchingSnapshot* snapshot,
                                                 ImportCounts& counts)
{
    auto lib = parse_symbol_library(path_str);
    if (!lib) return imp_failed(lib.error().format_message());
    if (!*lib) return imp_skipped();
    return write_symbol_library(**lib, comp_lib_id, snapshot, counts);
}

util::Result<std::optional<ImportPipeline::ParsedSymbolLib>> ImportPipeline::parse_symbol_library(
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
        return std::unexpected(result.error());  // not recorded — retried next run
    }

    ParsedSymbolLib lib;
    lib.path = path;
    lib.lib_name = result->name.empty() ? path.stem().string() : result->name;
    lib.items = std::move(result->items);
    return std::optional<ParsedSymbolLib>(std::move(lib));
}

IMP_Result ImportPipeline::write_symbol_library(ParsedSymbolLib& lib,
                                                const std::string& comp_lib_id,
                                                const classifier::MatchingSnapshot* snapshot,
                                                ImportCounts& counts)
{
    const fs::path& path = lib.path;
    kforge::util::log_info{}("IMPORT: {} -> {} items", path.string(), lib.items.size());

    // 决策在取得 DbSession 之前算好：匹配在不可变快照上做，各自独立 scratch/预算，
    // 不持 DBsession、不持配置锁；导入写批里只做「合并 + 写入」。
    // （并行解析阶段通常已经算过，这里只是补齐直接调用这条路径。）
    if (snapshot != nullptr && lib.decisions.size() != lib.items.size())
    {
        lib.decisions.clear();
        lib.decisions.reserve(lib.items.size());
        for (const auto& item : lib.items)
            lib.decisions.push_back(
                snapshot->evaluate(classifier::MatchSample::from_symbol(item)));
    }

    // 决策计数先攒在局部，只有整库事务真的提交成功才对外发布
    ImportCounts decided;
    int imported = 0;
    bool locked = false;
    std::optional<int64_t> snapshot_mtime;
    {
        auto session = db_->session();
        // 一个符号库的完整导入 = 一个事务：库行、每条符号行、源锁检查与
        // mtime 快照要么一起提交，要么由 DbSession 析构整体回滚
        if (auto begun = session.begin(); !begun)
        {
            return imp_failed(begun.error().format_message());
        }
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
            std::string kf_id;
            std::string pre_kf_id;
        };
        std::unordered_map<std::string, Existing> existing_by_name;
        {
            auto existing = repo.find_by_library(lib_id);
            if (!existing)
            {
                kforge::util::log_error{}("Cannot read library {}: {}", lib_id,
                                          existing.error().format_message());
                return imp_failed(existing.error().format_message());
            }
            for (auto& s : *existing)
            {
                seen.insert(s.name());
                existing_by_name[s.name()] = Existing{s.id(), s.component_type_id,
                                                     s.package_type_id,
                                                     s.Kicad_Forge_ID(),
                                                     s.Pre_Kicad_Forge_ID()};
            }
        }

        for (std::size_t item_index = 0; item_index < lib.items.size(); ++item_index)
        {
            auto& sym = lib.items[item_index];
            // 决策来自最终 parsed source 的四个真实字段；没有决策时按 Unmatched 处理
            const classifier::Decision no_decision;
            const classifier::Decision& decision =
                item_index < lib.decisions.size() ? lib.decisions[item_index] : no_decision;
            count_decision(decided, sym, path.filename().string(), decision);

            sym.set_library_id(lib_id);
            if (sym.Kicad_Forge_ID().empty())
                sym.set_Kicad_Forge_ID(core::Symbol::compute_hash(sym));

            if (seen.contains(sym.name()))
            {
                // File changed since last import — refresh the existing row.
                // 文件里没有类型信息：库内已有的分类结果（含认不出的 ID）原样带回；
                // 只有原本就没有类型（NULL/空串）才采用配置候选。
                const auto& previous = existing_by_name[sym.name()];
                sym.set_id(previous.id);
                // Version chain lives in the DB, not in whatever property the
                // source file happened to carry: seed the old fingerprint and
                // let the recomputation decide whether this edit changed identity.
                sym.set_Kicad_Forge_ID(previous.kf_id);
                sym.set_Pre_Kicad_Forge_ID(previous.pre_kf_id);
                sym.refresh_Kicad_Forge_ID();
                sym.component_type_id =
                    merge_component_type(previous.component_type_id, decision, /*new_row=*/false);
                if (sym.package_type_id.empty())
                    sym.package_type_id = previous.package_type_id;
                if (auto updated = repo.update(sym); !updated)
                {
                    kforge::util::log_error{}("Cannot update symbol {} in {}: {}", sym.name(),
                                              path.string(), updated.error().format_message());
                    return imp_failed(updated.error().format_message());
                }
                imported++;
                continue;
            }
            seen.insert(sym.name());
            // 新行：Matched 候选才落类型，其余落 "Unknown" 占位（未分类）。
            // 类型唯一来源是配置（精确特例 → 配置正则），不按位号/名字猜。
            sym.component_type_id =
                merge_component_type(sym.component_type_id, decision, /*new_row=*/true);
            auto ins = repo.insert(sym);
            if (!ins)
            {
                kforge::util::log_error{}("Cannot insert symbol {} into {}: {}", sym.name(),
                                          path.string(), ins.error().format_message());
                return imp_failed(ins.error().format_message());
            }
            imported++;  // linking happens in Phase 4
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

        // 快照与符号行同一事务记账（A04）：先写库，提交成功后发布到内存
        {
            std::error_code ec;
            auto ft = fs::last_write_time(path, ec);
            if (!ec)
            {
                snapshot_mtime = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::file_clock::to_sys(ft).time_since_epoch())
                                     .count();
                storage::ImportedFilesRepository ifr(session);
                if (auto snapshot = ifr.upsert(path.string(), *snapshot_mtime); !snapshot)
                {
                    kforge::util::log_error{}("Cannot record import snapshot for {}: {}",
                                              path.string(), snapshot.error().format_message());
                    return imp_failed(snapshot.error().format_message());
                }
            }
        }

        if (auto committed = session.commit(); !committed)
        {
            kforge::util::log_error{}("Cannot commit {}: {}", path.string(),
                                      committed.error().format_message());
            return imp_failed(committed.error().format_message());
        }
        // 提交成功才发布这次导入的决策计数与告警
        merge_counts(counts, decided);
    }

    // 提交成功后才公布内存快照：回滚过的文件下次必须重新导入
    if (snapshot_mtime)
    {
        std::lock_guard lock(mtimes_mtx_);
        file_mtimes_[path.string()] = *snapshot_mtime;
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
                    // 文件已改写：快照刷新走真实事务路径，失败也要可见
                    // （刷新不了只是下次重新解析，但绝不能假称已记账）；
                    // 错误事件由 record_file 内部统一报告，这里只留日志
                    if (auto recorded = record_file(path, counts); !recorded)
                    {
                        kforge::util::log_error{}("Cannot refresh snapshot for {}: {}",
                                                  path.string(),
                                                  recorded.error().format_message());
                    }
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
    if (!fp) return imp_failed(fp.error().format_message());
    if (!*fp) return imp_skipped();
    auto session = db_->session();
    if (auto begun = session.begin(); !begun)
    {
        return imp_failed(begun.error().format_message());
    }
    PendingPublish publish;
    auto result = write_footprint(session, path, **fp, publish, publish);
    if (result.is(ImportStatus::Failed))
    {
        return result;  // publish 丢掉，不发布
    }
    if (auto committed = session.commit(); !committed)
    {
        return imp_failed(committed.error().format_message());  // publish 丢掉，不发布
    }
    publish_pending(std::move(publish));
    return result;
}

void ImportPipeline::merge_publish(PendingPublish& target, PendingPublish&& from)
{
    for (auto& [key, fp] : from.footprints)
        target.footprints.insert_or_assign(key, std::move(fp));
    for (auto& [key, model] : from.models)
        target.models.insert_or_assign(key, std::move(model));
    for (auto& [key, mtime] : from.mtimes)
        target.mtimes.insert_or_assign(key, mtime);
    target.models_imported += from.models_imported;
}

/// 提交成功之后才把批内的内存状态写进成员池：失败/回滚过的文件与 mtime
/// 不会留在内存里，下一次执行仍会重新导入。
void ImportPipeline::publish_pending(PendingPublish&& pending)
{
    for (auto& [key, fp] : pending.footprints)
        fp_by_source_.insert_or_assign(key, std::move(fp));
    for (auto& [key, model] : pending.models)
        models_by_path_.insert_or_assign(key, std::move(model));
    // 由封装引用新建的模型条数只在这里计入：COMMIT 之前它不属于任何真实行
    model_ref_imported_ += pending.models_imported;
    if (!pending.mtimes.empty())
    {
        std::lock_guard lock(mtimes_mtx_);
        for (auto& [key, mtime] : pending.mtimes)
            file_mtimes_.insert_or_assign(key, mtime);
    }
}

util::Result<std::optional<core::Footprint>> ImportPipeline::parse_footprint_file(const fs::path& path)
{
    // Per-file mtime snapshot — skip parsing entirely when unchanged
    if (file_unchanged(path))
    {
        return std::nullopt;
    }

    auto result = parser::FootprintParser::parse(path);
    if (!result)
    {
        return std::unexpected(result.error());  // not recorded — retried next run
    }
    return std::optional<core::Footprint>(std::move(*result));
}

IMP_Result ImportPipeline::write_footprint(const storage::DbSession& session, const fs::path& path,
                                           const core::Footprint& fp_in, PendingPublish& batch,
                                           PendingPublish& local)
{
    core::Footprint fp = fp_in;  // mutable copy for id assignment
    fp.set_library_path(path.string());
    // 来源身份：规范路径 + .pretty 库名（同名不同目录靠它区分）
    fp.set_source_key(storage::existing_source_key(path));
    if (fp.footprint_lib_name().empty() && has_library_extension(path.parent_path(), ".pretty"))
    {
        fp.set_footprint_lib_name(path.parent_path().stem().string());
    }
    storage::FootprintRepository repo(session);
    std::string fp_id;
    // 先看本文件尚未 RELEASE 的新值，再看批内已成功释放的，最后看已提交的内存池；
    // 回滚掉的文件留下的值不在任何一层里，后续文件不会误用。
    const core::Footprint* existing = nullptr;
    if (!fp.source_key().empty())
    {
        if (auto it = local.footprints.find(fp.source_key()); it != local.footprints.end())
            existing = &it->second;
        else if (auto it = batch.footprints.find(fp.source_key()); it != batch.footprints.end())
            existing = &it->second;
        else if (auto it = fp_by_source_.find(fp.source_key()); it != fp_by_source_.end())
            existing = &it->second;
    }
    if (existing != nullptr)
    {
        // File changed since last import — refresh the existing row
        fp.set_id(existing->id());
        // 文件里没有类型信息：旧行的分类结果原样带回
        if (fp.package_type_id.empty())
            fp.package_type_id = existing->package_type_id;
        if (auto updated = repo.update(fp); !updated)
        {
            return imp_failed("Update footprint failed: " + updated.error().format_message());
        }
        fp_id = existing->id();

        // Clear old model links — the refs may have changed
        storage::RelationshipRepository rr(session);
        auto olds = rr.find_models_for_footprint(fp_id);
        if (!olds)
        {
            return imp_failed("Read old 3D model links failed for "
                              + path.filename().string() + ": "
                              + olds.error().format_message());
        }
        for (auto& mid : *olds)
        {
            if (auto unlinked = rr.unlink_footprint_model(fp_id, mid); !unlinked)
            {
                return imp_failed("Unlink old 3D model failed for "
                                  + path.filename().string() + ": "
                                  + unlinked.error().format_message());
            }
        }
        local.footprints.insert_or_assign(fp.source_key(), fp);  // 参考引用下面还要用
    }
    else
    {
        auto ins = repo.insert(fp);
        if (!ins)
        {
            return imp_failed("Insert footprint failed: " + ins.error().format_message());
        }
        fp_id = ins->id();
        if (!ins->source_key().empty())
        {
            local.footprints.insert_or_assign(ins->source_key(), *ins);
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
            continue;  // 本地不存在 / 无法解析的引用保持未分配，不是故障
        }
        std::string model_id;
        const core::Model3D* known_model = nullptr;
        if (auto mit = local.models.find(resolved.string()); mit != local.models.end())
            known_model = &mit->second;
        else if (auto mit = batch.models.find(resolved.string()); mit != batch.models.end())
            known_model = &mit->second;
        else if (auto mit = models_by_path_.find(resolved.string()); mit != models_by_path_.end())
            known_model = &mit->second;
        if (known_model != nullptr)
        {
            model_id = known_model->id();
        }
        else
        {
            core::Model3D nm;
            nm.set_file_path(resolved);
            nm.set_format(sexpr::model_format_of(resolved.extension().string()));
            nm.set_description(resolved.stem().string());
            nm.set_source("footprint_ref");
            auto nins = mrepo.insert(nm);
            if (!nins)
            {
                return imp_failed("Insert 3D model failed for " + resolved.filename().string()
                                  + ": " + nins.error().format_message());
            }
            model_id = nins->id();
            // 计数挂在局部发布上：只有包含它的 COMMIT 真的成功才算已导入
            local.models_imported++;
            if (auto recorded = record_file(session, resolved, local); !recorded)
            {
                return imp_failed("Record 3D model snapshot failed for "
                                  + resolved.filename().string() + ": "
                                  + recorded.error().format_message());
            }
            local.models.insert_or_assign(resolved.string(), *nins);  // 扫描阶段据此跳过
        }
        if (!model_id.empty())
        {
            auto linked = rr.link_footprint_to_model(fp_id, model_id, "explicit");
            if (!linked)
            {
                return imp_failed("Link 3D model failed for " + resolved.filename().string()
                                  + ": " + linked.error().format_message());
            }
            model_linked++;
        }
    }
    if (model_refs > 0)
    {
        kforge::util::log_info{}("FP 3D: {} refs, {} linked for {}", model_refs, model_linked,
                                 fp.name());
    }
    if (auto recorded = record_file(session, path, local); !recorded)
    {
        return imp_failed("Record footprint snapshot failed for " + path.filename().string()
                          + ": " + recorded.error().format_message());
    }
    return imp_added();
}

ImportPipeline::ImportDirResult ImportPipeline::import_directory(
    const std::string& dir_str, const std::string& comp_lib_id,
    const classifier::MatchingSnapshot* snapshot)
{
    ImportCounts counts;
    auto source = validate_library_source_path(dir_str, LibrarySourceKind::Symbols);
    if (!source)
    {
        report_error(counts, source.error().format_message());
        return ImportDirResult::err(ImportStatus::Failed, counts);
    }

    // Collect files
    std::vector<std::string> sym_files;
    std::vector<std::string> mod_files;
    std::vector<std::string> pretty_dirs;
    auto scan_directory = [&](const fs::path& directory, auto&& consume,
                              bool defer_pretty = false)
    {
        int scanned = 0;
        report_progress("scanning", 0, 0);
        std::error_code error;
        auto scan_error = [&]
        {
            const auto encoded = directory.u8string();
            report_error(counts, "cannot scan import directory (filesystem error "
                                     + std::to_string(error.value()) + "): "
                                     + std::string(encoded.begin(), encoded.end()));
            return false;
        };
        fs::recursive_directory_iterator cursor(directory, error), end;
        // 初始化失败直接变成 end，不能把错误检查只放在循环体内。
        if (error) return scan_error();
        while (cursor != end && !cancelled())
        {
            const auto& entry = *cursor;
            const auto status = entry.symlink_status(error);
            if (error) return scan_error();
            // 不跟随链接；.pretty 在独立批次里扫描，父扫描不再遍历其子树。
            const bool is_directory = fs::is_directory(status);
            const bool is_regular = fs::is_regular_file(status);
            if (defer_pretty && is_directory && has_library_extension(entry.path(), ".pretty"))
                cursor.disable_recursion_pending();
            consume(entry, is_directory, is_regular);
            if (is_regular) report_progress("scanning", ++scanned, 0);
            cursor.increment(error);
            if (error) return scan_error();
        }
        return true;
    };
    if (source->single_file)
    {
        sym_files.push_back(source->path.string());
        report_progress("scanning", 1, 0);
    }
    else if (!scan_directory(source->path, [&](const fs::directory_entry& entry,
                                               bool is_directory, bool is_regular)
    {
        if (is_directory && has_library_extension(entry.path(), ".pretty"))
        {
            pretty_dirs.push_back(entry.path().string());
        }
        else if (is_regular)
        {
            if (has_library_extension(entry.path(), ".kicad_sym"))
            {
                sym_files.push_back(entry.path().string());
            }
            else if (has_library_extension(entry.path(), ".kicad_mod"))
            {
                mod_files.push_back(entry.path().string());
            }
        }
    }, true))
    {
        return ImportDirResult::err(ImportStatus::Failed, counts);
    }

    // Symbols: bounded parallel parse (pure CPU, no DB access), then serial
    // transactional writes on the single connection — never touch sqlite3*
    // from more than one thread at a time
    if (!sym_files.empty())
    {
        const int total = static_cast<int>(sym_files.size());
        int parsed_files = 0;
        report_progress("symbols_parse", 0, total);
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
                    [this, &parsed_libs, &parsed_mtx, &counts, &f = sym_files[j], snapshot]()
                    {
                        auto parsed = parse_symbol_library(f);
                        if (parsed && *parsed)
                        {
                            auto& lib = **parsed;
                            // 决策在取得 DbSession 之前算：快照不可变，各次 evaluate
                            // 有独立 scratch/预算，可以在工作线程里并行求值
                            if (snapshot != nullptr)
                            {
                                lib.decisions.reserve(lib.items.size());
                                for (const auto& item : lib.items)
                                    lib.decisions.push_back(
                                        snapshot->evaluate(
                                            classifier::MatchSample::from_symbol(item)));
                            }
                            std::lock_guard lock(parsed_mtx);
                            parsed_libs.push_back(std::move(lib));
                        }
                        else
                        {
                            std::lock_guard lock(parsed_mtx);
                            if (parsed) counts.skipped++;
                            else
                            {
                                counts.failed_files++;
                                report_error(counts, "symbol library " + fs::path(f).filename().string()
                                                     + ": " + parsed.error().format_message());
                            }
                        }
                    }));
            }
            for (auto& fut : futures)
            {
                fut.get();
                report_progress("symbols_parse", ++parsed_files, total);
            }
        }

        int processed = 0;
        report_progress("symbols_import", 0, total);
        // 未变化和解析失败的文件都已处理，但只有未变化的文件计入 skipped。
        const int skipped_files = parsed_files - static_cast<int>(parsed_libs.size());
        for (int i = 0; i < skipped_files; ++i)
            report_progress("symbols_import", ++processed, total);
        for (auto& lib : parsed_libs)
        {
            if (cancelled())
            {
                break;
            }
            const std::string where = lib.path.filename().string();
            auto written = write_symbol_library(lib, comp_lib_id, snapshot, counts);
            if (written.is(ImportStatus::Added))
            {
                counts.symbols++;
            }
            else if (written.is(ImportStatus::Failed))
            {
                // imp_failed 不是 skipped：整个库事务已回滚，错误必须带出去
                counts.failed_files++;
                report_error(counts, "symbol library " + where + ": " + written.info);
            }
            else
            {
                counts.skipped++;
            }
            report_progress("symbols_import", ++processed, total);
        }
    }

    // Footprints: bounded parallel parse (no DB access), then serial transactional
    // write — one BEGIN/COMMIT per directory instead of per-file commits
    auto import_fp_files = [this](const std::vector<std::string>& files) -> ImportCounts
    {
        if (files.empty())
            return {};
        const int total = static_cast<int>(files.size());
        int processed = 0;
        int parsed_files = 0;
        report_progress("footprints_parse", 0, total);
        auto advance = [&] { report_progress("footprints", ++processed, total); };
        struct Pending
        {
            fs::path path;
            core::Footprint fp;
        };
        std::vector<Pending> pending;
        ImportCounts local;
        std::mutex pending_mtx;
        std::atomic<std::size_t> next_file{0};
        const std::size_t worker_count = std::min(files.size(), static_cast<std::size_t>(
            std::min(std::max(1u, std::thread::hardware_concurrency()), 16u)));
        std::vector<std::future<void>> futures;
        futures.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker)
        {
            futures.push_back(std::async(std::launch::async,
                                         [this, &files, &next_file, &pending, &pending_mtx,
                                          &parsed_files, &local, total]()
                                         {
                                             while (!cancelled())
                                             {
                                                 const auto index = next_file.fetch_add(1);
                                                 if (index >= files.size() || cancelled()) break;
                                                 const auto& file = files[index];
                                                 auto fp = parse_footprint_file(fs::path(file));
                                                 std::lock_guard lock(pending_mtx);
                                                 if (fp && *fp)
                                                     pending.push_back({file, std::move(**fp)});
                                                 else if (fp) local.skipped++;
                                                 else
                                                 {
                                                     local.failed_files++;
                                                     report_error(local, "footprint "
                                                         + fs::path(file).filename().string() + ": "
                                                         + fp.error().format_message());
                                                 }
                                                 // 完成即发布，且回调串行：取消可阻止工作者
                                                 // 继续领取文件，也不要求回调自己实现线程保护。
                                                 report_progress("footprints_parse", ++parsed_files, total);
                                             }
                                         }));
        }
        for (auto& fut : futures)
        {
            fut.get();
        }

        int added = 0;
        report_progress("footprints", 0, total);
        // 取消后没有领取的来源没有执行过，不能计入 skipped 或完成进度。
        const int skipped_files = parsed_files - static_cast<int>(pending.size());
        for (int i = 0; i < skipped_files; ++i)
            advance();
        if (cancelled()) return local;
        if (!pending.empty())
        {
            auto session = db_->session();
            auto begun = session.begin();
            if (!begun)
            {
                kforge::util::log_error{}("FP import: {}", begun.error().format_message());
                local.failed_files += pending.size();
                report_error(local, "footprint import: " + begun.error().format_message());
                return local;
            }
            // 批级暂存：只有各文件自己的 SAVEPOINT RELEASE 成功才合进来；外层
            // COMMIT 成功之前不写任何成员池/计数（publish_pending 才公开）。
            PendingPublish publish;
            int failed_files = 0;
            bool abort_batch = false;
            std::string abort_reason;
            for (auto& p : pending)
            {
                if (cancelled())
                {
                    break;
                }
                // 每个文件一个 SQLite 官方 SAVEPOINT：单文件失败只回滚它自己的写入，
                // 不是自研事务框架，也不拷贝整批 map 去伪造回滚
                if (auto sp = exec_control(session.handle(), "SAVEPOINT kforge_fp_file"); !sp)
                {
                    advance();
                    abort_batch = true;
                    abort_reason = sp.error().format_message();
                    break;
                }
                PendingPublish file_publish;
                auto written = write_footprint(session, p.path, p.fp, publish, file_publish);
                if (written.is(ImportStatus::Added))
                {
                    if (auto released =
                            exec_control(session.handle(), "RELEASE SAVEPOINT kforge_fp_file");
                        !released)
                    {
                        advance();
                        abort_batch = true;
                        abort_reason = released.error().format_message();
                        break;
                    }
                    merge_publish(publish, std::move(file_publish));
                    added++;
                    advance();
                    continue;
                }
                // 这一条写入失败：原子写原因先入账（后面的控制失败不能把它挤掉），
                // 再回滚该文件的全部行并丢掉它的局部发布，批内其他文件继续；
                // 回滚控制本身失败就终止整批 —— 此时错误里同时有写原因和控制原因
                failed_files++;
                report_error(local,
                             "footprint " + p.path.filename().string() + ": " + written.info);
                advance();
                if (auto rolled =
                        exec_control(session.handle(), "ROLLBACK TO SAVEPOINT kforge_fp_file");
                    !rolled)
                {
                    abort_batch = true;
                    abort_reason = rolled.error().format_message();
                    break;
                }
                if (auto released =
                        exec_control(session.handle(), "RELEASE SAVEPOINT kforge_fp_file");
                    !released)
                {
                    abort_batch = true;
                    abort_reason = released.error().format_message();
                    break;
                }
            }
            if (abort_batch)
            {
                // 事务已经不可继续：整批回滚，所有待发布行/counts/maps 一起丢弃，
                // 并按「这批里有几个文件没有提交」如实报告
                added = 0;
                local.failed_files = pending.size();
                // 第二事件：原 SAVEPOINT 控制的 abort_reason 照实报告
                report_error(local, "footprint import rolled back: " + abort_reason);
                // 第三事件：外层 ROLLBACK 的控制失败单独成事件，真实 DbError
                // 不与 abort_reason 拼接、不假加 failed_files
                if (auto rolled = session.rollback(); !rolled)
                {
                    report_error(local, "footprint import ROLLBACK: "
                                            + rolled.error().format_message());
                }
                return local;
            }
            if (auto committed = session.commit(); !committed)
            {
                // 整批回滚：先前计数的 added 一条都不能算，内存快照也不发布。
                // 一个 COMMIT 错误事件 + N 个没有提交的失败文件
                kforge::util::log_error{}("FP import: {}", committed.error().format_message());
                local.failed_files += added + static_cast<std::uint64_t>(failed_files);
                added = 0;
                report_error(local, "footprint import commit: "
                                        + committed.error().format_message());
            }
            else
            {
                publish_pending(std::move(publish));
                // 部分文件 SAVEPOINT 回滚但整批已提交：失败文件数照实发布，
                // 不因为 COMMIT 成功就把它们抹掉
                local.failed_files += failed_files;
            }
        }
        local.footprints = added;
        return local;
    };

    for (auto& pd : pretty_dirs)
    {
        std::vector<std::string> files;
        if (!scan_directory(fs::path(pd), [&](const fs::directory_entry& entry,
                                               bool, bool is_regular)
        {
            if (is_regular && has_library_extension(entry.path(), ".kicad_mod"))
            {
                files.push_back(entry.path().string());
            }
        }))
        {
            return ImportDirResult::err(ImportStatus::Failed, counts);
        }
        auto local = import_fp_files(files);
        merge_counts(counts, local);
        kforge::util::log_info{}("FP DIR: {} -> {} footprints", pd, local.footprints);
    }
    {
        auto local = import_fp_files(mod_files);
        merge_counts(counts, local);
    }

    kforge::util::log_info{}("DIR SCAN: {} -> {} symbols, {} footprints, {} skipped", dir_str,
                             counts.symbols, counts.footprints, counts.skipped);
    return ImportDirResult{ImportStatus::Added, counts};
}

ImportPipeline::ModelScanReport ImportPipeline::scan_3d_models(const std::string& dir_str)
{
    ModelScanReport report;
    auto source = validate_library_source_path(dir_str, LibrarySourceKind::Models);
    if (!source)
    {
        // 配置好的模型根不存在是真实错误，但目录不是来源文件：只记 1 个错误事件，
        // 不计入 failed_files（failed_files 只数没有提交的来源文件）
        report_error(report.counts, source.error().format_message());
        return report;
    }

    // 先扫描真实模型文件再公布总量；扫描期间不持数据库锁。
    std::vector<fs::path> files;
    int scanned = 0;
    report_progress("scanning", 0, 0);
    std::error_code scan_error;
    fs::recursive_directory_iterator cursor(source->path, scan_error), end;
    while (!scan_error && cursor != end && !cancelled())
    {
        const auto& entry = *cursor;
        const auto status = entry.symlink_status(scan_error);
        if (scan_error) break;
        if (fs::is_regular_file(status))
        {
            if (!sexpr::model_format_of(entry.path().extension().string()).empty())
                files.push_back(entry.path());
            report_progress("scanning", ++scanned, 0);
        }
        cursor.increment(scan_error);
    }
    // 已扫描到的文件仍可继续处理；错误与此前已提交阶段的计数一起返回。
    if (scan_error)
        report_error(report.counts, "cannot scan 3D model directory (filesystem error "
                                       + std::to_string(scan_error.value()) + "): " + dir_str);
    const int total = static_cast<int>(files.size());
    int processed = 0;
    report_progress("models", 0, total);
    auto advance = [&] { report_progress("models", ++processed, total); };
    // 只有确实要写库的文件才开 session。完成数表示处理过的文件，提交数仍在 report。
    for (const auto& path : files)
    {
        if (cancelled())
            break;
        const std::string format = sexpr::model_format_of(path.extension().string());

        // Canonicalize so lookups match paths resolved from footprint refs
        std::error_code ec;
        auto canon = fs::weakly_canonical(path, ec);
        auto store_path = ec ? fs::absolute(path, ec) : canon;
        if (ec)
        {
            report.counts.failed_files++;
            report_error(report.counts, "cannot resolve 3D model path: " + path.string());
            advance();
            continue;
        }

        auto it = models_by_path_.find(store_path.string());
        if (it != models_by_path_.end())
        {
            // Unchanged since last import → skip
            if (file_unchanged(store_path))
            {
                report.skipped++;
                advance();
                continue;
            }
            // 模型行与来源快照必须一起提交：失败就不算导入、不消费 mtime，
            // 同一份内容与 mtime 下次仍能重试（UPDATE 被拒时尤其重要）
            PendingPublish publish;
            auto session = db_->session();
            if (auto begun = session.begin(); !begun)
            {
                report.counts.failed_files++;
                report_error(report.counts,
                             "3D model scan: " + begun.error().format_message());
                advance();
                break;  // 事务开不起来：后面的文件同样写不了，不再假装继续
            }
            storage::Model3DRepository repo(session);
            if (auto updated = repo.update(it->second); !updated)
            {
                report.counts.failed_files++;
                report_error(report.counts,
                             "update 3D model " + store_path.filename().string() + ": "
                                 + updated.error().format_message());
                advance();
                if (auto undone = session.rollback(); !undone)
                {
                    // 显式 ROLLBACK 被拒是第二个真实错误事件：连接状态不明，
                    // 终止扫描，不继续写别的文件；析构重试兜底清理
                    report_error(report.counts,
                                 "rollback 3D model update " + store_path.filename().string()
                                     + ": " + undone.error().format_message());
                    break;
                }
                continue;
            }
            if (auto recorded = record_file(session, store_path, publish); !recorded)
            {
                report.counts.failed_files++;
                report_error(report.counts,
                             "record 3D model snapshot " + store_path.filename().string() + ": "
                                 + recorded.error().format_message());
                advance();
                if (auto undone = session.rollback(); !undone)
                {
                    report_error(report.counts,
                                 "rollback 3D model snapshot " + store_path.filename().string()
                                     + ": " + undone.error().format_message());
                    break;
                }
                continue;
            }
            if (auto committed = session.commit(); !committed)
            {
                report.counts.failed_files++;
                report_error(report.counts,
                             "commit 3D model " + store_path.filename().string() + ": "
                                 + committed.error().format_message());
                advance();
                // COMMIT 失败事务仍打开：显式 cleanup，成功不额外计错
                if (auto undone = session.rollback(); !undone)
                {
                    report_error(report.counts,
                                 "rollback 3D model after failed commit "
                                     + store_path.filename().string() + ": "
                                     + undone.error().format_message());
                    break;
                }
                continue;  // publish 丢掉，不发布
            }
            publish_pending(std::move(publish));
            report.imported++;
            advance();
            continue;
        }
        core::Model3D m;
        m.set_file_path(store_path);
        m.set_format(format);
        m.set_description(path.stem().string());
        m.set_source("filesystem");
        PendingPublish publish;
        auto session = db_->session();
        if (auto begun = session.begin(); !begun)
        {
            report.counts.failed_files++;
            report_error(report.counts, "3D model scan: " + begun.error().format_message());
            advance();
            break;
        }
        storage::Model3DRepository repo(session);
        auto ins = repo.insert(m);
        if (!ins)
        {
            report.counts.failed_files++;
            report_error(report.counts,
                         "insert 3D model " + store_path.filename().string() + ": "
                             + ins.error().format_message());
            advance();
            if (auto undone = session.rollback(); !undone)
            {
                // 显式 ROLLBACK 被拒是第二个真实错误事件：连接状态不明，
                // 终止扫描，不继续写别的文件；析构重试兜底清理
                report_error(report.counts,
                             "rollback 3D model insert " + store_path.filename().string()
                                 + ": " + undone.error().format_message());
                break;
            }
            continue;
        }
        if (auto recorded = record_file(session, store_path, publish); !recorded)
        {
            report.counts.failed_files++;
            report_error(report.counts,
                         "record 3D model snapshot " + store_path.filename().string() + ": "
                             + recorded.error().format_message());
            advance();
            if (auto undone = session.rollback(); !undone)
            {
                report_error(report.counts,
                             "rollback 3D model snapshot " + store_path.filename().string()
                                 + ": " + undone.error().format_message());
                break;
            }
            continue;
        }
        if (auto committed = session.commit(); !committed)
        {
            report.counts.failed_files++;
            report_error(report.counts,
                         "commit 3D model " + store_path.filename().string() + ": "
                             + committed.error().format_message());
            advance();
            // COMMIT 失败事务仍打开：显式 cleanup，成功不额外计错
            if (auto undone = session.rollback(); !undone)
            {
                report_error(report.counts,
                             "rollback 3D model after failed commit "
                                 + store_path.filename().string() + ": "
                                 + undone.error().format_message());
                break;
            }
            continue;  // publish 丢掉，池与 mtime 都不发布
        }
        publish_pending(std::move(publish));
        models_by_path_.emplace(store_path.string(), std::move(*ins));
        report.imported++;
        advance();
    }
    kforge::util::log_info{}("3D SCAN: {} -> {} imported, {} already in DB", dir_str,
                              report.imported, report.skipped);
    return report;
}

bool ImportPipeline::file_unchanged(const fs::path& p)
{
    if (force_reimport_)
    {
        // force：本次忽略已缓存 mtime，重新导入启用目标；全局快照表不动
        return false;
    }
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

util::Result<void> ImportPipeline::record_file(const fs::path& p, ImportCounts& counts)
{
    // KF ID 写回后的快照刷新：真实事务，主因与控制原因各自独立成事件、
    // 每个只报告一次，顺序先主因后控制；失败一律不 publish
    auto session = db_->session();
    if (auto begun = session.begin(); !begun)
    {
        report_error(counts, "record snapshot " + p.filename().string() + ": "
                                 + begun.error().format_message());
        return begun;
    }
    PendingPublish publish;
    if (auto recorded = record_file(session, p, publish); !recorded)
    {
        // 主因先报；显式回滚控制失败才追加独立 ROLLBACK 事件
        report_error(counts, "record snapshot " + p.filename().string() + ": "
                                 + recorded.error().format_message());
        if (auto rolled = session.rollback(); !rolled)
        {
            report_error(counts, "record snapshot " + p.filename().string()
                                     + " ROLLBACK: " + rolled.error().format_message());
        }
        return recorded;
    }
    if (auto committed = session.commit(); !committed)
    {
        // 快照没落盘就不发布：COMMIT 主因先报，显式清理失败才追加控制事件
        report_error(counts, "record snapshot " + p.filename().string() + " COMMIT: "
                                 + committed.error().format_message());
        if (auto cleaned = session.rollback(); !cleaned)
        {
            report_error(counts, "record snapshot " + p.filename().string()
                                     + " cleanup ROLLBACK: " + cleaned.error().format_message());
        }
        return committed;
    }
    publish_pending(std::move(publish));
    return {};
}

util::Result<void> ImportPipeline::record_file(const storage::DbSession& session, const fs::path& p,
                                               PendingPublish& publish)
{
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec)
    {
        // 读不到 mtime 就不能记「已导入」：文件下次仍会被重新解析
        return util::Result<void>(std::unexpected(util::Error::make<util::Error::Kind::IoError>(
            "cannot read mtime of " + p.string() + ": " + ec.message())));
    }
    auto mtime_s = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::file_clock::to_sys(ft).time_since_epoch())
                       .count();
    // 先真正写进 imported_files，成功之后才暂存内存快照（提交成功才发布）
    storage::ImportedFilesRepository ifr(session);
    if (auto stored = ifr.upsert(p.string(), mtime_s); !stored)
    {
        return stored;
    }
    publish.mtimes.insert_or_assign(p.string(), mtime_s);
    return {};
}

fs::path ImportPipeline::resolve_model_path(std::string_view ref, const fs::path& fp_dir) const
{
    // Shared implementation — see core/io/sexpr/path_util.h (binding manager
    // uses the same resolver when auto-linking models after a re-bind).
    return sexpr::resolve_model_path(ref, fp_dir, model_roots_);
}

}  // namespace kforge::services
