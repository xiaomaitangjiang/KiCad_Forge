#pragma once

#include "util/container/tagged_tuple.hpp"
#include "util/error.h"
#include "util/service_runtime.hpp"
#include "util/template/concept.hpp"
#include "util/template/traits.hpp"

#include <functional>
#include <array>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>


namespace kforge::launcher
{

// Server / util::instance_store / server_result / failed_by 已迁移至
// util/service_runtime.hpp（通用机制，供各层服务类使用）。

/**
 * @brief 把槽位解析为服务实例引用：
 *        引用槽 → 所引对象；自有槽（owned_tag）→ 静态实例存储中的实例。
 *
 * 定义于 Launcher 所在命名空间 —— find_args 的非限定调用经外围命名
 * 空间查找解析到此处，traits.hpp 无需反向依赖 Server。
 */
template <typename D>
D& resolve(std::variant<std::reference_wrapper<D>, typename Server<D>::owned_tag>& slot)
{
    if (auto* ref = std::get_if<std::reference_wrapper<D>>(&slot))
    {
        return ref->get();
    }
    return *util::instance_store<D>();
}

/**
 * @brief 服务生命周期管理器
 *
 * 模板参数即各个服务，构造函数按声明序接收全部槽位实参并立即绑定进
 * TaggedTuple。槽位为 variant<reference_wrapper<D>, Server<D>::owned_tag>：
 * 左值实参 -> 槽持引用；
 * 右值实参 -> 实例存入按类型的静态实例存储里，此时槽持空类标记 （这样槽位就可以恒定两个指针宽）
 * 服务类型无需继承 Server，owned_tag 仅是槽位空类标签。
 * 模板参数的 S&/S 写法仅表意，行为以实参值类别为准
 *（CTAD 推导：Launcher(http, logger) -> Launcher<HttpServer, Logger&>）。
 *
 * 服务契约：
 * - 可启动：S::build()必须存在且返回Result<void>
 * - 可选销毁：S::destroy()既可以存在也可以不存在，如果存在就必须返回Result<void>——
 *   destroy() / destroy_all() 按照LIFO销毁
 * - 重复类型/未注册槽位不由launcher负责，而是由TaggedTuple检查。
 *
 * 生命周期：
 * 引用对象自行管理生命周期
 * 实例存于静态实例存储由launcher管理
 * build 调用后需要清理，失败时也清理部分资源；未调用 build 的服务跳过。
 * 只有 destroy 的服务从构造起参与清理。每轮启动仅尝试一次 destroy，移动转交清理责任。
 */
template <typename... Servers>
requires(sizeof...(Servers) > 0)  // 空 Launcher 无意义
class Launcher
{
public:
    using Result = kforge::util::Result<void>;

private:
    using raw_slots = std::tuple<Servers...>;
    static constexpr std::size_t count_v = sizeof...(Servers);

    static constexpr bool has_cleanup = (util::Destroyable<std::decay_t<Servers>> || ...);
    struct NoCleanup {};
    using CleanupState = std::conditional_t<has_cleanup, std::array<bool, count_v>, NoCleanup>;

    static constexpr CleanupState initial_cleanup()
    {
        if constexpr (has_cleanup)
            return {(!util::Buildable<std::decay_t<Servers>> &&
                     util::Destroyable<std::decay_t<Servers>>) ...};
        else
            return {};
    }

    template <typename S>
    static constexpr std::size_t slot_index()
    {
        constexpr std::array matches{std::is_same_v<S, std::decay_t<Servers>>...};
        for (std::size_t i = 0; i < count_v; ++i)
            if (matches[i]) return i;
        return count_v;
    }

    // 槽位：variant<引用, 标记>
    template <typename S>
    using slot_t = std::variant<std::reference_wrapper<std::remove_reference_t<S>>,
                                typename Server<std::decay_t<S>>::owned_tag>;
    using Storage =
        tagged::TaggedTuple<std::pair<util::tag<std::decay_t<Servers>>, slot_t<Servers>>...>;

    // 检查构造实参是否为右值
    template <std::size_t I, typename ArgTuple>
    static constexpr bool arg_is_rvalue_v =
        !std::is_lvalue_reference_v<std::tuple_element_t<I, std::remove_cvref_t<ArgTuple>>>;

