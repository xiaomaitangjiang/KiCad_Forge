#pragma once

// 宿主编辑服务 S3：剪切意图（cut）与跨库移动（move）（ED-040..045）。
//
// 设计要点：
// - cut 只读：校验源对象/库/写权限并导出完整节点与 extends 闭包，生成一次性、
//   短期、进程内的不透明 token；不写文件、不改 DB、不删任何东西。
// - move 把「目标追加 + 源删除 + symbols.library_id/name 更新」放进同一次
//   SourceMutationService::apply：文件失败回滚 DB，DB 失败回滚文件。
// - 同文件移动一律 noop：绝不删一个再加一个。
// - token 只能消费一次；operation_id 幂等在进程内有界缓存（≤64 条），
//   同 id 同摘要重放首次结果，不同摘要 operation_conflict，重启后不保证。
// - 历史（撤销/重做）不在本模块，S4 单独授权。

#include <chrono>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/db/database.h"
#include "core/model/types.h"
#include "interface/service/editing/export.h"
#include "util/error.h"

namespace kforge::services
{

// 容量常量（与 webui/src/editing/limits.js 对齐；改这里必须同步前端与文档）
inline constexpr std::size_t kMaxCutItems = 200;
inline constexpr std::size_t kMaxCutIntents = 16;
inline constexpr std::chrono::seconds kCutTokenTtl{30 * 60};
inline constexpr std::size_t kMaxMoveOperationIdLength = 64;
inline constexpr std::size_t kMaxMoveIdempotencyEntries = 64;

struct CutResult
{
    std::string kind{"kforge.symbols.move"};
    int version{1};
    std::string token;
    std::string summary;
    std::vector<ExportedSymbol> items;  // 直接项 uuid/kf 有值；dependency 项 uuid/kf 为空
    std::vector<std::string> warnings;
};

struct MoveRequest
{
    std::string token;
    std::string target_library_id;
    std::string operation_id;
    std::string target_digest;  // 客户端读到的目标文件摘要；新建目标时为空
    bool dry_run{true};
    std::map<std::string, std::string, std::less<>> renames;  // 原名 → 新名
};

struct MoveTarget
{
    std::string library_id;
    std::string name;
    std::string file_path;
    std::string digest;  // 文件不存在时为空
};

struct MovePlanItem
{
    std::string uuid;
    std::string name;      // 源名
    std::string new_name;  // 改名后的落盘名
};

struct MoveDependency
{
    std::string name;
    bool dependency{true};
};

struct MoveConflict
{
    std::string name;
    std::string reason;  // name_conflict / missing_parent / unmoved_derivatives / already_in_target
};

struct MovePlan
{
    bool noop{false};
    MoveTarget target;
    std::vector<MovePlanItem> moves;
    std::vector<MoveDependency> dependencies;
    std::vector<MoveConflict> conflicts;
    std::vector<std::string> warnings;
};

struct MoveOutcome
{
    std::vector<MovePlanItem> moved;
    bool noop{false};
    bool replayed{false};
};

/// 建立剪切意图：只读校验 + 完整节点导出 + extends 闭包，返回一次性 token。
util::Result<CutResult> cut_symbols(storage::Database& db,
                                    const std::vector<core::Uuid>& ids);

/// 释放剪切意图；token 不存在也视为成功（Escape 清标记走这里）。
void clear_cut_token(std::string_view token);

/// 只读预检：不消费 token，不写文件/DB。
util::Result<MovePlan> preview_move(storage::Database& db, const MoveRequest& request);

/// 提交移动。noop 会消费 token 但不写；失败时文件与 DB 均回滚且 token 保留。
util::Result<MoveOutcome> move_symbols(storage::Database& db, const MoveRequest& request);

/// 错误码（用于 HTTP 映射）。服务内部错误消息以 "code: message" 形式携带稳定码。
std::string cut_error_code(const util::Error& error);
int cut_error_status(std::string_view code);
std::string move_error_code(const util::Error& error);
int move_error_status(std::string_view code);

/// 仅供测试：推进 token 时钟 / 清空进程内编辑状态。
/// 生产代码不得调用；放在独立命名空间避免与业务 API 混淆。
namespace testing
{
void advance_cut_clock(std::chrono::seconds delta);
void reset_editing_state();
/// 测试用：直接用给定 items 注册剪切意图（不从源文件补 extends 闭包），
/// 用于覆盖 move 的缺失父节点防御分支。生产代码不得调用。
std::string install_cut_intent_for_test(std::vector<ExportedSymbol> items);
}  // namespace testing

}  // namespace kforge::services
