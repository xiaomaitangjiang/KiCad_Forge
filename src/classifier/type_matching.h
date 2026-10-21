#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "classifier/matching_regex.h"
#include "core/model/symbol.h"
#include "util/error.h"

namespace kforge::classifier {

// ============================================================
// 元件类型匹配：可编辑配置（正则规则 + 精确特例）→ 不可变编译快照 → 纯匹配决策。
//
// 边界与保证：
//   * 只依赖 core/util 的现成件：nlohmann::json（实现内）、TypeRegistry 的 ID 查询、
//     PCRE2 适配层（classifier/matching_regex.h）/ std::filesystem / 锁、
//     util::replace_file。不依赖 HTTP、DB、业务 service，也不复制 ConfigStore；
//     类型注册表不反向依赖本模块。
//   * 唯一的类型依据是「实际存在的源字段」：name / value / model / reference。
//     reference 只读 Symbol::property("reference")；字段缺失就是没有这个字段，
//     不从 name、reference_prefix 默认值或历史名字兜底推断。
//   * 解析、字段结构（含 field 枚举取值）、id 唯一性、容量、type 引用与正则编译
//     在发布前一次做完；匹配循环里不重新编译正则，也不持有仓储锁。
//   * 正则引擎是 PCRE2 10.47（8-bit 静态库），语法是 PCRE2 而不是 ECMAScript，
//     按字节求值：不开 UTF/UCP，case_sensitive=false 只做 ASCII 大小写折叠，
//     ^$ 默认是整段文本边界，换行约定 ANYCRLF。完整说法见 matching_regex.h。
//     匹配有真实的工作量上限（callout 预算 + 解释器限额），超限是显式的
//     evaluation_limit 状态，不会被当成 unmatched；但它不是墙钟超时，也不承诺
//     「任意模式都能在固定毫秒内返回」。pattern/输入字段的字节上限是容量保护，
//     用来挡住超长输入，不是耗时保证。
// ============================================================

// ---------------- 容量与取值边界（校验与 API 共用同一套常量） ----------------

/// rules / exceptions 各自的上限（两张表各自限额，不是合计 500）。
inline constexpr std::size_t kMaxMatchingEntries = 500;
/// 单条正则模式的 UTF-8 字节上限。
inline constexpr std::size_t kMaxPatternBytes = 512;
/// 特例 key 的 UTF-8 字节上限（且必须非空）。
inline constexpr std::size_t kMaxExceptionKeyBytes = 4096;
/// 参与匹配的源字段 UTF-8 字节上限：任一个超长就整体返回 InputTooLong，
/// 绝不截断后匹配。
inline constexpr std::size_t kMaxSourceFieldBytes = 4096;
/// priority 是 int32 全范围，比较直接按数值大小，不做减法排序。
inline constexpr std::int32_t kMinRulePriority = std::numeric_limits<std::int32_t>::min();
inline constexpr std::int32_t kMaxRulePriority = std::numeric_limits<std::int32_t>::max();
/// revision / expected_revision 的取值范围：0 .. JS 安全整数。
inline constexpr std::uint64_t kMaxSafeRevision = 9007199254740991ULL;
/// 当前支持的配置 schema 版本。
inline constexpr std::int64_t kMatchingSchemaVersion = 1;

// ---------------- 字段 ----------------

/// 四个可匹配来源字段。model 是型号（MPN），不是 3D 模型。
enum class MatchField
{
    Name,       // Symbol::name()
    Value,      // Symbol::default_value()
    Model,      // Symbol::mpn()
    Reference,  // Symbol::property("reference")；缺失 = 没有这个字段
};

/// 配置里用的字段名（name/value/model/reference）；不认识返回 nullopt。
std::optional<MatchField> parse_match_field(std::string_view text);
/// 稳定的字段名，与配置 JSON 里写的文本一致。
const char* match_field_name(MatchField field);

// ---------------- 匹配样本 ----------------

/// 一次匹配的输入。每个字段都是 optional：「没有这个字段」和「字段是空串」是两回事 ——
/// reference 缺失时 ^$ 与 .* 都不该命中，显式空串才确实可以命中 ^$。
struct MatchSample
{
    std::optional<std::string> name;
    std::optional<std::string> value;
    std::optional<std::string> model;
    std::optional<std::string> reference;

