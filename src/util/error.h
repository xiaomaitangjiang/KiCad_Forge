// KiCad_Forge error type — builds on util::result.h generic primitives
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

#include "util/result.h"

namespace kforge::util
{

/// Legacy error context — message + source position
struct LegacyError
{
    std::string message;
    int line = 0;
    int column = 0;
};

/// Error type used across the project.
class Error : public ErrorVisitor<Error>
{
public:
    enum class Kind: std::uint8_t
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
    Error(Kind error_type, T&& msg, int l, int c)
        : result_{.status = error_type, .info = Info{LegacyError{std::forward<T>(msg), l, c}}}
    {
    }

    template <typename T>
    requires std::constructible_from<std::string, T>
    Error(Kind error_type, T&& msg)
        : result_{.status = error_type, .info = Info{std::forward<T>(msg)}}
    {
    }

    [[nodiscard]] Kind kind() const
    {
        return result_.status;
    }
    [[nodiscard]] const std::string& message() const
    {
        if (result_.info.is<std::string>())
        {
            return result_.info.get<std::string>();
        }
        return result_.info.get<LegacyError>().message;
    }
    [[nodiscard]] int line() const
    {
        if (result_.info.is<LegacyError>())
        {
            return result_.info.get<LegacyError>().line;
        }
        return 0;
    }
    [[nodiscard]] int column() const
    {
        if (result_.info.is<LegacyError>())
        {
            return result_.info.get<LegacyError>().column;
        }
        return 0;
    }

    template <Kind K>
    static Error make(std::string msg, int line = 0, int column = 0)
    {
        return {K, std::move(msg), line, column};
    }

private:
    using Info = ErrorInfo<std::string, LegacyError>;
    friend class ErrorVisitor<Error>;

    static auto visitor(const Error& self)
    {
        return ErrorVisitor<Error>::make(
            [](const std::string& msg)
            {
                return msg;
            },
            [](const LegacyError& le)
            {
                if (le.line > 0)
                {
                    return le.message + " at " + std::to_string(le.line) + ":" +
                           std::to_string(le.column);
                }
                return le.message;
            });
    }

public:
    [[nodiscard]] std::string format_message() const
    {
        return ErrorVisitor<Error>::dispatch(*this, result_.info);
    }

private:
    StatusResult<Kind, Info> result_;
};

template <typename T>
using Result = std::expected<T, Error>;

// Backward-compat wrapper — delegates to Error::format_message()
inline std::string error_formatter(const Error& e)
{
    return e.format_message();
}

}  // namespace kforge::util
