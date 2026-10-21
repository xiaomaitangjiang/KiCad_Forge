#include "classifier/type_matching.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/model/type_registry.h"
#include "util/file_write.h"

namespace kforge::classifier {

// 实现文件内部的简写（只在当前 TU 生效）
using nlohmann::json;

namespace {

util::Error invalid(std::string message)
{
    return util::Error::make<util::Error::Kind::ValidationError>(std::move(message));
}

// ---------------- 发布代次的两种固定拒绝 ----------------
// 两者都是「这次请求失败」，不是「仓储状态变坏」：current/path/load_error 都不因此改变。

/// 陈旧读取：锁外读取期间已经有更晚完成的状态发布。固定 "stale" 前缀便于调用方识别，
/// 且禁止继续编译、记录错误、清错误、重绑路径或发布快照。
util::Error stale_load_error(const std::string& path)
{
    return util::Error::make<util::Error::Kind::ServiceError>(
        "stale matching config load: " + path +
        " was read before a newer state was published; nothing was changed");
}

/// 发布代次到 uint64 上限：不能再形成新的状态发布，明确拒绝而不是回绕。
/// save 必须在 replace_file 之前用它拒绝，磁盘不能先变。
util::Error generation_exhausted_error()
{
    return util::Error::make<util::Error::Kind::ServiceError>(
        "matching config state generation has reached the uint64 limit and cannot be advanced; "
        "nothing was changed");
}

// ---------------- 求值被中止时的决策 ----------------
// 超限和引擎报错都不是「不匹配」：整次决策作废，已经收集到的最高优先级命中一律不采用。

Decision evaluation_limit_decision(const MatchingRule& rule, const RegexMatchResult& result)
{
    Decision decision;
    decision.status = MatchStatus::EvaluationLimit;
    decision.reason = "rule '" + rule.id + "' exceeded the matching work budget after " +
                      std::to_string(result.callouts) + " callouts (engine: " +
                      result.error_message + "; callout budget " +
                      std::to_string(kMaxCalloutsPerEvaluation) +
                      " per evaluation, plus the interpreter limits); the whole decision was discarded";
    return decision;
}

Decision evaluation_error_decision(const MatchingRule& rule, const RegexMatchResult& result)
{
    Decision decision;
    decision.status = MatchStatus::EvaluationError;
    decision.reason = "rule '" + rule.id + "' could not be evaluated: " + result.error_message +
                      " (PCRE2 error " + std::to_string(result.error_code) +
                      "); the whole decision was discarded";
    return decision;
}

// ---------------- JSON 取值：严格、先判类型再转换 ----------------

/// JSON 里的精确整数：bool、浮点、超出 int64 的值都不算。
/// nlohmann 把超出 uint64 的整数解析成浮点，所以拒绝浮点同时也就拒绝了溢出值；
/// 调用方拿到 int64 之后再按各自的上限判断，不截断也不回绕。
std::optional<std::int64_t> exact_int64(const json& value)
{
    if (value.is_boolean() || !value.is_number_integer())
        return std::nullopt;
    if (value.is_number_unsigned())
    {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return std::nullopt;
        return static_cast<std::int64_t>(number);
    }
    return value.get<std::int64_t>();
}

const json* member_of(const json& object, const char* key)
{
    auto it = object.find(key);
    if (it == object.end())
        return nullptr;
    return &(*it);
}

/// 必需字段：缺失与错类型都拒绝整份配置（不丢坏条目后接受残缺数组）。
util::Result<std::string> require_string(const json& object, const char* key, const std::string& where)
{
    const json* member = member_of(object, key);
    if (member == nullptr)
        return std::unexpected(invalid(where + " is missing"));
    if (!member->is_string())
        return std::unexpected(invalid(where + " must be a string"));
    return member->get<std::string>();
}

util::Result<bool> require_bool(const json& object, const char* key, const std::string& where)
{
    const json* member = member_of(object, key);
    if (member == nullptr)
        return std::unexpected(invalid(where + " is missing"));
    if (!member->is_boolean())
        return std::unexpected(invalid(where + " must be a boolean"));
    return member->get<bool>();
}

util::Result<std::int32_t> require_priority(const json& object, const char* key,
                                            const std::string& where)
{
    const json* member = member_of(object, key);
    if (member == nullptr)
        return std::unexpected(invalid(where + " is missing"));
    const auto number = exact_int64(*member);
    if (!number)
        return std::unexpected(invalid(where + " must be an integer"));
    if (*number < static_cast<std::int64_t>(kMinRulePriority) ||
        *number > static_cast<std::int64_t>(kMaxRulePriority))
        return std::unexpected(invalid(where + " is outside the int32 range"));
    return static_cast<std::int32_t>(*number);
}

util::Result<MatchField> require_field(const json& object, const char* key, const std::string& where)
{
    const auto text = require_string(object, key, where);
    if (!text)
        return std::unexpected(text.error());
    const auto field = parse_match_field(*text);
    if (!field)
        return std::unexpected(invalid(where + " must be one of name/value/model/reference"));
    return *field;
}

// ---------------- 结构校验（JSON 与直接构造的 DTO 走同一份规则） ----------------

/// 直接构造 DTO 时 field 可能是越界的枚举值（JSON 文本那条路径已被 require_field 挡住）：
/// 只有这四种取值合法，disabled 条目一样要查 —— 否则它会永远匹配不上，却按 field 名字
/// 序列化成一条语义完全不同的正常规则。
bool is_known_match_field(MatchField field)
{
    switch (field)
    {
        case MatchField::Name:
        case MatchField::Value:
        case MatchField::Model:
        case MatchField::Reference:
            return true;
    }
    return false;
}

util::Result<void> validate_structure(const MatchingConfig& config)
{
    if (config.schema_version != kMatchingSchemaVersion)
        return std::unexpected(invalid("unsupported schema_version: " + std::to_string(config.schema_version) +
                                       " (expected " + std::to_string(kMatchingSchemaVersion) + ")"));
    if (config.revision > kMaxSafeRevision)
        return std::unexpected(invalid("revision exceeds the safe integer range"));
    if (config.rules.size() > kMaxMatchingEntries)
        return std::unexpected(invalid("too many rules (limit " + std::to_string(kMaxMatchingEntries) + ")"));
    if (config.exceptions.size() > kMaxMatchingEntries)
        return std::unexpected(
            invalid("too many exceptions (limit " + std::to_string(kMaxMatchingEntries) + ")"));

    // id 在整份配置里唯一：rules 与 exceptions 之间也不能复用，disabled 条目一样算数。
    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < config.rules.size(); ++index)
    {
        const MatchingRule& rule = config.rules[index];
        const std::string at = "rules[" + std::to_string(index) + "]";
        if (rule.id.empty())
            return std::unexpected(invalid(at + ".id must not be empty"));
        if (!ids.insert(rule.id).second)
            return std::unexpected(invalid("duplicate id: " + rule.id));
        if (!is_known_match_field(rule.field))
            return std::unexpected(
                invalid(at + ".field is not one of name/value/model/reference"));
        if (rule.pattern.size() > kMaxPatternBytes)
            return std::unexpected(invalid(at + ".pattern is " + std::to_string(rule.pattern.size()) +
                                           " bytes (limit " + std::to_string(kMaxPatternBytes) + ")"));
        if (rule.type_id.empty())
            return std::unexpected(invalid(at + ".type_id must not be empty"));
    }