    /// 取符号的四个真实来源字段：name / default_value / mpn / 实际 reference 属性。
    /// 不读 reference_prefix，也不按名字补 Reference。
    static MatchSample from_symbol(const core::Symbol& symbol);

    /// 指针为空 = 没有这个字段；非空指针指向的字符串可能是空串（字段存在但为空）。
    const std::string* field(MatchField which) const;
};

// ---------------- 配置 DTO ----------------

/// 一条正则规则。id 在整份配置里唯一且稳定（匹配依据按它回报）。
struct MatchingRule
{
    std::string id;
    bool enabled = true;
    MatchField field = MatchField::Reference;
    std::string pattern;  // 原样交给 PCRE2：不自动加锚、不做 glob 转换、不改写语法
    bool case_sensitive = false;
    std::int32_t priority = 0;
    std::string type_id;
};

/// 一条精确特例：field + key 原文相等、区分大小写、不 trim、不模糊搜索。
struct MatchingException
{
    std::string id;
    bool enabled = true;
    MatchField field = MatchField::Reference;
    std::string key;  // 原文；必须非空
    std::string type_id;
};

/// 一整份配置。schema_version 固定 1；revision 是 CAS 用的非负安全整数。
struct MatchingConfig
{
    std::int64_t schema_version = kMatchingSchemaVersion;
    std::uint64_t revision = 0;
    std::vector<MatchingRule> rules;
    std::vector<MatchingException> exceptions;
};

// ---------------- 决策 ----------------

enum class MatchStatus
{
    Matched,          // 命中且结论唯一，type_id 非空
    Unmatched,        // 没有命中；调用方按未分类/Unknown 处理，不得据此改写已有类型
    Conflict,         // 命中多个不同类型；type_id 留空，不替调用方选一个
    InputTooLong,     // 某个源字段超过 kMaxSourceFieldBytes，没有做任何匹配
    EvaluationLimit,  // 匹配工作量超限（callout 预算或引擎限额）：整次结论作废
    EvaluationError,  // 引擎报错（不是不匹配）：整次结论作废
};

/// 稳定的状态名（matched/unmatched/conflict/input_too_long/evaluation_limit/evaluation_error），
/// 给日志与 API 用。
const char* match_status_name(MatchStatus status);

enum class MatchEvidenceSource
{
    Rule,
    Exception,
};

/// 一条匹配依据：命中的条目 id + 字段 + 字段原文（规则另带 priority）。
struct MatchEvidence
{
    MatchEvidenceSource source = MatchEvidenceSource::Rule;
    std::string id;
    MatchField field = MatchField::Reference;
    std::string text;     // 命中的源字段原文（不截断）
    std::int32_t priority = 0;  // 只有规则有意义
    std::string type_id;        // 该条目建议的类型
};

struct Decision
{
    MatchStatus status = MatchStatus::Unmatched;
    /// 只有 Matched 时非空；其余状态一律留空。
    std::string type_id;
    /// Matched 时是全部命中依据；Conflict 时是最高优先级（或全部特例）的命中项。
    /// 超限/引擎报错时为空：整次结论作废，不能再用已经收集到的最高优先级命中。
    std::vector<MatchEvidence> evidence;
    /// 人读细节（冲突类型、超长字段、触发超限的规则 id），不参与判断。
    std::string reason;

