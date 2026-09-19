// S-expression text-level utilities — shared by delete_symbol, patch_kf_id_to_file, merge_into_library
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kforge::sexpr {

/// Find opening '(' of (type "name" in s-expr text
std::optional<size_t> find_node(std::string_view text, std::string_view type,
                                std::string_view name);

/// Given an open-paren position, find the matching closing ')'
/// Handles quoted strings and nested parens. Returns npos on failure.
size_t find_matching_paren(std::string_view text, size_t open_paren);

/// Insert (property key value ...) before position `before_pos` in text.
/// Property is formatted in KiCad S-expression style with hide yes and font settings.
std::string insert_property(std::string_view text, size_t before_pos, std::string_view key,
                            std::string_view value);

/// Remove a subtree: text[start..end) inclusive
std::string remove_node(std::string_view text, size_t start, size_t end);

/// Replace the value of (property "KEY" ...) inside the (type "name" ...) node.
/// Only replaces the direct property's value token; preserves all other text.
/// Returns false on invalid input or a missing node/property.
bool replace_property_value(std::string& text, std::string_view node_type,
                            std::string_view node_name, std::string_view prop_key,
                            std::string_view new_value);

/// Patch (property "Kicad_Forge_ID" "...") into each symbol node listed in
/// name_to_kf_id. Skips symbols that already have the property. Returns the
/// number of patched symbols. Writes to the file only when at least one symbol
/// was patched.
int patch_kf_id_to_file(
    const std::filesystem::path& sym_file,
    const std::unordered_map<std::string, std::string>& name_to_kf_id);

}  // namespace kforge::sexpr
