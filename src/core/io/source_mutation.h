// 源文件统一变更入口：符号删除、库合并/删除、绑定、KF ID 写回、导入、插件提交
// 都从这里走 —— 进程内按规范路径串行，落盘前记恢复日志，崩溃后可判定、可恢复。
//
// 调用方只给「稳定对象 + 期望内容 + 目标变更」，不接受任意文件路径当授权；
// 每个目标都必须是已登记的可写来源（libraries/footprints 的 source_key）。
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "util/error.h"

namespace kforge::storage
{
class Database;
class DbSession;
}  // namespace kforge::storage

namespace kforge::io
{

/// 单个目标文件的变更请求
struct FileChange
{
    std::filesystem::path path;
    /// 新内容（UTF-8 文本）。`remove` 为真时忽略
    std::string content;
    /// 删除该文件（库删除场景）
    bool remove = false;
    /// 期望的原文件摘要（xxh64 十六进制）；空 = 不校验（新文件）
    std::string expected_digest;
    /// 允许目标不存在（新建文件）
    bool allow_missing = false;
    /// 期望目标**不存在**。与 allow_missing 的区别是强弱：allow_missing 只说
    /// 「不在也算数」，本字段断言「必须不在」—— 调用方按「还没有这个文件」做的
    /// 判定，在取锁之前被别人抢先建出来时必须报冲突。空摘要 = 不校验，光靠
    /// allow_missing 会把别人刚写下的内容当成可覆盖的新文件。
    /// 发布新文件时还会用原子排他操作保证目标仍不存在；预检后的抢占同样拒绝，
    /// 此前已发布的同组文件按原事务回滚，抢占者的文件保持原样。
    bool expected_missing = false;
    /// 归属的组件库 id（用于锁状态复核），可空
    std::string owner;
};

/// 官方安装目录（<...>/share/kicad/...）——只读，永不可写
bool is_official_kicad_source(const std::filesystem::path& path);

/// 锁/保护判定：目标属于官方目录、或与任一 locked/protected 组件库的根重叠
/// （或该库正是 owner）时返回 true。
/// 查询失败返回错误 —— 数据库读不出来不等于「允许写」。
util::Result<bool> source_write_blocked(const storage::DbSession& session,
                                        const std::filesystem::path& path,
                                        const std::string& owner = "");

/// 目标路径的写入授权，核心侧唯一实现（接口层只做薄封装，不复制第二套判断）：
///   * 官方目录、重叠 locked/protected 根一律拒绝；
///   * 已登记的精确来源：owner 一致之外还要核对**真实归属** —— 归属的组件库
///     必须存在、没被锁/保护，并且对应种类的 root 真的包含这个文件；
///     只有「没有归属的旧记录 + 精确登记文件」才保持兼容；
///   * 已登记的封装归属不在行上：由「哪个组件库的对应 root 包含它」判定，
///     调用方给的 owner 必须就是其中之一（不让伪 owner 假冒归属）；
///   * 未登记的新文件必须落在明确登记的可写 root 内，且 root 的种类要和文件
///     种类对上（kicad_sym→符号 root、kicad_mod→封装 root、3D→模型 root）：
///     给了 owner 就要与该 root 对上，省略 owner 只在唯一 root 归属时兼容；
///     没有配置 root 就拒绝。
/// 注意：allow_missing 只影响「文件在不在」的判定，不进入这里 —— 它不能豁免授权。
util::Result<void> authorize_source_write(const storage::DbSession& session,
                                          const std::filesystem::path& path,
                                          const std::string& owner = "");

/// 恢复扫描的结果
struct RecoveryReport
{
    int restored = 0;    // prepared 且已还原
    int finalized = 0;   // applied 且校验通过、清理备份
    int conflicts = 0;   // 摘要对不上：隔离并报告，不覆盖外部修改
    std::vector<std::string> messages;
};

/// 文件内容摘要（xxh64，与工程内 KF ID 用同一算法，不引新依赖）
std::string content_digest(const std::string& bytes);

class SourceMutationService
{
public:
    explicit SourceMutationService(storage::Database& db) : db_(&db)
    {
    }

    /// 执行一次源文件变更：
    ///   1. 按规范路径排序取全部文件锁，锁内复核来源登记/存在性/摘要
    ///   2. 写同目录唯一临时文件 + 备份，短事务落 prepared 日志
    ///   3. 已存在文件按平台替换；新文件原子排他发布，不覆盖预检后出现的目标
    ///   4. 业务事务（调用方在给定 session 里写 DB）+ 标记 applied，同一事务提交
    /// 业务事务失败：回滚 DB，并按备份还原已被替换的文件。
    util::Result<void> apply(const std::vector<FileChange>& changes,
                             const std::function<util::Result<void>(storage::DbSession&)>& business);

    /// 启动恢复：在开放 HTTP 与导入之前调用。
    /// prepared → 还原原状；applied → 校验新摘要后清理备份；对不上则隔离并报告。
    RecoveryReport recover();

private:
    storage::Database* db_;
};

}  // namespace kforge::io
