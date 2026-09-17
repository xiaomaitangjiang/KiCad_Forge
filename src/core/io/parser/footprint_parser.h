#pragma once

#include <filesystem>
#include <string_view>

#include "core/model/footprint.h"
#include "core/io/sexpr/dom/dom.h"
#include "util/error.h"

namespace kforge::parser {

/// Parses KiCad .kicad_mod footprint files (v6/v7/v8 format).
class FootprintParser {
public:
    static util::Result<core::Footprint> parse(
        const std::filesystem::path& path);

    static util::Result<core::Footprint> parse_buffer(std::string_view buffer);

private:
    static void parse_pad(core::Footprint& fp, const sexpr::DomNode& node);
    static void parse_model_3d(core::Footprint& fp, const sexpr::DomNode& node);
};

}  // namespace kforge::parser
