// 类型删除的生产入口：引用检查 + 持久化删除，两者在同一 DbSession 内完成。
// 刻意不依赖 HTTP / httplib —— 路由与测试调用的是同一段代码，
// 测试也就真的在测生产路径，而不是在测试里复制一份 if。
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "classifier/type_matching.h"
#include "core/db/database.h"
#include "core/model/type_registry.h"
#include "core/repo/repositories.h"
#include "util/error.h"

namespace kforge::api
{

/// 一次删除尝试的判定结果。
/// removed=false 且 references>0 = 仍被引用（业务拒绝，不是故障）；
/// 查询失败 / 写盘失败走 Result 错误，绝不返回「0 引用」蒙混过关。
/// matching_referenced：拒绝来自匹配配置/旧快照引用（guard 发现），与 DB 引用数无关。
struct RemovalOutcome
{
    bool removed = false;
    int references = 0;
    std::string reason;  // removed=false 时给用户看的原因
    bool matching_referenced = false;
};

namespace detail
{
inline util::Error reference_check_failed(const std::string& what, const util::Error& error)
{
    return util::Error::make<util::Error::Kind::DbError>(
        "Cannot check " + what + " references: " + error.format_message());
}

/// 引用检查 + 持久化删除。计数与删除共用同一个 session：
/// 期间没有别的 DB 写入能挤进来把类型重新挂上。
/// matching_referenced：调用方（remove_component_type）在同一 ReferenceGuard 生命期内
/// 查得的匹配配置/旧快照引用 —— 与 DB 计数并行生效，任一为真都拒删。
inline util::Result<RemovalOutcome> remove_type(storage::Database& db, const std::string& id,
                                                bool package,
                                                bool matching_referenced = false)
{
    if (id.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::NotFound>("Type not found"));
    bool builtin = package ? core::is_builtin_package_id(id) : core::is_builtin_component_id(id);
    if (builtin)
        return RemovalOutcome{false, 0, "Built-in types cannot be deleted"};

    auto session = db.session();
    if (!package)
    {
        auto symbols = storage::SymbolRepository(session).count_by_component_type(id);
        if (!symbols)
            return std::unexpected(reference_check_failed("component type", symbols.error()));
        if (matching_referenced || *symbols > 0)
        {
            // references 只反映真实 DB 计数；配置/旧快照引用如实走 matching_referenced，
            // 不把规则引用伪装成 DB 引用数。
            std::string reason = "Type is still referenced; reclassify first";
            if (matching_referenced)
            {
                if (*symbols == 0)
                    reason = "Type is referenced by matching rules or exceptions, or by "
                             "operations holding old snapshots; remove the matching rule and "
                             "exception references, and wait for the operations holding old "
                             "snapshots to finish";
                else
                    reason = "Type is referenced by symbols and by matching rules or "
                             "exceptions, or by operations holding old snapshots; reclassify "
                             "the symbols first to drop the database references, then remove "
                             "the matching rule and exception references, and wait for the "
                             "operations holding old snapshots to finish";
            }
            return RemovalOutcome{false, *symbols, std::move(reason), matching_referenced};
        }

        auto removed = core::TypeRegistry::instance().remove_component_type_persistent(id);
        if (!removed) return std::unexpected(removed.error());
        return RemovalOutcome{true, 0, {}, false};
    }

    // 包类型有两处引用：符号的 package_type 与封装自身的 package_type
    auto symbols = storage::SymbolRepository(session).count_by_package_type(id);
    if (!symbols)
        return std::unexpected(reference_check_failed("package type", symbols.error()));
    auto footprints = storage::FootprintRepository(session).count_by_package_type(id);
    if (!footprints)
        return std::unexpected(reference_check_failed("package type", footprints.error()));
    int references = *symbols + *footprints;
    if (references > 0)
        return RemovalOutcome{false, references, "Type is still referenced; reclassify first"};

    auto removed = core::TypeRegistry::instance().remove_package_type_persistent(id);
    if (!removed) return std::unexpected(removed.error());
    return RemovalOutcome{true, 0, {}};
}
}  // namespace detail

/// matching_store：匹配配置仓储的可选注入（nullptr = 用进程级单例）。
/// 删除全程消费 TypeMatchingStore::ReferenceGuard：领 guard（失败 = 未知/坏配置，
/// 带匹配配置原因返回 Result 错误，绝不当「0 引用」放行）→ 查 DB 符号计数 →
/// Registry 持久化删除，guard 保持到完整返回。固定锁顺序 config → DbSession →
/// Registry：先领 guard 再开 DB session，期间并发保存新规则被同一把配置锁挡住，
/// 查完引用就放锁再删等于没保护。
inline util::Result<RemovalOutcome> remove_component_type(storage::Database& db,
                                                          std::string_view id,
                                                          classifier::TypeMatchingStore* matching_store
                                                          = nullptr)
{
    auto& store = matching_store ? *matching_store : classifier::TypeMatchingStore::instance();
    auto guard = store.acquire_reference_guard();
    if (!guard)
        return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
            "Cannot delete type: matching reference guard unavailable: "
            + guard.error().format_message()));
    const bool matching_referenced = guard->references(id);
    return detail::remove_type(db, std::string(id), false, matching_referenced);
}

inline util::Result<RemovalOutcome> remove_package_type(storage::Database& db,
                                                        std::string_view id)
{
    return detail::remove_type(db, std::string(id), true);
}

}  // namespace kforge::api
