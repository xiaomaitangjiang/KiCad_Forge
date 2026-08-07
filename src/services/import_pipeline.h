// Pipeline-style import — chain steps with operator|
//
// Usage:
//   auto result = ImportPipeline(db)
//       | symbols_from{dir, lib_id}
//       | footprints_from{dir}
//       | models_from{dir}
//       | with_auto_link{}
//       | with_3d_linking{}
//       | progress{[](auto& p, int d, int t) { LOG_INFO("{} {}/{}", p, d, t); }}
//       | execute;
#pragma once

#include <functional>
#include <string>
#include <vector>

struct sqlite3;

namespace kforge::services {

// ---- Step types (lightweight data bags) ----
struct symbols_from { std::string dir; std::string comp_lib_id; };
struct footprints_from { std::string dir; };
struct models_from { std::string dir; };
struct with_auto_link {};
struct with_3d_linking {};
struct progress { std::function<void(const std::string& phase, int done, int total)> fn; };

// ---- Pipeline class ----
class ImportPipeline {
public:
    struct Result { int symbols = 0, footprints = 0, models_3d = 0, linked_symbols = 0, linked_models = 0; };

    explicit ImportPipeline(sqlite3* db) : db_(db) {}

    ImportPipeline& operator|(symbols_from src);
    ImportPipeline& operator|(footprints_from src);
    ImportPipeline& operator|(models_from src);
    ImportPipeline& operator|(with_auto_link);
    ImportPipeline& operator|(with_3d_linking);
    ImportPipeline& operator|(progress p);

    struct execute_t {};
    Result operator|(execute_t);

private:
    sqlite3* db_;

    // Pending steps
    std::vector<symbols_from> pending_symbols_;
    std::vector<footprints_from> pending_footprints_;
    std::vector<models_from> pending_models_;
    bool do_auto_link_ = false;
    bool do_3d_linking_ = false;
    progress progress_fn_;

    // Internal implementation (moved from LibraryService)
    int import_symbol_library(const std::string& path, const std::string& comp_lib_id);
    int import_footprint(const std::string& path);
    struct ImportStats { int symbols, footprints, errors; std::vector<std::string> messages; };
    ImportStats import_directory(const std::string& dir, const std::string& comp_lib_id = "");
    int scan_3d_models(const std::string& dir);
    void try_link_symbol_footprint(const std::string& sym_id, const std::string& fp_ref);
    int do_link_3d_models();
};

inline constexpr ImportPipeline::execute_t execute{};

}  // namespace kforge::services
