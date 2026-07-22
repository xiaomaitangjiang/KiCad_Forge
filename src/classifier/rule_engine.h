#pragma once

#include <string>
#include <vector>

#include "classifier/rule.h"
#include "core/symbol.h"
#include "core/footprint.h"
#include "core/component.h"

namespace kforge::classifier {

/// Evaluates classification rules against components.
///
/// Usage:
///   RuleEngine engine(loaded_rules);
///   auto results = engine.evaluate(component);
///   results sorted by (priority, confidence) descending
class RuleEngine {
public:
    explicit RuleEngine(std::vector<Rule> rules);

    /// Evaluate a single component against all rules.
    /// Returns matching results sorted by priority (best first).
    std::vector<ClassificationResult> evaluate(
        const core::Component& component) const;

    /// Evaluate a symbol against rules (without full Component).
    std::vector<ClassificationResult> evaluate_symbol(
        const core::Symbol& symbol) const;

    /// Batch evaluate multiple components.
    std::vector<std::vector<ClassificationResult>> evaluate_batch(
        const std::vector<core::Component>& components) const;

    /// Reload rules at runtime.
    void set_rules(std::vector<Rule> rules);
    const std::vector<Rule>& rules() const { return rules_; }

    /// Simple glob/wildcard matching (* ?) — public for visitor access.
    static bool match_glob(const std::string& pattern, const std::string& text);

private:
    bool evaluate_condition(const Condition& cond,
                            const core::Component& comp) const;
    bool evaluate_condition_symbol(const Condition& cond,
                                   const core::Symbol& sym) const;

    std::vector<Rule> rules_;
};

/// Infer component type from KiCad reference prefix (R=Resistor, C=Capacitor, etc.)
core::ComponentType guess_type_from_name(const std::string& name);

/// Convert ComponentType enum to human-readable string.
const char* component_type_to_string(core::ComponentType t);

}  // namespace kforge::classifier
