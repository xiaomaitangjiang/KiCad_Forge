// 封装/3D 模型「存在性」的唯一判定入口。
//
// 判定顺序固定为：DB 身份 → 物理文件 → 状态，缺一不可：
//   * 引用非空不等于封装存在（用户示例：本地无库，引用照样写在符号里）
//   * 关联 UUID 非空不等于文件还在（来源被删/被挪走）
//   * source_key（来源身份）是身份键，不是存在证明
// 状态与原因码是稳定契约：前端按字符串映射文案，不解析英文句子。
//
// 依赖方向：本文件只做「读快照 + 查文件」，不引用 nlohmann/httplib/React，
// 也不持有 DbSession 或 sqlite3*（快照按值拥有数据），便于测试与复用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/model/footprint.h"
#include "core/model/model_3d.h"
#include "core/model/symbol.h"
#include "core/repo/repositories.h"
#include "util/error.h"

namespace kforge::storage
{
class DbSession;
}

namespace kforge::services
{

/// 封装状态。unassigned/not_found/ambiguous 是「没有唯一定位到 DB 行」，
/// stale/check_failed 是「定位到了，但物理文件不可用/查不动」。
enum class FootprintStatus: std::uint8_t
{
    Unassigned,   // 符号没写 Footprint 引用
    NotFound,     // 有引用，但库里没有匹配的封装行
    Ambiguous,    // 有引用，但匹配到多条（同名跨来源），不猜
    Stale,        // 定位到了 DB 行，但文件缺失/被目录替代/来源身份变了
    Available,    // DB 行 + 真实普通文件 + 来源身份一致
    CheckFailed,  // 文件系统查询本身失败（权限等），不能当成「没有」
};

/// 3D 模型状态（比封装少 unassigned/not_found/ambiguous 之分）
enum class ModelStatus: std::uint8_t
{
    Unassigned,   // 没有关联模型 / 封装本身没定位到
    Stale,        // 有 DB 关联，但关联的文件一个都不在（或封装已失效）
    Available,    // 至少一个 DB 关联模型是真实普通文件
    CheckFailed,
};

const char* to_string(FootprintStatus status);
const char* to_string(ModelStatus status);

/// 稳定原因码。前端/测试/日志都按这些常量判断，不做字符串前缀匹配。
namespace fp_reason
{
inline constexpr const char* kNoReference = "no_reference";
inline constexpr const char* kNoMatch = "no_match";
inline constexpr const char* kMultipleMatches = "multiple_matches";
inline constexpr const char* kEmptyPath = "empty_path";
inline constexpr const char* kMissingFile = "missing_file";
inline constexpr const char* kNotRegularFile = "not_regular_file";
inline constexpr const char* kSourceKeyMismatch = "source_key_mismatch";
inline constexpr const char* kStatFailed = "stat_failed";
inline constexpr const char* kOk = "ok";
inline constexpr const char* kNoModelLink = "no_model_link";
inline constexpr const char* kModelFilesMissing = "model_files_missing";
inline constexpr const char* kFootprintUnavailable = "footprint_unavailable";
inline constexpr const char* kModelCheckFailed = "model_check_failed";
}  // namespace fp_reason

/// 一次文件检查的结果：一次 status() 的全部事实，判定只允许从这里读 ——
/// 不允许「先看 regular_file、再去 status 一次分目录/缺失」，那会漏计访问次数，
/// 也会让同一次判定看到两个不同的磁盘状态。
struct FileCheckResult
{
    bool exists{false};        // status() 查得到，路径确实存在
    bool regular_file{false};  // exists && 是普通文件
    bool directory{false};     // exists && 是目录（目录替代 → not_regular_file）
    bool failed{false};        // status() 失败且不是「这个路径上没有文件」（权限/IO）
    std::string canonical;     // 规范化路径（source_key 规则）；无法规范化时为空
};

/// 一次响应内共享的文件检查上下文：
///   * 输入路径 → 规范化键的映射与规范化键 → 结果两级缓存：同一份文件
///     （包括同一路径的不同写法）只查一次盘，也避免重复 weakly_canonical
///   * 全部走非抛异常的 std::filesystem 接口
///   * fresh=true 绕过去重（详情、变更前复核必须看当前磁盘状态）
/// 缓存不是正确性的前置条件：即使每次都 stat，判定结果也一致。
class FileCheckContext
{
public:
    FileCheckResult check(const std::filesystem::path& path, bool fresh = false);

