#include "classifier/rule_engine.h"

#include <algorithm>
#include <cctype>
#include <regex>

#include "core/type_registry.h"

namespace kforge::classifier {

RuleEngine::RuleEngine(std::vector<Rule> rules) : rules_(std::move(rules)) {}

void RuleEngine::set_rules(std::vector<Rule> rules) {
    rules_ = std::move(rules);
}

bool RuleEngine::match_glob(const std::string& pattern,
                            const std::string& text) {
    // Convert glob to regex: * → .*, ? → .
    std::string regex_str;
    regex_str.reserve(pattern.size() + 4);
    regex_str += '^';
    for (char c : pattern) {
        switch (c) {
        case '*': regex_str += ".*"; break;
        case '?': regex_str += '.'; break;
        case '.': regex_str += "\\."; break;
        case '+': regex_str += "\\+"; break;
        case '\\': regex_str += "\\\\"; break;
        default:  regex_str += c; break;
        }
    }
    regex_str += '$';

    try {
        std::regex re(regex_str, std::regex::icase);
        return std::regex_match(text, re);
    } catch (...) {
        // Fallback: case-insensitive substring
        auto lower_pat = pattern;
        auto lower_txt = text;
        std::transform(lower_pat.begin(), lower_pat.end(),
                       lower_pat.begin(), ::tolower);
        std::transform(lower_txt.begin(), lower_txt.end(),
                       lower_txt.begin(), ::tolower);
        return lower_txt.find(lower_pat) != std::string::npos;
    }
}

// Visitor for evaluating a condition against a Component
struct ConditionEvaluator {
    const core::Component& comp;

    bool operator()(const MatchType& c) const {
        // Match component type name
        return match_name(comp);
    }
    bool operator()(const MatchPackage& c) const {
        return RuleEngine::match_glob(c.pattern, "unknown");
    }
    bool operator()(const ValueInRange& c) const {
        auto vr = comp.value_range();
        if (!vr) return false;
        if (c.unit != vr->unit) return false;
        return vr->min >= c.min && vr->max <= c.max;
    }
    bool operator()(const PinCountRange& c) const {
        return comp.pin_count() >= c.min && comp.pin_count() <= c.max;
    }
    bool operator()(const PropertyExists& c) const {
        return comp.property(c.key).has_value();
    }
    bool operator()(const PropertyMatch& c) const {
        auto val = comp.property(c.key);
        if (!val) return false;
        return RuleEngine::match_glob(c.pattern, *val);
    }

private:
    bool match_name(const core::Component& comp) const {
        // Match by component type name — delegate to component type
        // This is a simplified version
        return true;  // Always match for now — actual matching in evaluate()
    }
};

// Visitor for evaluating against a Symbol
struct SymbolConditionEvaluator {
    const core::Symbol& sym;

