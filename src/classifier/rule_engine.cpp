#include "classifier/rule_engine.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string_view>

#include "core/model/type_registry.h"

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
        // 规则的 type 字段比较类型 ID（内置 ID 由静态表给出，与注册表顺序无关）
        return std::string(core::builtin_component_id(comp.type())) == c.type_name;
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

namespace {

char upper_ascii(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

/// 两位位号 → 内置类型（KiCad 约定：SW/FB/JP/TP/VR，以及 LED）
core::ComponentType two_letter_type(char first, char second) {
    if (first == 'L' && second == 'E') return core::ComponentType::LED;
    if (first == 'S' && second == 'W') return core::ComponentType::Switch;
    if (first == 'F' && second == 'B') return core::ComponentType::FerriteBead;
    if (first == 'J' && second == 'P') return core::ComponentType::Jumper;
    if (first == 'T' && second == 'P') return core::ComponentType::TestPoint;
    if (first == 'V' && second == 'R') return core::ComponentType::VoltageRegulator;
    return core::ComponentType::Unknown;
}

/// 单位号 → 内置类型。U（通用 IC，KiCad 里什么 IC 都叫 U）不在表内：
/// 宁可不定类型，也不能替用户猜一个具体类型。
core::ComponentType single_letter_type(char first) {
    switch (first) {
    case 'R': return core::ComponentType::Resistor;
    case 'C': return core::ComponentType::Capacitor;
    case 'L': return core::ComponentType::Inductor;
    case 'D': return core::ComponentType::Diode;
    case 'Q': return core::ComponentType::Transistor;
    case 'J': return core::ComponentType::Connector;
    case 'X':
    case 'Y': return core::ComponentType::Crystal;
    case 'F': return core::ComponentType::Fuse;
    case 'K': return core::ComponentType::Relay;
    case 'T': return core::ComponentType::Transformer;
    case 'H': return core::ComponentType::MountingHole;
    default:  return core::ComponentType::Unknown;
    }
}

/// 文本整体必须就是一个完整位号才算数：单个字母、已知的两位组合，
/// 或 LED 这个三位位号。多一个字符都不是位号 —— "LM"/"CD" 是型号名前缀，
/// "LEDdriver" 是型号名，不能因为前两位像 LED 就猜成 LED。
/// 首字母/前缀都不是类型依据。
core::ComponentType type_from_designator(std::string_view text) {
    if (text.size() == 1)
        return single_letter_type(upper_ascii(text[0]));
    if (text.size() == 2)
        return two_letter_type(upper_ascii(text[0]), upper_ascii(text[1]));
    if (text.size() == 3 && upper_ascii(text[0]) == 'L' && upper_ascii(text[1]) == 'E'
        && upper_ascii(text[2]) == 'D')
        return core::ComponentType::LED;
    return core::ComponentType::Unknown;
}

}  // namespace

std::string guess_symbol_type(const core::Symbol& symbol) {
    // 只有真实 Reference 算依据：它就是位号，语义最明确。
    // 这里不做任何名字兜底 —— 型号名（LM358 / CD4011 / LEDdriver）不是位号，
    // U 只是通用 IC，都不能替用户猜一个具体类型。没有 Reference 的老库由
    // parser 在继承完成后按本符号名字兜底（见 SymbolLibParser::parse_buffer），
    // 分类器不再补第二层。
    auto type = type_from_designator(symbol.reference_prefix());
    if (type == core::ComponentType::Unknown) return {};
    return std::string(core::builtin_component_id(type));
}

std::vector<ClassificationResult> RuleEngine::evaluate(
    const core::Component& component) const {
    // 旧路径的行为保持不变：类型为空时按名字前缀猜一个内置类型
    core::ComponentType effective_type = component.type();
    if (effective_type == core::ComponentType::Unknown) {
        effective_type = guess_type_from_name(component.name());
    }
    return evaluate_impl(component, core::builtin_component_id(effective_type));
}

std::vector<ClassificationResult> RuleEngine::evaluate_impl(
    const core::Component& component, const std::string& type_id) const {
    std::vector<ClassificationResult> results;

    // 类型身份由调用方给定：符号走真实 Reference / 已持久化 ID，
    // 组件走枚举 + 名字兜底。这里不再做任何回退。
    const std::string& effective_id = type_id;
    const core::ComponentType effective_type =
        core::builtin_component_type(effective_id);  // 非内置 ID → Unknown

    for (const auto& rule : rules_) {
        if (!rule.enabled) continue;

        bool matched = false;
        std::string reason;
        int confidence = rule.confidence;

        // Evaluate the condition variant
        matched = std::visit([&](const auto& cond) -> bool {
            using T = std::decay_t<decltype(cond)>;

            if constexpr (std::is_same_v<T, MatchType>) {
                // 比较类型 ID：本次求值的类型身份与规则里的 ID 相等才算命中
                bool m = effective_id == cond.type_name;
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
            result.inferred_type_id = effective_id;
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
    const core::Symbol& symbol, bool infer_reference) const {
    // Convert symbol to a minimal Component for evaluation
    core::Component comp;
    comp.set_name(symbol.name());
    comp.set_pin_count(symbol.pin_count());
    if (!symbol.footprint().empty())
        comp.set_property("footprint", symbol.footprint());
    if (!symbol.mpn().empty())
        comp.set_property("mpn", symbol.mpn());
    // 真实 Reference 也是分类依据：规则可能按 reference 属性匹配
    if (auto reference = symbol.property("reference");
        reference && !reference->empty()) {
        comp.set_property("reference", *reference);
    }

    // 类型身份：已持久化的原始 ID 原样带入（内置/custom/未注册都不经过枚举），
    // 只有「还没有类型」才按真实 Reference 推断 —— 空串是老行的写法，
    // 显式的 "Unknown" 是导入路径给未分类符号落的占位值（见 pipeline /
    // staged_import），两者都是待分类，不能因为 "Unknown" 非空就跳过推断。
    // 其余任何原始 ID（OpAmp、custom:xxx、LegacyRawId）一律保留。
    // 这里不启用名字兜底 —— 型号首字母不是类型依据（LM358 不是电感），
    // U 也只是通用 IC。infer_reference=false 时连位号推断也关掉：
    // 生产分类由匹配核心给出已决类型，推荐库只按该类型算，不得自行改判。
    std::string type_id = symbol.component_type_id;
    if (infer_reference
        && (type_id.empty() || type_id == core::builtin_component_id(core::ComponentType::Unknown)))
        type_id = guess_symbol_type(symbol);

    return evaluate_impl(comp, type_id);
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
    // 内置 ID 的静态映射 —— 不再经注册表下标转名字
    return core::builtin_component_id(t);
}

}  // namespace kforge::classifier