    // 同 field + key 的特例重复定义拒绝保存（区分大小写、原文比较、不 trim）。
    // 字段名里不会出现 '\0'，用它拼复合键不会把两组不同的 field/key 混成一组。
    std::unordered_set<std::string> exception_keys;
    for (std::size_t index = 0; index < config.exceptions.size(); ++index)
    {
        const MatchingException& entry = config.exceptions[index];
        const std::string at = "exceptions[" + std::to_string(index) + "]";
        if (entry.id.empty())
            return std::unexpected(invalid(at + ".id must not be empty"));
        if (!ids.insert(entry.id).second)
            return std::unexpected(invalid("duplicate id: " + entry.id));
        if (!is_known_match_field(entry.field))
            return std::unexpected(
                invalid(at + ".field is not one of name/value/model/reference"));
        if (entry.key.empty())
            return std::unexpected(invalid(at + ".key must not be empty"));
        if (entry.key.size() > kMaxExceptionKeyBytes)
            return std::unexpected(invalid(at + ".key is " + std::to_string(entry.key.size()) +
                                           " bytes (limit " + std::to_string(kMaxExceptionKeyBytes) + ")"));
        if (entry.type_id.empty())
            return std::unexpected(invalid(at + ".type_id must not be empty"));
        std::string composite = match_field_name(entry.field);
        composite.push_back('\0');
        composite += entry.key;
        if (!exception_keys.insert(std::move(composite)).second)
            return std::unexpected(invalid(at + " duplicates an existing exception for field '" +
                                           std::string(match_field_name(entry.field)) + "'"));
    }
    return {};
}

// ---------------- 决策收敛 ----------------

/// 把一批命中收敛成一个决策：类型唯一就是 Matched 并带上全部依据，
/// 出现不同型号就是 Conflict —— 不按列表顺序、名字或 confidence 选第一条。
Decision resolve_hits(std::vector<MatchEvidence> hits)
{
    Decision decision;
    const std::string& type_id = hits.front().type_id;
    bool conflict = false;
    for (const auto& hit : hits)
    {
        if (hit.type_id != type_id)
        {
            conflict = true;
            break;
        }
    }
    if (!conflict)
    {
        decision.status = MatchStatus::Matched;
        decision.type_id = type_id;
        decision.evidence = std::move(hits);
        return decision;
    }

    std::string types;
    for (const auto& hit : hits)
    {
        if (!types.empty())
            types += ", ";
        types += hit.type_id;
    }
    decision.status = MatchStatus::Conflict;
    decision.reason = "conflicting types: " + types;
    decision.evidence = std::move(hits);
    return decision;
}

// ---------------- 缺文件时的默认种子 ----------------
// 与随包分发的 config/component_type_matching.json 是同一份内容：没有另一套私有映射。
// 全部是 field=reference、enabled=true、case_sensitive=false、同级 priority 的可编辑规则，
// 用户可以禁用/删除/改写；U（通用 IC）故意没有规则。

constexpr std::string_view kDefaultMatchingConfigJson = R"JSON({
  "exceptions": [],
  "revision": 0,
  "rules": [
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-r",
      "pattern": "^R[0-9?]*$",
      "priority": 0,
      "type_id": "Resistor"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-c",
      "pattern": "^C[0-9?]*$",
      "priority": 0,
      "type_id": "Capacitor"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-l",
      "pattern": "^L[0-9?]*$",
      "priority": 0,
      "type_id": "Inductor"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-d",
      "pattern": "^D[0-9?]*$",
      "priority": 0,
      "type_id": "Diode"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-led",
      "pattern": "^(LE|LED)[0-9?]*$",
      "priority": 0,
      "type_id": "LED"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-sw",
      "pattern": "^SW[0-9?]*$",
      "priority": 0,
      "type_id": "Switch"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-fb",
      "pattern": "^FB[0-9?]*$",
      "priority": 0,
      "type_id": "FerriteBead"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-jp",
      "pattern": "^JP[0-9?]*$",
      "priority": 0,
      "type_id": "Jumper"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-tp",
      "pattern": "^TP[0-9?]*$",
      "priority": 0,
      "type_id": "TestPoint"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-vr",
      "pattern": "^VR[0-9?]*$",
      "priority": 0,
      "type_id": "VoltageRegulator"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-q",
      "pattern": "^Q[0-9?]*$",
      "priority": 0,
      "type_id": "Transistor"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-j",
      "pattern": "^J[0-9?]*$",
      "priority": 0,
      "type_id": "Connector"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-xy",
      "pattern": "^(X|Y)[0-9?]*$",
      "priority": 0,
      "type_id": "Crystal"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-f",
      "pattern": "^F[0-9?]*$",
      "priority": 0,
      "type_id": "Fuse"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-k",
      "pattern": "^K[0-9?]*$",
      "priority": 0,
      "type_id": "Relay"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-t",
      "pattern": "^T[0-9?]*$",
      "priority": 0,
      "type_id": "Transformer"
    },
    {
      "case_sensitive": false,
      "enabled": true,
      "field": "reference",
      "id": "designator-h",
      "pattern": "^H[0-9?]*$",
      "priority": 0,
      "type_id": "MountingHole"
    }
  ],
  "schema_version": 1
}
)JSON";

}  // namespace

