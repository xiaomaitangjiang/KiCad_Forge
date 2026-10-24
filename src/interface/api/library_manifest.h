// 组件库清单：按仓库根目录/目标元件库导出符号、封装与 3D 模型的可比较快照。
// 供 Git 语义 diff、导出等只读场景使用；不包含任何 Git 操作。
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace kforge::storage { class Database; }

namespace kforge::api {

/// 收集 root（仓库根）与 target（用户选中的元件库/符号库路径）下的符号库。
/// 只读取数据并复制出 session；封装焊盘在释放 session 后逐个解析。
/// 返回 {"ok":true,...}；路径无效或读取失败返回 {"ok":false,"error":...}。
nlohmann::json build_library_manifest(storage::Database& db,
                                  const std::string& root,
                                  const std::string& target);

}  // namespace kforge::api
