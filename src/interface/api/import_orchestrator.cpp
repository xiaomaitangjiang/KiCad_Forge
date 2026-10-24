#include "core/db/database.h"
#include "core/repo/repositories.h"
#include "interface/api/import_orchestrator.h"
#include "interface/service/editing/history.h"
#include "interface/service/import/source_path.h"
#include "util/config_store.h"
#include "util/logger.h"
#include "util/result.h"

#include <chrono>


namespace kforge::api
{

namespace
{

/// 后台线程与同步入口共用的执行体：装配启用库的步骤 → execute。
/// 健康门与计数都发生在 pipeline 内部；这里不碰任何 mtime 快照。
services::ImportPipeline::Result run_import(storage::Database& db, util::ConfigStore* config,
                                            std::atomic<bool>* cancel,
                                            classifier::TypeMatchingStore* store, bool force,
                                            services::progress progress)
{
    auto libs = [&db]
    {
        auto session = db.session();
        return storage::ComponentLibraryRepository(session).find_enabled();
    }();

    // 启用库读不出来 = 这次操作根本没开始：直接作为操作级错误返回，
    // 不能拿空 pipeline 跑一遍再报「完成」（那会推进序号/数据版本）。
    if (!libs)
    {
        const std::string message =
            "cannot read enabled libraries: " + libs.error().format_message();
        kforge::util::log_error{}("Import: {}", message);
        services::ImportPipeline::Result failed;
        failed.error_count = 1;
        failed.errors.push_back(message);
        return failed;
    }

    const bool write_kf_id = config != nullptr && config->get_bool("write_kf_id_to_file", false);
    // Validate every configured footprint field, including a path that equals
    // symbol_path and would otherwise be omitted by the directory dedup below.
    for (const auto& library : *libs)
    {
        if (library.footprint_path.empty()) continue;
        if (auto valid = services::validate_library_source_path(
                library.footprint_path, services::LibrarySourceKind::Footprints); !valid)
        {
            services::ImportPipeline::Result failed;
            failed.error_count = 1;
            failed.errors.push_back("Component library '" + library.name + "': "
                                    + valid.error().format_message());
            return failed;
        }
    }
    services::ImportPipeline pipe(db, cancel, write_kf_id, store, force);
    pipe | std::move(progress);
    for (auto& l : *libs)
    {
        if (!l.symbol_path.empty())
        {
            pipe | services::symbols_from{.dir = l.symbol_path, .comp_lib_id = l.id};
        }
        if (!l.footprint_path.empty() && l.footprint_path != l.symbol_path)
        {
            kforge::util::log_info{}("Import: adding footprints from {}", l.footprint_path);
            pipe | services::footprints_from{l.footprint_path};
        }
        if (!l.model_3d_path.empty())
        {
            kforge::util::log_info{}("Import: adding 3D models from {}", l.model_3d_path);
            pipe | services::models_from{l.model_3d_path};
        }
    }
    return pipe | services::execute;
}

/// 错误必须带着实际消息出现在日志里；正常告警同样明确记录。
/// errors/warnings 只保留有限明细，完整数量另行说明 —— 不假称逐条全部输出。
void log_result(const services::ImportPipeline::Result& result, const char* prefix)
{
    for (const auto& message : result.errors)
        kforge::util::log_error{}("{}: {}", prefix, message);
    for (const auto& message : result.warnings)
        kforge::util::log_warn{}("{} warning: {}", prefix, message);
    if (result.error_count > result.errors.size() || result.warning_count > result.warnings.size())
    {
        kforge::util::log_warn{}("{}: {} error(s), {} warning(s), {} failed file(s) total; "
                                 "{} error and {} warning detail(s) shown",
                                 prefix, result.error_count, result.warning_count,
                                 result.failed_files, result.errors.size(),
                                 result.warnings.size());
    }
}

/// 完成/数据版本信号只反映真实提交：一行都没提交的失败不推进，
/// 部分提交仍要发出刷新信号（错误本身在同批日志里说明）。
bool committed_something(const services::ImportPipeline::Result& result)
{
    return result.symbols > 0 || result.footprints > 0 || result.models_3d > 0;
}

}  // namespace

ImportOrchestrator::~ImportOrchestrator()
{
    stop();
}

ImportOrchestrator::ProgressSnapshot ImportOrchestrator::progress_snapshot() const
{
    std::lock_guard lock(progress_mutex_);
    return progress_;
}

void ImportOrchestrator::update_progress(const std::string& phase, int completed, int total)
{
    std::lock_guard lock(progress_mutex_);
    progress_ = {phase, completed, total};
}

void ImportOrchestrator::finish_progress(const services::ImportPipeline::Result& result)
{
    update_progress(cancel_requested() ? "cancelled" : result.ok() ? "done" : "failed", 0, 0);
}

void ImportOrchestrator::start()
{
    std::unique_lock execution(execution_mutex_, std::try_to_lock);
    if (!execution.owns_lock()) return;
    if (running_.load(std::memory_order_relaxed))
    {
        return;
    }

    if (import_thread_.joinable()) import_thread_.join();

    running_ = true;
    cancel_ = false;  // reset after a previous stop()
    sym_count_ = 0;
    fp_count_ = 0;
    update_progress("scanning", 0, 0);

    try
    {
        import_thread_ = std::thread(
            [this]()
            {
                try
                {
                    auto result = run_import(*db_, config_, &cancel_, matching_store_, /*force=*/false,
                        services::progress{[this](const auto& phase, int completed, int total)
                        {
                            update_progress(phase, completed, total);
                        }});
                    sym_count_ = result.symbols;
                    fp_count_ = result.footprints;

                    log_result(result, "Auto-import");
                    if (result.ok())
                    {
                        // In-process signal: MgrHolder watches this to refresh the
                        // SymbolBindingManager in-memory index after each import.
                        import_seq_.fetch_add(1, std::memory_order_relaxed);
                        db_->bump_data_version();
                        if (committed_something(result))
                            services::note_successful_edit();
                        kforge::util::log_info{}(
                            "Auto-import done: +{} new symbols, +{} new footprints, "
                            "{} skipped (up to date)",
                            sym_count_.load(), fp_count_.load(), result.skipped);
                    }
                    else if (committed_something(result))
                    {
                        // 部分提交：数据确实变了，刷新信号不能因为报错被吞掉
                        import_seq_.fetch_add(1, std::memory_order_relaxed);
                        db_->bump_data_version();
                        services::note_successful_edit();
                    }
                    finish_progress(result);
                }
                catch (const std::exception& e)
                {
                    update_progress("failed", 0, 0);
                    kforge::util::log_error{}("Auto-import exception: {}", e.what());
                }
                catch (...)
                {
                    update_progress("failed", 0, 0);
                    kforge::util::log_error{}("Auto-import unknown exception");
                }
                running_ = false;
            });
    }
    catch (...)
    {
        update_progress("failed", 0, 0);
        running_ = false;
        throw;
    }
}

void ImportOrchestrator::stop()
{
    // Cancellation must reach a synchronous import before waiting for its gate.
    cancel_ = true;
    std::lock_guard execution(execution_mutex_);
    stop_locked();
}

void ImportOrchestrator::stop_locked()
{
    cancel_ = true;
    if (import_thread_.joinable())
    {
        kforge::util::log_info{}("Shutdown: waiting for import thread...");
        import_thread_.join();
        kforge::util::log_info{}("Shutdown: import thread done");
    }
}

services::ImportPipeline::Result ImportOrchestrator::run_now(bool force)
{
    std::unique_lock execution(execution_mutex_, std::try_to_lock);
    if (!execution.owns_lock())
    {
        services::ImportPipeline::Result busy;
        busy.error_count = 1;
        busy.errors.push_back("Import already running; wait for the current import to finish");
        return busy;
    }
    stop_locked();
    cancel_ = false;
    sym_count_ = 0;
    fp_count_ = 0;
    update_progress("scanning", 0, 0);
    running_ = true;
    try
    {
        auto result = run_import(*db_, config_, &cancel_, matching_store_, force,
            services::progress{[this](const auto& phase, int completed, int total)
            {
                update_progress(phase, completed, total);
            }});
        sym_count_ = result.symbols;
        fp_count_ = result.footprints;
        log_result(result, "Import");
        if (result.ok() || committed_something(result))
        {
            // 完成信号只反映真实提交（成功完成或部分提交）；整体失败不推进
            import_seq_.fetch_add(1, std::memory_order_relaxed);
            db_->bump_data_version();
            if (committed_something(result))
                services::note_successful_edit();
        }
        finish_progress(result);
        running_ = false;
        return result;
    }
    catch (...)
    {
        update_progress("failed", 0, 0);
        running_ = false;
        throw;
    }
}

}  // namespace kforge::api
