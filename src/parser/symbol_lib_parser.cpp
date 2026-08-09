#include "parser/symbol_lib_parser.h"

#include <fstream>
#include <sstream>
#include <functional>

#include "sexpr/dom.h"
#include "sexpr/dom_builder.h"

namespace kforge::parser {
using Kind = util::Error::Kind;

util::Result<core::Library<core::Symbol>> SymbolLibParser::parse(
    const std::filesystem::path& path) {
    // Read file
    std::ifstream file(path.string(), std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected(
            util::Error::make<Kind::IoError>("Cannot open file: " + path.string()));
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    return parse_buffer(content, path.stem().string());
}

util::Result<core::Library<core::Symbol>> SymbolLibParser::parse_buffer(
    std::string_view buffer, const std::string& lib_name) {
    sexpr::DomBuilder builder;

    // Parse the top-level (kicad_symbol_lib ...) form
    auto root_result = builder.parse_one(buffer);
    if (!root_result) return std::unexpected(root_result.error());

    auto& root = *root_result;

    // Verify it's a symbol library
    if (root->type() != "kicad_symbol_lib") {
        return std::unexpected(util::Error::make<Kind::ParseError>(
            std::string("expected (kicad_symbol_lib ...), got (") +
                std::string(root->type()) + ")"));
    }

    core::Library<core::Symbol> lib;
    lib.name = lib_name;
    lib.file_path = lib_name + ".kicad_sym";

    // Parse each (symbol ...) child
    for (const auto& child : root->children()) {
        if (child->type() == "symbol") {
            auto sym_result = parse_symbol(*child);
            if (!sym_result) return std::unexpected(sym_result.error());
            lib.items.push_back(std::move(*sym_result));
        }
    }

    return lib;
}

util::Result<core::Symbol> SymbolLibParser::parse_symbol(
    const sexpr::DomNode& node) {
    core::Symbol sym;

    // Symbol name is stored as the node's atom value
    if (node.is_atom()) {
        sym.set_name(std::string(node.atom_value()));
    }

    // Parse children: (property ...) and (pin ...) nodes.
    // Pins may be nested inside a sub-(symbol ...) — recurse to find them.
    std::function<void(const sexpr::DomNode&)> collect = [&](const sexpr::DomNode& n) {
        for (const auto& child : n.children()) {
            if (child->type() == "property") {
                parse_property(sym, *child);
            } else if (child->type() == "pin") {
                parse_pin(sym, *child);
            } else if (child->type() == "symbol") {
                collect(*child);  // Recurse into nested symbol for pins
            }
        }
    };
    collect(node);

    // Set reference prefix from name (default: first letter)
    if (!sym.name().empty()) {
        std::string prefix;
        for (char c : sym.name()) {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                prefix += c;
            } else {
                break;
            }
        }
        if (!prefix.empty()) sym.set_reference_prefix(prefix);
    }

    return sym;
}

void SymbolLibParser::parse_property(core::Symbol& sym,
                                     const sexpr::DomNode& node) {
    for (const auto& [k, v] : node.properties()) {
        // Copy out of the zero-copy DOM into owned strings
        std::string key(k);
        std::string value(v);
        if (key == "Reference") {
            std::string ref = value;
            while (!ref.empty() && (ref.back() == '?' || std::isdigit(ref.back())))
            {
                ref.pop_back();
            }
            if (!ref.empty()) { sym.set_reference_prefix(ref); }
            sym.set_property("reference", value);
        } else if (key == "Value") {
            sym.set_default_value(value);
        } else if (key == "Footprint") {
            sym.set_footprint(value);
        } else if (key == "Datasheet") {
            sym.set_datasheet(value);
        } else if (key == "Description" || key == "ki_description") {
            sym.set_description(value);
        } else if (key == "Manufacturer") {
            sym.set_property("manufacturer", value);
        } else if (key == "MPN" || key == "Part Number") {
            sym.set_mpn(value);
        } else if (key == "LCSC" || key == "LCSC Part") {
            sym.set_property("lcsc_id", value);
        } else if (key == "Kicad_Forge_ID") {
            sym.set_Kicad_Forge_ID(value);
        } else if (key == "Pre_Kicad_Forge_ID") {
            sym.set_Pre_Kicad_Forge_ID(value);
        } else if (key == "ki_keywords") {
            sym.set_property("keywords", value);
        } else {
            sym.set_property(key, value);
        }
    }
}

void SymbolLibParser::parse_pin(core::Symbol& sym, const sexpr::DomNode& node) {
    core::PinDefinition pin;

    // Pin number is the node's atom value (set by DomBuilder::parse_node for "pin" type)
    if (node.is_atom()) { pin.number = std::string(node.atom_value()); }

    // Find name and type from child sub-nodes and properties
    for (const auto& child : node.children()) {
        // Child sub-nodes like (name "~") store their value as property
        std::string child_type = std::string(child->type());
        // Strip any prefix like "1:name" -> "name"
        auto colon = child_type.find(':');
        if (colon != std::string::npos) child_type = child_type.substr(colon + 1);

        if (child_type == "name") {
            // Get the first atom/property as the name value
            if (child->is_atom()) { pin.name = std::string(child->atom_value()); }
            for (const auto& [k, v] : child->properties()) { pin.name = std::string(v); break; }
        } else if (child_type == "type") {
            if (child->is_atom()) { pin.electrical_type = std::string(child->atom_value()); }
            for (const auto& [k, v] : child->properties()) { pin.electrical_type = std::string(v); break; }
        }
    }

    if (!pin.number.empty()) {
        sym.add_pin(std::move(pin));
    }
}

}  // namespace kforge::parser
