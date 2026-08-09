#pragma once

#include <concepts>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace kforge::util
{

/// Error type used across the project.
class Error
{
public:
    enum class Kind
    {
        None,
        ParseError,
        IoError,
        DbError,
        NetworkError,
        PluginError,
        ServiceError,
        ValidationError,
        NotFound,
        InternalError,
    };

public:
    Error() = default;
    ~Error() = default;

    template <typename T>
    requires std::constructible_from<std::string, T>
    Error(Kind error_type, T&& message, int line, int column)
        : kind_(error_type), message_(std::forward<T>(message)), line_(line), column_(column)
    {
    }

    // Getters
    [[nodiscard]] Kind kind() const
    {
        return kind_;
    }
    [[nodiscard]] const std::string& message() const
    {
        return message_;
    }
    [[nodiscard]] int line() const
    {
        return line_;
    }
    [[nodiscard]] int column() const
    {
        return column_;
    }

    // Template factory — all kinds share this, ParseError can pass extra line/column
    template <Kind K>
    static Error make(std::string msg, int line = 0, int column = 0)
    {
        return {K, msg, line, column};
    }

private:
    Kind kind_ = Kind::None;
    std::string message_;
    int line_ = 0;
    int column_ = 0;

    // Friend: formatter accesses private fields
    template <Kind K>
    friend struct ErrorFormatter;
};

template <typename T>
using Result = std::expected<T, Error>;

// ============================================================
// Error formatting
// ============================================================

template <Error::Kind K>
struct ErrorFormatter
{
    static std::string format(const Error& e)
    {
        return e.message_;
    }
};

template <>
struct ErrorFormatter<Error::Kind::ParseError>
{
    static std::string format(const Error& e)
    {
        if (e.line_ > 0)
            return e.message_ + " at " + std::to_string(e.line_) + ":" + std::to_string(e.column_);
        return e.message_;
    }
};

inline std::string error_formatter(const Error& e)
{
    switch (e.kind())
    {
        case Error::Kind::ParseError:
            return ErrorFormatter<Error::Kind::ParseError>::format(e);
        case Error::Kind::IoError:
            return ErrorFormatter<Error::Kind::IoError>::format(e);
        case Error::Kind::DbError:
            return ErrorFormatter<Error::Kind::DbError>::format(e);
        case Error::Kind::NetworkError:
            return ErrorFormatter<Error::Kind::NetworkError>::format(e);
        case Error::Kind::PluginError:
            return ErrorFormatter<Error::Kind::PluginError>::format(e);
        case Error::Kind::NotFound:
            return ErrorFormatter<Error::Kind::NotFound>::format(e);
        default:
            return e.message();
    }
}

// ============================================================
// Generic multi-status result — user-defined enum as state
// ============================================================

template <typename E>
requires std::is_enum_v<E>
struct [[nodiscard]] StatusResult
{
    E status;
    std::string error;

    [[nodiscard]] bool is(E s) const
    {
        return status == s;
    }
    [[nodiscard]] bool ok() const
    {
        return static_cast<int8_t>(status) >= 0;
    }

    static StatusResult make(E s)
    {
        return {s, {}};
    }
    static StatusResult make_error(E s, std::string msg)
    {
        return {s, std::move(msg)};
    }
};



}  // namespace kforge::util
