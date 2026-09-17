#pragma once

#include <tuple>
#include <type_traits>
namespace kforge::util
{

/**
 * @brief 函数参数提取模板工具
 * 
 * @tparam T 
 */
template <typename T>
struct function_traits;

/**
 * @brief 函数指针参数提取模板工具
 * 
 * @tparam Ret 
 * @tparam args 
 */
template <typename Ret, typename... args>
struct function_traits<Ret (*)(args...)>
{
    using return_type = Ret;
    using args_tuple = std::tuple<args...>;
};

template <typename Ret, typename... args>
struct function_traits<Ret(args...)>
{
    using return_type = Ret;
    using args_tuple = std::tuple<args...>;
};

template <typename Ret, typename Class, typename... Args>
struct function_traits<Ret (Class::*)(Args...)>
{
    using return_type = Ret;
    using args_tuple = std::tuple<Args...>;
};

template <typename Tuple>
struct remove_cvref_tuple;


template <typename... Ts>
struct remove_cvref_tuple<std::tuple<Ts...>>
{
    using type = std::tuple<std::remove_cvref_t<Ts>...>;
};


template <typename Tuple>
using remove_cvref_tuple_t = typename remove_cvref_tuple<Tuple>::type;

}  // namespace kforge::util

namespace kforge::launcher::util
{

/**
 * @brief Launcher 内部的槽位键类型，作为 TaggedTuple 的 first_type
 *
 * @tparam T
 */
template <typename T>
struct tag
{
};

/**
 * @brief Launcher的帮助函数，用于查找参数：按 build 形参类型（remove_cvref 后）
 *        取 util::tag<形参类型>，调用 TaggedTuple 的成员 get 查找槽位，
 *        再以引用组装参数元组（std::apply 时按 build 形参转换）；
 *        槽位解析由 kforge::launcher::resolve 完成（经外围命名空间查找解析）
 *
 * @tparam Tuple remove_cvref 后的 build 形参类型元组
 * @param storage Launcher 的槽位容器
 * @return auto (tuple<依赖引用...>)
 */
template <typename Tuple, typename Storage, std::size_t... I>
auto find_args(Storage& storage, std::index_sequence<I...>)
{
    return std::forward_as_tuple(
        resolve(storage.template get<tag<std::tuple_element_t<I, Tuple>>>())...);
}

}  // namespace kforge::launcher::util


namespace kforge::tagged::util
{

/**
 * @brief 检查类型唯一性的模板工具
 * 
 * @tparam  
 */
template <typename...>
struct all_unique : std::true_type
{
};

/**
 * @brief 检查类型唯一性的模板工具
 * 
 * @tparam T 
 * @tparam Rest 
 */
template <typename T, typename... Rest>
struct all_unique<T, Rest...>
    : std::bool_constant<(!std::is_same_v<T, Rest> && ...) && all_unique<Rest...>::value>
{
};

/**
 * @brief 计算Tag类型在形参包的索引的模板工具
 * 
 * @tparam Tag 
 * @tparam Pairs 
 */
template <typename Tag, typename... Pairs>
struct index_of;

/**
 * @brief 计算Tag类型在形参包的索引的模板工具
 * 
 * @tparam Tag 
 * @tparam First 
 * @tparam Rest 
 */
template <typename Tag, typename First, typename... Rest>
struct index_of<Tag, First, Rest...>
    : std::integral_constant<std::size_t, std::is_same_v<Tag, typename First::first_type>
                                              ? 0
                                              : 1 + index_of<Tag, Rest...>::value>
{
};

// 终止特化：未命中路径的哨兵（命中值来自命中分支，此值不参与结果；
// 调用方以 has_tag_v/static_assert 防护未注册的 Tag）
template <typename Tag>
struct index_of<Tag> : std::integral_constant<std::size_t, 0>
{
};
}  // namespace kforge::tagged::util