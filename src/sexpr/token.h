#pragma once

#include <cstdint>
#include <string_view>

namespace kforge::sexpr {

/// Token types for KiCAD S-expression format.
/// KiCAD S-expressions are simple: no dotted pairs, no reader macros.
/// Just parentheses, atoms (unquoted symbols), and quoted strings.
enum class TokenType : std::uint8_t {
    LParen,    // (
    RParen,    // )
    Atom,      // unquoted symbol: R, pin, at, 0, GND
    String,    // "quoted string"
    Eof,       // end of input
};

/// A single token with zero-copy reference into the source buffer.
struct Token {
    TokenType type{TokenType::Eof};
    std::string_view text;  // points into the source buffer — do not free
    int line{1};
    int column{1};
};

}  // namespace kforge::sexpr
