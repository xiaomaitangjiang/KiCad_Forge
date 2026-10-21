// Pipeline-style import — chain steps with operator|
//
// Usage:
//   auto result = ImportPipeline(db)
//       | symbols_from{dir, lib_id}
//       | footprints_from{dir}
//       | models_from{dir}
//       | progress{[](auto& p, int d, int t) { kforge::util::log_info{}("{} {}/{}", p, d, t; }})})
//       | execute;
#pragma once

#include "util/result.h"
#include "classifier/type_matching.h"
#include "core/model/symbol.h"
#include "core/model/footprint.h"
#include "core/model/model_3d.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kforge::storage { class Database; class DbSession; }

namespace kforge::services
{

/// 一次导入操作里保留的错误/告警明细条数上限。完整事件计数（error_count /
/// warning_count / failed_files）不受它影响，日志只能如实展示这一有限明细。
inline constexpr std::size_t kMaxReportMessages = 100;

// Concrete import-status alias — backwards compatible with IMP_Result
enum class ImportStatus : int8_t
{
    Failed = -1,
    Skipped = 0,
    Added = 1
};
using IMP_Result = util::StatusResult<ImportStatus>;

inline IMP_Result imp_added()
{
    return {.status = ImportStatus::Added, .info = {}};
}
inline IMP_Result imp_skipped()
{
    return {.status = ImportStatus::Skipped, .info = {}};
}
inline IMP_Result imp_failed(std::string msg)
{
    return {.status = ImportStatus::Failed, .info = std::move(msg)};
}

// ---- Step types (lightweight data bags) ----
struct symbols_from
{
    std::string dir;
    std::string comp_lib_id;
};
struct footprints_from
{
    std::string dir;
};
struct models_from
{
    std::string dir;
};
struct progress
{
    // 当前阶段的已处理 / 总来源文件数，阶段变化时重新计数；总量未知时为 0。
    // 这不是已提交条目数：失败文件和未变化文件同样会推进处理进度。
    std::function<void(const std::string& phase, int done, int total)> fn;
};

// ---- Pipeline class ----
class ImportPipeline
{
public:
    struct Result
    {
        // 旧计数（全部保留）：symbols 仍是「符号库文件数」，不是元件条目数
        int symbols = 0, footprints = 0, models_3d = 0, skipped = 0, linked_symbols = 0,
            linked_models = 0;
        // U04-B1：真实错误 / 告警通道。error_count 非零 = 这次操作没有完整成功，
        // 计数只代表已经 commit 的部分；warnings 是可解释但不阻断的状态。
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        /// 实际发生的错误事件数：明细只有前 kMaxReportMessages 条，计数不受上限影响
        std::uint64_t error_count = 0;
        /// 实际发生的告警事件数（同样不受明细上限影响）
        std::uint64_t warning_count = 0;
        /// 因失败而没有提交的来源文件数。一批 COMMIT 失败可以是 1 个错误事件 +
        /// N 个失败文件，两者不是一回事。
        std::uint64_t failed_files = 0;
        /// 本次操作的匹配决策计数（按符号条目；丢弃的结论不会写类型）
        int matched = 0, unmatched = 0, conflicts = 0, input_too_long = 0,
            evaluation_limited = 0, evaluation_errors = 0;
        [[nodiscard]] bool ok() const { return error_count == 0; }
    };

    /// 一条来源（一个符号库文件 / 一个封装批）的局部计数与消息
    struct ImportCounts
    {
        int symbols = 0;      // 符号库文件数
        int footprints = 0;
        int skipped = 0;      // 未变化等正常没有写入的文件；解析失败属于 failed_files
        /// 事务回滚的失败来源文件数（不算 skipped），与 Result::failed_files 同一口径
        std::uint64_t failed_files = 0;
        /// 完整事件计数：明细文本受 kMaxReportMessages 限制，计数不受限制
        std::uint64_t error_count = 0;
        std::uint64_t warning_count = 0;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        int matched = 0, unmatched = 0, conflicts = 0, input_too_long = 0,
            evaluation_limited = 0, evaluation_errors = 0;
    };

    explicit ImportPipeline(storage::Database& db, std::atomic<bool>* cancel = nullptr,
                           bool write_kf_id_to_file = false,
                           classifier::TypeMatchingStore* matching_store = nullptr,
                           bool force_reimport = false)
        : db_(&db), cancel_(cancel), write_kf_id_to_file_(write_kf_id_to_file),
          matching_store_(matching_store ? matching_store : &classifier::TypeMatchingStore::instance()),
          force_reimport_(force_reimport)
    {
    }

    ImportPipeline& operator|(symbols_from src);
    ImportPipeline& operator|(footprints_from src);
    ImportPipeline& operator|(models_from src);
    ImportPipeline& operator|(progress p);

    struct execute_t
    {
    };
    Result operator|(execute_t);

private:
    storage::Database* db_;
    std::atomic<bool>* cancel_ = nullptr;

    // Pending steps
    std::vector<symbols_from> pending_symbols_;
    std::vector<footprints_from> pending_footprints_;
    std::vector<models_from> pending_models_;
    progress progress_fn_;
    void report_progress(const std::string& phase, int completed, int total) const;
    Result execute_steps();

    // Incremental import state is per-file mtime snapshots in the imported_files table

    // 3D model roots — for resolving ${KICADx_3DMODEL_DIR} references
    std::vector<std::string> model_roots_;

    // Models imported from explicit footprint refs (counted into Result::models_3d)
    int model_ref_imported_ = 0;

