#include "correspondence/checker.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <vector>

namespace kforge::correspondence {

int CorrespondenceChecker::levenshtein_distance(const std::string& a,
                                                 const std::string& b) {
    size_t m = a.size(), n = b.size();
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));

    for (size_t i = 0; i <= m; i++) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= n; j++) dp[0][j] = static_cast<int>(j);

    for (size_t i = 1; i <= m; i++) {
        for (size_t j = 1; j <= n; j++) {
            int cost = (std::tolower(a[i - 1]) == std::tolower(b[j - 1])) ? 0 : 1;
            dp[i][j] = std::min({dp[i - 1][j] + 1,
                                 dp[i][j - 1] + 1,
                                 dp[i - 1][j - 1] + cost});
        }
    }
    return dp[m][n];
}

double CorrespondenceChecker::name_similarity(const std::string& a,
                                               const std::string& b) {
    if (a.empty() || b.empty()) return 0.0;
    int dist = levenshtein_distance(a, b);
    size_t max_len = std::max(a.size(), b.size());
    return 1.0 - static_cast<double>(dist) / static_cast<double>(max_len);
}

std::vector<Issue> CorrespondenceChecker::check(
    const std::vector<core::Symbol>& symbols,
    const std::vector<core::Footprint>& footprints,
    const std::vector<core::Model3D>& models,
    const std::unordered_map<core::Uuid, core::Uuid>& symbol_to_fp,
    const std::unordered_map<core::Uuid, std::vector<core::Uuid>>& fp_to_models) {

    std::vector<Issue> issues;

    // Build index: footprint name → footprint
    std::unordered_map<std::string, const core::Footprint*> fp_by_name;
    for (const auto& fp : footprints) {
        fp_by_name[fp.name()] = &fp;
    }

    // Check each symbol
    for (const auto& sym : symbols) {
        // Skip power symbols
        if (sym.is_power()) continue;

        auto fp_link = symbol_to_fp.find(sym.id());
        bool has_fp = (fp_link != symbol_to_fp.end());

        if (sym.footprint().empty() && !has_fp) {
            // No footprint assigned at all
            Issue issue;
            issue.type = Issue::Type::EmptyFootprintField;
            issue.severity = Issue::Severity::Warning;
            issue.entity_id = sym.id();
            issue.entity_name = sym.name();
            issue.message = "Symbol '" + sym.name() +
                            "' has no footprint assigned";
            issue.suggested_fix = "Assign a footprint or use auto-match";
            issues.push_back(issue);
        } else if (!sym.footprint().empty() && !has_fp &&
                   fp_by_name.find(sym.footprint()) == fp_by_name.end()) {
            // Footprint field set but file not found
            Issue issue;
            issue.type = Issue::Type::MissingFootprint;
            issue.severity = Issue::Severity::Error;
            issue.entity_id = sym.id();
            issue.entity_name = sym.name();
            issue.message = "Symbol '" + sym.name() +
                            "' references footprint '" + sym.footprint() +
                            "' but it was not found in any library";
            issue.suggested_fix = "Create footprint or update symbol property";
            issues.push_back(issue);
        }
    }

    // Check footprints for 3D models
    for (const auto& fp : footprints) {
        auto model_link = fp_to_models.find(fp.id());
        bool has_models = (model_link != fp_to_models.end() &&
                           !model_link->second.empty());

        if (!has_models) {
            Issue issue;
            issue.type = Issue::Type::MissingModel3D;
            issue.severity = Issue::Severity::Info;
            issue.entity_id = fp.id();
            issue.entity_name = fp.name();
            issue.message = "Footprint '" + fp.name() +
                            "' has no 3D model assigned";
            issue.suggested_fix = "Download or create a 3D model";
            issues.push_back(issue);
        }
    }

    // Check for orphan footprints (not linked to any symbol)
    std::unordered_set<core::Uuid> linked_fps;
    for (const auto& [sym_id, fp_id] : symbol_to_fp) {
        linked_fps.insert(fp_id);
    }
    for (const auto& fp : footprints) {
        if (linked_fps.find(fp.id()) == linked_fps.end()) {
            Issue issue;
            issue.type = Issue::Type::OrphanFootprint;
            issue.severity = Issue::Severity::Info;
            issue.entity_id = fp.id();
            issue.entity_name = fp.name();
            issue.message = "Footprint '" + fp.name() +
                            "' is not referenced by any symbol";
            issue.suggested_fix = "Link to a symbol or remove if unused";
            issues.push_back(issue);
        }
    }

    // Check for orphan 3D models
    std::unordered_set<core::Uuid> linked_models;
    for (const auto& [fp_id, model_ids] : fp_to_models) {
        for (const auto& mid : model_ids) {
            linked_models.insert(mid);
        }
    }
    for (const auto& model : models) {
        if (linked_models.find(model.id()) == linked_models.end()) {
            Issue issue;
            issue.type = Issue::Type::OrphanModel;
            issue.severity = Issue::Severity::Info;
            issue.entity_id = model.id();
            issue.entity_name = model.file_path().string();
            issue.message = "3D model '" + model.file_path().string() +
                            "' is not referenced by any footprint";
            issue.suggested_fix = "Link to a footprint or remove if unused";
            issues.push_back(issue);
        }
    }

    return issues;
}

