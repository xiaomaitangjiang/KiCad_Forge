#include "api/import_orchestrator.h"
#include "storage/repositories.h"
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
    sym_count_ = 0;
    fp_count_ = 0;

    import_thread_ = std::thread(
        [this]()
        {
            try
            {
                storage::ComponentLibraryRepository cl_repo(db_);
                auto libs = cl_repo.find_enabled();

                services::ImportPipeline pipe(db_);
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
                            LOG_INFO("Import: adding footprints from {}", l.footprint_path);
                            pipe | services::footprints_from{l.footprint_path};
                        }
                        if (!l.model_3d_path.empty())
                        {
                            LOG_INFO("Import: adding 3D models from {}", l.model_3d_path);
                            pipe | services::models_from{l.model_3d_path};
                        }
                    }
                }
                else
                {
                    storage::SettingsRepository settings(db_);
                    auto sym = settings.get("symbol_lib_path");
                    auto fp = settings.get("footprint_lib_path");
                    auto m3d = settings.get("model_3d_path");
                    if (sym && !sym->empty())
                    {
                        pipe | services::symbols_from{.dir = *sym, .comp_lib_id = ""};
                    }
                    if (fp && !fp->empty())
                    {
                        pipe | services::footprints_from{*fp};
                    }
                    if (m3d && !m3d->empty())
                    {
                        pipe | services::models_from{*m3d};
                    }
                }

                auto result = pipe | services::execute;
                sym_count_ = result.symbols;
                fp_count_ = result.footprints;

                storage::SettingsRepository settings(db_);
                auto sr = settings.set(
                    "last_import_ts",
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                if (!sr)
                {
                    LOG_ERROR("Failed to save last_import_ts: {}", util::error_formatter(sr.error()));
                }
                LOG_INFO("Auto-import done: {} symbols, {} footprints", sym_count_.load(),
                         fp_count_.load());
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Auto-import exception: {}", e.what());
            }
            catch (...)
            {
                LOG_ERROR("Auto-import unknown exception");
            }
            running_ = false;

            // Kick off async post-processing (auto-link + 3D link)
            if (!cancel_.load(std::memory_order_relaxed))
            {
                async_link();
            }
        });
}

void ImportOrchestrator::stop()
{
    cancel_ = true;
    if (import_thread_.joinable())
    {
        LOG_INFO("Shutdown: waiting for import thread...");
        import_thread_.join();
        LOG_INFO("Shutdown: import thread done");
    }
    if (link_thread_.joinable())
    {
        LOG_INFO("Shutdown: waiting for link thread...");
        link_thread_.join();
        LOG_INFO("Shutdown: link thread done");
    }
}

services::ImportPipeline::Result ImportOrchestrator::run_now()
{
    storage::ComponentLibraryRepository cl_repo(db_);
    auto libs = cl_repo.find_enabled();

    services::ImportPipeline pipe(db_);
    if (libs && !libs->empty())
    {
        for (auto& l : *libs)
        {
            if (!l.symbol_path.empty())
                pipe | services::symbols_from{l.symbol_path, l.id};
            if (!l.footprint_path.empty() && l.footprint_path != l.symbol_path)
                pipe | services::footprints_from{l.footprint_path};
            if (!l.model_3d_path.empty())
                pipe | services::models_from{l.model_3d_path};
        }
    }
    else
    {
        storage::SettingsRepository settings(db_);
        auto sym = settings.get("symbol_lib_path");
        auto fp = settings.get("footprint_lib_path");
        auto m3d = settings.get("model_3d_path");
        if (sym && !sym->empty())
            pipe | services::symbols_from{*sym, ""};
        if (fp && !fp->empty())
            pipe | services::footprints_from{*fp};
        if (m3d && !m3d->empty())
            pipe | services::models_from{*m3d};
    }

    auto result = pipe | services::execute;

    storage::SettingsRepository settings(db_);
    auto sr =
        settings.set("last_import_ts",
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!sr)
    {
        LOG_ERROR("Failed to save last_import_ts: {}", util::error_formatter(sr.error()));
    }

    // Post-processing runs async — response returns immediately
    async_link();
    return result;
}

void ImportOrchestrator::async_link()
{
    if (cancel_.load(std::memory_order_relaxed))
        return;
    if (link_thread_.joinable())
        link_thread_.join();

    linking_ = true;
    link_thread_ = std::thread(
        [this]()
        {
            LOG_INFO("Async link: starting...");
            try
            {
                services::ImportPipeline pipe(db_, &cancel_);
                pipe | services::with_3d_linking{};
                auto result = pipe | services::execute;
                LOG_INFO("Async link done: {} symbol->footprint, {} footprint->3D model",
                         result.linked_symbols, result.linked_models);
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Async link exception: {}", e.what());
            }
            catch (...)
            {
                LOG_ERROR("Async link unknown exception");
            }
            linking_ = false;
        });
}

}  // namespace kforge::api