    // 左值持引用；右值存入静态实例存储，槽持 owned_tag。
    template <std::size_t I, typename ArgTuple>
    static constexpr auto slot_arg(ArgTuple&& args)
    {
        using slot_val = std::remove_reference_t<std::tuple_element_t<I, raw_slots>>;
        using slot = slot_t<std::tuple_element_t<I, raw_slots>>;
        if constexpr (arg_is_rvalue_v<I, ArgTuple>)
        {
            // 右值实例直接存储到静态容器；
            // 槽内只放空类标记 —— 槽位恒定两个指针宽，与 sizeof(服务) 无关
            util::instance_store<slot_val>().emplace(std::get<I>(std::forward<ArgTuple>(args)));
            return slot(typename Server<slot_val>::owned_tag{});
        }
        else
        {
            return slot(std::ref(std::get<I>(args)));  // 持有引用
        }
    }

    template <typename ArgTuple, std::size_t... I>
    static constexpr Storage bind_slots(ArgTuple&& args, std::index_sequence<I...> /*unused*/)
    {
        return Storage(slot_arg<I>(std::forward<ArgTuple>(args))...);
    }

    // launch_all 跳过非Buildable的槽
    template <typename S>
    server_result<Servers...> launch_if_buildable()
    {
        server_result<Servers...> out;
        if constexpr (util::Buildable<std::decay_t<S>>)
        {
            out = launch<std::decay_t<S>>();
        }
        return out;
    }

    // destroy_all 跳过非Destroyable的槽
    template <typename S>
    server_result<Servers...> destroy_if_destroyable()
    {
        server_result<Servers...> out;
        if constexpr (util::Destroyable<std::decay_t<S>>)
        {
            out = destroy<std::decay_t<S>>();
        }
        return out;
    }

    // 反序序列：destroy_all 按声明序逆序销毁
    template <std::size_t... S>
    server_result<Servers...> destroy_all_impl(std::index_sequence<S...> /*unused*/)
    {
        server_result<Servers...> outcome;
        using pack = std::tuple<Servers...>;
        constexpr std::size_t N = sizeof...(Servers);
        // 首个失败记录原始 Result + 服务 tag，短路终止后续销毁
        (void)((([&] {
            using T = std::decay_t<std::tuple_element_t<N - 1 - S, pack>>;
            auto r = destroy_if_destroyable<T>();
            if (!r.ok() && outcome.ok())
            {
                outcome.result = std::move(r.result);
                outcome.fail = typename Server<T>::owned_tag{};
            }
            return outcome.ok();
        })()) && ...);
        return outcome;
    }

    // 正序序列：launch_all 按声明序启动
    template <std::size_t... S>
    server_result<Servers...> launch_all_impl(std::index_sequence<S...> /*unused*/)
    {
        server_result<Servers...> outcome;
        using pack = std::tuple<Servers...>;
        // 首个失败记录原始 Result + 服务 tag，短路终止后续启动
        (void)((([&] {
            using T = std::decay_t<std::tuple_element_t<S, pack>>;
            auto r = launch_if_buildable<T>();
            if (!r.ok() && outcome.ok())
            {
                outcome.result = std::move(r.result);
                outcome.fail = typename Server<T>::owned_tag{};
            }
            return outcome.ok();
        })()) && ...);
        return outcome;
    }

public:
    /**
     * @brief 全量构造：按声明序接收每个槽位的实参，构造期即绑定进 taggedtuple
     */
    template <typename... Ts>
    requires(sizeof...(Ts) == count_v)
    explicit Launcher(Ts&&... servers)
        : ServerList(bind_slots(std::forward_as_tuple(std::forward<Ts>(servers)...),
                                std::make_index_sequence<count_v>{}))
    {
    }

    /**
     * @brief 按服务类型取槽位内容（统一返回槽内实例的引用）
     */
    template <typename Service>
    requires util::RegisteredSlot<Service, Servers...>  // Service 必须已注册为槽位
    constexpr decltype(auto) get(this auto&& self)
    {
        using S = std::decay_t<Service>;
        auto&& slot = std::forward_like<decltype(self)>(self.ServerList).template get<util::tag<S>>();
        return resolve(slot);
    }

