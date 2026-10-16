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
    /// 归属的组件库 id（用于锁状态复核），可空
    std::string owner;
};

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
    ///   3. 平台替换原文件
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