std::vector<MatchSuggestion> CorrespondenceChecker::suggest_matches(
    const std::vector<core::Symbol>& symbols,
    const std::vector<core::Footprint>& footprints) {

    std::vector<MatchSuggestion> suggestions;
    const double SIMILARITY_THRESHOLD = 0.5;

    for (const auto& sym : symbols) {
        if (sym.is_power()) continue;

        // Check if the footprint_ref actually matches an existing footprint
        if (!sym.footprint().empty()) {
            bool found = false;
            for (const auto& fp : footprints) {
                if (fp.name() == sym.footprint()) { found = true; break; }
            }
            if (found) continue;  // Footprint already exists — no need to match
        }

        MatchSuggestion best;
        best.symbol_id = sym.id();
        best.symbol_name = sym.name();
        best.score = 0.0;

        for (const auto& fp : footprints) {
            double score = name_similarity(sym.name(), fp.name());

            // Boost score if pin/pad counts match
            if (sym.pin_count() > 0 && sym.pin_count() == fp.pad_count()) {
                score = std::min(1.0, score + 0.2);
            }

            if (score > best.score) {
                best.footprint_id = fp.id();
                best.footprint_name = fp.name();
                best.score = score;
                if (score > 0.8) {
                    best.reason = "Name and pin count match";
                } else if (score > 0.5) {
                    best.reason = "Partial name match";
                }
            }
        }

        if (best.score >= SIMILARITY_THRESHOLD && !best.footprint_id.empty()) {
            suggestions.push_back(std::move(best));
        }
    }

    // Sort by score descending
    std::sort(suggestions.begin(), suggestions.end(),
              [](const MatchSuggestion& a, const MatchSuggestion& b) {
                  return a.score > b.score;
              });

    return suggestions;
}

const char* issue_type_to_string(Issue::Type type) {
    switch (type) {
    case Issue::Type::MissingFootprint:    return "Missing Footprint";
    case Issue::Type::MissingModel3D:      return "Missing 3D Model";
    case Issue::Type::EmptyFootprintField: return "No Footprint Assigned";
    case Issue::Type::OrphanModel:         return "Orphan 3D Model";
    case Issue::Type::OrphanFootprint:     return "Orphan Footprint";
    case Issue::Type::PinMismatch:         return "Pin/Pad Mismatch";
    case Issue::Type::NameMismatch:        return "Name Mismatch";
    default: return "Unknown";
    }
}

const char* issue_severity_to_string(Issue::Severity sev) {
    switch (sev) {
    case Issue::Severity::Error:   return "Error";
    case Issue::Severity::Warning: return "Warning";
    case Issue::Severity::Info:    return "Info";
    default: return "Unknown";
    }
}

}  // namespace kforge::correspondence