// ---------------- 文件读取：只有「确实不存在」才允许走种子 ----------------

ConfigFileReadResult read_matching_config_file(const std::string& path)
{
    ConfigFileReadResult result;
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if ((!error && status.type() == std::filesystem::file_type::not_found) ||
        error == std::errc::no_such_file_or_directory)
    {
        result.status = ConfigFileReadStatus::Missing;
        return result;
    }
    if (error)
    {
        result.detail = "cannot inspect the path: " + error.message();
        return result;
    }
    if (status.type() != std::filesystem::file_type::regular)
    {
        result.detail = "path is not a regular file";
        return result;
    }
    try
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            result.detail = "cannot open the file";
            return result;
        }
        std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (input.bad())
        {
            result.detail = "read was interrupted";
            return result;
        }
        result.status = ConfigFileReadStatus::Ok;
        result.bytes = std::move(raw);
    }
    catch (const std::exception& error)
    {
        result.detail = error.what();
        result.status = ConfigFileReadStatus::Failed;
    }
    return result;
}

// ---------------- 字段名 ----------------

std::optional<MatchField> parse_match_field(std::string_view text)
{
    if (text == "name")
        return MatchField::Name;
    if (text == "value")
        return MatchField::Value;
    if (text == "model")
        return MatchField::Model;
    if (text == "reference")
        return MatchField::Reference;
    return std::nullopt;
}

const char* match_field_name(MatchField field)
{
    switch (field)
    {
        case MatchField::Name:
            return "name";
        case MatchField::Value:
            return "value";
        case MatchField::Model:
            return "model";
        case MatchField::Reference:
            return "reference";
    }
    // 校验已经保证只有上面四种取值；万一将来漏改，也绝不再把未知枚举悄悄写成 reference。
    return "unknown";
}

const char* match_status_name(MatchStatus status)
{
    switch (status)
    {
        case MatchStatus::Matched:
            return "matched";
        case MatchStatus::Unmatched:
            return "unmatched";
        case MatchStatus::Conflict:
            return "conflict";
        case MatchStatus::InputTooLong:
            return "input_too_long";
        case MatchStatus::EvaluationLimit:
            return "evaluation_limit";
        case MatchStatus::EvaluationError:
            return "evaluation_error";
    }
    return "unmatched";
}

