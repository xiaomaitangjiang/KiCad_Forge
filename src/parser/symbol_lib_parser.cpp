#include "parser/symbol_lib_parser.h"
#include "sexpr/dom.h"
#include "sexpr/dom_builder.h"

#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace kforge::parser {
using Kind = util::Error::Kind;

util::Result<core::Library<core::Symbol>> SymbolLibParser::parse(const std::filesystem::path& path) {
    std::ifstream file(path.string(), std::ios::binary);
    if (!file.is_open())
        return std::unexpected(util::Error::make<Kind::IoError>("Cannot open file: " + path.string()));
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse_buffer(buffer.str(), path.stem().string());
}

util::Result<core::Library<core::Symbol>> SymbolLibParser::parse_buffer(std::string_view buffer,
                                                                        const std::string& lib_name) {
    sexpr::DomBuilder builder;
    auto root_result = builder.parse_one(buffer);
    if (!root_result) return std::unexpected(root_result.error());

    auto& root = *root_result;
    if (root->type() != "kicad_symbol_lib")
        return std::unexpected(util::Error::make<Kind::ParseError>(
            std::string("expected (kicad_symbol_lib ...), got (") + std::string(root->type()) + ")"));

    core::Library<core::Symbol> lib;
    lib.name = lib_name;
    lib.file_path = lib_name + ".kicad_sym";

    // Parse each (symbol ...) child — tolerate individual failures
    for (const auto& child : root->children()) {
        if (child->type() != "symbol") continue;
        auto sym_result = parse_symbol(*child);
        if (!sym_result) continue;  // skip broken symbol, keep going
        lib.items.push_back(std::move(*sym_result));
    }

    // Resolve extends within the file
    resolve_extends(lib);
    return lib;
}

// ---- Node dispatcher for symbol children ----

using NodeHandler = std::function<void(core::Symbol&, const sexpr::DomNode&)>;
using HandlerMap = std::unordered_map<std::string_view, NodeHandler>;

static void dispatch_children(core::Symbol& sym, const sexpr::DomNode& node,
                               const HandlerMap& handlers) {
    for (const auto& child : node.children()) {
        auto it = handlers.find(child->type());
        if (it != handlers.end()) it->second(sym, *child);
    }
}

static void handle_extends(core::Symbol& sym, const sexpr::DomNode& node) {
    if (node.is_atom()) sym.set_property("extends", std::string(node.atom_value()));
}

util::Result<core::Symbol> SymbolLibParser::parse_symbol(const sexpr::DomNode& node) {
    core::Symbol sym;
    if (node.is_atom()) sym.set_name(std::string(node.atom_value()));

    // Build handler registry — add new node types here
    HandlerMap handlers;
    handlers["property"] = [](core::Symbol& s, const sexpr::DomNode& n) { parse_property(s, n); };
    handlers["pin"]      = [](core::Symbol& s, const sexpr::DomNode& n) { parse_pin(s, n); };
    handlers["extends"]  = handle_extends;

    // Recursive dispatch — nested (symbol ...) re-enters
    handlers["symbol"] = [&handlers](core::Symbol& s, const sexpr::DomNode& n) {
        dispatch_children(s, n, handlers);
    };

    dispatch_children(sym, node, handlers);

    // Set reference prefix from name
    if (!sym.name().empty()) {
        std::string prefix;
        for (char c : sym.name()) {
            if (std::isalpha(static_cast<unsigned char>(c))) prefix += c;
            else break;
        }
        if (!prefix.empty()) sym.set_reference_prefix(prefix);
    }
    return sym;
}

void SymbolLibParser::resolve_extends(core::Library<core::Symbol>& lib) {
    std::unordered_map<std::string, size_t> name_index;
    for (size_t i = 0; i < lib.items.size(); i++)
        name_index[lib.items[i].name()] = i;

    for (auto& sym : lib.items) {
        auto ext = sym.property("extends");
        if (!ext) continue;
        auto it = name_index.find(*ext);
        if (it == name_index.end()) continue;

        auto& base = lib.items[it->second];
        std::unordered_set<std::string> pin_nums;
        for (auto& p : sym.pins()) pin_nums.insert(p.number);
        for (auto& p : base.pins())
            if (!pin_nums.count(p.number)) sym.add_pin(p);
        for (auto& [key, val] : base.properties())
            if (!sym.property(key)) sym.set_property(key, val);
    }
}

// ---- Property & pin parsing (unchanged) ----

void SymbolLibParser::parse_property(core::Symbol& sym, const sexpr::DomNode& node) {
    for (const auto& [k, v] : node.properties()) {
        std::string key(k), value(v);
        if (key == "Reference") {
            std::string ref = value;
            while (!ref.empty() && (ref.back() == '?' || std::isdigit(ref.back())))
                ref.pop_back();
            if (!ref.empty()) sym.set_reference_prefix(ref);
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
    if (node.is_atom()) pin.number = std::string(node.atom_value());

    for (const auto& child : node.children()) {
        std::string child_type = std::string(child->type());
        auto colon = child_type.find(':');
        if (colon != std::string::npos) child_type = child_type.substr(colon + 1);

        if (child_type == "name") {
            if (child->is_atom()) pin.name = std::string(child->atom_value());
            for (const auto& [k, v] : child->properties()) { pin.name = std::string(v); break; }
        } else if (child_type == "type") {
            if (child->is_atom()) pin.electrical_type = std::string(child->atom_value());
            for (const auto& [k, v] : child->properties()) { pin.electrical_type = std::string(v); break; }
        }
    }
    if (!pin.number.empty()) sym.add_pin(std::move(pin));
}

}  // namespace kforge::parser
