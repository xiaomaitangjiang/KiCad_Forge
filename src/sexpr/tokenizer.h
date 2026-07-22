#pragma once

#include <string_view>
#include <vector>

#include "sexpr/token.h"
#include "util/result.h"

namespace kforge::sexpr {

/// Zero-copy streaming tokenizer for KiCAD S-expression format.
///
/// Usage:
///   Tokenizer tz(source_text);
///   while (auto tok = tz.next()) {
///       process(*tok);
///   }
class Tokenizer {
public:
    explicit Tokenizer(std::string_view input);

    /// Return the next token, or Eof at end of input.
    /// Returns Error on syntax errors (unterminated string, etc.).
    util::Result<Token> next();

    /// Tokenize all at once (convenience, may allocate).
    util::Result<std::vector<Token>> tokenize_all();

    /// Remaining un-tokenized text.
    std::string_view remaining() const { return input_.substr(pos_); }

private:
    void skip_whitespace_and_comments();
    util::Result<Token> read_atom();
    util::Result<Token> read_string();
    Token make_token(TokenType type, size_t start, size_t len);

    std::string_view input_;
    size_t pos_{0};
    int line_{1};
    int column_{1};
};

}  // namespace kforge::sexpr
