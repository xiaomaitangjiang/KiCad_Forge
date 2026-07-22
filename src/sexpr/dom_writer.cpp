#include "sexpr/dom_writer.h"

#include <algorithm>

namespace kforge::sexpr {

std::string DomWriter::indent_str(int depth, int indent_size) {
    return std::string(static_cast<size_t>(depth * indent_size), ' ');
}

std::string DomWriter::escape_string(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    out += '"';
    return out;
}

void DomWriter::write_node(std::string& out, const DomNode& node,
                           int depth, int indent) {
    if (depth > 0) {
        out += indent_str(depth, indent);
    }

    out += '(' + node.type();

    // Properties: write as (key value) on the same line or next
    bool first = true;
    for (const auto& [key, value] : node.properties()) {
        if (!first) {
            // Multi-property nodes: each property on its own line
            out += '\n';
            out += indent_str(depth + 1, indent);
        }
        out += ' ' + key;
        if (!value.empty()) {
            out += ' ';
            // Heuristic: if value looks like a KiCAD identifier, write as atom
            bool is_atom = std::all_of(value.begin(), value.end(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) ||
                       c == '_' || c == '-' || c == '.' || c == '/' || c == ':';
            });
            if (is_atom) {
                out += value;
            } else {
                out += escape_string(value);
            }
        }
        first = false;
    }

    // Children
    if (!node.children().empty()) {
        if (!node.properties().empty()) {
            out += '\n';
        }
        for (const auto& child : node.children()) {
            out += '\n';
            write_node(out, *child, depth + 1, indent);
        }
        out += '\n';
        out += indent_str(depth, indent);
    } else if (!node.properties().empty()) {
        // Single property — keep it tight
    }

    out += ')';
}

std::string DomWriter::write(const DomNode& root, int indent) {
    std::string out;
    // If root is a container (type == "__root__"), just write children
    if (root.type() == "__root__") {
        bool first = true;
        for (const auto& child : root.children()) {
            if (!first) out += "\n\n";
            write_node(out, *child, 0, indent);
            first = false;
        }
    } else {
        write_node(out, root, 0, indent);
    }
    out += '\n';
    return out;
}

std::string DomWriter::write_compact(const DomNode& root) {
    // For compact mode, just call write_node without indentation
    std::string out;
    write_node(out, root, 0, 0);
    return out;
}

void DomWriter::write_node_compact(std::string& out, const DomNode& node) {
    out += '(' + node.type();
    for (const auto& [key, value] : node.properties()) {
        out += ' ' + key;
        if (!value.empty()) out += ' ' + value;
    }
    for (const auto& child : node.children()) {
        out += ' ';
        write_node_compact(out, *child);
    }
    out += ')';
}

}  // namespace kforge::sexpr
