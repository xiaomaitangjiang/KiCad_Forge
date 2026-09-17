#pragma once

#include <filesystem>
#include <string_view>

#include "core/model/symbol.h"
#include "core/model/types.h"
#include "core/io/sexpr/dom/dom.h"
#include "util/error.h"

namespace kforge::parser {

/// Parses KiCad .kicad_sym symbol library files (v6/v7/v8 format).
class SymbolLibParser {
public:
    static util::Result<core::Library<core::Symbol>> parse(
        const std::filesystem::path& path);

    static util::Result<core::Library<core::Symbol>> parse_buffer(
        std::string_view buffer, const std::string& lib_name = "imported");

private:
    static util::Result<core::Symbol> parse_symbol(const sexpr::DomNode& node);
    static void parse_property(core::Symbol& sym, const sexpr::DomNode& node);
    static void parse_pin(core::Symbol& sym, const sexpr::DomNode& node);
    static void resolve_extends(core::Library<core::Symbol>& lib);
};

}  // namespace kforge::parser
