// Generic error handling primitives — standard-library style
//   ErrorInfo<ErrorTs...>  — variant dispatch
//   StatusResult<E, Info>  — enum status + typed context
//   ErrorVisitor<Derived>  — CRTP visitor factory + dispatch
#pragma once

#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace kforge::util
{

namespace detail
{
template <typename... Fs>
struct Overloaded : Fs...
{
    using Fs::operator()...;
};
template <typename... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;
}  // namespace detail

// ============================================================
// ErrorInfo<ErrorTs...> — variant dispatch
// ============================================================
template <typename... ErrorTs>
class ErrorInfo
{
public:
    using Variant = std::variant<ErrorTs...>;
    static constexpr size_t N = sizeof...(ErrorTs);

    // The first error type acts as the empty state for default construction
    // (used by StatusResult::ok() aggregate init and Error's default ctor)
    static_assert(std::is_default_constructible_v<std::variant_alternative_t<0, Variant>>,
                  "First error type must be default-constructible (acts as the empty state)");

    template <typename T>
    requires(std::is_same_v<std::decay_t<T>, ErrorTs> || ...)
    explicit ErrorInfo(T&& v) noexcept(std::is_nothrow_constructible_v<Variant, T>)
        : value_(std::forward<T>(v))
    {
    }

    ErrorInfo() = default;

    // Per-type visitor — one lambda per error type; the compiler checks
    // coverage, and an auto&& fallback may cover the remaining types
    template <typename... Visitors>
    decltype(auto) match(Visitors&&... visitors) const
    {
        return std::visit(detail::Overloaded{std::forward<Visitors>(visitors)...}, value_);
    }

    // Pre-built overloaded visitor — single callable covering all types
    template <typename Visitor>
    [[nodiscard]] decltype(auto) visit(Visitor&& v) const
        noexcept(noexcept(std::visit(std::forward<Visitor>(v), value_)))
    {
        return std::visit(std::forward<Visitor>(v), value_);
    }

    // Check if holds a specific error type
    template <typename T>
    [[nodiscard]] bool is() const noexcept
    {
        return std::holds_alternative<T>(value_);
    }

    // Get the held value by type or by index (mutable and const access)
    template <typename T>
    [[nodiscard]] T& get()
    {
        return std::get<T>(value_);
    }
    template <typename T>
    [[nodiscard]] const T& get() const
    {
        return std::get<T>(value_);
    }
    template <std::size_t T>
    [[nodiscard]] decltype(auto) get() const
    {
        return std::get<T>(value_);
    }

    [[nodiscard]] const Variant& variant() const noexcept
    {
        return value_;
    }

private:
    Variant value_{};
};

// deduction guide
template <typename T>
ErrorInfo(T) -> ErrorInfo<std::decay_t<T>>;

// ---- make_error_visitor: create an overloaded visitor on the fly ----
template <typename... Fs>
auto make_error_visitor(Fs&&... fns)
{
    return detail::Overloaded<std::decay_t<Fs>...>{std::forward<Fs>(fns)...};
}

// ============================================================
// ErrorVisitor<Derived> — CRTP visitor factory + dispatch
// Derived provides: static visitor(const Derived&) → visitor
// ============================================================
template <typename Derived>
class ErrorVisitor
{
public:
    template <typename... Fs>
    static auto make(Fs&&... fns)
    {
        return make_error_visitor(std::forward<Fs>(fns)...);
    }

    template <typename... ErrorTs>
    static decltype(auto) dispatch(const Derived& self, const ErrorInfo<ErrorTs...>& info)
    {
        return info.visit(Derived::visitor(self));
    }
};

// ============================================================
// StatusResult<E, Info> — enum status + typed error context
// ============================================================
template <typename E, typename Info = std::string>
requires std::is_enum_v<E>
struct [[nodiscard]] StatusResult
{
    E status{};
    Info info{};

    [[nodiscard]] constexpr bool is(E s) const noexcept
    {
        return status == s;
    }

    static constexpr StatusResult ok(E s) noexcept
    {
        return {s, {}};
    }
    static StatusResult err(E s, Info i) noexcept
    {
        return {s, std::move(i)};
    }
};

}  // namespace kforge::util
