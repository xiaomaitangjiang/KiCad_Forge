// 存在性状态 → HTTP 响应字段的薄适配器。
// 路线与测试调用同一个函数：前端拿到的字段就是测试断言的字段，
// 不允许各路线各写一份拼装逻辑。
#pragma once

#include <nlohmann/json.hpp>

#include "interface/service/library/footprint_resolver.h"

namespace kforge::api
{

/// 完整状态字段（列表 / 详情 / 绑定查询共用）。
/// has_footprint / has_3d_model 只由状态推导 —— 不看名字、不看非空 UUID、不看来源键。
inline void apply_footprint_status(nlohmann::json& out, const services::Resolution& resolution)
{
    out["footprint_status"] = services::to_string(resolution.status);
    out["footprint_reason"] = resolution.reason;
    // 原始引用永远保留：not_found/stale 时前端仍要显示用户写的是什么
    out["footprint_reference"] = resolution.reference;
    out["footprint_id"] = resolution.footprint_id;
    out["footprint_resolved_reference"] = resolution.resolved_reference;
    out["footprint_name"] = resolution.footprint_name;
    out["has_footprint"] = resolution.status == services::FootprintStatus::Available;
    out["model_status"] = services::to_string(resolution.model_status);
    out["model_reason"] = resolution.model_reason;
    out["model_id"] = resolution.model_id;
    out["model_name"] = resolution.model_name;
    out["has_3d_model"] = resolution.status == services::FootprintStatus::Available &&
                          resolution.model_status == services::ModelStatus::Available;
}

/// 搜索候选：保留既有 id/name/library/score/pad_count，只追加状态与原因
inline void apply_footprint_candidate_status(nlohmann::json& out,
                                             const services::Resolution& resolution)
{
    out["footprint_status"] = services::to_string(resolution.status);
    out["footprint_reason"] = resolution.reason;
}

}  // namespace kforge::api
