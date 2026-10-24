// HTTP 无关的匹配配置适配器；正式路由与测试共用解析、调用和报告序列化。
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "classifier/type_matching.h"
#include "interface/service/classification/service.h"
#include "util/logger.h"
#include "util/service_runtime.hpp"

namespace kforge::api
{

struct TypeMatchingStartup
{
    classifier::TypeMatchingStore* store;
    std::string configured_path;
    bool attempted = false;

    // 构造只保存正式装配决定的路径；I/O 发生在 Registry 启动之后。
    TypeMatchingStartup(classifier::TypeMatchingStore& matching_store, std::string path)
        : store(&matching_store), configured_path(std::move(path)) {}

    static util::Result<void> build()
    {
        auto& slot = launcher::util::instance_store<TypeMatchingStartup>();
        if (!slot || !slot->store || slot->configured_path.empty())
            return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
                "Type matching startup is not configured"));
        slot->attempted = true;
        auto loaded = slot->store->load(slot->configured_path);
        if (!loaded)
            util::log_error{}("Type matching config load failed ({}): {}",
                              slot->configured_path, loaded.error().format_message());
        // 坏配置仍需开放状态展示和显式 reload；业务健康门由 store/service 维护。
        return {};
    }
};

struct TypeMatchingContext
{
    storage::Database& db;
    classifier::TypeMatchingStore& store;
    std::string configured_path;

    TypeMatchingContext(storage::Database& database, classifier::TypeMatchingStore& matching_store,
                        std::string path)
        : db(database), store(matching_store), configured_path(std::move(path)) {}
};

// 只在路由注册时读取启动槽位；请求闭包持有路径副本，不再查询全局装配。
inline util::Result<TypeMatchingContext> configured_type_matching_context(storage::Database& db)
{
    const auto& slot = launcher::util::instance_store<TypeMatchingStartup>();
    if (!slot || !slot->attempted || !slot->store || slot->configured_path.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
            "Type matching API assembly error: startup has not attempted its configured load"));
    return TypeMatchingContext(db, *slot->store, slot->configured_path);
}

inline nlohmann::json type_matching_error(std::string message)
{
    return {{"ok", false}, {"error", std::move(message)}};
}

inline util::Result<nlohmann::json> type_matching_parse_request_body(std::string_view text)
{
    try
    {
        auto body = nlohmann::json::parse(text);
        if (!body.is_object())
            return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
                "Request body must be an object"));
        return body;
    }
    catch (const nlohmann::json::exception& e)
    {
        return std::unexpected(util::Error::make<util::Error::Kind::ParseError>(
            std::string("Invalid JSON request: ") + e.what()));
    }
}

namespace type_matching_detail
{
inline std::string validate_keys(const nlohmann::json& body,
                                  std::initializer_list<std::string_view> allowed)
{
    if (!body.is_object()) return "Request body must be an object";
    for (auto it = body.begin(); it != body.end(); ++it)
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
            return "Unsupported request field: " + it.key();
    return {};
}

inline util::Result<std::uint64_t> expected_revision(const nlohmann::json& body)
{
    const auto it = body.find("expected_revision");
    if (it == body.end())
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Missing required field: expected_revision"));
    if (!it->is_number_integer() ||
        (it->is_number_integer() && !it->is_number_unsigned() && it->get<std::int64_t>() < 0))
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "expected_revision must be a non-negative safe integer"));
    const auto value = it->get<std::uint64_t>();
    if (value > classifier::kMaxSafeRevision)
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "expected_revision exceeds the maximum safe integer"));
    return value;
}

inline util::Result<classifier::MatchSample> sample(const nlohmann::json& input)
{
    if (!input.is_object())
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "sample must be an object"));
    classifier::MatchSample result;
    for (const auto field : {classifier::MatchField::Name, classifier::MatchField::Value,
                             classifier::MatchField::Model, classifier::MatchField::Reference})
    {
        const auto* key = classifier::match_field_name(field);
        const auto it = input.find(key);
        if (it == input.end()) continue;
        if (!it->is_string())
            return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
                std::string("sample.") + key + " must be a string when present"));
        const auto value = it->get<std::string>();
        switch (field)
        {
            case classifier::MatchField::Name: result.name = value; break;
            case classifier::MatchField::Value: result.value = value; break;
            case classifier::MatchField::Model: result.model = value; break;
            case classifier::MatchField::Reference: result.reference = value; break;
        }
    }
    const auto error = validate_keys(input, {"name", "value", "model", "reference"});
    if (!error.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "sample: " + error));
    return result;
}
}  // namespace type_matching_detail

inline nlohmann::json type_matching_sample_json(const classifier::MatchSample& sample)
{
    nlohmann::json out = nlohmann::json::object();
    for (const auto field : {classifier::MatchField::Name, classifier::MatchField::Value,
                             classifier::MatchField::Model, classifier::MatchField::Reference})
        if (const auto* text = sample.field(field)) out[classifier::match_field_name(field)] = *text;
    return out;
}

