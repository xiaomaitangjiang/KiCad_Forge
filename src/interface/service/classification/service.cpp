#include "interface/service/classification/service.h"

#include "core/db/database.h"
#include "core/model/type_registry.h"
#include "core/repo/repositories.h"
#include "classifier/rule_engine.h"
#include "classifier/rule_loader.h"
#include "util/logger.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kforge::services {

const char* ClassificationService::write_status_name(WriteStatus status)
{
    switch (status)
    {
        case WriteStatus::ReadOnly: return "read_only";
        case WriteStatus::NotEligible: return "not_eligible";
        case WriteStatus::Unchanged: return "unchanged";
        case WriteStatus::NotAttempted: return "not_attempted";
        case WriteStatus::Updated: return "updated";
        case WriteStatus::SourceChanged: return "source_changed";
        case WriteStatus::CasSkipped: return "cas_skipped";
        case WriteStatus::Deleted: return "deleted";
        case WriteStatus::RolledBack: return "rolled_back";
    }
    return "read_only";
}

namespace
{

using classifier::Decision;
using classifier::MatchSample;
using classifier::MatchStatus;
using WriteStatus = ClassificationService::WriteStatus;
using Kind = util::Error::Kind;

/// 每批一个短事务的边界：与旧实现一致，500 条后提交并释放 DB 锁。
constexpr std::size_t kBatchSize = 500;
/// 明细默认上限（计数不受它影响，扫描永不提前停止）。
constexpr std::size_t kDefaultDetailLimit = 100;

/// 对外展示用的类型 ID：已决候选优先，其次保留已有原值（含 custom/未注册 ID），
/// 两者都没有才算 Unknown。
std::string effective_type_id(const std::string& decided, const std::string& current)
{
    if (!decided.empty()) return decided;
    if (!current.empty()) return current;
    return core::builtin_component_id(core::ComponentType::Unknown);
}

/// 已有类型身份（内置 / custom / 未注册历史 ID）一律保护：只有 NULL/空串与
/// 字面 "Unknown" 属于「还没有类型」，才允许被配置候选补齐。
bool is_known_type(const std::string& raw_type_id)
{
    return !raw_type_id.empty()
           && raw_type_id != core::builtin_component_id(core::ComponentType::Unknown);
}

/// 结果里的类型身份：已知身份原样；否则用配置候选；两者都没有显示 Unknown。
std::string displayed_type_id(const std::string& raw_type_id,
                              const std::string& decided_type_id)
{
    if (is_known_type(raw_type_id)) return raw_type_id;
    return effective_type_id(decided_type_id, raw_type_id);
}

/// 推荐库兼容输入：已知 raw ID 原样带入；未分类且配置 Matched 时带入候选；
/// 其余未分类保留原值（空串/Unknown）。调用方必须关闭隐藏的位号推断 ——
/// 推荐结果只是展示，绝不用它的 inferred_type_id 决定 DB 类型。
core::Symbol recommendation_input(const core::Symbol& symbol, const std::string& raw_type_id,
                                  const std::string& decided_type_id)
{
    core::Symbol input = symbol;
    if (!is_known_type(raw_type_id) && !decided_type_id.empty())
        input.component_type_id = decided_type_id;
    return input;
}

/// 旧推荐库字段（target/confidence）只作兼容展示，条件与排序不变。
void fill_recommendation(const classifier::RuleEngine& engine, const core::Symbol& symbol,
                         const std::string& raw_type_id, const std::string& decided_type_id,
                         ClassificationService::Result& result)
{
    const auto recommendations = engine.evaluate_symbol(
        recommendation_input(symbol, raw_type_id, decided_type_id), /*infer_reference=*/false);
    if (recommendations.empty()) return;
    result.target_library = recommendations.front().target_library;
    result.confidence = recommendations.front().confidence;
}

/// 命中依据摘要（字段:条目 id -> 类型 + 规则优先级）；不带可能超长的字段原文。
std::string evidence_text(const classifier::MatchEvidence& evidence)
{
    std::string text = std::string(classifier::match_field_name(evidence.field)) + ":"
                       + evidence.id + " -> " + evidence.type_id;
    if (evidence.source == classifier::MatchEvidenceSource::Rule)
        text += " (priority " + std::to_string(evidence.priority) + ")";
    else
        text += " (exception)";
    return text;
}

/// 一行的判定事实：raw/sample 取自同一次仓储读快照。已知身份先保护、不跑 regex，
/// 也没有候选/依据；其余行才走真实核心决策。整份计划与单符号只读共用这一处。
struct RowFacts
{
    std::string raw_type_id;
    MatchSample sample;
    bool is_protected = false;
    Decision decision;                 // protected 时保持默认值，不采用
    std::string candidate_type_id;     // 只有 eligible 且 Matched 时非空
    bool would_update = false;
    bool unchanged = false;
};

RowFacts evaluate_row(const classifier::MatchingSnapshot& snapshot, const core::Symbol& symbol)
{
    RowFacts facts;
    facts.raw_type_id = symbol.component_type_id;
    facts.sample = MatchSample::from_symbol(symbol);
    if (is_known_type(facts.raw_type_id))
    {
        facts.is_protected = true;
        return facts;   // 已知身份：跳过 regex，不伪造候选与依据
    }
    facts.decision = snapshot.evaluate(facts.sample);
    if (facts.decision.matched())
    {
        facts.candidate_type_id = facts.decision.type_id;
        // 候选与原值相同（例如 Matched 到字面 Unknown 而原值已是 Unknown）不排入写计划
        facts.would_update = !facts.candidate_type_id.empty()
                             && facts.candidate_type_id != facts.raw_type_id;
        facts.unchanged = !facts.would_update;
    }
    return facts;
}

/// 决策/保护状态名：known 行是独立的业务状态 "protected"，其余沿用核心状态名。
const char* row_status_name(const RowFacts& facts)
{
    if (facts.is_protected) return "protected";
    return classifier::match_status_name(facts.decision.status);
}

/// 只读路径的写状态原因（preview / classify_symbol 从不尝试写）。
const char* readonly_reason(const RowFacts& facts)
{
    if (facts.is_protected) return "read only: existing type identity is kept";
    if (facts.would_update) return "read only: candidate differs from the stored type";
    if (facts.decision.matched()) return "read only: stored type already equals the candidate";
    return "read only: no write candidate";
}

/// apply 计划的初始写状态：protected/nonmatched 没有可写候选；Matched 里候选等于原值
/// 的不需要写；真正待写的候选在本批开始前是 NotAttempted。
WriteStatus planned_write_status(const RowFacts& facts)
{
    if (facts.is_protected || !facts.decision.matched()) return WriteStatus::NotEligible;
    return facts.would_update ? WriteStatus::NotAttempted : WriteStatus::Unchanged;
}

std::string planned_write_reason(const RowFacts& facts)
{
    if (facts.is_protected) return "not eligible: protected by an existing type identity";
    if (!facts.decision.matched())
        return std::string("not eligible: ")
               + classifier::match_status_name(facts.decision.status);
    if (facts.would_update) return "not attempted: pending the write batch";
    return "unchanged: stored type already equals the candidate";
}

/// 同一次读快照的事实 → 对外 DTO（旧字段与新字段一次填齐，避免第二套易漂移判定）。
void fill_result(const RowFacts& facts, const core::Symbol& symbol, const std::string& symbol_id,
                 ClassificationService::Result& result)
{
    result.symbol_name = symbol.name();
    result.symbol_id = symbol_id;
    result.status = row_status_name(facts);
    result.reason = facts.is_protected ? "protected: existing component type is kept"
                                       : facts.decision.reason;
    result.evidence.clear();
    result.evidence.reserve(facts.decision.evidence.size());
    for (const auto& evidence : facts.decision.evidence)
        result.evidence.push_back(evidence_text(evidence));
    result.raw_type_id = facts.raw_type_id;
    result.sample = facts.sample;
    result.candidate_type_id = facts.candidate_type_id;
    result.structured_evidence = facts.decision.evidence;
    result.is_protected = facts.is_protected;
    result.would_update = facts.would_update;
    result.type_id = displayed_type_id(facts.raw_type_id, facts.candidate_type_id);
    result.type_name = core::TypeRegistry::instance().component_display(result.type_id);
    // 只读是默认：写回路径会按计划/提交结果覆盖成真实写过程状态
    result.write_status = WriteStatus::ReadOnly;
    result.write_reason = readonly_reason(facts);
}

/// 一次待写回的类型变更。CAS 需要分类时读到的原值（NULL/空串读成空串），
/// 期间被人改过就放弃这一条，绝不拿旧快照覆盖。
struct PendingTypeUpdate
{
    std::string symbol_id;
    std::string expected_type_id;
    std::string new_type_id;
    /// 决策时四个真实来源字段的原值（缺失与空串是两回事）
    MatchSample expected;
    /// 进入 capped 明细的行才有：写过程状态要回写到同一条明细上
    std::optional<std::size_t> detail_index;
};

struct ClassificationPlan
{
    ClassificationService::Summary summary;
    std::vector<PendingTypeUpdate> updates;
};

/// 四个匹配来源字段逐项相等才算「来源没变」：不改大小写、不 trim，
/// optional 的缺失与显式空串也区分开。不比较整 properties_json / data_version。
bool same_match_inputs(const MatchSample& expected, const MatchSample& current)
{
    return expected.name == current.name && expected.value == current.value
           && expected.model == current.model && expected.reference == current.reference;
}

/// 读一次 DB、在给定快照上把全部行的决策算完；不写任何东西。
/// 会话在匹配之前释放：下面的正则求值不持 DBsession，也不持配置锁。
util::Result<ClassificationPlan> build_plan(storage::Database& db,
                                            const classifier::MatchingSnapshot& snapshot,
                                            std::size_t detail_limit, bool writable)
{
    std::vector<core::Symbol> syms;
    {
        auto session = db.session();
        auto loaded = storage::SymbolRepository(session).find_all();
        if (!loaded)
        {
            // 读失败不是「没有可分类的符号」：报错，别返回一个空摘要冒充成功
            return std::unexpected(loaded.error());
        }
        syms = std::move(*loaded);
    }

    classifier::RuleEngine engine(classifier::RuleLoader::default_rules());

    ClassificationPlan plan;
    plan.summary.total = static_cast<int>(syms.size());
    plan.summary.revision = snapshot.revision();
    plan.updates.reserve(syms.size());

    for (const auto& sym : syms)
    {
        const RowFacts facts = evaluate_row(snapshot, sym);
        ClassificationService::Summary& counts = plan.summary;
        if (facts.is_protected)
        {
            // 保护行是独立轴：既不进 eligible 六状态，也不被超限/报错牵连
            counts.protected_count++;
        }
        else
        {
            switch (facts.decision.status)
            {
                case MatchStatus::Matched: counts.matched++; break;
                case MatchStatus::Unmatched: counts.unmatched++; break;
                case MatchStatus::Conflict: counts.conflicts++; break;
                case MatchStatus::InputTooLong: counts.input_too_long++; break;
                case MatchStatus::EvaluationLimit: counts.evaluation_limited++; break;
                case MatchStatus::EvaluationError: counts.evaluation_errors++; break;
            }
            if (facts.would_update)
                counts.would_update++;
            else if (facts.decision.matched())
                counts.unchanged++;
        }

        std::optional<std::size_t> detail_index;
        if (counts.results.size() < detail_limit)
        {
            detail_index = counts.results.size();
            ClassificationService::Result result;
            fill_result(facts, sym, sym.id(), result);
            if (writable)
            {
                result.write_status = planned_write_status(facts);
                result.write_reason = planned_write_reason(facts);
            }
            fill_recommendation(engine, sym, facts.raw_type_id, facts.candidate_type_id, result);
            counts.results.push_back(std::move(result));
        }
        if (writable && facts.would_update)
            plan.updates.push_back(PendingTypeUpdate{sym.id(), facts.raw_type_id,
                                                      facts.candidate_type_id, facts.sample,
                                                      detail_index});
    }
    return plan;
}

/// 把上一批失败包装成旧格式的兼容错误：此前已提交多少条必须写清楚。
util::Error batch_failed(std::uint64_t committed, const util::Error& error)
{
    return util::Error(error.kind(),
                       "classification stopped after " + std::to_string(committed)
                           + " committed type updates: " + error.format_message());
}

/// 一批里一条候选的局部写结果：只在 COMMIT 成功后发布到明细上。
struct BatchEntry
{
    std::optional<std::size_t> detail_index;
    WriteStatus status = WriteStatus::NotAttempted;
    std::string reason;
};

void mark_detail(ClassificationService::Summary& summary, const std::optional<std::size_t>& index,
                 WriteStatus status, std::string reason)
{
    if (!index || *index >= summary.results.size())
        return;   // 明细被上限截掉的行只计数量，不额外补读报告
    ClassificationService::Result& result = summary.results[*index];
    result.write_status = status;
    result.write_reason = std::move(reason);
}

void mark_range(ClassificationService::Summary& summary,
                const std::vector<PendingTypeUpdate>& updates, std::size_t begin, std::size_t end,
                WriteStatus status, const char* reason)
{
    for (std::size_t index = begin; index < end; ++index)
        mark_detail(summary, updates[index].detail_index, status, reason);
}

/// 按 500 条短事务写回。每批在自己的作用域里取 DbSession：COMMIT 与局部状态发布
/// 结束后析构释放 DB 锁，再进入下一批；批内任何一步失败都整批回滚并报错，
/// updated / source_changed / cas_skipped / deleted 只在 commit 成功后发布。
/// 返回 nullopt = 全部提交。
std::optional<util::Error> apply_updates(storage::Database& db,
                                         ClassificationService::Summary& summary,
                                         const std::vector<PendingTypeUpdate>& updates)
{
    for (std::size_t offset = 0; offset < updates.size(); offset += kBatchSize)
    {
        const std::size_t end = std::min(offset + kBatchSize, updates.size());
        std::optional<util::Error> failure;
        std::vector<BatchEntry> entries;
        entries.reserve(end - offset);
        int updated = 0;
        int source_changed = 0;
        int cas_skipped = 0;
        int deleted = 0;
        {
            storage::DbSession session = db.session();
            auto begun = session.begin();
            if (!begun)
            {
                // 没有事务：本批与后续候选都没被尝试过，绝不算 RolledBack
                mark_range(summary, updates, offset, end, WriteStatus::NotAttempted,
                           "not attempted: batch transaction could not begin");
                mark_range(summary, updates, end, updates.size(), WriteStatus::NotAttempted,
                           "not attempted: an earlier batch failed");
                summary.not_attempted += static_cast<int>(updates.size() - offset);
                return batch_failed(summary.type_updated, begun.error());
            }

            storage::SymbolRepository repo(session);
            for (std::size_t index = offset; index < end; ++index)
            {
                const PendingTypeUpdate& item = updates[index];
                // 批内重读当前行：只比较四个真实来源字段 + 原类型，不比较其他列
                auto current = repo.find_by_id(item.symbol_id);
                if (!current)
                {
                    if (current.error().kind() == Kind::NotFound)
                    {
                        deleted++;   // 行已删除：跳过，不是错误
                        entries.push_back({item.detail_index, WriteStatus::Deleted,
                                           "deleted: row no longer exists"});
                        continue;
                    }
                    failure = current.error();
                    break;
                }
                if (!same_match_inputs(item.expected, MatchSample::from_symbol(*current)))
                {
                    source_changed++;   // 决策所依据的输入已经被改：旧结论作废
                    entries.push_back({item.detail_index, WriteStatus::SourceChanged,
                                       "source changed: matching inputs differ at write time"});
                    continue;
                }
                // 原值仍等于决策时读到的原始 ID（NULL/空串都按空串比较）才写这一列
                auto changed = repo.update_component_type_if_current(item.symbol_id,
                                                                     item.expected_type_id,
                                                                     item.new_type_id);
                if (!changed)
                {
                    failure = changed.error();
                    break;
                }
                if (*changed)
                {
                    updated++;
                    entries.push_back({item.detail_index, WriteStatus::Updated,
                                       "updated: type written in a committed batch"});
                }
                else
                {
                    cas_skipped++;   // 并发改了类型 / 值本来就相同
                    entries.push_back({item.detail_index, WriteStatus::CasSkipped,
                                       "cas skipped: stored type changed since the decision"});
                }
            }

            if (!failure)
            {
                auto committed = session.commit();
                if (!committed)
                    failure = committed.error();
            }
        }   // 未提交事务在这里真实回滚，DB 锁随即释放

        if (failure)
        {
            // 当前批全部候选未提交；后续候选从未开始
            mark_range(summary, updates, offset, end, WriteStatus::RolledBack,
                       "rolled back: the current batch was not committed");
            mark_range(summary, updates, end, updates.size(), WriteStatus::NotAttempted,
                       "not attempted: an earlier batch failed");
            summary.rolled_back += static_cast<int>(end - offset);
            summary.not_attempted += static_cast<int>(updates.size() - end);
            return batch_failed(summary.type_updated, *failure);
        }

        // 只有真正提交成功才发布这一批的计数与明细状态
        for (const auto& entry : entries)
            mark_detail(summary, entry.detail_index, entry.status, entry.reason);
        summary.type_updated += updated;
        summary.source_changed += source_changed;
        summary.cas_skipped += cas_skipped;
        summary.deleted += deleted;
    }
    return std::nullopt;
}

}  // namespace

