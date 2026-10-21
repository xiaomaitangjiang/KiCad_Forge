#pragma once

#include <string>
#include <vector>

#include "classifier/rule.h"
#include "core/model/symbol.h"
#include "core/model/footprint.h"
#include "core/model/component.h"

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
    /// infer_reference=false 明确关闭「空/Unknown 类型按真实 Reference 推断」这条
    /// 隐藏兜底：调用方自己给出已决类型（临时副本带入），空串就是空串。
    /// 默认 true = 旧基准行为，其他调用方（测试/基准）不受影响。
    std::vector<ClassificationResult> evaluate_symbol(
        const core::Symbol& symbol, bool infer_reference = true) const;

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

    /// 规则求值内核：type_id 是本次求值使用的类型身份（可为空串 = 认不出），
    /// 匹配与结果都基于它。空串不会被替换成 "Unknown" —— 调用方要靠它区分
    /// 「本来就没有类型」和「确实推断出 Unknown」。
    std::vector<ClassificationResult> evaluate_impl(
        const core::Component& component, const std::string& type_id) const;

    std::vector<Rule> rules_;
};

/// Infer component type from KiCad reference prefix (R=Resistor, C=Capacitor, etc.)
core::ComponentType guess_type_from_name(const std::string& name);

/// 按符号的真实 Reference 推断类型 ID（R → Resistor、C → Capacitor、SW/LED…）。
/// 只有「整个文本本身就是位号」才算数：单个字母、已知的两位位号组合，
/// 或 LED。U 只代表通用 IC，不猜成具体类型；型号名（LM358 的 "LM"、
/// CD4001 的 "CD"、"LEDdriver"）一律不猜 —— 前几位字母不是类型依据。
/// 这里不读符号名字兜底：没有 Reference 的老库由 parser 自己按名字前缀
/// 兜底一次。推不出返回空串。
std::string guess_symbol_type(const core::Symbol& symbol);

/// Convert ComponentType enum to human-readable string.
const char* component_type_to_string(core::ComponentType t);

}  // namespace kforge::classifier
