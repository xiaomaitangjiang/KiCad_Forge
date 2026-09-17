// Symbol-Footprint-Model binding manager
// OOP interface for callers, DOD internals (contiguous storage + hash indices)
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "util/error.h"

struct sqlite3;

namespace kforge::core
{
struct Footprint;
struct Model3D;
}  // namespace kforge::core

namespace kforge::services
{

class SymbolBindingManager
{
public:
    explicit SymbolBindingManager(sqlite3* db);

    // ---- OOP interface ----

    struct Binding
    {
        std::string fp_id;
        std::string fp_name;
        std::string model_id;
        std::string model_name;
    };

    // Query current binding for a symbol
    Binding get(const std::string& symbol_id) const;

    // Assign footprint to symbol (replaces existing link). Syncs the
    // .kicad_sym source file first and auto-links the new footprint's 3D
    // models; fails (without touching the DB) when the file cannot be updated.
    util::Result<void> assign_footprint(const std::string& symbol_id,
                                        const std::string& footprint_id);

    // Assign 3D model to footprint (replaces existing link)
    void assign_model(const std::string& footprint_id, const std::string& model_id);

    // Search from in-memory index, no DB queries
    struct FootprintRef
    {
        std::string id;
        std::string name;
        int pad_count;
    };
    // `query` filters by substring; when query is empty and `anchor` is given,
    // returns the Top-N most relevant candidates for that value instead of
    // the first N rows (which would otherwise be a fixed, useless batch)
    // (param named `anchor` because `near` is a Windows legacy macro)
    std::vector<FootprintRef> search_footprints(const std::string& query, int limit = 20,
                                                const std::string& anchor = "") const;

    struct ModelRef
    {
        std::string id;
        std::string name;
        std::string format;
    };
    std::vector<ModelRef> search_models(const std::string& query, int limit = 20,
                                        const std::string& anchor = "") const;

private:
    // ---- DOD internals ----
    sqlite3* db_;

    // Contiguous storage
    std::vector<core::Footprint> footprints_;
    std::vector<core::Model3D> models_;

    // Hash indices: name/stem → offset into the vector
    std::unordered_map<std::string, size_t> fp_by_name_;
    std::unordered_map<std::string, size_t> model_by_stem_;
};

}  // namespace kforge::services