ClassificationService::ClassificationService(storage::Database* db,
                                             classifier::TypeMatchingStore* matching_store)
    : db_(db), matching_store_(matching_store ? matching_store
                                            : &classifier::TypeMatchingStore::instance()) {}

util::Result<ClassificationService::Summary>
ClassificationService::preview_all(std::shared_ptr<const classifier::MatchingSnapshot> draft,
                                   std::size_t detail_limit)
{
    // 健康判定与操作 lease 与 draft 无关：坏配置 / 未 load 的 store 在读 DB 之前拒绝。
    // 这份 lease 一直持到整次读与计算结束，draft 只替换求值快照。
    auto acquired = matching_store_->acquire_business_snapshot();
    if (!acquired)
        return std::unexpected(acquired.error());
    std::shared_ptr<const classifier::MatchingSnapshot> lease = std::move(*acquired);

    const classifier::MatchingSnapshot& evaluation = draft ? *draft : *lease;
    auto plan = build_plan(*db_, evaluation, detail_limit, /*writable=*/false);
    if (!plan)
        return std::unexpected(plan.error());
    // 预览不写库：type_updated 恒为 0，would_update 等计划事实照实上报
    plan->summary.type_updated = 0;
    return std::move(plan->summary);
}

util::Result<ClassificationService::ApplyReport>
ClassificationService::apply_all(std::optional<std::uint64_t> expected_revision)
{
    // 健康判定 + lease 登记在取得 DbSession 之前原子完成；这份 lease 一直持有到
    // 所有 commit/rollback 结束，绝不在这里换用「最后有效快照」。
    auto acquired = matching_store_->acquire_business_snapshot();
    if (!acquired)
        return std::unexpected(acquired.error());
    std::shared_ptr<const classifier::MatchingSnapshot> lease = std::move(*acquired);

    if (expected_revision && lease->revision() != *expected_revision)
        return std::unexpected(util::Error::make<Kind::ServiceError>(
            "stale matching revision: expected " + std::to_string(*expected_revision)
            + ", current " + std::to_string(lease->revision())));

    auto plan = build_plan(*db_, *lease, kDefaultDetailLimit, /*writable=*/true);
    if (!plan)
        return std::unexpected(plan.error());

    ApplyReport report;
    report.summary = std::move(plan->summary);
    report.error = apply_updates(*db_, report.summary, plan->updates);
    return report;
}

