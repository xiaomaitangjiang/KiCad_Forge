#pragma once

#include <string>
#include <vector>

#include "core/types.h"
#include "core/symbol.h"
#include "core/footprint.h"
#include "core/model_3d.h"
#include "core/component.h"

namespace kforge::correspondence {

/// Issue found during correspondence checking.
struct Issue {
    enum class Severity { Error, Warning, Info };
    enum class Type {
        MissingFootprint,      // Symbol has footprint field but file not found
        MissingModel3D,        // Footprint references model, file not found
        EmptyFootprintField,   // Symbol has no footprint assigned
        OrphanModel,           // 3D model not referenced by any footprint
        OrphanFootprint,       // Footprint not referenced by any symbol
        PinMismatch,           // Symbol pins != footprint pads
        NameMismatch,          // Footprint property doesn't match any known fp
    };

    Severity severity{Severity::Warning};
    Type type{Type::MissingFootprint};
    core::Uuid entity_id;
    std::string entity_name;
    std::string message;
    std::string suggested_fix;
    bool resolved{false};
};

/// Heuristic matcher for symbol→footprint name similarity.
struct MatchSuggestion {
    core::Uuid symbol_id;
    core::Uuid footprint_id;
    std::string symbol_name;
    std::string footprint_name;
    double score{0.0};     // 0.0 to 1.0
    std::string reason;    // Human-readable explanation
};

/// Checks symbol-footprint-3D model correspondence.
class CorrespondenceChecker {
public:
    /// Run all checks and return issues found.
    /// @param symbols All known symbols
    /// @param footprints All known footprints
    /// @param models All known 3D models
    /// @param symbol_to_fp Map of symbol_id → footprint_id
    /// @param fp_to_models Map of footprint_id → vector<model_id>
    static std::vector<Issue> check(
        const std::vector<core::Symbol>& symbols,
        const std::vector<core::Footprint>& footprints,
        const std::vector<core::Model3D>& models,
        const std::unordered_map<core::Uuid, core::Uuid>& symbol_to_fp,
        const std::unordered_map<core::Uuid, std::vector<core::Uuid>>& fp_to_models);

    /// Suggest footprint matches for symbols without explicit Footprint field.
    static std::vector<MatchSuggestion> suggest_matches(
        const std::vector<core::Symbol>& symbols,
        const std::vector<core::Footprint>& footprints);

    /// Compute similarity between two names (0.0 to 1.0).
    static double name_similarity(const std::string& a, const std::string& b);

private:
    static int levenshtein_distance(const std::string& a, const std::string& b);
};

/// Converts issue type to display string.
const char* issue_type_to_string(Issue::Type type);

/// Converts issue severity to display string.
const char* issue_severity_to_string(Issue::Severity sev);

}  // namespace kforge::correspondence
