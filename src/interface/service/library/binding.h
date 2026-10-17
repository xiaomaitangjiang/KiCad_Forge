// Symbol-Footprint-Model binding manager
// OOP interface for callers, DOD internals (contiguous storage + hash indices)
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "util/error.h"

namespace kforge::storage
{
class Database;
}

namespace kforge::core
{
struct Footprint;
struct Model3D;
struct Symbol;
}  // namespace kforge::core

namespace kforge::services
{

class SymbolBindingManager
{
public:
    explicit SymbolBindingManager(storage::Database& db);

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

    // Update the source reference and DB links; roll back DB changes if the
    // source cannot be replaced. Models are read from the selected footprint.
    util::Result<void> assign_footprint(const std::string& symbol_id,
                                        const std::string& footprint_id);

    // Assign 3D model to footprint (replaces existing link). Validates both
    // objects first and swaps the links inside a transaction — a failure
    // leaves the previous links untouched.
    util::Result<void> assign_model(const std::string& footprint_id,
                                    const std::string& model_id);

    // Search from in-memory index, no DB queries
    struct FootprintRef
    {
        std::string id;
        std::string name;
        std::string library;  // .pretty library name (for "Lib:Name" footprint refs)
        double score;         // similarity score (0-1) when anchor is used, else 1.0
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
        double score;  // similarity score (0-1) when anchor is used, else 1.0
    };
    std::vector<ModelRef> search_models(const std::string& query, int limit = 20,
                                        const std::string& anchor = "") const;

    struct SymbolRef
    {
        std::string id;
        std::string name;
        double score;  // similarity score (0-1) when anchor is used, else 1.0
    };
    std::vector<SymbolRef> search_symbols(const std::string& query, int limit = 20,
                                          const std::string& anchor = "") const;

private:
    // ---- DOD internals ----
    // 只借连接持有者，不缓存裸 sqlite3*：每次操作自取 session
    storage::Database* db_;

    // Contiguous storage
    std::vector<core::Footprint> footprints_;
    std::vector<core::Model3D> models_;
    std::vector<core::Symbol> symbols_;

    // Hash indices: name/stem → offset into the vector
    std::unordered_map<std::string, size_t> fp_by_name_;
    std::unordered_map<std::string, size_t> model_by_stem_;
};

}  // namespace kforge::services
