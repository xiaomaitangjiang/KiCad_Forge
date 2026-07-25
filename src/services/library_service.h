#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "util/result.h"
#include "core/symbol.h"
#include "core/footprint.h"
#include "core/types.h"

namespace kforge::storage { class Database; }

namespace kforge::services {

/// Scans directories, parses KiCad files, and populates the database
/// along with relationship tables.
class LibraryService {
public:
    explicit LibraryService(storage::Database* db);

    /// Import a .kicad_sym symbol library file into the database.
    /// Symbols get deduplicated by name, footprints are linked if matching
    /// footprints already exist in the DB.
    util::Result<int> import_symbol_library(const std::filesystem::path& path);

    /// Import a .kicad_mod footprint file into the database.
    util::Result<int> import_footprint(const std::filesystem::path& path);

    /// Scan a directory for .kicad_sym files and import all of them.
    struct ImportStats { int symbols; int footprints; int errors; int models_3d; std::vector<std::string> messages; };
    util::Result<ImportStats> import_directory(const std::filesystem::path& dir);

    /// Scan a directory for 3D model files (.step, .wrl, .iges) and register them.
    util::Result<int> scan_3d_models(const std::filesystem::path& dir);

    /// Heuristic: for each symbol with a footprint_ref, try to find a 3D model
    /// whose filename partially matches the footprint name or symbol name.
    util::Result<int> link_3d_models_to_symbols();

    /// Set target library for subsequent imports (empty = auto-create from file name)
    void set_target_library(const std::string& id) { sym_target_library_ = id; }

    /// Merge a .kicad_sym file's symbols into an existing library file.
    /// Appends symbols and writes back using the S-expression writer.
    /// Returns number of symbols appended.
    util::Result<int> merge_into_library(const std::filesystem::path& source_sym,
                                          const std::filesystem::path& target_sym);

    /// Delete a library: removes symbols from DB, deletes .kicad_sym file, drops DB record.
    struct DeleteLibResult { int symbols_removed; std::string deleted_file; };
    util::Result<DeleteLibResult> delete_library(const core::Uuid& lib_id);

    /// Delete a symbol: removes from DB and rewrites the .kicad_sym file without it.
    util::Result<void> delete_symbol(const core::Uuid& sym_id);

    /// Get basic stats from the database.
    int symbol_count() const;
    int footprint_count() const;
    int model_3d_count() const;

private:
    void try_link_symbol_footprint(const core::Symbol& sym);

    storage::Database* db_;
    core::Uuid sym_target_library_;
};

}  // namespace kforge::services
