#pragma once
#include <concepts>
#include <tuple>
#include <type_traits>

#include "util/error.h"
#include "util/template/traits.hpp"

namespace kforge::tagged::util
{
/**
 * @brief 查看T类型是否为taggedtuple要求的tag类型
 * 
 * @tparam T 
 */
template <typename T>
concept TaggedElement = requires {
    typename T::first_type;   // 标签类型
    typename T::second_type;  // 值类型
};
}  // namespace kforge::tagged::util


namespace kforge::launcher
{
// 前向声明：Server<D>::owned_tag 作为槽位 variant 的空类备选（类型标签），
// 服务类型无需继承它；定义位于 launcher.hpp
template <typename Derived>
struct Server;
}

namespace kforge::launcher::util
{

/**
 * @brief 检查T类型是否具有start()和stop()方法
 *
 * @tparam T
 */
template <typename T>
concept Startable = requires(T t) {
    { t.start() } -> std::same_as<void>;
    { t.stop() } -> std::same_as<void>;
};

/**
 * @brief 检查T类型是否拥有静态build工厂（build可访问且是静态成员函数）
 *
 * @tparam T
 */
template <typename T>
concept Buildable = requires {
    requires(!std::is_member_pointer_v<decltype(&T::build)>);
};

template <typename T>
concept TaggedElement=tagged::util::TaggedElement<T>;

/**
 * @brief 判断 decay 后的服务类型 T 是否已出现在 Launcher 模板参数包中
 *
 * @tparam T
 * @tparam Servers
 */
template <typename T, typename... Servers>
concept RegisteredSlot =
    (std::is_same_v<std::decay_t<T>, std::decay_t<Servers>> || ...);

/**
 * @brief build 形参元组（remove_cvref 后）逐元素均已注册为槽位
 *
 * @tparam Tuple
 * @tparam Servers
 */
template <typename Tuple, typename... Servers>
struct tuple_all_registered_helper;

template <typename... Ts, typename... Servers>
struct tuple_all_registered_helper<std::tuple<Ts...>, Servers...>
    : std::bool_constant<(RegisteredSlot<Ts, Servers...> && ...)>
{
};

/**
 * @brief 服务可启动：Buildable + 已注册 + build 返回 Result<void> + build 形参全注册。
 *        供 Launcher::launch 的 requires 使用 —— 单一概念替代多行展开
 *
 * @tparam T
 * @tparam Servers
 */
template <typename T, typename... Servers>
concept BuildableService =
    Buildable<T> &&
    RegisteredSlot<T, Servers...> &&
    std::same_as<typename kforge::util::function_traits<decltype(&T::build)>::return_type,
                 kforge::util::Result<void>> &&
    tuple_all_registered_helper<
        kforge::util::remove_cvref_tuple_t<
            typename kforge::util::function_traits<decltype(&T::build)>::args_tuple>,
        Servers...>::value;

/**
 * @brief 服务可销毁：存在静态 destroy() 返回 Result<void>
 *
 * @tparam T
 */
template <typename T>
concept Destroyable = requires {
    { T::destroy() } -> std::same_as<kforge::util::Result<void>>;
};

}  // namespace kforge::launcher::util