// ---------------- 样本 ----------------

MatchSample MatchSample::from_symbol(const core::Symbol& symbol)
{
    MatchSample sample;
    sample.name = symbol.name();
    sample.value = symbol.default_value();
    sample.model = symbol.mpn();
    // reference 只读实际属性：缺失就是没有这个字段，不从 prefix/名字补出来
    sample.reference = symbol.property("reference");
    return sample;
}

const std::string* MatchSample::field(MatchField which) const
{
    switch (which)
    {
        case MatchField::Name:
            return name ? &*name : nullptr;
        case MatchField::Value:
            return value ? &*value : nullptr;
        case MatchField::Model:
            return model ? &*model : nullptr;
        case MatchField::Reference:
            return reference ? &*reference : nullptr;
    }
    return nullptr;
}

// ---------------- 快照 ----------------

MatchingSnapshot::MatchingSnapshot(MatchingConfig config, std::vector<detail::CompiledRule> compiled)
    : config_(std::move(config)), compiled_(std::move(compiled))
{
    std::vector<std::string> ids;
    ids.reserve(config_.rules.size() + config_.exceptions.size());
    for (const auto& rule : config_.rules)
        ids.push_back(rule.type_id);
    for (const auto& entry : config_.exceptions)
        ids.push_back(entry.type_id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    referenced_type_ids_ = std::move(ids);
}

Decision MatchingSnapshot::evaluate(const MatchSample& sample) const
{
    Decision decision;

    // 容量保护：任一个待匹配字段超长就整体拒绝，绝不截断后去匹配。
    constexpr MatchField kFields[] = {MatchField::Name, MatchField::Value, MatchField::Model,
                                      MatchField::Reference};
    for (MatchField field : kFields)
    {
        const std::string* text = sample.field(field);
        if (text == nullptr)
            continue;
        if (text->size() > kMaxSourceFieldBytes)
        {
            decision.status = MatchStatus::InputTooLong;
            decision.reason = std::string("field '") + match_field_name(field) + "' is " +
                              std::to_string(text->size()) + " bytes (limit " +
                              std::to_string(kMaxSourceFieldBytes) + "); nothing was matched";
            return decision;
        }
    }

    // 1) 精确特例优先：field + key 原文相等、区分大小写、不 trim、不模糊搜索。
    //    只要有一条命中就不再评估正则。
    std::vector<MatchEvidence> exception_hits;
    for (const auto& entry : config_.exceptions)
    {
        if (!entry.enabled)
            continue;
        const std::string* text = sample.field(entry.field);
        if (text == nullptr || *text != entry.key)
            continue;
        MatchEvidence evidence;
        evidence.source = MatchEvidenceSource::Exception;
        evidence.id = entry.id;
        evidence.field = entry.field;
        evidence.text = *text;
        evidence.type_id = entry.type_id;
        exception_hits.push_back(std::move(evidence));
    }
    if (!exception_hits.empty())
        return resolve_hits(std::move(exception_hits));

    // 2) enabled 正则：PCRE2 regex_search（用户要全文自己写 ^$），取最高数值 priority。
    //    一次 evaluate 一个会话：callout 预算跨全部规则与全部起点累计；并发 evaluate
    //    各自持有自己的会话，不共享计数、match context 或 match data。
    RegexSession session;
    bool found = false;
    std::int32_t best_priority = kMinRulePriority;
    std::vector<MatchEvidence> rule_hits;
    for (const auto& compiled : compiled_)
    {
        const MatchingRule& rule = compiled.rule;
        if (!rule.enabled)
            continue;
        const std::string* text = sample.field(rule.field);
        if (text == nullptr)
            continue;
        RegexMatchResult result;
        if (!session.valid())
        {
            result.outcome = RegexOutcome::EngineError;
            result.error_message = "the PCRE2 match context is not available";
        }
        else
        {
            result = session.search(compiled.program, *text);
        }
        if (result.outcome == RegexOutcome::LimitExceeded)
            return evaluation_limit_decision(rule, result);
        if (result.outcome == RegexOutcome::EngineError)
            return evaluation_error_decision(rule, result);
        if (result.outcome != RegexOutcome::Matched)
            continue;
        if (!found || rule.priority > best_priority)
        {
            found = true;
            best_priority = rule.priority;
            rule_hits.clear();
        }
        else if (rule.priority < best_priority)
        {
            continue;
        }
        MatchEvidence evidence;
        evidence.source = MatchEvidenceSource::Rule;
        evidence.id = rule.id;
        evidence.field = rule.field;
        evidence.text = *text;
        evidence.priority = rule.priority;
        evidence.type_id = rule.type_id;
        rule_hits.push_back(std::move(evidence));
    }
    if (!found)
    {
        decision.status = MatchStatus::Unmatched;
        return decision;
    }
    return resolve_hits(std::move(rule_hits));
}

// ---------------- 解析 / 序列化 ----------------

util::Result<MatchingConfig> parse_matching_config_json(std::string_view json_text)
{
    json root;
    try
    {
        root = json::parse(std::string(json_text));
    }
    catch (const std::exception& error)
    {
        return std::unexpected(invalid(std::string("invalid JSON: ") + error.what()));
    }
    if (!root.is_object())
        return std::unexpected(invalid("configuration must be a JSON object"));

    MatchingConfig config;

    const json* schema = member_of(root, "schema_version");
    if (schema == nullptr)
        return std::unexpected(invalid("schema_version is missing"));
    const auto schema_version = exact_int64(*schema);
    if (!schema_version)
        return std::unexpected(invalid("schema_version must be an integer"));
    config.schema_version = *schema_version;

    const json* revision = member_of(root, "revision");
    if (revision == nullptr)
        return std::unexpected(invalid("revision is missing"));
    const auto revision_value = exact_int64(*revision);
    if (!revision_value)
        return std::unexpected(invalid("revision must be an integer"));
    if (*revision_value < 0)
        return std::unexpected(invalid("revision must not be negative"));
    if (static_cast<std::uint64_t>(*revision_value) > kMaxSafeRevision)
        return std::unexpected(invalid("revision exceeds the safe integer range"));
    config.revision = static_cast<std::uint64_t>(*revision_value);

    const json* rules = member_of(root, "rules");
    if (rules == nullptr || !rules->is_array())
        return std::unexpected(invalid("rules must be an array"));
    config.rules.reserve(rules->size());
    for (std::size_t index = 0; index < rules->size(); ++index)
    {
        const json& item = (*rules)[index];
        const std::string at = "rules[" + std::to_string(index) + "]";
        if (!item.is_object())
            return std::unexpected(invalid(at + " must be an object"));

        MatchingRule rule;
        const auto id = require_string(item, "id", at + ".id");
        if (!id)
            return std::unexpected(id.error());
        rule.id = *id;
        const auto enabled = require_bool(item, "enabled", at + ".enabled");
        if (!enabled)
            return std::unexpected(enabled.error());
        rule.enabled = *enabled;
        const auto field = require_field(item, "field", at + ".field");
        if (!field)
            return std::unexpected(field.error());
        rule.field = *field;
        const auto pattern = require_string(item, "pattern", at + ".pattern");
        if (!pattern)
            return std::unexpected(pattern.error());
        rule.pattern = *pattern;
        const auto case_sensitive = require_bool(item, "case_sensitive", at + ".case_sensitive");
        if (!case_sensitive)
            return std::unexpected(case_sensitive.error());
        rule.case_sensitive = *case_sensitive;
        const auto priority = require_priority(item, "priority", at + ".priority");
        if (!priority)
            return std::unexpected(priority.error());
        rule.priority = *priority;
        const auto type_id = require_string(item, "type_id", at + ".type_id");
        if (!type_id)
            return std::unexpected(type_id.error());
        rule.type_id = *type_id;
        config.rules.push_back(std::move(rule));
    }

    const json* exceptions = member_of(root, "exceptions");
    if (exceptions == nullptr || !exceptions->is_array())
        return std::unexpected(invalid("exceptions must be an array"));
    config.exceptions.reserve(exceptions->size());
    for (std::size_t index = 0; index < exceptions->size(); ++index)
    {
        const json& item = (*exceptions)[index];
        const std::string at = "exceptions[" + std::to_string(index) + "]";
        if (!item.is_object())
            return std::unexpected(invalid(at + " must be an object"));

        MatchingException entry;
        const auto id = require_string(item, "id", at + ".id");
        if (!id)
            return std::unexpected(id.error());
        entry.id = *id;
        const auto enabled = require_bool(item, "enabled", at + ".enabled");
        if (!enabled)
            return std::unexpected(enabled.error());
        entry.enabled = *enabled;
        const auto field = require_field(item, "field", at + ".field");
        if (!field)
            return std::unexpected(field.error());
        entry.field = *field;
        const auto key = require_string(item, "key", at + ".key");
        if (!key)
            return std::unexpected(key.error());
        entry.key = *key;
        const auto type_id = require_string(item, "type_id", at + ".type_id");
        if (!type_id)
            return std::unexpected(type_id.error());
        entry.type_id = *type_id;
        config.exceptions.push_back(std::move(entry));
    }

    if (auto valid = validate_structure(config); !valid)
        return std::unexpected(valid.error());
    return config;
}

std::string matching_config_to_json(const MatchingConfig& config)
{
    json rules = json::array();
    for (const auto& rule : config.rules)
    {
        json item;
        item["case_sensitive"] = rule.case_sensitive;
        item["enabled"] = rule.enabled;
        item["field"] = match_field_name(rule.field);
        item["id"] = rule.id;
        item["pattern"] = rule.pattern;
        item["priority"] = rule.priority;
        item["type_id"] = rule.type_id;
        rules.push_back(std::move(item));
    }

    json exceptions = json::array();
    for (const auto& entry : config.exceptions)
    {
        json item;
        item["enabled"] = entry.enabled;
        item["field"] = match_field_name(entry.field);
        item["id"] = entry.id;
        item["key"] = entry.key;
        item["type_id"] = entry.type_id;
        exceptions.push_back(std::move(item));
    }

    json root;
    root["exceptions"] = std::move(exceptions);
    root["revision"] = config.revision;
    root["rules"] = std::move(rules);
    root["schema_version"] = config.schema_version;
    return root.dump(2);
}

std::string default_matching_config_json()
{
    return std::string(kDefaultMatchingConfigJson);
}

// ---------------- 编译 ----------------

bool registry_has_component_type(const std::string& type_id)
{
    // 只认当前已加载的元件表：用户的历史配置可能省略某些内置行，
    // 单查 builtin 静态集合会放过一个注册表里根本没有的 ID。
    return core::TypeRegistry::instance().find_component(type_id).has_value();
}

util::Result<std::shared_ptr<const MatchingSnapshot>> compile_matching_config(
    const MatchingConfig& config, const TypeExistenceFn& type_exists)
{
    if (auto valid = validate_structure(config); !valid)
        return std::unexpected(valid.error());
    if (!type_exists)
        return std::unexpected(invalid("component type lookup is not available"));

    // 引用的类型必须在当前已加载的元件表里，disabled 条目一样校验。
    for (const auto& rule : config.rules)
    {
        if (!type_exists(rule.type_id))
            return std::unexpected(
                invalid("rules type_id is not a loaded component type: " + rule.type_id));
    }
    for (const auto& entry : config.exceptions)
    {
        if (!type_exists(entry.type_id))
            return std::unexpected(
                invalid("exceptions type_id is not a loaded component type: " + entry.type_id));
    }

    // 模式在这里编译一次；disabled 条目也编译，免得打开后才变坏。
    // 错误消息里带条目 id 和出错偏移：坏模式必须能定位到是哪一条、哪个位置。
    std::vector<detail::CompiledRule> compiled;
    compiled.reserve(config.rules.size());
    for (std::size_t index = 0; index < config.rules.size(); ++index)
    {
        const MatchingRule& rule = config.rules[index];
        const std::string where = "rules[" + std::to_string(index) + "] (id '" + rule.id + "')";
        auto program = RegexProgram::compile(rule.pattern, rule.case_sensitive, where);
        if (!program)
            return std::unexpected(program.error());
        compiled.push_back(detail::CompiledRule{rule, std::move(*program)});
    }
    return std::make_shared<const MatchingSnapshot>(config, std::move(compiled));
}

util::Result<std::shared_ptr<const MatchingSnapshot>> compile_matching_config(
    std::string_view json, const TypeExistenceFn& type_exists)
{
    auto config = parse_matching_config_json(json);
    if (!config)
        return std::unexpected(config.error());
    return compile_matching_config(*config, type_exists);
}

util::Result<std::shared_ptr<const MatchingSnapshot>> compile_matching_config(std::string_view json)
{
    return compile_matching_config(json, TypeExistenceFn(&registry_has_component_type));
}

// ---------------- 仓储 ----------------

TypeMatchingStore::TypeMatchingStore() : type_exists_(&registry_has_component_type)
{
}

TypeMatchingStore::TypeMatchingStore(TypeExistenceFn type_exists) : type_exists_(std::move(type_exists))
{
}

TypeMatchingStore::TypeMatchingStore(TypeExistenceFn type_exists, ConfigFileReadFn read_file)
    : type_exists_(std::move(type_exists)), read_file_(std::move(read_file))
{
}

TypeMatchingStore& TypeMatchingStore::instance()
{
    // 只给对象，不在这里读任何路径 —— 正式装配由调用方显式 load。
    static TypeMatchingStore store;
    return store;
}

util::Result<void> TypeMatchingStore::load(const std::string& path)
{
    // 1) 进锁只捕获当前发布代次，立刻放锁：读取在锁外完成（不把 I/O 关进锁里）。
    //    已有 load_error 不阻止 reload —— 重新 load 正是恢复错误状态的路径。
    std::uint64_t captured = 0;
    {
        std::lock_guard lock(mutex_);
        captured = generation_;
    }

    // 2) 锁外调用读取器：注入的读取器必须自己先调用生产读取器，不能另写一套语义；
    //    没注入时直接用生产默认读取器。读取器抛出的 std::exception 就是这次读取失败，
    //    在这里就地转成失败结果，但不发布任何状态 —— 回锁后先核代次。
    ConfigFileReadResult read;
    try
    {
        if (read_file_)
            read = read_file_(path);
        else
            read = read_matching_config_file(path);
    }
    catch (const std::exception& error)
    {
        read = ConfigFileReadResult{};
        read.status = ConfigFileReadStatus::Failed;
        read.detail = std::string("the reader threw: ") + error.what();
    }

    // 3) 回锁先核代次：读取期间有更新的状态发布（成功 save、被接受的成功/失败 load）时，
    //    这份读取已经过期 —— 不编译、不记错误、不清错误、不重绑路径、不发布快照，
    //    current/path/load_error 一个都不碰。
    std::lock_guard lock(mutex_);
    if (captured != generation_)
        return std::unexpected(stale_load_error(path));
    // 4) 代次到 uint64 上限：这次 load 的结果没办法形成新的发布，明确拒绝并保持当前状态。
    if (generation_ == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(generation_exhausted_error());

    if (read.status == ConfigFileReadStatus::Failed)
    {
        // 目录 / 权限 / 读取中断：保留当前快照与保存绑定，只记录错误。
        // 字符串和完整错误结果都在发布前准备好（两者都可能分配），发布只用
        // noexcept 的 swap / ++，避免仓储已进入坏状态、代次已推进后才构造错误。
        // 具名返回的 NRVO 非强制，Error 也只有复制（无 noexcept move）：
        // 不保证任意 OOM 都能以 Result 返回，只保证这里的已知构造发生在提交之前。
        std::string next_error = "cannot read matching config " + path + ": " + read.detail;
        util::Result<void> failure =
            std::unexpected(util::Error::make<util::Error::Kind::IoError>(next_error));
        load_error_.swap(next_error);
        ++generation_;
        return failure;
    }

    const bool missing = read.status == ConfigFileReadStatus::Missing;
    const std::string text = missing ? default_matching_config_json() : read.bytes;
    auto compiled = compile_matching_config(text, type_exists_);
    if (!compiled)
    {
        // 坏内容不发布、不绑定：下一次普通保存不会覆盖用户原本的文件。
        // 同样的失败再次完成也照样发布（错误字符串会一模一样），代次照常推进。
        // 与读失败分支同样：错误文本、Error 和结果对象全部先于状态提交构造。
        std::string next_error =
            missing ? "default matching seed rejected: " + compiled.error().format_message()
                    : "matching config rejected " + path + ": " + compiled.error().format_message();
        util::Result<void> failure =
            std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(next_error));
        load_error_.swap(next_error);
        ++generation_;
        return failure;
    }

    // 成功：先把可能抛的路径拷贝准备好，再全部用 noexcept 交换发布 ——
    // current/path/load_error 要么一起换到新状态，要么一个都不动。
    std::string next_path = path;
    auto next_snapshot = std::move(*compiled);
    current_.swap(next_snapshot);
    path_.swap(next_path);
    load_error_.clear();
    ++generation_;
    return {};
}

util::Result<std::uint64_t> TypeMatchingStore::save(std::string_view json,
                                                    std::uint64_t expected_revision)
{
    auto config = parse_matching_config_json(json);
    if (!config)
        return std::unexpected(config.error());
    return save(*config, expected_revision);
}

util::Result<std::uint64_t> TypeMatchingStore::save(const MatchingConfig& config,
                                                    std::uint64_t expected_revision)
{
    std::lock_guard lock(mutex_);

    if (!load_error_.empty())
        return std::unexpected(
            invalid("matching config has a load error; fix the file and reload before saving: " +
                    load_error_));
    if (!current_)
        return std::unexpected(invalid("no matching config is loaded; call load() first"));
    if (path_.empty())
        return std::unexpected(
            util::Error::make<util::Error::Kind::IoError>("matching config path is not set"));
    if (config.revision != expected_revision)
        return std::unexpected(invalid("revision mismatch: expected " +
                                       std::to_string(expected_revision) + " but the candidate carries " +
                                       std::to_string(config.revision)));
    // CAS 的另一半：期望的 revision 必须是当前这一份，否则过期草稿会把新配置顶掉。
    if (expected_revision != current_->revision())
        return std::unexpected(invalid("revision mismatch: the stored config is at revision " +
                                       std::to_string(current_->revision()) +
                                       " but expected_revision is " + std::to_string(expected_revision)));
    if (config.revision >= kMaxSafeRevision)
        return std::unexpected(invalid("revision " + std::to_string(config.revision) +
                                       " cannot be incremented (limit " +
                                       std::to_string(kMaxSafeRevision) + ")"));

    // 发布代次预检：到 uint64 上限时在写盘之前就拒绝 —— 磁盘不能先被改成新内容，
    // 再因为代次无法推进而报错。预检与提交、增量同在这一个临界区内。
    if (generation_ == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(generation_exhausted_error());

    // 锁内完成校验 + 编译 + 序列化 + 原子替换：发布前的类型引用检查与发布之间
    // 没有窗口能让删除挤进来（删除路径要先拿到同一把锁，见 acquire_reference_guard）。
    MatchingConfig persisted = config;
    persisted.revision = config.revision + 1;
    auto compiled = compile_matching_config(persisted, type_exists_);
    if (!compiled)
        return std::unexpected(compiled.error());

    // 序列化与写盘在同一个异常边界里：序列化也可能抛（例如 DTO 里带了不是合法 UTF-8
    // 的字节串），公开接口只承诺 Result，不能把异常漏给调用方，也不能在写盘失败之后
    // 才发布快照。两者都在发布之前。
    try
    {
        const std::string text = matching_config_to_json(persisted);
        util::replace_file(path_, text);
    }
    catch (const std::exception& error)
    {
        // 写盘失败不是状态发布：磁盘与内存快照都不动，代次也不推进。
        return std::unexpected(util::Error::make<util::Error::Kind::IoError>(
            "cannot save matching config to " + path_ + ": " + error.what()));
    }

    // 发布成功才推进代次：在途的旧读取从这一刻起过期。此后不再执行任何可能失败的
    // 代次校验，直接返回新 revision。
    current_ = std::move(*compiled);
    ++generation_;
    return persisted.revision;
}

void TypeMatchingStore::register_lease_locked(
    const std::shared_ptr<const MatchingSnapshot>& snapshot) const
{
    // 同一份快照只登记一条：按对象身份去重，并顺手清掉已过期的登记。
    // 登记数量随「存活的快照数」增长，不随 current() 的调用次数增长 ——
    // 配置长期不变时，反复领取同一份快照不会让登记表无限变大。
    // 快照释放只让 weak 自然过期，不会回调本仓储。
    leases_.erase(std::remove_if(leases_.begin(), leases_.end(),
                                 [&snapshot](const std::weak_ptr<const MatchingSnapshot>& lease) {
                                     const auto existing = lease.lock();
                                     return !existing || existing.get() == snapshot.get();
                                 }),
                  leases_.end());
    leases_.emplace_back(snapshot);
}

std::shared_ptr<const MatchingSnapshot> TypeMatchingStore::current() const
{
    std::lock_guard lock(mutex_);
    auto snapshot = current_;
    if (snapshot)
        register_lease_locked(snapshot);
    return snapshot;
}

util::Result<std::shared_ptr<const MatchingSnapshot>> TypeMatchingStore::acquire_business_snapshot() const
{
    // 健康判定与租约登记必须在同一临界区内完成：不能在 guard 之后另开窗口登记，
    // 也不能让调用方用 current() + load_error() 两次加锁拼出一个可以穿过的检查。
    std::lock_guard lock(mutex_);
    if (!load_error_.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
            "matching config has a load error; business operations cannot proceed: " + load_error_));
    if (!current_)
        return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
            "matching config is not loaded; business operations cannot proceed"));
    auto snapshot = current_;
    register_lease_locked(snapshot);
    return snapshot;
}