    bool matched() const { return status == MatchStatus::Matched; }
};

// ---------------- 编译后的不可变快照 ----------------

namespace detail
{
/// 规则 DTO + 已编译模式。只由 compile_matching_config 组装。
struct CompiledRule
{
    MatchingRule rule;
    RegexProgram program;
};
}  // namespace detail

class MatchingSnapshot
{
public:
    /// 只应由 compile_matching_config 调用（入参是已完成全部校验的候选）。
    /// 快照发布后不再修改：所有访问器都是 const，数据私有。
    MatchingSnapshot(MatchingConfig config, std::vector<detail::CompiledRule> compiled);

    std::uint64_t revision() const { return config_.revision; }
    const MatchingConfig& config() const { return config_; }

    /// 匹配一次。顺序固定：先精确特例（区分大小写，任一命中就不再评估正则），
    /// 再看 enabled 正则（regex_search，取最高数值 priority）。
    Decision evaluate(const MatchSample& sample) const;

    /// 配置里引用到的全部 type_id（含 disabled 条目），升序去重 —— 类型删除前查询用。
    const std::vector<std::string>& referenced_type_ids() const { return referenced_type_ids_; }

private:
    MatchingConfig config_;
    std::vector<detail::CompiledRule> compiled_;
    std::vector<std::string> referenced_type_ids_;
};

// ---------------- 严格解析与编译 ----------------

/// 类型存在性判断。默认问当前已加载的 TypeRegistry 元件表 —— 不能只查 builtin 静态
/// ID 集合：用户的历史配置可能省略某些内置行。可注入，便于装配与测试。
using TypeExistenceFn = std::function<bool(const std::string& type_id)>;

/// 默认判断：TypeRegistry::instance().find_component(id) 有登记。
bool registry_has_component_type(const std::string& type_id);

/// 严格解析配置结构：schema 版本、revision 范围、必需字段与类型、id 唯一性（跨两张表）、
/// 特例 field+key 唯一、容量与字节上限。不做正则编译与 type 引用校验。
/// 多出来的未知键会被忽略，但任何必需字段缺失/错类型都会拒绝整份配置 —— 不丢坏条目
/// 后接受一份残缺数组。
util::Result<MatchingConfig> parse_matching_config_json(std::string_view json);

/// 规范 JSON（字段顺序固定、2 空格缩进），保存与 API 展示共用。
std::string matching_config_to_json(const MatchingConfig& config);

/// 缺文件时使用的默认种子（与分发 config/component_type_matching.json 同一份内容）。
std::string default_matching_config_json();

/// 严格解析 + 编译：结构校验之外再查 type 引用（含 disabled 条目）与正则编译
/// （含 disabled 条目，打开后不能变坏）。全部通过才返回快照。
util::Result<std::shared_ptr<const MatchingSnapshot>> compile_matching_config(
    std::string_view json, const TypeExistenceFn& type_exists);
util::Result<std::shared_ptr<const MatchingSnapshot>> compile_matching_config(
    std::string_view json);
util::Result<std::shared_ptr<const MatchingSnapshot>> compile_matching_config(
    const MatchingConfig& config, const TypeExistenceFn& type_exists);

// ---------------- 文件读取适配 ----------------

/// 生产文件读取的结果：只有「路径确实不存在」才是 Missing；
/// 目录、权限、读取中断都是 Failed —— 缺文件初始化和读失败是两回事。
enum class ConfigFileReadStatus
{
    Ok,       // 读到完整内容（bytes 是文件的全部字节，空文件也是 Ok）
    Missing,  // 路径不存在：允许走缺文件初始化
    Failed,   // 目录 / 权限 / 读取中断等：不能当成缺文件，也不能当成空数组
};

struct ConfigFileReadResult
{
    ConfigFileReadStatus status = ConfigFileReadStatus::Failed;
    std::string bytes;   // 只有 Ok 时有意义
    std::string detail;  // 只有 Failed 时有意义
};

/// 显式 path → 结果 的读取器。测试可以注入「先调用生产读取器、再人为暂停」的适配器，
/// 但不能另写一套读取语义，也不能硬编码内容。
using ConfigFileReadFn = std::function<ConfigFileReadResult(const std::string& path)>;

/// 仓储默认使用的读取器：直接检查路径、读取文件并区分 Missing/Failed/Ok。
ConfigFileReadResult read_matching_config_file(const std::string& path);

// ---------------- 配置仓储 ----------------

/// 显式路径的加载/保存 + 不可变快照发布 + 快照引用保护。
///
/// 锁与生命周期：
///   * 发布（load/save）与快照领取（current）共用同一把配置 mutex；快照拷贝/登记与
///     读取同锁原子发生，不存在「guard 之后另开窗口登记」。
///   * 固定锁序：匹配配置 mutex → TypeRegistry mutex。类型注册表内部不会反向调用本模块。
///   * 匹配在快照上做，不持有本仓储的锁；save/load 持锁期间不调用任何业务写库动作。
///   * 锁外文件读取配一个内部发布代次（受同一把 mutex 保护）：读取前捕获、回锁后核验，
///     「先完成发布的一方」让在途的旧读取过期。代次不持久化、不透出 API。
///   * 快照按值拥有 DTO 与已编译正则；外部拿到的只有 shared_ptr<const ...>，
///     析构不会回调本仓储（登记表里是 weak_ptr，过期即自然解除）。
class TypeMatchingStore
{
public:
    /// 类型引用保护 guard：持配置锁期间可查「当前 + 仍被调用方持有的旧快照」引用的
    /// 全部 type_id（含 disabled）。调用方的删除动作必须在同一 guard 生命期内完成 ——
    /// 先查完就放锁、再去删类型，等于没保护。
    class ReferenceGuard
    {
    public:
        ReferenceGuard(ReferenceGuard&&) noexcept = default;
        ReferenceGuard& operator=(ReferenceGuard&&) = delete;
        ReferenceGuard(const ReferenceGuard&) = delete;
        ReferenceGuard& operator=(const ReferenceGuard&) = delete;

