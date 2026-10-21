#pragma once

#include <string>
#include <vector>
#include <variant>

#include "core/model/types.h"

namespace kforge::classifier {

// ============================================================
// Condition AST — simple predicates (no recursion needed)
// ============================================================

struct MatchType     { std::string type_name; };
struct MatchPackage  { std::string pattern; };
struct ValueInRange  { double min{0}; double max{0}; std::string unit; };
struct PinCountRange { int min{0}; int max{0}; };
struct PropertyExists { std::string key; };
struct PropertyMatch { std::string key; std::string pattern; };

using Condition = std::variant<
    MatchType,
    MatchPackage,
    ValueInRange,
    PinCountRange,
    PropertyExists,
    PropertyMatch
>;

// ============================================================
// Rule
// ============================================================

struct Rule {
    std::string name;
    int priority{100};
    std::string target_library;
    Condition condition;
    int confidence{80};
    bool enabled{true};

    Rule() = default;
    Rule(std::string n, int p, std::string t, Condition c, int conf = 80)
        : name(std::move(n)), priority(p), target_library(std::move(t)),
          condition(std::move(c)), confidence(conf) {}
};

// ============================================================
// Classification Result
// ============================================================

struct ClassificationResult {
    std::string component_id;
    std::string component_name;
    std::string rule_name;
    std::string target_library;
    int confidence{0};
    std::string reason;
    core::ComponentType inferred_type{core::ComponentType::Unknown};
    /// 推断出的类型身份 ID。可能是内置 ID、custom: 开头的自定义 ID，
    /// 也可能是历史遗留的未注册 ID —— 都按原始字符串传递，不经过枚举。
    /// 空串表示「推不出」：调用方不得据此改写已有类型。
    std::string inferred_type_id;
};

}  // namespace kforge::classifier
