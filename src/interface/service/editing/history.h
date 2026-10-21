#pragma once

// 宿主编辑服务 S4：会话内编辑历史与撤销/重做（ED-050..056）。
//
// 设计要点：
// - 进程内单例，不新增 DB 表；重启后历史为空（GET 返回空且 can_undo/can_redo=false）。
// - 一个历史单元 = 一次成功的批量粘贴或一次成功的批量移动；dry_run、失败、noop、
//   幂等重放都不入历史。
// - 撤销/重做先由 SourceMutationService::apply 一次性恢复全部文件字节，DB 复核与
//   恢复在同一业务事务里；任一文件摘要/行状态不符则整体回滚并返回 history_conflict，
//   历史指针不前移。
// - cursor 指向下一个可重做位置：undo 作用于 cursor-1，redo 作用于 cursor。
// - 容量在提交前检查：单个单元过大或任一文件内容超过单文件预算时，提交前直接拒绝，
//   文件和 DB 都不写。总容量超出时整条淘汰最旧单元，绝不截断单元。
//
// 并发：paste/move 提交与 undo/redo 共用 editing_commit_mutex()，保证历史指针与
// 幂等缓存不会交错。precheck_history_capacity()/push_history() 由调用方在持有该
// mutex 时调用；其余公开入口自行加锁。

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/db/database.h"
#include "core/model/symbol.h"
#include "util/error.h"

namespace kforge::services
{

// 容量常量（与 webui/src/editing/limits.js 对齐；改这里必须同步前端与文档）
inline constexpr std::size_t kMaxHistoryEntries = 20;
inline constexpr std::size_t kMaxHistoryBytes = 32u * 1024u * 1024u;
inline constexpr std::size_t kMaxHistoryFileBytes = 16u * 1024u * 1024u;

/// 一个文件在历史单元里的 before/after 快照。
struct HistoryFileSnapshot
{
    std::string path;  // UTF-8 绝对路径
    std::string owner; // 组件库 id（重新授权时使用）
    bool exists_before{false};
    std::string before_content;
    bool exists_after{false};
    std::string after_content;
};

/// 移动单元的 DB 归属快照；UUID/KF 不变，只记录旧/新库与名称。
struct HistoryMoveRow
{
    std::string uuid;
    std::string old_library_id;
    std::string old_name;
    std::string old_lib_id;
    std::string new_library_id;
    std::string new_name;
    std::string new_lib_id;
    std::string kf_id;
};

struct HistoryEntry
{
    std::string id;
    std::string kind;  // "paste" | "move"
    std::size_t count{0};
    std::string target_library_name;
    std::string source_library_name;  // 移动时有值
    long long created_at_ms{0};
    std::vector<HistoryFileSnapshot> files;
    /// 粘贴单元：创建时每行的完整副本（含 UUID/KF/属性/引脚），用于重做恢复同一身份。
    std::vector<core::Symbol> created_symbols;
    /// 移动单元：每个移动对象的 DB 归属前后状态。
    std::vector<HistoryMoveRow> moved_rows;
};

struct HistoryEntrySummary
{
    std::string kind;
    std::size_t count{0};
    std::string target_library_name;
    std::string source_library_name;
    long long created_at_ms{0};
};

struct HistoryState
{
    bool can_undo{false};
    bool can_redo{false};
    std::size_t count{0};
    std::size_t bytes{0};
    /// 有值表示下一次 undo/redo 会作用到的单元。
    std::vector<HistoryEntrySummary> entries;  // 最旧 → 最新
    std::size_t undo_index{0};                 // entries 下标；无 undo 时为 entries.size()
    std::size_t redo_index{0};                 // entries 下标；无 redo 时为 entries.size()
};

struct HistoryRestored
{
    std::string uuid;
    std::string name;
};

struct HistoryOutcome
{
    std::string kind;
    std::size_t count{0};
    std::vector<HistoryRestored> restored;
    bool can_undo{false};
    bool can_redo{false};
};

/// 全进程编辑提交/历史互斥；paste、move、undo、redo 必须共用这一把锁。
std::mutex& editing_commit_mutex();

/// 仅测试：清空历史并恢复默认容量限制。
/// 生产代码不得调用；放在独立命名空间避免与业务 API 混淆。
namespace testing
{
void reset_history();
/// 仅测试：调整容量限制（条目数/总字节/单文件字节）。
void set_history_limits(std::size_t entries, std::size_t bytes, std::size_t file_bytes);
}  // namespace testing

/// 提交前容量检查；调用方必须持有 editing_commit_mutex()。
/// 单次操作自身超过总预算、或任一文件内容超过单文件预算 → history_full。
util::Result<void> precheck_history_capacity(const HistoryEntry& entry);

/// 记录一个已成功提交的历史单元；调用方必须持有 editing_commit_mutex()，且已通过
/// precheck_history_capacity()。会先清空 redo 分支，再按容量从最旧整条淘汰。
void push_history(HistoryEntry entry);

/// 任一成功编辑（删除/绑定/分类/导入等）后调用：清空 redo 分支。
/// 失败、noop、选择、复制、读取不得调用；内部加锁，可与编辑提交互斥。
void note_successful_edit();

/// 只读快照；内部加锁。
HistoryState history_state();

/// 撤销/重做；内部加锁并整体提交。冲突时文件/DB/历史指针都不改变。
util::Result<HistoryOutcome> undo_history(storage::Database& db);
util::Result<HistoryOutcome> redo_history(storage::Database& db);

/// 错误码（用于 HTTP 映射）。服务内部错误消息以 "code: message" 形式携带稳定码。
std::string history_error_code(const util::Error& error);
int history_error_status(std::string_view code);

/// 历史单元 id / 当前毫秒时间（paste/move 构造单元时使用）。
std::string new_history_entry_id();
long long history_now_ms();

}  // namespace kforge::services