inline nlohmann::json type_matching_evidence_json(
    const std::vector<classifier::MatchEvidence>& evidence)
{
    auto out = nlohmann::json::array();
    for (const auto& item : evidence)
        out.push_back({{"source", item.source == classifier::MatchEvidenceSource::Rule
                                       ? "rule" : "exception"},
                       {"id", item.id}, {"field", classifier::match_field_name(item.field)},
                       {"text", item.text}, {"priority", item.priority}, {"type_id", item.type_id}});
    return out;
}

inline nlohmann::json type_matching_result_json(const services::ClassificationService::Result& result)
{
    return {{"name", result.symbol_name}, {"library", result.target_library},
            {"type_id", result.type_id}, {"type", result.type_name},
            {"confidence", result.confidence}, {"symbol_id", result.symbol_id},
            {"status", result.status}, {"reason", result.reason}, {"evidence", result.evidence},
            {"raw_type_id", result.raw_type_id}, {"sample", type_matching_sample_json(result.sample)},
            {"candidate_type_id", result.candidate_type_id},
            {"structured_evidence", type_matching_evidence_json(result.structured_evidence)},
            {"is_protected", result.is_protected}, {"would_update", result.would_update},
            {"write_status", services::ClassificationService::write_status_name(result.write_status)},
            {"write_reason", result.write_reason}};
}

inline nlohmann::json type_matching_summary_json(const services::ClassificationService::Summary& summary)
{
    return {{"total", summary.total}, {"revision", summary.revision},
            {"protected_count", summary.protected_count}, {"matched", summary.matched},
            {"unmatched", summary.unmatched}, {"conflicts", summary.conflicts},
            {"input_too_long", summary.input_too_long},
            {"evaluation_limited", summary.evaluation_limited},
            {"evaluation_errors", summary.evaluation_errors}, {"would_update", summary.would_update},
            {"unchanged", summary.unchanged}, {"type_updated", summary.type_updated},
            {"source_changed", summary.source_changed}, {"cas_skipped", summary.cas_skipped},
            {"deleted", summary.deleted}, {"rolled_back", summary.rolled_back},
            {"not_attempted", summary.not_attempted}};
}

inline nlohmann::json type_matching_report_json(const services::ClassificationService::Summary& summary)
{
    auto details = nlohmann::json::array();
    for (const auto& result : summary.results)
    {
        if (details.size() == 100) break;
        details.push_back(type_matching_result_json(result));
    }
    return {{"ok", true}, {"summary", type_matching_summary_json(summary)},
            {"details", std::move(details)}};
}

inline nlohmann::json type_matching_get(const TypeMatchingContext& context)
{
    const auto snapshot = context.store.current();
    const auto load_error = context.store.load_error();
    auto out = snapshot ? nlohmann::json::parse(classifier::matching_config_to_json(snapshot->config()))
                        : nlohmann::json::object();
    out["ok"] = snapshot != nullptr && load_error.empty();
    out["load_error"] = load_error;
    out["has_snapshot"] = snapshot != nullptr;
    out["scope"] = "global";
    out["engine"] = {{"name", "PCRE2"}, {"dialect", "PCRE2"}, {"code_unit_width", 8},
                     {"semantics", "byte"}, {"utf", false}, {"ucp", false},
                     {"case_folding", "ASCII"}, {"jit", false}, {"search", "regex_search"},
                     {"newline", "ANYCRLF"}, {"multiline", false}, {"dotall", false},
                     {"dollar_end_only", true}, {"wall_clock_timeout", false}};
    out["limits"] = {{"max_rules", classifier::kMaxMatchingEntries},
                     {"max_exceptions", classifier::kMaxMatchingEntries},
                     {"max_pattern_bytes", classifier::kMaxPatternBytes},
                     {"max_exception_key_bytes", classifier::kMaxExceptionKeyBytes},
                     {"max_source_field_bytes", classifier::kMaxSourceFieldBytes},
                     {"min_priority", classifier::kMinRulePriority},
                     {"max_priority", classifier::kMaxRulePriority},
                     {"max_revision", classifier::kMaxSafeRevision},
                     {"max_callouts_per_evaluation", classifier::kMaxCalloutsPerEvaluation},
                     {"match_limit", classifier::kRegexMatchLimit},
                     {"depth_limit", classifier::kRegexDepthLimit},
                     {"heap_limit_kib", classifier::kRegexHeapLimitKib},
                     {"parens_nest_limit", classifier::kRegexParensNestLimit}};
    return out;
}