        /// 升序去重的 type_id（当前快照 + 仍被持有的旧快照）。
        const std::vector<std::string>& type_ids() const { return type_ids_; }
        bool references(std::string_view type_id) const;

    private:
        friend class TypeMatchingStore;
        ReferenceGuard(std::unique_lock<std::mutex> lock, std::vector<std::string> type_ids);
        // 声明顺序即析构顺序：先丢掉 id 列表，最后才解锁
        std::unique_lock<std::mutex> lock_;
        std::vector<std::string> type_ids_;
    };

    TypeMatchingStore();
    explicit TypeMatchingStore(TypeExistenceFn type_exists);
    /// 注入文件读取器：空函数（默认构造的 std::function）一律退回生产默认读取器
    /// read_matching_config_file，与默认构造一致，不会出现 bad_function_call。
    TypeMatchingStore(TypeExistenceFn type_exists, ConfigFileReadFn read_file);

    /// 进程级单例：只提供对象，不自动读取任何路径（正式装配由后续阶段做）。
    static TypeMatchingStore& instance();

    /// 显式路径加载。
    ///   * 文件缺失：安装默认种子并把该路径绑为保存目标（首次运行语义），返回成功。
    ///     种子引用了当前 TypeRegistry 里不存在的类型时不算成功：不静默过滤种子规则，
    ///     也不绑定保存目标，只记录 load_error。
    ///   * 内容坏 / 读取失败：当前有效快照与保存绑定一律不动，记录 load_error 并返回错误。
    ///     坏路径不会被绑成「下次自动覆盖」的目标。
    ///   * 读取在配置 mutex 之外完成（不把 I/O 关进锁里）：进锁捕获当前发布代次 → 锁外
    ///     真实读取（读取器抛出的 std::exception 算这次读取失败）→ 回锁先核代次。
    ///     代次已变（读取期间有 load/save 完成了新的状态发布）就返回带 "stale" 前缀的
    ///     ServiceError：不编译、不记录旧错误、不重绑路径、不发布快照，
    ///     current/path/load_error 一个都不动。重新 load 可以恢复。
    ///   * 代次到 uint64 上限时本次 load 明确失败并保持当前状态，不回绕。
    util::Result<void> load(const std::string& path);

