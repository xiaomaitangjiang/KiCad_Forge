#include <doctest/doctest.h>

#include "core/db/database.h"
#include "core/io/parser/footprint_parser.h"
#include "core/repo/repositories.h"
#include "interface/service/correspondence/service.h"
#include "support/test_environment.h"

using namespace kforge;

TEST_CASE("SDKQUERY search combines text and library filters before returning rows")
{
    test_support::TempDirectory temp;
    auto opened = storage::Database::open(temp.path() / "queries.db");
    REQUIRE(opened.has_value());
    auto db = std::move(*opened);
    auto session = db->session();
    storage::LibraryRepository libraries(session);
    core::LibraryMeta a, b;
    a.name = "QueryA";
    b.name = "QueryB";
    auto first = libraries.insert(a);
    auto second = libraries.insert(b);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    storage::SymbolRepository symbols(session);
    for (const auto& library : {*first, *second})
    {
        core::Symbol symbol;
        symbol.set_name("SamePart");
        symbol.set_library_id(library.id);
        REQUIRE(symbols.insert(symbol).has_value());
    }
    auto all = symbols.search("SamePart");
    REQUIRE(all.has_value());
    CHECK(all->size() == 2);
    auto filtered = symbols.search("SamePart", first->id);
    REQUIRE(filtered.has_value());
    REQUIRE(filtered->size() == 1);
    CHECK(filtered->front().library_id() == first->id);
    auto absent = symbols.search("SamePart", "unknown-library");
    REQUIRE(absent.has_value());
    CHECK(absent->empty());
}

TEST_CASE("SDKQUERY single-symbol matches preserve not-found and database failures")
{
    test_support::TempDirectory temp;
    auto opened = storage::Database::open(temp.path() / "errors.db");
    REQUIRE(opened.has_value());
    auto db = std::move(*opened);
    services::CorrespondenceService service(db.get());
    auto missing = service.suggest_matches_for("missing-symbol", 10);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().kind() == util::Error::Kind::NotFound);
    std::string symbol_id;
    {
        auto session = db->session();
        core::LibraryMeta library;
        library.name = "QueryErrors";
        auto inserted = storage::LibraryRepository(session).insert(library);
        REQUIRE(inserted.has_value());
        core::Symbol symbol;
        symbol.set_name("Present");
        symbol.set_library_id(inserted->id);
        auto row = storage::SymbolRepository(session).insert(symbol);
        REQUIRE(row.has_value());
        symbol_id = row->id();
    }
    REQUIRE(db->execute("DROP TABLE footprints").has_value());
    auto failed = service.suggest_matches_for(symbol_id, 10);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().kind() == util::Error::Kind::DbError);
}

TEST_CASE("SDKQUERY single-symbol lock lookup preserves false true and failures")
{
    test_support::TempDirectory temp;
    auto opened = storage::Database::open(temp.path() / "locked.db");
    REQUIRE(opened.has_value());
    auto db = std::move(*opened);
    auto session = db->session();
    storage::ComponentLibraryRepository components(session);
    storage::ComponentLibraryRepository::ComponentLibrary group;
    group.name = "QueryLocks";
    auto added = components.insert(group);
    REQUIRE(added.has_value());
    core::LibraryMeta library;
    library.name = "QueryLocks";
    library.component_library_id = added->id;
    auto inserted = storage::LibraryRepository(session).insert(library);
    REQUIRE(inserted.has_value());
    core::Symbol symbol;
    symbol.set_name("LockState");
    symbol.set_library_id(inserted->id);
    storage::SymbolRepository symbols(session);
    auto row = symbols.insert(symbol);
    REQUIRE(row.has_value());
    auto unlocked = symbols.is_library_locked(row->id());
    REQUIRE(unlocked.has_value());
    CHECK_FALSE(*unlocked);
    added->locked = true;
    REQUIRE(components.update(*added).has_value());
    auto locked = symbols.is_library_locked(row->id());
    REQUIRE(locked.has_value());
    CHECK(*locked);
    auto missing = symbols.is_library_locked("absent");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().kind() == util::Error::Kind::NotFound);
}

