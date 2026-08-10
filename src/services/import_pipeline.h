// Pipeline-style import — chain steps with operator|
//
// Usage:
//   auto result = ImportPipeline(db)
//       | symbols_from{dir, lib_id}
//       | footprints_from{dir}
//       | models_from{dir}
//       | progress{[](auto& p, int d, int t) { LOG_INFO("{} {}/{}", p, d, t); }}
//       | execute;
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "util/result.h"

struct sqlite3;

namespace kforge::services {

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
    return {.status = ImportStatus::Added, .error = {}};
}
inline IMP_Result imp_skipped()
{
    return {.status = ImportStatus::Skipped, .error = {}};
}
inline IMP_Result imp_failed(std::string msg)
{
    return {.status = ImportStatus::Failed, .error = std::move(msg)};
}

// ---- Step types (lightweight data bags) ----
struct symbols_from { std::string dir; std::string comp_lib_id; };
struct footprints_from { std::string dir; };
struct models_from { std::string dir; };
struct progress{ std::function<void(const std::string& phase, int done, int total)> fn; };

// ---- Pipeline class ----
class ImportPipeline {
public:
    struct Result { int symbols = 0, footprints = 0, models_3d = 0, linked_symbols = 0, linked_models = 0; };

    explicit ImportPipeline(sqlite3* db, std::atomic<bool>* cancel = nullptr)
        : db_(db), cancel_(cancel) {}

    ImportPipeline& operator|(symbols_from src);
    ImportPipeline& operator|(footprints_from src);
    ImportPipeline& operator|(models_from src);
    ImportPipeline& operator|(progress p);

    struct execute_t {};
    Result operator|(execute_t);

private:
    sqlite3* db_;
    std::atomic<bool>* cancel_ = nullptr;
    std::unordered_map<std::string, std::string> model_name_index_;

    // Pending steps
    std::vector<symbols_from> pending_symbols_;
    std::vector<footprints_from> pending_footprints_;
    std::vector<models_from> pending_models_;
    progress progress_fn_;

    // Internal implementation (moved from LibraryService)
    int import_symbol_library(const std::string& path, const std::string& comp_lib_id);
    IMP_Result import_footprint(const std::string& path);
    struct ImportStats { int symbols = 0, footprints = 0, errors = 0; std::vector<std::string> messages; };
    ImportStats import_directory(const std::string& dir, const std::string& comp_lib_id = "");
    int scan_3d_models(const std::string& dir);
    void try_link_symbol_footprint(const std::string& sym_id, const std::string& fp_ref);
};

inline constexpr ImportPipeline::execute_t execute{};

}  // namespace kforge::services