bool TypeMatchingStore::has_snapshot() const
{
    std::lock_guard lock(mutex_);
    return static_cast<bool>(current_);
}

std::string TypeMatchingStore::load_error() const
{
    std::lock_guard lock(mutex_);
    return load_error_;
}

std::string TypeMatchingStore::path() const
{
    std::lock_guard lock(mutex_);
    return path_;
}

std::vector<std::string> TypeMatchingStore::referenced_type_ids_locked() const
{
    std::vector<std::string> ids = current_->referenced_type_ids();
    for (const auto& lease : leases_)
    {
        if (auto snapshot = lease.lock())
        {
            const auto& referenced = snapshot->referenced_type_ids();
            ids.insert(ids.end(), referenced.begin(), referenced.end());
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

util::Result<TypeMatchingStore::ReferenceGuard> TypeMatchingStore::acquire_reference_guard() const
{
    std::unique_lock lock(mutex_);
    if (!load_error_.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
            "matching config has a load error; type references cannot be verified: " + load_error_));
    if (!current_)
        return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
            "matching config is not loaded; type references cannot be verified"));
    // 查询与后续删除必须共用这一个 guard：锁一直持有到 guard 析构。
    auto ids = referenced_type_ids_locked();
    return ReferenceGuard(std::move(lock), std::move(ids));
}

TypeMatchingStore::ReferenceGuard::ReferenceGuard(std::unique_lock<std::mutex> lock,
                                                  std::vector<std::string> type_ids)
    : lock_(std::move(lock)), type_ids_(std::move(type_ids))
{
}

bool TypeMatchingStore::ReferenceGuard::references(std::string_view type_id) const
{
    // 比较器的两个参数都取 string_view：binary_search 会以 comp(*it, value) 求划分、
    // 再以 comp(value, *it) 判等价，只接受一种实参类型就会在其中一侧编译不过。
    // type_ids_ 是按 std::string 的 operator< 升序去重的；两者同为 char_traits<char>
    // 的逐字节比较，顺序前提一致。
    return std::binary_search(type_ids_.begin(), type_ids_.end(), type_id,
                              [](std::string_view entry, std::string_view wanted) {
                                  return entry < wanted;
                              });
}

}  // namespace kforge::classifier
