// Tests for Kicad_Forge_ID: determinism, collision resistance, regeneration
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/core/model/symbol.h"

#include <string>

using namespace kforge::core;

TEST_CASE("kf_id determinism")
{
    Symbol a;
    a.set_name("OPA333");
    a.set_footprint("SOT-23-5");
    a.set_default_value("OPA333");
    a.add_pin({"1", "OUT", "output", 0, 0});
    a.add_pin({"2", "V-", "power_in", 0, 0});
    a.add_pin({"3", "IN+", "input", 0, 0});

    std::string h1 = Symbol::compute_hash(a);
    std::string h2 = Symbol::compute_hash(a);
    CHECK_EQ(h1, h2);
    CHECK_FALSE(h1.empty());
    CHECK_EQ(h1.substr(0, 2), "KF");
}

TEST_CASE("kf_id collision resistance")
{
    Symbol a;
    a.set_name("OPA333");
    a.set_footprint("SOT-23-5");
    Symbol b;
    b.set_name("OPA333");
    b.set_footprint("SOIC-8");
    CHECK_NE(Symbol::compute_hash(a), Symbol::compute_hash(b));
}

TEST_CASE("kf_id pin sensitivity")
{
    Symbol a;
    a.set_name("RES");
    a.set_footprint("0402");
    a.add_pin({"1", "A", "passive", 0, 0});
    a.add_pin({"2", "B", "passive", 0, 0});

    Symbol b = a;
    b.add_pin({"3", "C", "passive", 0, 0});
    CHECK_NE(Symbol::compute_hash(a), Symbol::compute_hash(b));
}

TEST_CASE("kf_id metadata sensitivity")
{
    Symbol a;
    a.set_name("IC");
    a.set_description("Op Amp");
    a.set_mpn("LM358");
    Symbol b = a;
    b.set_description("Comparator");
    CHECK_NE(Symbol::compute_hash(a), Symbol::compute_hash(b));
}

TEST_CASE("regeneration moves ID to Pre")
{
    Symbol s;
    s.set_name("TEST");
    s.set_Kicad_Forge_ID("KF_OLD_VALUE");
    s.regenerate_Kicad_Forge_ID();
    CHECK_EQ(s.Pre_Kicad_Forge_ID(), "KF_OLD_VALUE");
    CHECK_NE(s.Kicad_Forge_ID(), "KF_OLD_VALUE");
    CHECK_FALSE(s.Kicad_Forge_ID().empty());
}

TEST_CASE("unique IDs for realistic set")
{
    std::set<std::string> ids;
    const char* names[] = {"OPA333", "LM358", "STM32F103", "NE555", "74HC14"};
    const char* fps[] = {"SOT-23-5", "SOIC-8", "LQFP-48", "DIP-8", "SOIC-14"};
    for (int i = 0; i < 5; i++)
    {
        Symbol s;
        s.set_name(names[i]);
        s.set_footprint(fps[i]);
        s.set_mpn(names[i]);
        s.add_pin({"1", "P1", "passive", 0, 0});
        ids.insert(Symbol::compute_hash(s));
    }
    CHECK_EQ(ids.size(), 5u);
}