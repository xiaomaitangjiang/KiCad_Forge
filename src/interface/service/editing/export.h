// 宿主编辑服务 —— 对象复制的只读导出（ED-030）。
//
// 设计要点：
//   * 只读：不写 DB、不写文件、不取任何写锁，因此锁定库/只读库/官方库里的对象
//     也可以导出；
//   * 保真：导出的是源文件里该符号**完整的顶层节点原文**（含图形、多单元子符号、
//     引脚、属性位置、隐藏字段、未知节点），不是由 DB 列摘要重建的；
//   * 派生符号：把 extends 继承链上的父节点一并带出，标 dependency=true，
//     让客户端拿到「最小必要继承闭包」，而不是一个无法独立恢复的残对象；
//   * 不做任何授权判断：导出不改变任何状态，授权在粘贴阶段（S2）做。
#pragma once

#include <string>
#include <vector>

#include "core/model/types.h"
#include "util/error.h"

namespace kforge::storage
{
class Database;
}  // namespace kforge::storage

namespace kforge::services
{

/// 一个可恢复的导出对象。sexpr 是源文件顶层 (symbol "Name" ...) 的完整原文，
/// 客户端剪贴板与粘贴路径都以它为准 —— 不要在这里做重排/重建。
struct ExportedSymbol
{
    std::string uuid;         // DB 身份；dependency 项没有直接选中，可能为空
    std::string kf_id;        // KF 内容指纹（Kicad_Forge_ID）；dependency 项可能为空
    std::string name;         // (symbol "Name" ...) 里的名字
    std::string library_id;   // libraries.id
    std::string library_name; // libraries.name
    std::string source_path;  // 源 .kicad_sym 文件（UTF-8）
    std::string source_digest;// 整文件 xxh64 十六进制（小写/大写由 content_digest 决定）
    std::string sexpr;        // 完整顶层节点原文（含括号）
    /// true = 派生符号继承闭包里带出的父节点（隐含依赖，不是用户直接选中）
    bool dependency{false};
};

/// 导出信封。版本化，便于前端/插件在格式演进时明确拒绝旧版本。
struct ExportResult
{
    std::string kind{"kforge.symbols"};
    int version{1};
    std::vector<ExportedSymbol> items;
};

/// 对象复制的唯一导出入口。
///
/// * 每个 id 都必须存在（NotFound 原样上报，不静默跳过）；
/// * 重复 id 只导出一次；
/// * ids 为空或超过 200 个 → ValidationError；
/// * 库未登记、库没有源文件路径、源文件打不开 → 明确错误；
/// * 源文件里找不到同名顶层节点 → 错误（DB 与文件不一致，绝不用摘要重建）。
util::Result<ExportResult> export_symbols(storage::Database& db,
                                          const std::vector<core::Uuid>& ids);

}  // namespace kforge::services
