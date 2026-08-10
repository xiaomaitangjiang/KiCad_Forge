// Tests for Kicad_Forge_ID: determinism, collision resistance, regeneration
#include "../src/core/symbol.h"
#include <cstdio>
#include <set>
#include <string>

using namespace kforge::core;

static int passed = 0, failed = 0;
void ok() { printf("OK\n"); passed++; }
void fail(const char* msg) { printf("FAIL: %s\n", msg); failed++; }

int main() {
    printf("=== Symbol Hash Tests ===\n");

    // 1. Determinism: same input → same hash
    printf("  determinism ... ");
    {
        Symbol a; a.set_name("OPA333"); a.set_footprint("SOT-23-5"); a.set_default_value("OPA333");
        a.add_pin({"1", "OUT", "output", 0, 0});
        a.add_pin({"2", "V-", "power_in", 0, 0});
        a.add_pin({"3", "IN+", "input", 0, 0});

        std::string h1 = Symbol::compute_hash(a);
        std::string h2 = Symbol::compute_hash(a);
        if (h1 == h2 && !h1.empty() && h1.substr(0,2) == "KF") ok();
        else fail((h1 + " != " + h2).c_str());
    }

    // 2. Different input → different hash
    printf("  collision resistance ... ");
    {
        Symbol a; a.set_name("OPA333"); a.set_footprint("SOT-23-5");
        Symbol b; b.set_name("OPA333"); b.set_footprint("SOIC-8");
        std::string ha = Symbol::compute_hash(a);
        std::string hb = Symbol::compute_hash(b);
        if (ha != hb) ok(); else fail((ha + " == " + hb).c_str());
    }

    // 3. Pin change → hash change
    printf("  pin sensitivity ... ");
    {
        Symbol a; a.set_name("RES"); a.set_footprint("0402");
        a.add_pin({"1", "A", "passive", 0, 0});
        a.add_pin({"2", "B", "passive", 0, 0});

        Symbol b = a;
        b.add_pin({"3", "C", "passive", 0, 0});
        if (Symbol::compute_hash(a) != Symbol::compute_hash(b)) ok();
        else fail("identical hash despite different pin count");
    }

    // 4. Metadata change → hash change
    printf("  metadata sensitivity ... ");
    {
        Symbol a; a.set_name("IC"); a.set_description("Op Amp"); a.set_mpn("LM358");
        Symbol b = a; b.set_description("Comparator");
        if (Symbol::compute_hash(a) != Symbol::compute_hash(b)) ok();
        else fail("description change not reflected");
    }

    // 5. regenerate_Kicad_Forge_ID moves current → Pre
    printf("  regeneration moves ID to Pre ... ");
    {
        Symbol s; s.set_name("TEST");
        s.set_Kicad_Forge_ID("KF_OLD_VALUE");
        s.regenerate_Kicad_Forge_ID();
        if (s.Pre_Kicad_Forge_ID() == "KF_OLD_VALUE"
            && s.Kicad_Forge_ID() != "KF_OLD_VALUE"
            && !s.Kicad_Forge_ID().empty()) ok();
        else fail(("Pre=" + s.Pre_Kicad_Forge_ID() + " Cur=" + s.Kicad_Forge_ID()).c_str());
    }

    // 6. Unique IDs for realistic set
    printf("  uniqueness ... ");
    {
        std::set<std::string> ids;
        const char* names[] = {"OPA333","LM358","STM32F103","NE555","74HC14"};
        const char* fps[]   = {"SOT-23-5","SOIC-8","LQFP-48","DIP-8","SOIC-14"};
        for (int i = 0; i < 5; i++) {
            Symbol s; s.set_name(names[i]); s.set_footprint(fps[i]); s.set_mpn(names[i]);
            s.add_pin({"1","P1","passive",0,0});
            ids.insert(Symbol::compute_hash(s));
        }
        if (ids.size() == 5) ok();
        else fail(("only " + std::to_string(ids.size()) + " unique").c_str());
    }

    printf("\n=== %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
