#include "core/io/sexpr/text_util.h"
#include <algorithm>
#include <fstream>
#include <iterator>

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

bool replace_property_value(std::string& text, std::string_view node_type,
                            std::string_view node_name, std::string_view prop_key,
                            std::string_view new_value) {
    auto node_start = find_node(text, node_type, node_name);
    if (!node_start) return false;
    auto node_end = find_matching_paren(text, *node_start);
    if (node_end == std::string_view::npos) return false;

    // Locate (property "KEY" inside the node
    std::string marker = std::string("(property \"") + std::string(prop_key) + "\"";
    auto rel = text.find(marker, *node_start);
    if (rel == std::string::npos || rel > node_end) return false;

    // The '(' of the property subtree and its matching ')'
    auto prop_open = text.rfind('(', rel);
    if (prop_open == std::string::npos || prop_open < *node_start) return false;
    auto prop_close = find_matching_paren(text, prop_open);
    if (prop_close == std::string::npos || prop_close > node_end) return false;

    // Rebuild the property node with the new value — same layout as
    // insert_property so formatting stays consistent
    std::string prop = "(property \"" + std::string(prop_key) + "\" \"" +
                       std::string(new_value) +
                       "\"\n"
                       "\t\t\t(at 0 -22.86 0)\n"
                       "\t\t\t(effects\n"
                       "\t\t\t\t(font (size 1.27 1.27))\n"
                       "\t\t\t\t(hide yes)\n"
                       "\t\t\t)\n"
                       "\t\t)";
    text.replace(prop_open, prop_close - prop_open + 1, prop);
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
        auto node_start = find_node(text, "symbol", name);
        if (!node_start) continue;

        auto sym_end = text.find("(symbol ", *node_start + 8);
        if (sym_end == std::string::npos) sym_end = text.size();
        if (text.find("Kicad_Forge_ID", *node_start) < static_cast<size_t>(sym_end)) continue;

        // Insertion point: before sub-symbol if any, otherwise before the
        // symbol's closing paren.
        std::string sub_marker = "(symbol " + name + "_0_1";
        auto insert_pos = text.find(sub_marker, *node_start);
        if (insert_pos == std::string::npos || insert_pos >= static_cast<size_t>(sym_end))
            insert_pos = find_matching_paren(text, *node_start);
        if (insert_pos == std::string_view::npos) continue;

        text = insert_property(text, insert_pos, "Kicad_Forge_ID", kf_id);
        patched++;
    }

    if (patched > 0) {
        std::ofstream out(sym_file, std::ios::binary);
        if (out.is_open()) out << text;
    }
    return patched;
}

}  // namespace kforge::sexpr