    /// CAS 保存：候选过了全部校验（含 type 引用与正则编译）、candidate.revision 与
    /// expected_revision 一致、且写盘成功之后，才发布 revision+1 的新快照。
    /// 任何失败（含写盘失败、target 冲突）磁盘原文与内存快照都不动，也不推进发布代次。
    /// 成功发布同时推进发布代次：在途的旧读取（见 load 的代次核验）就此过期。
    /// 代次到 uint64 上限时在 replace_file 之前拒绝，磁盘不会先被改成无法推进代次的内容。
    /// load_error 未清除时拒绝保存：坏状态不能被一次普通保存悄悄覆盖掉。
    util::Result<std::uint64_t> save(std::string_view json, std::uint64_t expected_revision);
    util::Result<std::uint64_t> save(const MatchingConfig& config, std::uint64_t expected_revision);

    /// 当前快照；没有有效配置时返回 nullptr。领取与租约登记同锁完成。
    /// 这是「最后有效内容」的只读展示口：坏配置期间它仍返回旧快照，供界面展示；
    /// 业务健康判定不能靠它 —— 那要用下面的 acquire_business_snapshot()。
    std::shared_ptr<const MatchingSnapshot> current() const;

    /// 业务操作领取快照：健康判定与租约登记在同一把配置锁内原子完成。
    ///   * load_error 非空（未知/坏配置）或还没有有效配置时明确失败：业务不能拿
    ///     「最后有效内容」继续匹配，也不能把坏配置当成「0 引用」放行。
    ///   * 成功才拷贝 current 并登记 lease；调用方在整段业务写入（含 commit/rollback）
    ///     期间持有这一份 lease，期间它的 type_id 引用继续受保护。
    ///   * 不自动 load/seed 任何路径，也不取 DB 锁；has_snapshot()/current()/load_error()
    ///     的多次调用拼不出这里的原子语义。
    util::Result<std::shared_ptr<const MatchingSnapshot>> acquire_business_snapshot() const;

    bool has_snapshot() const;
    /// 最近一次加载/保存失败的原因（空 = 正常）。只读接口可以展示「最后有效内容 + 这个错误」。
    std::string load_error() const;
    /// 当前绑定的保存目标（未绑定 = 空串）。
    std::string path() const;

    /// 取引用保护 guard。load_error 状态或还没有有效配置时返回错误 ——
    /// 未知/坏配置不能当成「0 引用」放行删除。
    util::Result<ReferenceGuard> acquire_reference_guard() const;

private:
    void register_lease_locked(const std::shared_ptr<const MatchingSnapshot>& snapshot) const;
    std::vector<std::string> referenced_type_ids_locked() const;

    mutable std::mutex mutex_;
    /// 状态发布代次：成功 save、非 stale 的 load（成功或失败结果）都在同一临界区内 +1；
    /// 失败 save 与只读查询不推进。只增不减、不持久化、不透出 API，uint64 上限处直接
    /// 拒绝新的发布而不回绕。load 在锁外读取之前捕获它，回锁后据此判断这次读取是否
    /// 已经被更晚完成的状态发布超越。
    std::uint64_t generation_ = 0;
    /// 领取过的快照都登记 weak_ptr：旧快照被调用方持有期间，它的 type_id 引用仍算数。
    /// 同一份快照只登记一条（按对象身份去重），过期的登记在下次登记时清掉 ——
    /// 登记数量随存活快照数增长，不随 current() 调用次数增长。
    /// 快照析构只让 weak 自然失效，不会回过来取本仓储的锁。
    mutable std::vector<std::weak_ptr<const MatchingSnapshot>> leases_;
    std::shared_ptr<const MatchingSnapshot> current_;
    std::string path_;
    std::string load_error_;
    TypeExistenceFn type_exists_;
    ConfigFileReadFn read_file_;
};

}  // namespace kforge::classifier
