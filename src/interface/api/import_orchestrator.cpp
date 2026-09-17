#include "core/repo/repositories.h"
#include "interface/api/import_orchestrator.h"
#include "util/config_store.h"
#include "util/logger.h"
#include "util/result.h"

#include <chrono>


namespace kforge::api
{

ImportOrchestrator::~ImportOrchestrator()
{
    stop();
}

void ImportOrchestrator::start()
{
    if (running_.load(std::memory_order_relaxed))
    {
        return;
    }

    running_ = true;
    cancel_ = false;  // reset after a previous stop()
    sym_count_ = 0;
    fp_count_ = 0;

    import_thread_ = std::thread(
        [this]()
        {
            try
            {
                storage::ComponentLibraryRepository cl_repo(db_);
                auto libs = cl_repo.find_enabled();

                bool write_kf_id = config_ ? config_->get_bool("write_kf_id_to_file", false) : false;
                services::ImportPipeline pipe(db_, &cancel_, write_kf_id);
                if (libs && !libs->empty())
                {
                    for (auto& l : *libs)
                    {
                        if (!l.symbol_path.empty())
                        {
                            pipe | services::symbols_from{.dir = l.symbol_path, .comp_lib_id = l.id};
                        }
                        if (!l.footprint_path.empty() && l.footprint_path != l.symbol_path)
                        {
                            kforge::util::log_info{}("Import: adding footprints from {}",
                                                     l.footprint_path);
                            pipe | services::footprints_from{l.footprint_path};
                        }
                        if (!l.model_3d_path.empty())
                        {
                            kforge::util::log_info{}("Import: adding 3D models from {}",
                                                     l.model_3d_path);
                            pipe | services::models_from{l.model_3d_path};
                        }
                    }
                }

                auto result = pipe | services::execute;
                sym_count_ = result.symbols;
                fp_count_ = result.footprints;

                // In-process signal: MgrHolder watches this to refresh the
                // SymbolBindingManager in-memory index after each import.
                import_seq_.fetch_add(1, std::memory_order_relaxed);
                kforge::util::log_info{}("Auto-import done: +{} new symbols, +{} new footprints, "
                                         "{} skipped (up to date)",
                                         sym_count_.load(), fp_count_.load(), result.skipped);
            }
            catch (const std::exception& e)
            {
                kforge::util::log_error{}("Auto-import exception: {}", e.what());
            }
            catch (...)
            {
                kforge::util::log_error{}("Auto-import unknown exception");
            }
            running_ = false;
        });
}

void ImportOrchestrator::stop()
{
    cancel_ = true;
    if (import_thread_.joinable())
    {
        kforge::util::log_info{}("Shutdown: waiting for import thread...");
        import_thread_.join();
        kforge::util::log_info{}("Shutdown: import thread done");
    }
}

services::ImportPipeline::Result ImportOrchestrator::run_now()
{
    storage::ComponentLibraryRepository cl_repo(db_);
    auto libs = cl_repo.find_enabled();

    services::ImportPipeline pipe(db_, &cancel_);
    if (libs && !libs->empty())
    {
        for (auto& l : *libs)
        {
            if (!l.symbol_path.empty())
            {
                pipe | services::symbols_from{.dir = l.symbol_path, .comp_lib_id = l.id};
            }
            if (!l.footprint_path.empty() && l.footprint_path != l.symbol_path)
            {
                pipe | services::footprints_from{l.footprint_path};
            }
            if (!l.model_3d_path.empty())
            {
                pipe | services::models_from{l.model_3d_path};
            }
        }
    }

    auto result = pipe | services::execute;
    import_seq_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

}  // namespace kforge::api