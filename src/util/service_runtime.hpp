// 通用服务运行时机制：服务槽位标签 Server、每类型静态实例存储、
// 服务生命周期操作结果 server_result 与失败判定。
// 位于 util 层 —— 各层服务类均可 include（不反向依赖 interface）。
#pragma once

#include "util/error.h"

#include <optional>
#include <type_traits>
#include <variant>

namespace kforge::launcher
{

/**
 * @brief 服务基标签：每个服务类型获得互不相同的专属空类 owned_tag（sizeof == 1），
 *        作为槽位 variant 的「自有实例」备选 —— 槽位恒定两个指针宽。
 *        服务类型无需继承 Server（owned_tag 仅是类型标签）。
 */
template <typename Derived>
struct Server
{
    struct owned_tag
    {
    };
};

namespace util
{

/**
 * @brief 服务静态实例存储：每类型一份。右值槽构造的服务 emplace 进这里，
 *        build/resolve 命中 owned_tag 槽时从这里取回。
 *        语义边界：同类型多实例冲突由组合根单 Launcher 保证；非线程安全（启动期单线程）。
 */
template <typename D>
std::optional<D>& instance_store()
{
    static std::optional<D> store;
    return store;
}

}  // namespace util

/**
 * @brief 服务生命周期操作的结果：内嵌服务的 Result（build/destroy 原始透传），
 *        失败服务以 owned_tag 空类型实例标识（类型即身份）。
 */
template <typename... Servers>
struct server_result
{
    kforge::util::Result<void> result{};  // 服务操作结果（原始透传）
    std::variant<typename Server<std::decay_t<Servers>>::owned_tag...> fail{};
    // fail：result 无值（失败）时 holds_alternative 对应服务的 owned_tag（空类型，无存储开销）

    [[nodiscard]] bool ok() const { return result.has_value(); }
};

/// 判断 server_result 的失败是否来自服务 T
template <typename Service, typename... Servers>
bool failed_by(const server_result<Servers...>& outcome)
{
    return !outcome.ok() &&
           std::holds_alternative<typename Server<std::decay_t<Service>>::owned_tag>(outcome.fail);
}

}  // namespace kforge::launcher
