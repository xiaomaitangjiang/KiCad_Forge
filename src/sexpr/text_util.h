// S-expression text-level utilities — shared by delete_symbol, patch_kf_id_to_file, merge_into_library
#pragma once
#include <optional>
#include <string>
#include <string_view>

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

}  // namespace kforge::sexpr