    // Patch Kicad_Forge_ID property back into .kicad_sym on successful write.
    // Default false; locked and official libraries remain read-only even when enabled.
    bool write_kf_id_to_file_ = false;
    classifier::TypeMatchingStore* matching_store_;
    // force 入口的本次语义：忽略已缓存 mtime 重新导入当前启用目标，
    // 不删全局 imported_files 快照表（清表属于 db/reset）。
    bool force_reimport_ = false;

    // Internal implementation (moved from LibraryService)
    IMP_Result import_symbol_library(const std::string& path, const std::string& comp_lib_id,
                                     const classifier::MatchingSnapshot* snapshot,
                                     ImportCounts& counts);
    IMP_Result import_footprint(const std::string& path);

    // Symbol library import split: parallel-safe parse (no DB access) + serial
    // DB write — the pipeline must never touch the single sqlite3* connection
    // from more than one thread at a time
    struct ParsedSymbolLib
    {
        std::filesystem::path path;
        std::string lib_name;
        std::vector<core::Symbol> items;
        /// 与 items 一一对应的决策（在取得 DbSession 之前算好）
        std::vector<classifier::Decision> decisions;
    };
    util::Result<std::optional<ParsedSymbolLib>> parse_symbol_library(const std::string& path);
    IMP_Result write_symbol_library(ParsedSymbolLib& lib, const std::string& comp_lib_id,
                                    const classifier::MatchingSnapshot* snapshot,
                                    ImportCounts& counts);

    // Footprint import split: parallel-safe parse (no DB access) + serial DB write
    util::Result<std::optional<core::Footprint>> parse_footprint_file(const std::filesystem::path& path);

    /// 批内暂存的「内存状态发布」：事务提交成功才写进成员池/快照表，
    /// 回滚时直接丢掉，失败的文件不会被当成已导入而永远跳过。
    struct PendingPublish
    {
        std::unordered_map<std::string, core::Footprint> footprints;  // 按来源键
        std::unordered_map<std::string, core::Model3D> models;        // 按路径
        std::unordered_map<std::string, int64_t> mtimes;              // 按路径
        /// 本批内新建的「由封装引用导入」的 3D 模型条数：只有包含它的那次 COMMIT
        /// 真的成功才计入 Result（事务回滚时模型行并不存在）。
        int models_imported = 0;
    };
    /// batch 是批级（已 RELEASE 的 SAVEPOINT 合并结果），local 是本文件自己的
    /// 暂存：查找按 local → batch → 已提交池的顺序，写入只落在 local。
    IMP_Result write_footprint(const storage::DbSession& session, const std::filesystem::path& path,
                               const core::Footprint& fp, PendingPublish& batch,
                               PendingPublish& local);
    /// 一个文件的 SAVEPOINT RELEASE 成功后，把它的局部发布合入批级暂存
    static void merge_publish(PendingPublish& target, PendingPublish&& from);
    /// 真正 COMMIT 之后才公开批级暂存（池 / 模型数量 / mtime 快照）
    void publish_pending(PendingPublish&& pending);


    // In-memory pools, loaded once before the footprint phase — replace
    // per-file find_by_name / find_by_path queries.
    // 封装按来源键索引：同名不同 .pretty 目录是两个对象。
    std::unordered_map<std::string, core::Footprint> fp_by_source_;
    std::unordered_map<std::string, core::Model3D> models_by_path_;

    using ImportDirResult = util::StatusResult<ImportStatus, ImportCounts>;
    /// snapshot 是一次 execute 领取的健康快照（不可变、可并发 evaluate）；
    /// 为空表示调用方没有可用的匹配配置（execute 会在更早处拦住这种情况）。
    ImportDirResult import_directory(const std::string& dir, const std::string& comp_lib_id = "",
                                     const classifier::MatchingSnapshot* snapshot = nullptr);
    /// 一个 3D 模型目录的扫描报告：成功计数 + 真实错误通道（不再只返回 int 吞错误）
    struct ModelScanReport
    {
        int imported = 0;
        int skipped = 0;          // 未变化、没有重新写入的文件
        ImportCounts counts;      // 失败文件 / 错误 / 告警
    };
    ModelScanReport scan_3d_models(const std::string& dir);

    // In-memory mtime snapshots, loaded once per execute (guarded: symbol import
    // runs in parallel worker threads)
    std::unordered_map<std::string, int64_t> file_mtimes_;
    std::mutex mtimes_mtx_;

    // Per-file mtime snapshot: true when the file is unchanged since the last import。
    // force 入口的本次语义：忽略已缓存 mtime（不删全局快照表）。
    bool file_unchanged(const std::filesystem::path& p);
    // Record the file's current mtime as imported（独立事务路径：BEGIN/写/
    // COMMIT 各自的失败主因先报，显式 rollback 控制失败追加独立事件，
    // 每个 error 只报告一次；提交成功才 publish 内存快照）
    util::Result<void> record_file(const std::filesystem::path& p, ImportCounts& counts);
    // 同上，但复用调用方的 session —— 让快照与业务行落在同一事务；只有
    // ImportedFilesRepository::upsert 真的成功才把 mtime 挂到 publish 上，
    // 事务提交成功才真正发布（回滚过的文件下次必须重新导入）。
    // mtime 读取失败或 SQL 失败一律返回错误，由调用方回滚该文件/批。
    util::Result<void> record_file(const storage::DbSession& session, const std::filesystem::path& p,
                                   PendingPublish& publish);
    // True when the orchestrator requested a stop
    bool cancelled() const { return cancel_ && cancel_->load(std::memory_order_relaxed); }
    // Resolve a footprint's 3D model reference to an existing absolute path
    std::filesystem::path resolve_model_path(std::string_view ref,
                                             const std::filesystem::path& fp_dir) const;
};

inline constexpr ImportPipeline::execute_t execute{};

}  // namespace kforge::services