    bool operator()(const MatchType& c) const {
        // Check type via properties
        return true;
    }
    bool operator()(const MatchPackage& c) const {
        // Check footprint property
        return RuleEngine::match_glob(c.pattern, sym.footprint());
    }
    bool operator()(const ValueInRange& c) const {
        // Simplified — parse value string
        return false;
    }
    bool operator()(const PinCountRange& c) const {
        int pc = sym.pin_count();
        return pc >= c.min && pc <= c.max;
    }
    bool operator()(const PropertyExists& c) const {
        return sym.property(c.key).has_value();
    }
    bool operator()(const PropertyMatch& c) const {
        auto val = sym.property(c.key);
        if (!val) return false;
        return RuleEngine::match_glob(c.pattern, *val);
    }
};

bool RuleEngine::evaluate_condition(const Condition& cond,
                                    const core::Component& comp) const {
    return std::visit(ConditionEvaluator{comp}, cond);
}

bool RuleEngine::evaluate_condition_symbol(const Condition& cond,
                                           const core::Symbol& sym) const {
    return std::visit(SymbolConditionEvaluator{sym}, cond);
}

// Guess component type from standard KiCad reference prefix
core::ComponentType guess_type_from_name(const std::string& name) {
    if (name.empty()) return core::ComponentType::Unknown;
    char first = (char)std::toupper(name[0]);

    // Two-letter prefixes
    if (name.size() >= 2) {
        char second = (char)std::toupper(name[1]);
        if (first == 'L' && second == 'E') return core::ComponentType::LED;
        if (first == 'S' && second == 'W') return core::ComponentType::Switch;
        if (first == 'F' && second == 'B') return core::ComponentType::FerriteBead;
        if (first == 'J' && second == 'P') return core::ComponentType::Jumper;
        if (first == 'T' && second == 'P') return core::ComponentType::TestPoint;
        if (first == 'V' && second == 'R') return core::ComponentType::VoltageRegulator;
    }

    switch (first) {
    case 'R': return core::ComponentType::Resistor;
    case 'C': return core::ComponentType::Capacitor;
    case 'L': return core::ComponentType::Inductor;
    case 'D': return core::ComponentType::Diode;
    case 'Q': {
        // Q could be transistor or MOSFET — check pin count
        return core::ComponentType::Transistor;
    }
    case 'U': return core::ComponentType::Microcontroller;
    case 'J': return core::ComponentType::Connector;
    case 'X':
    case 'Y':
    {
        // Only classify as crystal if name looks like a crystal/oscillator,
        // not an IC (Xilinx, XC3S400) or connector (XLR-3)
        constexpr size_t MAX_CRYSTAL_LEN = 6;
        if (name.size() <= MAX_CRYSTAL_LEN
            || name.contains("MHz") || name.contains("kHz")
            || name.contains("TAL") || name.contains("tal")
            || name.contains("OSC") || name.contains("Osc"))
        {
            return core::ComponentType::Crystal;
        }
        return core::ComponentType::Unknown;
    }
    case 'F': return core::ComponentType::Fuse;
    case 'K': return core::ComponentType::Relay;
    case 'T': return core::ComponentType::Transformer;
    case 'H': return core::ComponentType::MountingHole;
    default:  return core::ComponentType::Unknown;
    }
}

std::vector<ClassificationResult> RuleEngine::evaluate(
    const core::Component& component) const {
    std::vector<ClassificationResult> results;

    // Infer type from name if not already set
    core::ComponentType effective_type = component.type();
    if (effective_type == core::ComponentType::Unknown) {
        effective_type = guess_type_from_name(component.name());
    }

    for (const auto& rule : rules_) {
        if (!rule.enabled) continue;

        bool matched = false;
        std::string reason;
        int confidence = rule.confidence;

        // Evaluate the condition variant
        matched = std::visit([&](const auto& cond) -> bool {
            using T = std::decay_t<decltype(cond)>;

            if constexpr (std::is_same_v<T, MatchType>) {
                // Use TypeRegistry for dynamic type name lookup
                int rule_idx = core::component_type_index(cond.type_name);
                if (rule_idx < 0) return false;
                auto rule_type = static_cast<core::ComponentType>(rule_idx);
                bool m = (effective_type == rule_type);
                if (m) {
                    reason = "Name prefix matches type '" + cond.type_name + "'";
                    confidence = 90;
                }
                return m;
            }
            else if constexpr (std::is_same_v<T, PinCountRange>) {
                int pc = component.pin_count();
                bool m = (pc >= cond.min && pc <= cond.max);
                if (m) {
                    reason = std::to_string(pc) + " pins in range " +
                             std::to_string(cond.min) + "-" + std::to_string(cond.max);
                    confidence = 75;
                }
                return m;
            }
            else if constexpr (std::is_same_v<T, MatchPackage>) {
                bool m = match_glob(cond.pattern,
                    component.property("footprint").value_or(""));
                if (m) reason = "Package matches '" + cond.pattern + "'";
                return m;
            }
            else if constexpr (std::is_same_v<T, PropertyExists>) {
                bool m = component.property(cond.key).has_value();
                if (m) reason = "Has property '" + cond.key + "'";
                return m;
            }
            else if constexpr (std::is_same_v<T, PropertyMatch>) {
                auto val = component.property(cond.key);
                if (!val) return false;
                bool m = match_glob(cond.pattern, *val);
                if (m) reason = "Property '" + cond.key + "' matches";
                return m;
            }
            else {
                return false;  // ValueInRange not implemented yet
            }
        }, rule.condition);

        if (matched) {
            ClassificationResult result;
            result.component_id = component.id();
            result.component_name = component.name();
            result.rule_name = rule.name;
            result.target_library = rule.target_library;
            result.confidence = rule.confidence;
            result.reason = reason;
            result.inferred_type = effective_type;
            results.push_back(std::move(result));
        }
    }

    // Sort by confidence descending, then priority ascending
    std::sort(results.begin(), results.end(),
              [](const ClassificationResult& a, const ClassificationResult& b) {
                  if (a.confidence != b.confidence)
                      return a.confidence > b.confidence;
                  return a.rule_name < b.rule_name;
              });

    return results;
}

std::vector<ClassificationResult> RuleEngine::evaluate_symbol(
    const core::Symbol& symbol) const {
    // Convert symbol to a minimal Component for evaluation
    core::Component comp;
    comp.set_name(symbol.name());
    comp.set_pin_count(symbol.pin_count());
    if (!symbol.footprint().empty())
        comp.set_property("footprint", symbol.footprint());
    if (!symbol.mpn().empty())
        comp.set_property("mpn", symbol.mpn());

    return evaluate(comp);
}

std::vector<std::vector<ClassificationResult>> RuleEngine::evaluate_batch(
    const std::vector<core::Component>& components) const {
    std::vector<std::vector<ClassificationResult>> results;
    results.reserve(components.size());
    for (const auto& comp : components) {
        results.push_back(evaluate(comp));
    }
    return results;
}

const char* component_type_to_string(core::ComponentType t) {
    // Cache the string in a thread_local to support returning const char*
    static thread_local std::string cached;
    cached = core::component_type_name(static_cast<int>(t));
    return cached.c_str();
}

}  // namespace kforge::classifier
