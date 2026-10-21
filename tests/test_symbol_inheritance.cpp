#include <doctest/doctest.h>

#include "core/io/parser/symbol_lib_parser.h"

#include <algorithm>
#include <array>
#include <string>

namespace
{
using namespace kforge;

const core::Symbol& inherited_symbol(const core::Library<core::Symbol>& library,
                                     const std::string& name)
{
    const auto found = std::find_if(library.items.begin(), library.items.end(),
                                    [&](const auto& symbol) { return symbol.name() == name; });
    REQUIRE(found != library.items.end());
    return *found;
}
}

TEST_CASE("DERIVED inherits dedicated fields and properties without inheriting identity")
{
    auto parsed = parser::SymbolLibParser::parse_buffer(R"((kicad_symbol_lib
      (symbol "Base"
        (property "Reference" "R") (property "Value" "10k")
        (property "Footprint" "Resistor_SMD:R_0603")
        (property "Datasheet" "https://example.invalid/base.pdf")
        (property "ki_description" "precision resistor") (property "MPN" "BASE-PART")
        (property "Manufacturer" "Example") (property "ki_keywords" "resistor precision")
        (property "Custom" "base custom")
        (property "Kicad_Forge_ID" "base-kf") (property "Pre_Kicad_Forge_ID" "base-pre")
        (symbol "Base_1_1"
          (pin passive line (name "A") (number "1"))
          (pin passive line (name "B") (number "2"))))
      (symbol "Child" (extends "Base"))))");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == 2);
    const auto& child = inherited_symbol(*parsed, "Child");
    CHECK(child.reference_prefix() == "R");
    CHECK(child.property("reference") == "R");
    CHECK(child.default_value() == "10k");
    CHECK(child.footprint() == "Resistor_SMD:R_0603");
    CHECK(child.datasheet() == "https://example.invalid/base.pdf");
    CHECK(child.description() == "precision resistor");
    CHECK(child.mpn() == "BASE-PART");
    CHECK(child.property("manufacturer") == "Example");
    CHECK(child.property("keywords") == "resistor precision");
    CHECK(child.property("Custom") == "base custom");
    CHECK(child.property("extends") == "Base");
    REQUIRE(child.pins().size() == 2);
    CHECK(child.pins()[1].name == "B");
    CHECK(child.Kicad_Forge_ID().empty());
    CHECK(child.Pre_Kicad_Forge_ID().empty());
    CHECK(child.id().empty());
}

TEST_CASE("DERIVED preserves explicit empty fields and child overrides")
{
    auto parsed = parser::SymbolLibParser::parse_buffer(R"((kicad_symbol_lib
      (symbol "Base" (property "Reference" "R") (property "Value" "base value")
        (property "Footprint" "Pkg:Base") (property "Datasheet" "base datasheet")
        (property "Description" "base description") (property "MPN" "base mpn")
        (property "Custom" "base custom"))
      (symbol "Empty" (extends "Base") (property "Reference" "C?")
        (property "Value" "") (property "Footprint" "") (property "Datasheet" "")
        (property "ki_description" "") (property "Part Number" "") (property "Custom" ""))
      (symbol "Override" (extends "Base") (property "Footprint" "Pkg:Child")
        (property "Description" "child description") (property "MPN" "child mpn")
        (property "Kicad_Forge_ID" "child-kf") (property "Pre_Kicad_Forge_ID" "child-pre"))))");
    REQUIRE(parsed.has_value());
    const auto& empty = inherited_symbol(*parsed, "Empty");
    CHECK(empty.reference_prefix() == "C");
    CHECK(empty.property("reference") == "C?");
    CHECK(empty.default_value().empty());
    CHECK(empty.footprint().empty());
    CHECK(empty.datasheet().empty());
    CHECK(empty.description().empty());
    CHECK(empty.mpn().empty());
    CHECK(empty.property("Custom") == "");
    const auto& own = inherited_symbol(*parsed, "Override");
    CHECK(own.default_value() == "base value");
    CHECK(own.footprint() == "Pkg:Child");
    CHECK(own.description() == "child description");
    CHECK(own.mpn() == "child mpn");
    CHECK(own.Kicad_Forge_ID() == "child-kf");
    CHECK(own.Pre_Kicad_Forge_ID() == "child-pre");
}

