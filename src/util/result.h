#pragma once

#include <expected>
#include <string>

namespace kforge::util {

/// Error type used across the project for parse/IO/DB operations.
struct Error {
    enum class Kind {
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

    Kind kind;
    std::string message;
    int line{0};
    int column{0};

    static Error parse(std::string msg, int l = 0, int c = 0) {
        return {Kind::ParseError, std::move(msg), l, c};
    }
    static Error io(std::string msg) {
        return {Kind::IoError, std::move(msg)};
    }
    static Error db(std::string msg) {
        return {Kind::DbError, std::move(msg)};
    }
    static Error network(std::string msg) {
        return {Kind::NetworkError, std::move(msg)};
    }
    static Error plugin(std::string msg) {
        return {Kind::PluginError, std::move(msg)};
    }
    static Error not_found(std::string msg) {
        return {Kind::NotFound, std::move(msg)};
    }
    static Error internal_err(std::string msg) {
        return {Kind::InternalError, std::move(msg)};
    }
};

/// Type-safe result type used throughout the codebase.
/// Prefer this over exceptions for predictable error handling.
template <typename T>
using Result = std::expected<T, Error>;

}  // namespace kforge::util
