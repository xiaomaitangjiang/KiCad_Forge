#include <doctest/doctest.h>
#include "core/io/sexpr/text_util.h"

using kforge::sexpr::replace_property_value;

TEST_CASE("property replacement preserves all text except its value")
{
    std::string text = R"((kicad_symbol_lib (symbol "R" (property "Footprint" "Old" (at 1 2 90) (effects (font (size 2 2)))))))";
    auto expected = text;
    expected.replace(expected.find("Old"), 3, "New");
    REQUIRE(replace_property_value(text, "symbol", "R", "Footprint", "New"));
    CHECK(text == expected);
}

TEST_CASE("property replacement only targets direct children of top-level symbols")
{
    std::string text = R"((kicad_symbol_lib
      (symbol "Parent" (symbol "R" (property "Footprint" "Nested")))
      (symbol "R2" (property "Footprint" "Prefix"))
      (symbol "R" (symbol "R_0_1" (property "Footprint" "Inner")) (property "Footprint" "Target"))))";
    auto expected = text;
    expected.replace(expected.find("Target"), 6, "New");
    REQUIRE(replace_property_value(text, "symbol", "R", "Footprint", "New"));
    CHECK(text == expected);
}

TEST_CASE("property replacement escapes names and values and accepts whitespace")
{
    std::string text = R"((kicad_symbol_lib (symbol
      "R\"1" (property
      "Footprint" "Old" (at 0 0 0)))))";
    REQUIRE(replace_property_value(text, "symbol", "R\"1", "Footprint", "Lib:A\\B\"C"));
    CHECK(text.find(R"("Lib:A\\B\"C")") != std::string::npos);
}

TEST_CASE("property replacement failure leaves malformed or missing input unchanged")
{
    std::string text;
    SUBCASE("missing node") { text = R"((kicad_symbol_lib (symbol "Other")))"; }
    SUBCASE("nested property only") { text = R"((symbol "R" (symbol "Inner" (property "Footprint" "Old"))))"; }
    SUBCASE("unbalanced") { text = R"((symbol "R" (property "Footprint" "Old"))"; }
    SUBCASE("unterminated string") { text = R"((symbol "R" (property "Footprint" "Old)))"; }
    const auto original = text;
    CHECK_FALSE(replace_property_value(text, "symbol", "R", "Footprint", "New"));
    CHECK(text == original);
}

TEST_CASE("paren matching handles escaped backslashes and rejects invalid positions")
{
    const std::string text = R"((symbol "R" (property "Description" "path\\") (property "Footprint" "Old")))";
    CHECK(kforge::sexpr::find_matching_paren(text, 0) == text.size() - 1);
    CHECK(kforge::sexpr::find_matching_paren(text, 1) == std::string::npos);
    CHECK(kforge::sexpr::find_matching_paren(text, text.size()) == std::string::npos);
}

TEST_CASE("property lookup ignores comments and preserves UTF-8 text")
{
    std::string text = R"sexp((kicad_symbol_lib
      # (symbol "电阻" (property "Footprint" "Comment"))
      (symbol "电阻" (property "Description" "(symbol text)")
        (property "Footprint" "旧封装"))))sexp";
    auto expected = text;
    expected.replace(expected.find("旧封装"), std::string("旧封装").size(), "库:新封装");
    REQUIRE(replace_property_value(text, "symbol", "电阻", "Footprint", "库:新封装"));
    CHECK(text == expected);
}
