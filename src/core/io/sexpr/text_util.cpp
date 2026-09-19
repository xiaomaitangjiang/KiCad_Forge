#include "core/io/sexpr/text_util.h"
#include "core/io/sexpr/token/tokenizer.h"
#include "util/file_write.h"
#include <algorithm>
#include <fstream>
#include <iterator>

namespace kforge::sexpr {

namespace {
std::string escape_value(std::string_view value) {
    std::string result;
    for (char c : value) {
        if (c == '\\' || c == '"') result += '\\';
        result += c;
    }
    return result;
}

struct TextNodes {
    std::vector<Token> tokens;
    std::vector<size_t> parent, close;
    bool valid = false;
    explicit TextNodes(std::string_view text) {
        auto result = Tokenizer(text).tokenize_all();
        if (!result) return;
        tokens = std::move(*result);
        parent.resize(tokens.size(), std::string::npos);
        close.resize(tokens.size(), std::string::npos);
        std::vector<size_t> stack;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!stack.empty()) parent[i] = stack.back();
            if (tokens[i].type == TokenType::LParen) stack.push_back(i);
            if (tokens[i].type == TokenType::RParen) {
                if (stack.empty()) return;
                close[stack.back()] = i;
                stack.pop_back();
            }
        }
        valid = stack.empty();
    }
    bool matches(size_t i, std::string_view type, std::string_view name) const {
        return i + 2 < tokens.size() && tokens[i].type == TokenType::LParen &&
            tokens[i + 1].type == TokenType::Atom && tokens[i + 1].text == type &&
            (tokens[i + 2].type == TokenType::Atom || tokens[i + 2].type == TokenType::String) &&
            tokens[i + 2].text == (tokens[i + 2].type == TokenType::String ? escape_value(name) : std::string(name));
    }
    std::optional<size_t> node(std::string_view type, std::string_view name) const {
        if (!valid) return {};
        for (size_t i = 0; i < tokens.size(); ++i) {
            auto p = parent[i];
            bool top = p == std::string::npos ||
                (parent[p] == std::string::npos && p + 1 < tokens.size() &&
                 tokens[p + 1].text == "kicad_symbol_lib");
            if (top && matches(i, type, name)) return i;
        }
        return {};
    }
    std::optional<size_t> property(size_t node, std::string_view key) const {
        for (size_t i = node + 1; i < close[node]; ++i)
            if (parent[i] == node && matches(i, "property", key)) return i;
        return {};
    }
};
}

std::optional<size_t> find_node(std::string_view text, std::string_view type,
                                std::string_view name) {
    TextNodes nodes(text);
    if (auto index = nodes.node(type, name))
        return static_cast<size_t>(nodes.tokens[*index].text.data() - text.data());
    return {};
}

size_t find_matching_paren(std::string_view text, size_t open_paren) {
    if (open_paren >= text.size() || text[open_paren] != '(') return std::string_view::npos;
    Tokenizer tokenizer(text.substr(open_paren));
    int depth = 0;
    while (auto token = tokenizer.next()) {
        if (token->type == TokenType::Eof) break;
        if (token->type == TokenType::LParen) ++depth;
        if (token->type == TokenType::RParen && --depth == 0)
            return static_cast<size_t>(token->text.data() - text.data());
    }
    return std::string_view::npos;
}

std::string insert_property(std::string_view text, size_t before_pos, std::string_view key,
                            std::string_view value) {
    std::string prop = "\n\t\t(property \"" + std::string(key) + "\" " +
                       std::string(value) + "\n"
                       "\t\t\t(at 0 -22.86 0)\n"
                       "\t\t\t(effects\n"
                       "\t\t\t\t(font (size 1.27 1.27))\n"
                       "\t\t\t\t(hide yes)\n"
                       "\t\t\t)\n"
                       "\t\t)";
    std::string result(text.substr(0, before_pos));
    result += prop;
    result += text.substr(before_pos);
    return result;
}

std::string remove_node(std::string_view text, size_t start, size_t end) {
    std::string result(text.substr(0, start));
    result += text.substr(end);
    return result;
}

bool replace_property_value(std::string& text, std::string_view node_type,
                            std::string_view node_name, std::string_view prop_key,
                            std::string_view new_value) {
    TextNodes nodes(text);
    auto node = nodes.node(node_type, node_name);
    if (!node) return false;
    auto prop = nodes.property(*node, prop_key);
    if (!prop || *prop + 3 >= nodes.close[*prop]) return false;
    const auto& value = nodes.tokens[*prop + 3];
    if (value.type != TokenType::String && value.type != TokenType::Atom) return false;
    size_t start = static_cast<size_t>(value.text.data() - text.data());
    size_t length = value.text.size();
    if (value.type == TokenType::String) { --start; length += 2; }
    auto replacement = "\"" + escape_value(new_value) + "\"";
    text.replace(start, length, replacement);
    return true;
}

int patch_kf_id_to_file(
    const std::filesystem::path& sym_file,
    const std::unordered_map<std::string, std::string>& name_to_kf_id)
{
    if (!std::filesystem::exists(sym_file)) return 0;

    std::ifstream in(sym_file, std::ios::binary);
    if (!in.is_open()) return 0;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    in.close();

    int patched = 0;
    for (const auto& [name, kf_id] : name_to_kf_id) {
        TextNodes nodes(text);
        auto node = nodes.node("symbol", name);
        if (!node || nodes.property(*node, "Kicad_Forge_ID")) continue;
        auto position = static_cast<size_t>(nodes.tokens[nodes.close[*node]].text.data() - text.data());
        text = insert_property(text, position, "Kicad_Forge_ID", "\"" + escape_value(kf_id) + "\"");
        ++patched;
    }
    if (patched > 0) util::replace_file(sym_file, text);
    return patched;
}

}  // namespace kforge::sexpr
