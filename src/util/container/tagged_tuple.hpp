#pragma once

#include "util/template/traits.hpp"
#include "util/template/concept.hpp"

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>


namespace kforge::tagged
{

using namespace kforge::tagged::util;

/**
 * @brief 标签tag对象，用于为taggedtuple提供更加轻量的初始化方式
 * 
 * @tparam Tag 
 */
template <typename Tag>
struct tag_t
{
    using tag_type = Tag;

    template <typename T>
    constexpr auto operator()(T&& value) const
    {
        return std::pair<Tag, std::decay_t<T>>{std::forward<T>(value)};
    }
};

//CTAD
template<typename T>
tag_t(T)->tag_t<T>;

/**
 * @brief taggedtuple 标签化tuple，提供标签化访问
 * 
 * @tparam Pairs 
 */
template <TaggedElement... Pairs>
requires (all_unique<typename Pairs::first_type...>::value)  // 标签互异（concept/requires 约束）
class TaggedTuple
{

private:
    // 核心存储：仅存值，标签仅存在于类型系统中
    std::tuple<typename Pairs::second_type...> values_;

    template <typename Tag>
    static constexpr size_t idx_v = index_of<Tag, Pairs...>::value;

    template <typename Tag>
    static constexpr bool has_tag_v = (std::is_same_v<Tag, typename Pairs::first_type> || ...);

public:
    constexpr TaggedTuple() = default;

    // 从值列表构造（顺序随意，但推荐与 Pairs 顺序一致）
    template <typename... Us>
    explicit constexpr TaggedTuple(Us&&... args) : values_(std::forward<Us>(args)...)
    {
    }

    // 从 std::tuple 转换
    template <typename... Ts>
    explicit constexpr TaggedTuple(const std::tuple<Ts...>& tup) : values_(tup)
    {
    }

    template <typename... Ts>
    explicit constexpr TaggedTuple(std::tuple<Ts...>&& tup) : values_(std::move(tup))
    {
    }

    //tag访问 get<tag>
    template <auto Tag>
    constexpr decltype(auto) get(this auto&& self)
    {
        using Tag_T = typename std::remove_cvref_t<decltype(Tag)>::tag_type;
        static_assert(has_tag_v<Tag_T>, "Tag not found in TaggedTuple.");
        return std::get<idx_v<Tag_T>>(std::forward_like<decltype(self)>(self.values_));
    }

    template <typename Tag>
    constexpr decltype(auto) get(this auto&& self)
    {
        static_assert(has_tag_v<Tag>, "Tag not found in TaggedTuple.");
        return std::get<idx_v<Tag>>(std::forward_like<decltype(self)>(self.values_));
    }


    // 索引访问 get<index>
    template <size_t I>
    constexpr decltype(auto) get(this auto&& self)
    {
        static_assert(I < sizeof...(Pairs), "Index out of range.");
        return std::get<I>(std::forward_like<decltype(self)>(self.values_));
    }

    //工具
    static constexpr size_t size() noexcept
    {
        return sizeof...(Pairs);
    }

    template <typename Tag>
    static constexpr bool contains() noexcept
    {
        return has_tag_v<Tag>;
    }

    // 暴露内部 tuple
    constexpr auto& as_tuple() &
    {
        return values_;
    }
    constexpr const auto& as_tuple() const&
    {
        return values_;
    }
    constexpr auto as_tuple() &&
    {
        return std::move(values_);
    }
};

//CTAD 推导指引
template <typename... Pairs>
requires(TaggedElement<std::decay_t<Pairs>> && ...)
TaggedTuple(Pairs&&...) -> TaggedTuple<std::decay_t<Pairs>...>;

template <typename... Tags, typename... Values>
TaggedTuple(std::pair<Tags, Values>...) -> TaggedTuple<std::pair<Tags, Values>...>;
/*
// 工厂函数
// 将值包装为 Tagged 元素 (std::pair)
template <typename Tag, typename T>
constexpr auto make_element(T&& v)
{
    return std::pair<Tag, std::decay_t<T>>{std::forward<T>(v)};
}

// 批量工厂（显式指定标签）
template <auto... Tags, typename... Values>
constexpr auto make_tagged_tuple(Values&&... values)
{
    static_assert(sizeof...(Tags) == sizeof...(Values));
    return TaggedTuple<std::pair<std::remove_cv_t<decltype(Tags)>, std::decay_t<Values>>...>{std::forward<Values>(values)...};
}
*/
}  // namespace kforge::tagged

//结构化绑定
namespace std
{
using namespace kforge::tagged;

template <TaggedElement... Pairs>
struct tuple_size<TaggedTuple<Pairs...>> : integral_constant<size_t, sizeof...(Pairs)>
{
};

template <size_t I, TaggedElement... Pairs>
struct tuple_element<I, TaggedTuple<Pairs...>>
{
    using type = tuple_element_t<I, tuple<typename Pairs::second_type...>>;
};
}  // namespace std