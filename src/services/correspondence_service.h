#pragma once

#include <string>
#include <vector>

#include "util/result.h"
#include "correspondence/checker.h"

namespace kforge::storage { class Database; }

namespace kforge::services {

/// Runs correspondence checks and heuristic matching using relationship tables.
class CorrespondenceService {
public:
    explicit CorrespondenceService(storage::Database* db);

    /// Check all symbols, footprints, and models for missing links.
    /// Uses the relationship tables for explicit links; falls back to
    /// symbol footprint property and footprint model references.
    util::Result<std::vector<correspondence::Issue>> check_all();

    /// Run heuristic name+pin matching and return suggestions.
    util::Result<std::vector<correspondence::MatchSuggestion>> suggest_matches();

    /// Attempt to auto-link symbols to footprints based on heuristics.
    /// Actually writes matches to the relationship tables.
    util::Result<int> auto_link();  // returns number of links created

private:
    storage::Database* db_;
};

}  // namespace kforge::services
