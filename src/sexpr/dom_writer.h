#pragma once

#include <string>

#include "sexpr/dom.h"

namespace kforge::sexpr {

/// Writes a DomNode tree back to formatted S-expression text.
class DomWriter {
public:
    /// Write with pretty-printing (indented, one property per line).
    static std::string write(const DomNode& root, int indent = 2);

    /// Write compact (single line, for small expressions).
    static std::string write_compact(const DomNode& root);

private:
    static void write_node(std::string& out, const DomNode& node,
                           int depth, int indent);
    static void write_node_compact(std::string& out, const DomNode& node);
    static std::string escape_string(std::string_view s);
    static std::string indent_str(int depth, int indent_size);
};

}  // namespace kforge::sexpr
