#include <doctest/doctest.h>

#include "core/db/database.h"
#include "core/repo/repositories.h"
#include "interface/api/library_manifest.h"
#include "support/test_environment.h"

#include <filesystem>
#include <fstream>

using namespace kforge;

namespace
{
void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << text;
}
}  // namespace

TEST_CASE("LIBRARYMANIFEST exports semantic fields and parses pads outside the db session")
{
    test_support::TempDirectory temp;
    const auto root = temp.path();
    const auto symbol_path = root / "symbols";
    const auto footprint_path = root / "footprints";
    const auto model_path = root / "3dmodels";
    std::filesystem::create_directories(symbol_path);
    write_text(symbol_path / "Fixture.kicad_sym", "(kicad_symbol_lib (version 20240108))");
    write_text(footprint_path / "FP.pretty" / "Pad.kicad_mod",
               R"((footprint "Pad"
  (version 20240108)
  (generator pcbnew)
  (layer "F.Cu")
  (fp_text reference "REF**" (at 0 0) (layer "F.SilkS"))
  (pad "1" smd roundrect (at 1.5 2.5) (size 1.2 0.8) (layers "F.Cu" "F.Paste" "F.Mask") (roundrect_rratio 0.25))
))");
    write_text(model_path / "model.step", "ISO-10303-21;");

    auto opened = storage::Database::open(root / "manifest.db");
    REQUIRE(opened.has_value());
    auto db = std::move(*opened);
    {
        auto session = db->session();
        storage::ComponentLibraryRepository components(session);
        storage::ComponentLibraryRepository::ComponentLibrary group;
        group.name = "Fixture";
        group.symbol_path = symbol_path.string();
        group.footprint_path = footprint_path.string();
        group.model_3d_path = model_path.string();
        auto inserted_group = components.insert(group);
        REQUIRE(inserted_group.has_value());

        core::LibraryMeta library;
        library.name = "Fixture";
        library.file_path = symbol_path / "Fixture.kicad_sym";
        library.component_library_id = inserted_group->id;
        auto inserted_library = storage::LibraryRepository(session).insert(library);
        REQUIRE(inserted_library.has_value());

        core::Symbol symbol;
        symbol.set_id("symbol-1");
        symbol.set_library_id(inserted_library->id);
        symbol.set_name("R1");
        symbol.set_footprint("FP:Pad");
        symbol.set_property("Value", "10k");
        symbol.set_property("_pin_count", "1");
        symbol.set_Kicad_Forge_ID("KF-1");
        core::PinDefinition pin;
        pin.number = "1";
        pin.name = "~";
        pin.electrical_type = "passive";
        symbol.add_pin(pin);
        REQUIRE(storage::SymbolRepository(session).insert(symbol).has_value());

        core::Footprint footprint;
        footprint.set_id("footprint-1");
        footprint.set_name("Pad");
        footprint.set_library_path((footprint_path / "FP.pretty" / "Pad.kicad_mod").string());
        footprint.set_footprint_lib_name("FP");
        REQUIRE(storage::FootprintRepository(session).insert(footprint).has_value());

        core::Model3D model;
        model.set_id("model-1");
        model.set_file_path(model_path / "model.step");
        model.set_format("step");
        REQUIRE(storage::Model3DRepository(session).insert(model).has_value());

        storage::RelationshipRepository relationships(session);
        REQUIRE(relationships.link_symbol_to_footprint("symbol-1", "footprint-1").has_value());
        REQUIRE(relationships.link_footprint_to_model("footprint-1", "model-1").has_value());
    }

    auto manifest = api::build_library_manifest(*db, root.string(), symbol_path.string());
    REQUIRE(manifest.value("ok", false));

    REQUIRE(manifest["libraries"].size() == 1);
    REQUIRE(manifest["symbols"].size() == 1);
    const auto& symbol = manifest["symbols"].front();
    CHECK(symbol["uuid"] == "symbol-1");
    CHECK(symbol["name"] == "R1");
    CHECK(symbol["footprint_ref"] == "FP:Pad");
    CHECK(symbol["footprint_uuid"] == "footprint-1");
    CHECK(symbol["kf_id"] == "KF-1");
    REQUIRE(symbol["pins"].size() == 1);
    CHECK(symbol["pins"].front()["number"] == "1");
    CHECK(symbol["properties"]["Value"] == "10k");
    CHECK_FALSE(symbol["properties"].contains("_pin_count"));

    REQUIRE(manifest["footprints"].size() == 1);
    const auto& footprint = manifest["footprints"].front();
    CHECK(footprint["uuid"] == "footprint-1");
    CHECK(footprint["name"] == "Pad");
    CHECK(footprint["library"] == "FP");
    REQUIRE(footprint["pads"].size() == 1);
    const auto& pad = footprint["pads"].front();
    CHECK(pad["number"] == "1");
    CHECK(pad["type"] == "smd");
    CHECK(pad["shape"] == "roundrect");
    CHECK(pad["x"] == doctest::Approx(1.5));
    CHECK(pad["y"] == doctest::Approx(2.5));
    CHECK(pad["width"] == doctest::Approx(1.2));
    CHECK(pad["height"] == doctest::Approx(0.8));

    REQUIRE(manifest["models"].size() == 1);
    CHECK(manifest["models"].front()["path"] == "3dmodels/model.step");
}
