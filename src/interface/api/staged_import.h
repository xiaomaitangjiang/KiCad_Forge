// C02：宿主暂存提交的插件导入（staged-import-v1）。
// 插件只在宿主给的私有暂存目录里产出文件；目标库怎么改、封装放哪、数据库写什么
// 全由宿主决定并校验 —— 插件既不读也不覆盖目标库。
#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

#include "util/error.h"

namespace kforge::plugin { class PluginManager; }
namespace kforge::storage { class Database; }
namespace kforge::classifier { class TypeMatchingStore; }

namespace kforge::api
{

class StagedImporter
{
public:
    /// 使用最终符号字段匹配类型；未注入时使用启动阶段已加载的进程级仓储。
    /// 新操作需要健康配置，已成功操作仍可直接回放原结果。
    StagedImporter(storage::Database& db, plugin::PluginManager& plugins,
                   classifier::TypeMatchingStore* matching_store = nullptr)
        : db_(&db), plugins_(&plugins), matching_store_(matching_store)
    {
    }

    struct Request
    {
        std::string plugin_id;
        std::string action{"import"};
        std::string provider{"LCSC"};
        std::string external_id;        // LCSC 编号
        std::string target_library_id;  // 旧模式：符号库（libraries 行）UUID
        std::string target_library_path;  // 旧协议回退：唯一解析到已登记库
        /// 新模式：**元件库包**（component_libraries 行）UUID —— 符号库 + 封装库
        /// 的组合。符号最终写进该包的符号路径（目录 → LCSC.kicad_sym，
        /// 或直接配置的 .kicad_sym 文件），封装写进该包的封装路径。
        /// 与 target_library_id 互斥；独立字段，不改旧字段语义。
        std::string target_component_library_id;
        /// 可选包内符号子库选择：existing 使用相对 path，new 使用单文件 stem。
        /// 留空保留旧客户端默认落点；只有元件库包模式允许使用。
        std::string symbol_library_mode;
        std::string symbol_library_path;
        std::string symbol_library_name;
        /// new | reuse。组件库模式（target_component_library_id）下空值 = 没做选择，
        /// 直接拒绝；旧符号库模式保留空值按 "new" 处理的历史语义。
        std::string footprint_mode;
        std::string footprint_id;  // reuse 时必须是已登记封装 UUID（新模式下必须显式给出）
        std::string operation_id;           // 幂等键（可空 = 不启用）
    };

    /// 执行一次暂存导入。ok:true 只在宿主提交完成后返回。
    ///
    /// Hooks（阶段 4 任务服务）：
    ///   * cancelled：staging 阶段轮询取消；取消先行时不会开始提交、不会有
    ///     文件/数据库写入；
    ///   * on_phase：发布 "staging"/"committing" 子阶段（普通动作没有阶段）；
    ///   * try_enter_commit：与取消请求共用同一把任务锁的原子闸门。返回 false
    ///     = 取消赢，调用方必须立即放弃；返回 true = 之后不可取消。
    /// 默认空 hooks 保持旧的同步语义。
    struct RunHooks
    {
        std::function<bool()> cancelled;
        std::function<void(const std::string& phase)> on_phase;
        std::function<bool()> try_enter_commit;
    };
    util::Result<nlohmann::json> run(const Request& request, const RunHooks& hooks = {});

private:
    storage::Database* db_;
    plugin::PluginManager* plugins_;
    classifier::TypeMatchingStore* matching_store_ = nullptr;
};

/// HTTP 请求体 → Request 的解析结果。旧前端在 footprint_choice 里给 library/name，
/// 需要查库才能定位封装 UUID，所以这一段留给调用方（routes）在解析之后做。
struct StagedRequestFields
{
    StagedImporter::Request request;
    std::string legacy_library;  // footprint_choice.library（仅旧符号库模式使用）
    std::string legacy_name;     // footprint_choice.name（仅旧符号库模式使用）
};

/// 解析插件任务请求体（routes 与 C++ 测试共用同一实现，
/// 避免文档写嵌套 footprint_choice.id、解析只认顶层 footprint_id 的两套写法）：
///   * 显式封装 UUID 接受 footprint_choice.id、footprint_choice.footprint_id
///     与顶层 footprint_id 三处写法（向后兼容），三处都给且不一致直接拒绝；
///   * 元件库包模式（target_component_library_id）必须显式给 footprint_choice，
///     mode 只能是 new/reuse，new 不允许携带任何旧 UUID；
///   * 字符串字段的类型错误在取用之前就给具体消息，不落到笼统的 "Execute error"。
util::Result<StagedRequestFields> parse_staged_request(const nlohmann::json& body,
                                                       const std::string& plugin_id,
                                                       const std::string& action);

/// 列出包符号目录内真实存在的 .kicad_sym 普通文件（含空文件/未登记文件）。
/// 不解析符号内容、不登记新行；已登记 UUID 按 source_key 关联，拒绝跨包归属。
/// 不遍历符号链接或 Windows 目录联接；错误不会伪装成空列表。
util::Result<nlohmann::json> list_component_symbol_libraries(storage::Database& db,
                                                           const std::string& component_id);

/// 真实 LCSC 插件的编号规范化：去首尾空白、统一大写、纯数字补 C；
/// 只接受 C+数字，其余（空、路径字符、混杂编号）都是 ValidationError。
/// 只在元件库包模式、且插件是 com.kicad_forge.lcsc_import 时使用 ——
/// 通用第三方插件的外部编号不受这条规则约束。
util::Result<std::string> canonical_lcsc_external_id(const std::string& value);

}  // namespace kforge::api
