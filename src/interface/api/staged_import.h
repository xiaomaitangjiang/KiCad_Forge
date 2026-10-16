// C02：宿主暂存提交的插件导入（staged-import-v1）。
// 插件只在宿主给的私有暂存目录里产出文件；目标库怎么改、封装放哪、数据库写什么
// 全由宿主决定并校验 —— 插件既不读也不覆盖目标库。
#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "util/error.h"

namespace kforge::plugin { class PluginManager; }
namespace kforge::storage { class Database; }

namespace kforge::api
{

class StagedImporter
{
public:
    StagedImporter(storage::Database& db, plugin::PluginManager& plugins)
        : db_(&db), plugins_(&plugins)
    {
    }

    struct Request
    {
        std::string plugin_id;
        std::string action{"import"};
        std::string provider{"LCSC"};
        std::string external_id;        // LCSC 编号
        std::string target_library_id;  // 首选：元件库 UUID
        std::string target_library_path;  // 旧协议回退：唯一解析到已登记库
        std::string footprint_mode{"new"};  // new | reuse
        std::string footprint_id;           // reuse 时必须是已登记封装 UUID
        std::string operation_id;           // 幂等键（可空 = 不启用）
    };

    /// 执行一次暂存导入。ok:true 只在宿主提交完成后返回。
    util::Result<nlohmann::json> run(const Request& request);

private:
    storage::Database* db_;
    plugin::PluginManager* plugins_;
};

}  // namespace kforge::api
