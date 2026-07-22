#include "dom_builder.h"

namespace kforge::sexpr {

util::Result<std::unique_ptr<DomNode>> DomBuilder::build(std::string_view input) {
    auto root = std::make_unique<DomNode>("__root__");

    size_t offset = 0;
    while (offset < input.size()) {
        // Find the next '('
        size_t paren = input.find('(', offset);
        if (paren == std::string_view::npos) break;

        // Skip whitespace before '(' to check it's top-level
        bool is_top_level = true;
        for (size_t i = offset; i < paren; ++i) {
            if (input[i] != ' ' && input[i] != '\t' &&
                input[i] != '\n' && input[i] != '\r' && input[i] != '#') {
                // Non-whitespace before '(' means it's nested
                // Actually for KiCad, just look for the end of the previous form
                break;
            }
        }

        // Parse the form starting at '('
        auto node_result = parse_one(input.substr(paren));
        if (!node_result) return std::unexpected(node_result.error());

        root->add_child(std::move(*node_result));

        // Advance past this form by counting parens
        int depth = 0;
        size_t i = paren;
        bool in_string = false;
        while (i < input.size()) {
            char c = input[i];
            if (in_string) {
                if (c == '"') in_string = false;
                else if (c == '\\' && i + 1 < input.size()) i++;
            } else {
                if (c == '"') in_string = true;
                else if (c == '(') depth++;
                else if (c == ')') {
                    depth--;
                    if (depth == 0) { i++; break; }
                }
            }
            i++;
        }
        offset = i;
    }

    return root;
}

util::Result<std::unique_ptr<DomNode>> DomBuilder::parse_one(
    std::string_view input) {
    Tokenizer tz(input);

    auto tok_result = tz.next();
    if (!tok_result) return std::unexpected(tok_result.error());
    if (tok_result->type == TokenType::Eof) {
        return std::unexpected(util::Error::parse("empty input"));
    }
    if (tok_result->type != TokenType::LParen) {
        return std::unexpected(util::Error::parse("expected '('", tok_result->line,
                                  tok_result->column));
    }

    return parse_node(tz);
}

util::Result<std::unique_ptr<DomNode>> DomBuilder::parse_node(Tokenizer& tz) {
    // Read the type name (first atom after '(')
    auto type_tok = tz.next();
    if (!type_tok) return std::unexpected(type_tok.error());
    if (type_tok->type != TokenType::Atom && type_tok->type != TokenType::String) {
        return std::unexpected(util::Error::parse("expected type name after '('",
                                  type_tok->line, type_tok->column));
    }

    auto node = std::make_unique<DomNode>(std::string(type_tok->text));

    // KiCad: "symbol" and "pin" nodes have a name/number as the next atom.
    bool has_name_atom = (node->type() == "symbol" || node->type() == "pin");
    if (has_name_atom) {
        auto name_tok = tz.next();
        if (!name_tok) return std::unexpected(name_tok.error());
        if (name_tok->type == TokenType::Atom || name_tok->type == TokenType::String) {
            node->set_atom(std::string(name_tok->text));
        } else if (name_tok->type == TokenType::LParen) {
            // No name atom — parse as child
            auto child = parse_node(tz);
            if (!child) return std::unexpected(child.error());
            node->add_child(std::move(*child));
        } else if (name_tok->type == TokenType::RParen) {
            return node;  // Empty symbol node
        }
    }

    // Parse remaining children
    auto result = parse_children(tz, *node);
    if (!result) return std::unexpected(result.error());

    return node;
}

util::Result<void> DomBuilder::parse_children(Tokenizer& tz, DomNode& node) {
    std::string pending_key;

    while (true) {
        auto tok_result = tz.next();
        if (!tok_result) return std::unexpected(tok_result.error());
        auto& tok = *tok_result;

        switch (tok.type) {
        case TokenType::RParen:
            if (!pending_key.empty()) {
                node.add_property(std::move(pending_key), "");
            }
            return {};

        case TokenType::LParen: {
            auto child = parse_node(tz);
            if (!child) return std::unexpected(child.error());
            if (!pending_key.empty()) {
                (*child)->set_type(pending_key + ":" + (*child)->type());
                pending_key.clear();
            }
            node.add_child(std::move(*child));
            break;
        }

        case TokenType::Atom:
        case TokenType::String: {
            if (pending_key.empty()) {
                pending_key = std::string(tok.text);
            } else {
                node.add_property(std::move(pending_key), std::string(tok.text));
                pending_key.clear();
            }
            break;
        }

        case TokenType::Eof:
            return std::unexpected(util::Error::parse("unexpected end of input, missing ')'",
                                      tok.line, tok.column));
        }
    }
}

}  // namespace kforge::sexpr
