#pragma once

#include <memory>
#include <string_view>

#include "sexpr/dom.h"
#include "sexpr/tokenizer.h"
#include "util/result.h"

namespace kforge::sexpr {

/// Builds a DomNode tree from a stream of tokens.
///
/// Recursive descent: each '(' starts a new node, ')' closes it.
/// Atoms after the type name become property-value pairs.
class DomBuilder {
public:
    DomBuilder() = default;

    /// Parse a complete source string into a DomNode tree.
    /// Returns the root node (type = "__root__", children = top-level forms).
    util::Result<std::unique_ptr<DomNode>> build(std::string_view input);

    /// Parse a single top-level form from a string.
    /// The returned node's type is the first atom of the form.
    util::Result<std::unique_ptr<DomNode>> parse_one(std::string_view input);

private:
    util::Result<std::unique_ptr<DomNode>> parse_node(Tokenizer& tz);
    util::Result<void> parse_children(Tokenizer& tz, DomNode& node);
};

}  // namespace kforge::sexpr
