#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "util/result.h"
#include "core/symbol.h"
#include "core/footprint.h"
#include "core/types.h"

namespace kforge::storage { class Database; }

namespace kforge::services {

/// File-level library operations — delete, merge, patch, stats.
/// Import logic has been moved to ImportPipeline; bridge wrappers kept for compatibility.
class LibraryService {
public:
    explicit LibraryService(storage::Database* db);

    // -- Import bridges (delegate to ImportPipeline) --
    util::Result<int> import_symbol_library(const std::filesystem::path& path,
                                             const std::string& component_library_id = "");
    util::Result<int> import_footprint(const std::filesystem::path& path);
    struct ImportStats { int symbols = 0, footprints = 0, errors = 0, models_3d = 0; std::vector<std::string> messages; };
    util::Result<ImportStats> import_directory(const std::filesystem::path& dir,
                                               const std::string& component_library_id = "");
    util::Result<int> scan_3d_models(const std::filesystem::path& dir);
    util::Result<int> link_3d_models_to_symbols();

    // -- Core operations --
    util::Result<int> merge_into_library(const std::filesystem::path& source_sym,
                                          const std::filesystem::path& target_sym);
    struct DeleteLibResult { int symbols_removed; std::string deleted_file; };
    util::Result<DeleteLibResult> delete_library(const core::Uuid& lib_id);
    util::Result<void> delete_symbol(const core::Uuid& sym_id);
    int patch_kf_id_to_file(const std::filesystem::path& sym_file,
                            const std::unordered_map<std::string, std::string>& name_to_kf_id);
    int symbol_count() const;
    int footprint_count() const;
    int model_3d_count() const;

private:
    storage::Database* db_;
};

}  // namespace kforge::services
