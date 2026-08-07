#include "api/import_orchestrator.h"

#include "storage/repositories.h"
#include "util/logger.h"

#include <chrono>

namespace kforge::api {

ImportOrchestrator::~ImportOrchestrator() { stop(); }

void ImportOrchestrator::start() {
    if (running_.load(std::memory_order_relaxed)) return;

    running_ = true;
    sym_count_ = 0;
    fp_count_ = 0;

    thread_ = std::thread([this]() {
        try {
            services::ImportPipeline pipe(db_);

            // Read component libraries and build pipeline
            storage::ComponentLibraryRepository cl_repo(db_);
            auto libs = cl_repo.find_enabled();

            if (libs && !libs->empty()) {
                for (auto& l : *libs) {
                    if (!l.symbol_path.empty())
                        pipe | services::symbols_from{l.symbol_path, l.id};
                    if (!l.footprint_path.empty() && l.footprint_path != l.symbol_path)
                        pipe | services::footprints_from{l.footprint_path};
                    if (!l.model_3d_path.empty())
                        pipe | services::models_from{l.model_3d_path};
                }
            } else {
                // Legacy fallback
                storage::SettingsRepository settings(db_);
                auto sym = settings.get("symbol_lib_path");
                auto fp = settings.get("footprint_lib_path");
                auto m3d = settings.get("model_3d_path");
                if (sym && !sym->empty())
                    pipe | services::symbols_from{*sym, ""};
                if (fp && !fp->empty() && *fp != (sym ? *sym : ""))
                    pipe | services::footprints_from{*fp};
                if (m3d && !m3d->empty())
                    pipe | services::models_from{*m3d};
            }

            // Only auto-link if still running (user hasn't closed window)
            if (!cancel_.load(std::memory_order_relaxed)) {
                pipe | services::with_auto_link{};
                pipe | services::with_3d_linking{};
            }

            auto result = pipe | services::execute;
            sym_count_ = result.symbols;
            fp_count_ = result.footprints;

            storage::SettingsRepository settings(db_);
            settings.set("last_import_ts",
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            LOG_INFO("Auto-import done: {} symbols, {} footprints",
                     sym_count_.load(), fp_count_.load());
        } catch (const std::exception& e) {
            LOG_ERROR("Auto-import exception: {}", e.what());
        } catch (...) {
            LOG_ERROR("Auto-import unknown exception");
        }
        running_ = false;
    });
}

void ImportOrchestrator::stop() {
    cancel_ = true;
    if (thread_.joinable()) {
        LOG_INFO("Shutdown: waiting for import thread...");
        thread_.join();
        LOG_INFO("Shutdown: import thread done");
    }
}

services::ImportPipeline::Result ImportOrchestrator::run_now() {
    services::ImportPipeline pipe(db_);
    storage::ComponentLibraryRepository cl_repo(db_);
    auto libs = cl_repo.find_enabled();

    if (libs && !libs->empty()) {
        for (auto& l : *libs) {
            if (!l.symbol_path.empty())
                pipe | services::symbols_from{l.symbol_path, l.id};
            if (!l.footprint_path.empty() && l.footprint_path != l.symbol_path)
                pipe | services::footprints_from{l.footprint_path};
            if (!l.model_3d_path.empty())
                pipe | services::models_from{l.model_3d_path};
        }
    } else {
        storage::SettingsRepository settings(db_);
        auto sym = settings.get("symbol_lib_path");
        if (sym && !sym->empty()) pipe | services::symbols_from{*sym, ""};
    }

    pipe | services::with_auto_link{} | services::with_3d_linking{};
    auto result = pipe | services::execute;

    storage::SettingsRepository settings(db_);
    settings.set("last_import_ts",
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    return result;
}

}  // namespace kforge::api