TEST_CASE("DERIVED resolves multilevel inheritance independently of declaration order")
{
    const std::array<std::string, 3> symbols = {
        R"((symbol "Root" (property "Reference" "U") (property "Value" "root value")
          (property "Footprint" "Pkg:Root") (property "Datasheet" "root datasheet")
          (property "Custom" "root custom")
          (symbol "Root_1_1" (pin input line (name "IN") (number "1")))))",
        R"((symbol "Middle" (extends "Root") (property "Description" "middle description")
          (property "MPN" "middle mpn")))",
        R"((symbol "Leaf" (extends "Middle") (property "Value" "leaf value")))",
    };
    std::array<int, 3> order{0, 1, 2};
    do
    {
        INFO("order=", order[0], ",", order[1], ",", order[2]);
        const auto text = "(kicad_symbol_lib " + symbols[order[0]] + symbols[order[1]]
                          + symbols[order[2]] + ")";
        auto parsed = parser::SymbolLibParser::parse_buffer(text);
        REQUIRE(parsed.has_value());
        const auto& leaf = inherited_symbol(*parsed, "Leaf");
        CHECK(leaf.default_value() == "leaf value");
        CHECK(leaf.footprint() == "Pkg:Root");
        CHECK(leaf.datasheet() == "root datasheet");
        CHECK(leaf.description() == "middle description");
        CHECK(leaf.mpn() == "middle mpn");
        CHECK(leaf.reference_prefix() == "U");
        CHECK(leaf.property("reference") == "U");
        CHECK(leaf.property("Custom") == "root custom");
        CHECK(leaf.property("extends") == "Middle");
        REQUIRE(leaf.pins().size() == 1);
        CHECK(leaf.pins()[0].name == "IN");
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST_CASE("DERIVED inherits the nearest explicit empty override across forward links")
{
    auto parsed = parser::SymbolLibParser::parse_buffer(R"((kicad_symbol_lib
      (symbol "Leaf" (extends "Middle"))
      (symbol "Middle" (extends "Root") (property "Footprint" "")
        (property "Description" "") (property "MPN" ""))
      (symbol "Root" (property "Value" "root") (property "Footprint" "Pkg:Root")
        (property "Description" "root description") (property "MPN" "root mpn")) ))");
    REQUIRE(parsed.has_value());
    const auto& leaf = inherited_symbol(*parsed, "Leaf");
    CHECK(leaf.default_value() == "root");
    CHECK(leaf.footprint().empty());
    CHECK(leaf.description().empty());
    CHECK(leaf.mpn().empty());
}

TEST_CASE("DERIVED rejects missing ambiguous and cyclic parents instead of partial success")
{
    std::string body;
    std::string expected;
    SUBCASE("missing parent")
    {
        body = R"((symbol "Child" (extends "Missing")))";
        expected = "Missing";
    }
    SUBCASE("self inheritance")
    {
        body = R"((symbol "Self" (extends "Self")))";
        expected = "Self";
    }
    SUBCASE("indirect cycle")
    {
        body = R"((symbol "A" (extends "B")) (symbol "B" (extends "C"))
                  (symbol "C" (extends "A")))";
        expected = "A";
    }
    SUBCASE("ambiguous parent")
    {
        body = R"((symbol "Base") (symbol "Base") (symbol "Child" (extends "Base")))";
        expected = "Base";
    }
    auto parsed = parser::SymbolLibParser::parse_buffer("(kicad_symbol_lib " + body + ")");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().format_message().find(expected) != std::string::npos);
}

TEST_CASE("DERIVED resolves a deep forward chain without recursive stack growth")
{
    constexpr int depth = 2048;
    std::string text = "(kicad_symbol_lib ";
    for (int i = 0; i < depth; ++i)
        text += "(symbol \"S" + std::to_string(i) + "\" (extends \"S"
                + std::to_string(i + 1) + "\"))";
    text += "(symbol \"S" + std::to_string(depth)
            + "\" (property \"Reference\" \"U\") (property \"Footprint\" \"Pkg:Deep\")"
              "(symbol \"Unit_1_1\" (pin passive line (name \"P\") (number \"1\")))))";
    auto parsed = parser::SymbolLibParser::parse_buffer(text);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->items.size() == depth + 1);
    const auto& first = inherited_symbol(*parsed, "S0");
    CHECK(first.footprint() == "Pkg:Deep");
    CHECK(first.reference_prefix() == "U");
    CHECK(first.pin_count() == 1);
}

TEST_CASE("DERIVED Reference fallback uses each symbol only after resolving missing fields")
{
    auto parsed = parser::SymbolLibParser::parse_buffer(R"((kicad_symbol_lib
      (symbol "LM358" (extends "TL071"))
      (symbol "TL071" (extends "OPA123"))
      (symbol "OPA123")
      (symbol "123")))");
    REQUIRE(parsed.has_value());
    const auto& leaf = inherited_symbol(*parsed, "LM358");
    const auto& middle = inherited_symbol(*parsed, "TL071");
    const auto& base = inherited_symbol(*parsed, "OPA123");
    CHECK(leaf.reference_prefix() == "LM");
    CHECK(middle.reference_prefix() == "TL");
    CHECK(base.reference_prefix() == "OPA");
    CHECK(inherited_symbol(*parsed, "123").reference_prefix() == "U");
    // Fallback is a local display compatibility value, never a real Reference
    // that descendants or the classifier can inherit as authored input.
    CHECK_FALSE(leaf.property("reference").has_value());
    CHECK_FALSE(middle.property("reference").has_value());
    CHECK_FALSE(base.property("reference").has_value());
}

TEST_CASE("DERIVED Reference normalization follows inherited values and explicit overrides")
{
    auto parsed = parser::SymbolLibParser::parse_buffer(R"((kicad_symbol_lib
      (symbol "LM358" (extends "Middle"))
      (symbol "Own" (extends "Middle") (property "Reference" "Q24?"))
      (symbol "Empty" (extends "Middle") (property "Reference" ""))
      (symbol "Middle" (extends "Base"))
      (symbol "Base" (property "Reference" "R12?"))))");
    REQUIRE(parsed.has_value());
    const auto& leaf = inherited_symbol(*parsed, "LM358");
    CHECK(leaf.reference_prefix() == "R");
    CHECK(leaf.property("reference") == "R12?");
    const auto& own = inherited_symbol(*parsed, "Own");
    CHECK(own.reference_prefix() == "Q");
    CHECK(own.property("reference") == "Q24?");
    const auto& empty = inherited_symbol(*parsed, "Empty");
    CHECK(empty.reference_prefix() == "U");
    CHECK(empty.property("reference") == "");
}
