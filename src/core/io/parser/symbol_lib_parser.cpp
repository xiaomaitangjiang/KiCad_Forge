#include "core/io/parser/symbol_lib_parser.h"
#include "core/io/sexpr/dom/dom.h"
#include "core/io/sexpr/dom/dom_builder.h"

#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace kforge::parser
{
using Kind = util::Error::Kind;

namespace
{
// Dedicated Symbol fields do not live in properties(). Keep presence only while
// parsing so an explicit empty override remains distinct from a missing field.
enum DeclaredField : std::uint8_t
{
    ValueField = 1 << 0,
    FootprintField = 1 << 1,
    DatasheetField = 1 << 2,
    DescriptionField = 1 << 3,
    MpnField = 1 << 4,
};
}

util::Result<core::Library<core::Symbol>> SymbolLibParser::parse(const std::filesystem::path& path)
{
    // 使用 path 重载，让 Windows 标准库按原生宽路径打开；转成窄串会让
    // CRT 把 UTF-8 文件名按当前代码页解释，中文路径因此无法打开。
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        const auto utf8 = path.u8string();
        return std::unexpected(util::Error::make<Kind::IoError>(
            "Cannot open file: " + std::string(utf8.begin(), utf8.end())));
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto stem = path.stem().u8string();
    return parse_buffer(buffer.str(), std::string(stem.begin(), stem.end()));
}

util::Result<core::Library<core::Symbol>> SymbolLibParser::parse_buffer(std::string_view buffer,
                                                                        const std::string& lib_name)
{
    sexpr::DomBuilder builder;
    auto root_result = builder.parse_one(buffer);
    if (!root_result)
        return std::unexpected(root_result.error());

    auto& root = *root_result;
    if (root->type() != "kicad_symbol_lib")
        return std::unexpected(util::Error::make<Kind::ParseError>(
            std::string("expected (kicad_symbol_lib ...), got (") + std::string(root->type()) + ")"));

    core::Library<core::Symbol> lib;
    lib.name = lib_name;
    lib.file_path = lib_name + ".kicad_sym";

    std::vector<std::uint8_t> declared_fields;
    declared_fields.reserve(root->children().size());

    // Parse each (symbol ...) child — tolerate individual failures
    for (const auto& child : root->children())
    {
        if (child->type() != "symbol")
            continue;
        std::uint8_t fields = 0;
        auto sym_result = parse_symbol(*child, fields);
        if (!sym_result)
            continue;  // skip broken symbol, keep going
        lib.items.push_back(std::move(*sym_result));
        declared_fields.push_back(fields);
    }

    // Resolve extends within the file
    if (auto resolved = resolve_extends(lib, declared_fields); !resolved)
        return std::unexpected(resolved.error());

    // 先继承真实 Reference，最后统一生成前缀。缺失属性时的名称兜底只用于
    // 当前符号，不能成为后代的继承输入，也不写成真实 reference 属性。
    for (auto& sym : lib.items)
    {
        std::string prefix;
        if (const auto reference = sym.property("reference"))
        {
            prefix = *reference;
            while (!prefix.empty() &&
                   (prefix.back() == '?' ||
                    std::isdigit(static_cast<unsigned char>(prefix.back()))))
                prefix.pop_back();
        }
        else
        {
            for (char c : sym.name())
            {
                if (!std::isalpha(static_cast<unsigned char>(c)))
                    break;
                prefix += c;
            }
        }
        if (!prefix.empty())
            sym.set_reference_prefix(std::move(prefix));
    }
    return lib;
}

// ---- Node dispatcher for symbol children ----

using NodeHandler = std::function<void(core::Symbol&, const sexpr::DomNode&)>;
using HandlerMap = std::unordered_map<std::string_view, NodeHandler>;

static void dispatch_children(core::Symbol& sym, const sexpr::DomNode& node,
                              const HandlerMap& handlers)
{
    for (const auto& child : node.children())
    {
        auto it = handlers.find(child->type());
        if (it != handlers.end())
            it->second(sym, *child);
    }
}

static void handle_extends(core::Symbol& sym, const sexpr::DomNode& node)
{
    if (node.is_atom())
        sym.set_property("extends", std::string(node.atom_value()));
}

util::Result<core::Symbol> SymbolLibParser::parse_symbol(const sexpr::DomNode& node,
                                                        std::uint8_t& declared_fields)
{
    core::Symbol sym;
    if (node.is_atom())
        sym.set_name(std::string(node.atom_value()));

    // Build handler registry — add new node types here
    HandlerMap handlers;
    handlers["property"] = [&declared_fields](core::Symbol& s, const sexpr::DomNode& n)
    {
        parse_property(s, n, declared_fields);
    };
    handlers["pin"] = [](core::Symbol& s, const sexpr::DomNode& n)
    {
        parse_pin(s, n);
    };
    handlers["extends"] = handle_extends;

    // Recursive dispatch — nested (symbol ...) re-enters
    handlers["symbol"] = [&handlers](core::Symbol& s, const sexpr::DomNode& n)
    {
        dispatch_children(s, n, handlers);
    };

    dispatch_children(sym, node, handlers);
    return sym;
}

util::Result<void> SymbolLibParser::resolve_extends(
    core::Library<core::Symbol>& lib, const std::vector<std::uint8_t>& declared_fields)
{
    std::unordered_map<std::string, size_t> name_index;
    name_index.reserve(lib.items.size());
    for (size_t i = 0; i < lib.items.size(); i++)
        if (!name_index.emplace(lib.items[i].name(), i).second)
            return std::unexpected(util::Error::make<Kind::ParseError>(
                "Duplicate symbol name: " + lib.items[i].name()));

    // Iterative post-order traversal: each parent is fully resolved before its
    // children, independent of file order, without recursion on untrusted depth.
    enum class State : std::uint8_t { Unvisited, Visiting, Resolved };
    std::vector<State> states(lib.items.size(), State::Unvisited);
    std::vector<size_t> chain;
    for (size_t start = 0; start < lib.items.size(); ++start)
    {
        if (states[start] == State::Resolved)
            continue;

        size_t current = start;
        while (states[current] != State::Resolved)
        {
            auto& sym = lib.items[current];
            if (states[current] == State::Visiting)
                return std::unexpected(util::Error::make<Kind::ParseError>(
                    "Cyclic symbol inheritance at: " + sym.name()));
            states[current] = State::Visiting;
            chain.push_back(current);
            const auto parent = sym.property("extends");
            if (!parent)
                break;
            const auto found = name_index.find(*parent);
            if (found == name_index.end())
                return std::unexpected(util::Error::make<Kind::ParseError>(
                    "Symbol \"" + sym.name() + "\" extends missing symbol \"" + *parent + "\""));
            current = found->second;
        }

        while (!chain.empty())
        {
            const auto index = chain.back();
            chain.pop_back();
            auto& sym = lib.items[index];
            if (const auto parent = sym.property("extends"))
            {
                const auto& base = lib.items[name_index.at(*parent)];
                if (sym.pins().empty())
                    sym.pins() = base.pins();
                else
                {
                    // Preserve the existing behavior for locally declared pins.
                    std::unordered_set<std::string> own_pin_numbers;
                    for (const auto& pin : sym.pins())
                        own_pin_numbers.insert(pin.number);
                    for (const auto& pin : base.pins())
                        if (!own_pin_numbers.count(pin.number))
                            sym.add_pin(pin);
                }

                const auto fields = declared_fields[index];
                if (!(fields & ValueField)) sym.set_default_value(base.default_value());
                if (!(fields & FootprintField)) sym.set_footprint(base.footprint());
                if (!(fields & DatasheetField)) sym.set_datasheet(base.datasheet());
                if (!(fields & DescriptionField)) sym.set_description(base.description());
                if (!(fields & MpnField)) sym.set_mpn(base.mpn());

                for (const auto& [key, value] : base.properties())
                    if (key != "extends" && !sym.property(key))
                        sym.set_property(key, value);
                // UUID and KF/Pre identities belong to the child, never the base.
            }
            states[index] = State::Resolved;
        }
    }
    return {};
}

// ---- Property & pin parsing ----

void SymbolLibParser::parse_property(core::Symbol& sym, const sexpr::DomNode& node,
                                     std::uint8_t& declared_fields)
{
    for (const auto& [k, v] : node.properties())
    {
        std::string key(k), value(v);
        if (key == "Reference")
        {
            sym.set_property("reference", value);
        }
        else if (key == "Value")
        {
            declared_fields |= ValueField;
            sym.set_default_value(value);
        }
        else if (key == "Footprint")
        {
            declared_fields |= FootprintField;
            sym.set_footprint(value);
        }
        else if (key == "Datasheet")
        {
            declared_fields |= DatasheetField;
            sym.set_datasheet(value);
        }
        else if (key == "Description" || key == "ki_description")
        {
            declared_fields |= DescriptionField;
            sym.set_description(value);
        }
        else if (key == "Manufacturer")
        {
            sym.set_property("manufacturer", value);
        }
        else if (key == "MPN" || key == "Part Number")
        {
            declared_fields |= MpnField;
            sym.set_mpn(value);
        }
        else if (key == "LCSC" || key == "LCSC Part")
        {
            sym.set_property("lcsc_id", value);
        }
        else if (key == "Kicad_Forge_ID")
        {
            sym.set_Kicad_Forge_ID(value);
        }
        else if (key == "Pre_Kicad_Forge_ID")
        {
            sym.set_Pre_Kicad_Forge_ID(value);
        }
        else if (key == "ki_keywords")
        {
            sym.set_property("keywords", value);
        }
        else
        {
            sym.set_property(key, value);
        }
    }
}

/// KiCad 的引脚电气类型关键字。(pin input line ...) 的第一个 atom 是它们之一
static bool is_electrical_type_keyword(std::string_view text)
{
    static constexpr std::string_view kKeywords[] = {
        "input",        "output",     "bidirectional", "tri_state",
        "passive",      "free",       "unspecified",   "power_in",
        "power_out",    "open_collector", "open_emitter", "no_connect",
    };
    for (auto keyword : kKeywords)
    {
        if (text == keyword)
            return true;
    }
    return false;
}

void SymbolLibParser::parse_pin(core::Symbol& sym, const sexpr::DomNode& node)
{
    core::PinDefinition pin;
    // 标准 KiCad 引脚：第一个 atom 是电气类型（(pin input line ...)）；
    // 旧简化格式把它当编号，所以先记下来，按内容决定归到哪一边。
    std::string first_atom;
    if (node.is_atom())
        first_atom = std::string(node.atom_value());

    for (const auto& child : node.children())
    {
        std::string child_type = std::string(child->type());
        auto colon = child_type.find(':');
        if (colon != std::string::npos)
            child_type = child_type.substr(colon + 1);

        if (child_type == "name")
        {
            if (child->is_atom())
                pin.name = std::string(child->atom_value());
            for (const auto& [k, v] : child->properties())
            {
                pin.name = std::string(v);
                break;
            }
        }
        else if (child_type == "number")
        {
            // 编号只认 (number ...)；不能再拿第一个 atom 顶替
            if (child->is_atom())
                pin.number = std::string(child->atom_value());
            for (const auto& [k, v] : child->properties())
            {
                pin.number = std::string(v);
                break;
            }
        }
        else if (child_type == "type")
        {
            if (child->is_atom())
                pin.electrical_type = std::string(child->atom_value());
            for (const auto& [k, v] : child->properties())
            {
                pin.electrical_type = std::string(v);
                break;
            }
        }
    }

    if (is_electrical_type_keyword(first_atom))
    {
        if (pin.electrical_type.empty())
            pin.electrical_type = first_atom;
    }
    else if (pin.number.empty())
    {
        pin.number = std::move(first_atom);  // 旧格式：第一个 atom 就是编号
    }

    if (!pin.number.empty())
        sym.add_pin(std::move(pin));
}

}  // namespace kforge::parser
