#pragma once

// 宿主编辑服务 S2：对象粘贴（ED-031..036）。
//
// 设计要点：
// - 剪贴板内容只提供 name/sexpr/dependency；目标写权限永远由宿主按目标库重新
//   校验（authorize_source_write），不信任客户端携带的路径/UUID/库 id。
// - 预检（preview_paste）只读；提交（paste_symbols）把文件追加与 DB 行插入放进
//   同一次 SourceMutationService::apply —— 文件失败回滚 DB，DB 失败回滚文件。
// - operation_id 幂等是进程内有界缓存（≤64 条，按插入顺序淘汰），进程重启后
//   不保证；同 id 不同请求摘要返回 operation_conflict，绝不替换内容执行。

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/db/database.h"
#include "core/model/types.h"
#include "util/error.h"

namespace kforge::services
{

// 容量常量（与 webui/src/editing/limits.js 对齐；改这里必须同步前端与文档）
inline constexpr std::size_t kMaxPasteItems = 200;
inline constexpr std::size_t kMaxPasteBytes = 4u * 1024u * 1024u;  // 整请求序列化上限
inline constexpr std::size_t kMaxPasteOperationIdLength = 64;
inline constexpr std::size_t kMaxPasteNameLength = 255;
inline constexpr std::size_t kMaxPasteIdempotencyEntries = 64;

/// 客户端只允许提供这三项；其余字段（源路径、uuid、source_digest 等）不进入服务。
struct PasteItem
{
    std::string name;
    std::string sexpr;
    bool dependency{false};
};

struct PasteRequest
{
    std::string target_library_id;
    std::string operation_id;
    std::string target_digest;  // 客户端读到的目标文件摘要；新建文件时为空
    bool dry_run{false};
    std::map<std::string, std::string, std::less<>> renames;  // 原名 → 新名
    std::vector<PasteItem> items;
};

struct PasteTarget
{
    std::string library_id;
    std::string name;
    std::string file_path;
    std::string digest;  // 当前文件摘要；文件不存在时为空
    bool exists{true};
};

struct PasteCreate
{
    std::string source_name;
    std::string new_name;
};

struct PastePlan
{
    PasteTarget target;
    std::vector<PasteCreate> creates;
    std::vector<std::string> conflicts;  // 仍未解决的冲突（按源名）
    std::vector<std::string> warnings;
};

struct PasteCreated
{
    std::string uuid;
    std::string name;
};

struct PasteOutcome
{
    std::vector<PasteCreated> created;
    bool replayed{false};  // 同 operation_id 同摘要的重放
};

/// 只读预检：目标校验、冲突、依赖闭包、容量与封装引用警告；不写文件/DB。
util::Result<PastePlan> preview_paste(storage::Database& db, const PasteRequest& request);

/// 提交一次粘贴。成功返回新建对象（DB UUID + 最终名称）；失败时文件与 DB 均回滚。
util::Result<PasteOutcome> paste_symbols(storage::Database& db, const PasteRequest& request);

/// 错误码（用于 HTTP 映射）。服务内部错误消息以 "code: message" 形式携带稳定码。
std::string paste_error_code(const util::Error& error);
int paste_error_status(std::string_view code);

namespace testing
{
/// 仅供测试：清空 operation_id 幂等缓存；不触碰历史、cut token 或文件。
void reset_paste_state();
}  // namespace testing

}  // namespace kforge::services