inline nlohmann::json type_matching_save(const TypeMatchingContext& context, const nlohmann::json& body)
{
    const auto error = type_matching_detail::validate_keys(body, {"config", "expected_revision"});
    if (!error.empty()) return type_matching_error(error);
    auto revision = type_matching_detail::expected_revision(body);
    if (!revision) return type_matching_error(revision.error().format_message());
    const auto config = body.find("config");
    if (config == body.end()) return type_matching_error("Missing required field: config");
    if (!config->is_object()) return type_matching_error("config must be an object");
    auto saved = context.store.save(config->dump(), *revision);
    if (!saved) return type_matching_error(saved.error().format_message());
    // 报告这次已经提交的版本；随后另一次 save/load 不应改变本次成功结果。
    return {{"ok", true}, {"revision", *saved}};
}

inline nlohmann::json type_matching_reload(const TypeMatchingContext& context, const nlohmann::json& body)
{
    const auto error = type_matching_detail::validate_keys(body, {});
    if (!error.empty()) return type_matching_error(error);
    if (context.configured_path.empty())
        return type_matching_error("Type matching API assembly error: configured path is empty");
    auto loaded = context.store.load(context.configured_path);
    auto out = type_matching_get(context);
    if (!loaded)
    {
        out["ok"] = false;
        out["error"] = loaded.error().format_message();
    }
    return out;
}

inline nlohmann::json type_matching_preview(const TypeMatchingContext& context, const nlohmann::json& body)
{
    const auto error = type_matching_detail::validate_keys(body, {"config", "sample"});
    if (!error.empty()) return type_matching_error(error);
    const bool has_sample = body.contains("sample");
    const bool has_draft = body.contains("config");
    std::optional<classifier::MatchSample> sample;
    if (has_sample)
    {
        auto parsed = type_matching_detail::sample(body.at("sample"));
        if (!parsed) return type_matching_error(parsed.error().format_message());
        sample = std::move(*parsed);
    }
    std::shared_ptr<const classifier::MatchingSnapshot> draft;
    if (has_draft)
    {
        if (!body.at("config").is_object()) return type_matching_error("config must be an object");
        auto compiled = classifier::compile_matching_config(body.at("config").dump());
        if (!compiled) return type_matching_error(compiled.error().format_message());
        draft = std::move(*compiled);
    }
    if (has_sample)
    {
        auto snapshot = draft;
        if (!snapshot)
        {
            auto healthy = context.store.acquire_business_snapshot();
            if (!healthy) return type_matching_error(healthy.error().format_message());
            snapshot = std::move(*healthy);
        }
        const auto decision = snapshot->evaluate(*sample);
        const auto load_error = context.store.load_error();
        return {{"ok", true}, {"mode", "sample"}, {"source", has_draft ? "draft" : "saved"},
                {"revision", snapshot->revision()}, {"sample", type_matching_sample_json(*sample)},
                {"status", classifier::match_status_name(decision.status)},
                {"type_id", decision.type_id}, {"reason", decision.reason},
                {"structured_evidence", type_matching_evidence_json(decision.evidence)},
                {"load_error", load_error}, {"saved_healthy", load_error.empty() && context.store.has_snapshot()}};
    }
    services::ClassificationService service(&context.db, &context.store);
    auto summary = service.preview_all(std::move(draft), 100);
    if (!summary) return type_matching_error(summary.error().format_message());
    auto out = type_matching_report_json(*summary);
    out["mode"] = "database";
    out["source"] = has_draft ? "draft" : "saved";
    return out;
}

inline nlohmann::json type_matching_apply_report_json(
    const util::Result<services::ClassificationService::ApplyReport>& report)
{
    if (!report) return type_matching_error(report.error().format_message());
    auto out = type_matching_report_json(report->summary);
    if (report->error)
    {
        out["ok"] = false;
        out["error"] = report->error->format_message();
    }
    out["source"] = "saved";
    return out;
}

inline nlohmann::json type_matching_apply(const TypeMatchingContext& context, const nlohmann::json& body)
{
    const auto error = type_matching_detail::validate_keys(body, {"expected_revision"});
    if (!error.empty()) return type_matching_error(error);
    auto revision = type_matching_detail::expected_revision(body);
    if (!revision) return type_matching_error(revision.error().format_message());
    services::ClassificationService service(&context.db, &context.store);
    return type_matching_apply_report_json(service.apply_all(*revision));
}

inline nlohmann::json type_matching_classify(const TypeMatchingContext& context)
{
    services::ClassificationService service(&context.db, &context.store);
    auto report = service.apply_all({});
    auto out = type_matching_apply_report_json(report);
    if (report)
    {
        // 旧界面仍读取这四个顶层字段；明细与新接口共用事实序列化。
        out["total"] = report->summary.total;
        out["matched"] = report->summary.matched;
        out["type_updated"] = report->summary.type_updated;
        out["results"] = out.at("details");
    }
    return out;
}

}  // namespace kforge::api