util::Result<ClassificationService::Summary> ClassificationService::classify_all()
{
    auto report = apply_all({});
    if (!report)
        return std::unexpected(report.error());
    if (report->error)
        return std::unexpected(*report->error);
    return std::move(report->summary);
}

util::Result<std::vector<ClassificationService::Result>>
ClassificationService::classify_symbol(const std::string& symbol_id)
{
    // 同一健康门：坏配置 / 未 load 的 store 一律拒绝，只读接口也不能例外
    auto acquired = matching_store_->acquire_business_snapshot();
    if (!acquired)
        return std::unexpected(acquired.error());
    std::shared_ptr<const classifier::MatchingSnapshot> lease = std::move(*acquired);

    core::Symbol symbol;
    {
        auto session = db_->session();
        auto row = storage::SymbolRepository(session).find_by_id(symbol_id);
        if (!row) return std::unexpected(row.error());
        symbol = std::move(*row);
    }

    // 与批量同一条判定内核：known 先保护、不跑 regex；会话已释放才求值
    const RowFacts facts = evaluate_row(*lease, symbol);

    classifier::RuleEngine engine(classifier::RuleLoader::default_rules());
    const auto recommendations = engine.evaluate_symbol(
        recommendation_input(symbol, facts.raw_type_id, facts.candidate_type_id),
        /*infer_reference=*/false);

    std::vector<Result> results;
    results.reserve(std::max<std::size_t>(recommendations.size(), 1));
    for (const auto& recommendation : recommendations)
    {
        Result result;
        fill_result(facts, symbol, symbol_id, result);
        result.target_library = recommendation.target_library;
        result.confidence = recommendation.confidence;
        results.push_back(std::move(result));
    }
    if (results.empty())
    {
        // 没有推荐库结果也不能丢匹配核心的结论与状态
        Result result;
        fill_result(facts, symbol, symbol_id, result);
        results.push_back(std::move(result));
    }
    return results;
}

}  // namespace kforge::services