TEST_CASE("SDKQUERY matching projections keep name order, limits and score fields")
{
    test_support::TempDirectory temp;
    auto opened = storage::Database::open(temp.path() / "matching.db");
    REQUIRE(opened.has_value());
    auto db = std::move(*opened);
    auto session = db->session();

    core::LibraryMeta library;
    library.name = "MatchingLib";
    auto library_row = storage::LibraryRepository(session).insert(library);
    REQUIRE(library_row.has_value());

    core::Symbol zeta;
    zeta.set_name("Zeta");
    zeta.set_library_id(library_row->id);
    zeta.set_footprint("Pkg:Zeta");
    zeta.set_property("_pin_count", "2");
    REQUIRE(storage::SymbolRepository(session).insert(zeta).has_value());

    core::Symbol alpha;
    alpha.set_name("Alpha");
    alpha.set_library_id(library_row->id);
    alpha.set_property("_pin_count", "4");
    REQUIRE(storage::SymbolRepository(session).insert(alpha).has_value());

    core::Symbol power;
    power.set_name("Power");
    power.set_library_id(library_row->id);
    power.set_power(true);
    power.set_property("_pin_count", "1");
    REQUIRE(storage::SymbolRepository(session).insert(power).has_value());

    core::Footprint zfoot;
    zfoot.set_name("ZFoot");
    zfoot.set_property("_pad_count", "2");
    REQUIRE(storage::FootprintRepository(session).insert(zfoot).has_value());

    core::Footprint afoot;
    afoot.set_name("AFoot");
    afoot.set_property("_pad_count", "4");
    REQUIRE(storage::FootprintRepository(session).insert(afoot).has_value());

    auto symbols = storage::SymbolRepository(session).find_for_matching(2);
    REQUIRE(symbols.has_value());
    REQUIRE(symbols->size() == 2);
    CHECK(symbols->at(0).name() == "Alpha");
    CHECK(symbols->at(0).pin_count() == 4);
    CHECK(symbols->at(1).name() == "Power");
    CHECK(symbols->at(1).is_power());

    auto all_symbols = storage::SymbolRepository(session).find_for_matching(0);
    REQUIRE(all_symbols.has_value());
    CHECK(all_symbols->size() == 3);

    auto footprints = storage::FootprintRepository(session).find_for_matching(1);
    REQUIRE(footprints.has_value());
    REQUIRE(footprints->size() == 1);
    CHECK(footprints->at(0).name() == "AFoot");
    CHECK(footprints->at(0).pad_count() == 4);

    auto all_footprints = storage::FootprintRepository(session).find_for_matching(0);
    REQUIRE(all_footprints.has_value());
    CHECK(all_footprints->size() == 2);
}

TEST_CASE("SDKQUERY footprint parser keeps positional pad fields")
{
    // KiCad v6+ writes pad type/shape and at/size/drill as positional atoms.
    // The parser must not lose them when the DOM stores them as key/value pairs.
    constexpr std::string_view text = R"(
(footprint "PADFIELDS"
  (pad "" smd roundrect (at 0 0) (size 1 1))
  (pad "1" smd roundrect (at 1.5 -2) (size 0.5 1.25))
  (pad "2" thru_hole circle (at 0 0) (size 1 1) (drill 0.6))
  (model "part.step" (offset (xyz 1 2 3)))
)
)";
    auto parsed = parser::FootprintParser::parse_buffer(text);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->pads().size() == 2);
    const auto& first = parsed->pads()[0];
    CHECK(first.number == "1");
    CHECK(first.type == "smd");
    CHECK(first.shape == "roundrect");
    CHECK(first.x == doctest::Approx(1.5));
    CHECK(first.y == doctest::Approx(-2.0));
    CHECK(first.width == doctest::Approx(0.5));
    CHECK(first.height == doctest::Approx(1.25));
    const auto& second = parsed->pads()[1];
    CHECK(second.number == "2");
    CHECK(second.type == "thru_hole");
    CHECK(second.shape == "circle");
    CHECK(second.drill == doctest::Approx(0.6));
}
