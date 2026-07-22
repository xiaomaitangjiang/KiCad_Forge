#include "sexpr/tokenizer.h"

#include <cctype>

namespace kforge::sexpr {

Tokenizer::Tokenizer(std::string_view input)
    : input_(input), pos_(0), line_(1), column_(1) {}

Token Tokenizer::make_token(TokenType type, size_t start, size_t len) {
    Token tok;
    tok.type = type;
    tok.text = input_.substr(start, len);
    tok.line = line_;
    tok.column = column_;
    return tok;
}

void Tokenizer::skip_whitespace_and_comments() {
    while (pos_ < input_.size()) {
        char c = input_[pos_];

        // Whitespace
        if (c == ' ' || c == '\t' || c == '\r') {
            column_++;
            pos_++;
            continue;
        }

        // Newline
        if (c == '\n') {
            line_++;
            column_ = 1;
            pos_++;
            continue;
        }

        // Line comment (KiCAD uses # for comments in some files)
        if (c == '#') {
            while (pos_ < input_.size() && input_[pos_] != '\n') {
                pos_++;
            }
            continue;
        }

        break;
    }
}

util::Result<Token> Tokenizer::read_atom() {
    size_t start = pos_;
    int start_col = column_;

    while (pos_ < input_.size()) {
        char c = input_[pos_];
        // Atom characters: anything not whitespace, parens, or quotes
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '(' || c == ')' || c == '"') {
            break;
        }
        pos_++;
    }

    size_t len = pos_ - start;
    if (len == 0) {
        return std::unexpected(util::Error::parse("empty atom", line_, start_col));
    }

    auto tok = make_token(TokenType::Atom, start, len);
    column_ += static_cast<int>(len);
    return tok;
}

util::Result<Token> Tokenizer::read_string() {
    // Skip opening quote
    pos_++;  // skip '"'
    column_++;
    size_t start = pos_;
    int start_col = column_;

    while (pos_ < input_.size()) {
        char c = input_[pos_];
        if (c == '"') {
            size_t len = pos_ - start;
            auto tok = make_token(TokenType::String, start, len);
            pos_++;  // skip closing quote
            column_++;
            return tok;
        }
        if (c == '\\' && pos_ + 1 < input_.size()) {
            // Escaped character — skip it
            pos_ += 2;
            column_ += 2;
            continue;
        }
        if (c == '\n') {
            line_++;
            column_ = 1;
        } else {
            column_++;
        }
        pos_++;
    }

    return std::unexpected(util::Error::parse("unterminated string", line_, start_col));
}

util::Result<Token> Tokenizer::next() {
    skip_whitespace_and_comments();

    if (pos_ >= input_.size()) {
        return Token{TokenType::Eof, {}, line_, column_};
    }

    char c = input_[pos_];

    if (c == '(') {
        auto tok = make_token(TokenType::LParen, pos_, 1);
        pos_++;
        column_++;
        return tok;
    }

    if (c == ')') {
        auto tok = make_token(TokenType::RParen, pos_, 1);
        pos_++;
        column_++;
        return tok;
    }

    if (c == '"') {
        return read_string();
    }

    return read_atom();
}

util::Result<std::vector<Token>> Tokenizer::tokenize_all() {
    std::vector<Token> tokens;
    while (true) {
        auto tok = next();
        if (!tok) return std::unexpected(tok.error());
        if (tok->type == TokenType::Eof) break;
        tokens.push_back(*std::move(tok));
    }
    return tokens;
}

}  // namespace kforge::sexpr