    /// Context 自己发起过的 status() 次数（去重效果的可测证据）。
    /// 只描述这里的显式查询，不代表整个进程的文件系统访问次数。
    std::size_t stat_calls() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> canonical_by_input_;  // 输入路径 → 规范化键
    std::unordered_map<std::string, FileCheckResult> cache_;           // 规范化键 → 结果
    mutable std::size_t stat_calls_{0};
};

/// UTF-8 文本 ⇄ filesystem 路径：清单与数据库里的路径文本是 UTF-8 契约，
/// 窄串会按 ANSI 代码页重解释（中文路径会坏），必须经 char8_t 转换。
std::filesystem::path path_from_utf8(const std::string& utf8);
std::string path_to_utf8(const std::filesystem::path& path);

/// 选中来源的轻量身份：UUID + 名称 + 库名 + 路径/来源键。
/// 「先读、加锁后再复核」的两阶段提交用它确认还是同一行、同一份来源 ——
/// 同 UUID 被改名/换源/挪到另一份真实文件时，文件和关联都不是当初选中的那个。
/// KiCad 库名与封装名精确比较；路径按 source_key 规范化语义（Windows 折叠大小写）。
struct FootprintIdentity
{
    std::string id;
    std::string name;
    std::string footprint_lib_name;
    std::string source_key;
    std::string library_path;  // library_path 的规范化路径键（source_key 规则）
};

FootprintIdentity capture_footprint_identity(const core::Footprint& footprint);
bool same_footprint_identity(const FootprintIdentity& left, const FootprintIdentity& right);

/// 一次读取的完整目录快照：拥有全部数据，不持有 session / sqlite3*。
/// 调用方（读接口）必须在同一个 DbSession 里批量取完它，释放 session 之后
/// 再拿它做磁盘检查 —— 不要在每符号循环里查库。
struct Snapshot
{
    std::vector<core::Footprint> footprints;
    std::vector<core::Model3D> models;
    // 逐行保留：同一符号可能挂多条关联，单值 map 会覆盖
    std::vector<storage::RelationshipRepository::SymbolLinkRow> symbol_links;
    std::unordered_map<std::string, std::vector<std::string>> model_links;  // fp → model ids

    // ---- 索引 ----
    // 库名与封装名是精确键（KiCad 引用身份大小写敏感）：Pkg:foo 与 Pkg:Foo 是
    // 两个身份，索引里不能折叠；只有 Windows 物理路径按 source_key 折叠大小写。
    std::unordered_map<std::string, std::size_t> footprint_index;            // uuid → row
    std::unordered_map<std::string, std::vector<std::size_t>> by_name;       // 精确名字 → rows
    std::unordered_map<std::string, std::vector<std::size_t>> by_lib_name;   // lib\x1fname（精确）→ rows
    std::unordered_map<std::string, std::size_t> model_index;                // uuid → row
    std::unordered_map<std::string, std::vector<std::size_t>> links_by_symbol;

    const core::Footprint* footprint(const std::string& id) const;
    const core::Model3D* model(const std::string& id) const;
    /// .pretty 库名：优先 footprint_lib_name 列，旧行为空时回退父目录名
    std::string library_of(const core::Footprint& fp) const;
};

/// 同一 DbSession 内批量加载；任何一张表读失败都是错误（不返回「成功空集」）
util::Result<Snapshot> load_snapshot(const storage::DbSession& session);

/// 只填 resolver 需要的身份字段（封装 id/名称/库路径/来源键，模型 id/路径），
/// 跳过符号/封装的描述与属性 JSON。列表页必须用它，否则一次全量 JSON
/// 反序列化会把 DbSession 占住几十秒，导入只能排队等锁。
util::Result<Snapshot> load_snapshot_light(const storage::DbSession& session);

/// 解析结果：身份 + 状态 + 原因 + 模型展示字段
struct Resolution
{
    std::string reference;            // 符号原始引用（永远保留，不因错误清空）
    FootprintStatus status{FootprintStatus::Unassigned};
    std::string reason{fp_reason::kNoReference};
    std::string footprint_id;         // 唯一定位出的真实 DB UUID（无唯一定位为空）
    std::string footprint_name;       // DB 行名字（展示用）
    std::string resolved_reference;   // 规范 "Lib:Name"（可定位时提供）

    ModelStatus model_status{ModelStatus::Unassigned};
    std::string model_reason{fp_reason::kNoModelLink};
    std::string model_id;             // 唯一选定的可用模型
    std::string model_name;           // 展示名（文件名）
};

/// 按符号当前引用解析封装与 3D（引用为空 → unassigned；不反向恢复陈旧关联）
Resolution resolve_symbol(const core::Symbol& symbol, const Snapshot& snapshot,
                          FileCheckContext& files);

/// 单独检查一条已知封装行（搜索候选/详情）：无歧义可言，
/// 只会得到 available / stale / check_failed。
/// with_model=false 时跳过模型文件检查（列表页只看封装本身）。
Resolution inspect_footprint(const core::Footprint& footprint, const Snapshot& snapshot,
                             FileCheckContext& files, bool with_model = true);

/// 变更前复核：只查这一行的物理文件，不需要全库快照。
/// 用全新的 FileCheckContext 调用即为 fresh 检查（不复用请求级缓存）。
Resolution inspect_footprint_files(const core::Footprint& footprint, FileCheckContext& files);

}  // namespace kforge::services
