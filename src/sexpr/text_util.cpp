#include "sexpr/text_util.h"
#include <algorithm>

namespace kforge::sexpr {

std::optional<size_t> find_node(std::string_view text, std::string_view type,
                                std::string_view name) {
    std::string marker = std::string("(") + std::string(type) + " \"" +
                         std::string(name) + "\"";
    auto pos = text.find(marker);
    // Also try without quotes (atom-style names)
    if (pos == std::string_view::npos) {
        marker = std::string("(") + std::string(type) + " " + std::string(name);
        pos = text.find(marker);
        // Verify it's a word boundary after the name
        if (pos != std::string_view::npos) {
            size_t after = pos + marker.size();
            if (after < text.size() && text[after] != ' ' && text[after] != '\n' &&
                text[after] != '\t' && text[after] != '\r' && text[after] != ')')
                pos = std::string_view::npos;
        }
    }
    if (pos == std::string_view::npos) return std::nullopt;
    return pos;
}

size_t find_matching_paren(std::string_view text, size_t open_paren) {
    int depth = 0;
    bool in_str = false;
    for (size_t i = open_paren; i < text.size(); i++) {
        char c = text[i];
        if (in_str) {
            if (c == '"' && (i == 0 || text[i - 1] != '\\')) in_str = false;
        } else {
            if (c == '"') in_str = true;
            else if (c == '(') depth++;
            else if (c == ')') {
                depth--;
                if (depth == 0) return i;
            }
        }
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

}  // namespace kforge::sexpr
