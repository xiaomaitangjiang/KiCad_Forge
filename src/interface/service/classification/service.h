#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "util/error.h"
#include "classifier/rule.h"
#include "classifier/type_matching.h"
#include "core/model/symbol.h"

namespace kforge::storage { class Database; }

namespace kforge::services {

/// Runs the saved matching config against database symbols and writes the
/// decided component type back (one column, CAS on the original raw value).
///
/// 类型判定的唯一来源是 TypeMatchingStore 里已保存的配置：精确特例 → 配置正则 →
/// Unmatched。已知类型身份（内置 / custom / 未注册历史 ID）原样保留，只有
/// NULL/空串/字面 "Unknown" 才允许被 Matched 候选补齐；其余状态一律不写。
///
/// 一行要么受保护、要么恰好落入一个 eligible 决策状态，两者互斥：
///   total == protected_count + matched + unmatched + conflicts
///          + input_too_long + evaluation_limited + evaluation_errors
/// 计数覆盖全部行，结果明细的条数上限不影响计数。
class ClassificationService {
public:
    explicit ClassificationService(storage::Database* db,
                                   classifier::TypeMatchingStore* matching_store = nullptr);

    /// 真实写回过程的状态（与决策状态分开）。名字是 B2 的稳定映射键：
    /// read_only / not_eligible / unchanged / not_attempted / updated /
    /// source_changed / cas_skipped / deleted / rolled_back。
    enum class WriteStatus
    {
        ReadOnly,       // preview / classify_symbol：只读，从未尝试写入
        NotEligible,    // protected 或非 Matched：本次没有可写候选
        Unchanged,      // Matched 但候选等于原值：不需要写
        NotAttempted,   // apply 里待写候选：本批未开始，或前批失败后没轮到
        Updated,        // 已提交批中确实写入了类型
        SourceChanged,  // 已提交批中发现四个来源字段变化，跳过
        CasSkipped,     // 已提交批中原类型已被并发改写，CAS 没写
        Deleted,        // 已提交批中行已不存在，跳过
        RolledBack,     // 当前批失败：本批候选未提交
    };
    static const char* write_status_name(WriteStatus status);

    struct Result {
        std::string symbol_name;
        std::string target_library;   // 推荐库（兼容展示，不参与类型判定）
        std::string type_id;          // 类型身份
        std::string type_name;        // 展示名
        int confidence = 0;
        // ---- U04-B1：来源与决策数据（旧字段全部保留） ----
        std::string symbol_id;
        /// 匹配状态名：matched/unmatched/conflict/input_too_long/
        /// evaluation_limit/evaluation_error；已知身份受保护时是 "protected"
        std::string status;
        /// 人读原因（冲突类型、超长字段、触发超限的规则 id）
        std::string reason;
        /// 命中依据摘要（字段:条目 id -> 类型）；不含超长原文
        std::vector<std::string> evidence;
        // ---- U04-B1 返工：同一次仓储读快照产生的事实（B2 直接消费） ----
        /// 仓储读到的原值；SQL NULL 与空串在现有仓储里都读成空串（不区分）
        std::string raw_type_id;
        /// 四个真实来源字段（缺失与显式空串是两回事，空白不 trim，model 是 MPN）
        classifier::MatchSample sample;
        /// 只有 eligible 且实际 Matched 时非空；protected 行不伪造候选
        std::string candidate_type_id;
        /// 核心 MatchEvidence 原样复制（source/id/field/text/priority/type_id）
        std::vector<classifier::MatchEvidence> structured_evidence;
        bool is_protected = false;    // 原值是已知类型身份，拒绝改写
        bool would_update = false;    // eligible + Matched + 候选非空且不同于原值
        WriteStatus write_status = WriteStatus::ReadOnly;
        std::string write_reason;     // 写状态的人读原因
    };

    struct Summary {
        int total = 0;          // 读取的全部行
        int matched = 0;        // eligible 的配置候选数（decision == Matched）
        int type_updated = 0;   // 只计已提交的类型更新
        std::vector<Result> results;
        // ---- U04-B1 新增计数：决策状态按行分区，protected 是独立的保护轴 ----
        std::uint64_t revision = 0;   // 本次使用的求值快照 revision
        int protected_count = 0;      // 已有已知类型身份、拒绝改写的行
        int unmatched = 0;
        int conflicts = 0;
        int input_too_long = 0;
        int evaluation_limited = 0;
        int evaluation_errors = 0;
        int source_changed = 0;   // 已提交批中四个来源字段被并发改过 → 跳过
        int cas_skipped = 0;      // 已提交批中原类型已被并发改写 / 值未变 → 跳过
        // ---- U04-B1 返工：写计划与真实写过程的计数 ----
        int would_update = 0;     // 计划事实：eligible + Matched + 候选 != 原值
        int unchanged = 0;        // 计划事实：Matched 但候选等于原值，不需要写
        int deleted = 0;          // 只计已提交批中行已不存在的候选
        int rolled_back = 0;      // 当前失败批里已 begin 的候选数（本批未提交）
        int not_attempted = 0;    // 没成功开启的当前批 + 后续未开始的候选
    };

    /// apply 的结构化部分提交报告：error 非空时 summary 是此前已提交的部分。
    struct ApplyReport {
        Summary summary;
        std::optional<util::Error> error;
    };

    /// 只读预览：先做业务健康领取（坏/未 load 的配置一律拒绝），lease 持到整次读与
    /// 计算结束；draft 只替换求值快照，不替代健康判定。detail_limit 只限制 results
    /// 明细条数，计数始终覆盖全部行。所有明细的 write_status 都是 ReadOnly。
    util::Result<Summary> preview_all(std::shared_ptr<const classifier::MatchingSnapshot> draft = {},
                                      std::size_t detail_limit = 100);

    /// 执行写回：只使用业务健康领取到的已保存快照。expected_revision 给了就必须与
    /// 该 lease 的 revision 一致，否则在任何写入之前失败。每 500 条一个短事务，
    /// COMMIT 成功才发布该批的计数与明细状态；当前批失败时以 ApplyReport::error
    /// 返回错误，summary 保留此前已提交的计数。
    util::Result<ApplyReport> apply_all(std::optional<std::uint64_t> expected_revision = {});

    /// Classify all symbols in the database using the saved matching config.
    /// 写回只更新 component_type 一列（带原值比较），已有类型原样保留。
    /// 任一批次失败即返回错误并回滚该批；已提交的批次不会回退，
    /// 数量写在错误信息里，Summary::type_updated 只计已提交的变更。
    util::Result<Summary> classify_all();

    /// Classify a single symbol by ID. 与批量分类同一健康门与同一条判定内核，
    /// 只读：不写类型、不暗改 DB，所有明细 write_status 都是 ReadOnly。
    util::Result<std::vector<Result>> classify_symbol(const std::string& symbol_id);

private:
    storage::Database* db_;
    classifier::TypeMatchingStore* matching_store_;
};

}  // namespace kforge::services
