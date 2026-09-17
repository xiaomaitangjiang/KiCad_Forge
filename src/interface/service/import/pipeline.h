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
#include "core/model/symbol.h"
#include "core/model/footprint.h"
#include "core/model/model_3d.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct sqlite3;

namespace kforge::services
{

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
    std::function<void(const std::string& phase, int done, int total)> fn;
};

// ---- Pipeline class ----
class ImportPipeline
{
public:
    struct Result
    {
        int symbols = 0, footprints = 0, models_3d = 0, skipped = 0, linked_symbols = 0,
            linked_models = 0;
    };

    explicit ImportPipeline(sqlite3* db, std::atomic<bool>* cancel = nullptr,
                           bool write_kf_id_to_file = false)
        : db_(db), cancel_(cancel), write_kf_id_to_file_(write_kf_id_to_file)
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
    sqlite3* db_;
    std::atomic<bool>* cancel_ = nullptr;

    // Pending steps
    std::vector<symbols_from> pending_symbols_;
    std::vector<footprints_from> pending_footprints_;
    std::vector<models_from> pending_models_;
    progress progress_fn_;

    // Incremental import state is per-file mtime snapshots in the imported_files table

    // 3D model roots — for resolving ${KICADx_3DMODEL_DIR} references
    std::vector<std::string> model_roots_;

    // Models imported from explicit footprint refs (counted into Result::models_3d)
    int model_ref_imported_ = 0;

    // Patch Kicad_Forge_ID property back into .kicad_sym on successful write.
    // Default false — official KiCad files must NEVER be touched without explicit opt-in.
    bool write_kf_id_to_file_ = false;

    // Internal implementation (moved from LibraryService)
    IMP_Result import_symbol_library(const std::string& path, const std::string& comp_lib_id);
    IMP_Result import_footprint(const std::string& path);

    // Symbol library import split: parallel-safe parse (no DB access) + serial
    // DB write — the pipeline must never touch the single sqlite3* connection
    // from more than one thread at a time
    struct ParsedSymbolLib
    {
        std::filesystem::path path;
        std::string lib_name;
        std::vector<core::Symbol> items;
    };
    std::optional<ParsedSymbolLib> parse_symbol_library(const std::string& path);
    IMP_Result write_symbol_library(ParsedSymbolLib& lib,
                                    const std::string& comp_lib_id);

    // Footprint import split: parallel-safe parse (no DB access) + serial DB write
    std::optional<core::Footprint> parse_footprint_file(const std::filesystem::path& path);
    IMP_Result write_footprint(const std::filesystem::path& path,
                               const core::Footprint& fp);

    // In-memory pools, loaded once before the footprint phase — replace
    // per-file find_by_name / find_by_path queries
    std::unordered_map<std::string, core::Footprint> fp_by_name_;
    std::unordered_map<std::string, core::Model3D> models_by_path_;

    struct ImportCounts
    {
        int symbols = 0;
        int footprints = 0;
        int skipped = 0;
    };
    using ImportDirResult = util::StatusResult<ImportStatus, ImportCounts>;
    ImportDirResult import_directory(const std::string& dir, const std::string& comp_lib_id = "");
    int scan_3d_models(const std::string& dir);

    // In-memory mtime snapshots, loaded once per execute (guarded: symbol import
    // runs in parallel worker threads)
    std::unordered_map<std::string, int64_t> file_mtimes_;
    std::mutex mtimes_mtx_;

    // Per-file mtime snapshot: true when the file is unchanged since the last import
    bool file_unchanged(const std::filesystem::path& p);
    // Record the file's current mtime as imported
    void record_file(const std::filesystem::path& p);
    // True when the orchestrator requested a stop
    bool cancelled() const { return cancel_ && cancel_->load(std::memory_order_relaxed); }
    // Resolve a footprint's 3D model reference to an existing absolute path
    std::filesystem::path resolve_model_path(std::string_view ref,
                                             const std::filesystem::path& fp_dir) const;
};

inline constexpr ImportPipeline::execute_t execute{};

}  // namespace kforge::services