    /**
     * @brief 启动单个服务：按 build 形参类型从槽位注入依赖并调用 build（build 即 start）
     * @return server_result：失败时 failed_by<Service> 可判定
     */
    template <util::BuildableService<Servers...> Service>
    server_result<Servers...> launch()
    {
        server_result<Servers...> outcome;
        using S = std::decay_t<Service>;
        using build_fn = decltype(&S::build);
        using traits = kforge::util::function_traits<build_fn>;
        using bare_args_t = kforge::util::remove_cvref_tuple_t<typename traits::args_tuple>;

        auto params = util::find_args<bare_args_t>(
            ServerList, std::make_index_sequence<std::tuple_size_v<bare_args_t>>{});
        // build 失败或抛出时也可能已分配资源，需要清理。
        if constexpr (util::Destroyable<S>)
            cleanup_[slot_index<S>()] = true;
        outcome.result = std::apply(&S::build, std::move(params));
        if (!outcome.ok())
        {
            outcome.fail = typename Server<S>::owned_tag{};
        }
        return outcome;
    }

    /**
     * @brief 按声明序启动所有 Buildable 服务（跳过非 Buildable 的自启动服务）；
     *        首个失败立即停止；失败时 failed_by<T> 判定失败服务
     */
    server_result<Servers...> launch_all()
    {
        return launch_all_impl(std::make_index_sequence<count_v>{});
    }

    /**
     * @brief 销毁单个服务：调用 S::destroy()
     */
    template <util::Destroyable Service>
    requires util::RegisteredSlot<Service, Servers...>
    server_result<Servers...> destroy()
    {
        server_result<Servers...> outcome;
        using S = std::decay_t<Service>;
        // 每轮启动只尝试一次销毁，包括返回错误或抛异常的情况。
        if (!std::exchange(cleanup_[slot_index<S>()], false))
            return outcome;
        outcome.result = S::destroy();
        if (!outcome.ok())
        {
            outcome.fail = typename Server<S>::owned_tag{};
        }
        return outcome;
    }

    /**
     * @brief 按声明序逆序销毁所有 Destroyable 服务（跳过非 Destroyable 槽）；
     *        首个失败立即停止；失败时 failed_by<T> 判定失败服务
     */
    server_result<Servers...> destroy_all()
    {
        return destroy_all_impl(std::make_index_sequence<count_v>{});
    }

private:
    // 非短路销毁：逆序逐个尝试，单个失败/异常不影响其余 —— 供析构兜底
    template <std::size_t... S>
    void destroy_all_unchecked_impl(std::index_sequence<S...> /*unused*/)
    {
        using pack = std::tuple<Servers...>;
        constexpr std::size_t N = sizeof...(Servers);
        // 逗号折叠（非短路）：每个服务独立 try，异常吞掉
        (void)(void([&] {
            try { (void) destroy_if_destroyable<std::tuple_element_t<N - 1 - S, pack>>(); }
            catch (...) {}
        }()), ...);
    }

public:
    // 析构兜底：全量逆序清理，忽略 Result/异常（与 destroy_all 的短路语义区分）
    ~Launcher()
    {
        destroy_all_unchecked_impl(std::make_index_sequence<count_v>{});
    }
    // 移出对象不再负责清理，服务实例地址保持不变。
    Launcher(Launcher&& other) noexcept
        : ServerList(std::move(other.ServerList)),
          cleanup_(std::exchange(other.cleanup_, CleanupState{}))
    {
    }

    Launcher& operator=(Launcher&& other) noexcept
    {
        if (this != &other)
        {
            destroy_all_unchecked_impl(std::make_index_sequence<count_v>{});
            ServerList = std::move(other.ServerList);
            cleanup_ = std::exchange(other.cleanup_, CleanupState{});
        }
        return *this;
    }

private:
    Storage ServerList;
    [[no_unique_address]] CleanupState cleanup_ = initial_cleanup();
};


// CTAD：左值推导为引用（T&），右值推导为值（T）
template <typename... Ts>
Launcher(Ts&&...) -> Launcher<Ts...>;
}  // namespace kforge::launcher
