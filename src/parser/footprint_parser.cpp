#include "parser/footprint_parser.h"

#include <fstream>
#include <sstream>

#include "sexpr/dom.h"
#include "sexpr/dom_builder.h"

namespace kforge::parser {

util::Result<core::Footprint> FootprintParser::parse(
    const std::filesystem::path& path) {
    std::ifstream file(path.string(), std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected(
            util::Error::io("Cannot open file: " + path.string()));
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse_buffer(buffer.str());
}

util::Result<core::Footprint> FootprintParser::parse_buffer(
    std::string_view buffer) {
    sexpr::DomBuilder builder;

    auto root_result = builder.parse_one(buffer);
    if (!root_result) return std::unexpected(root_result.error());

    auto& root = *root_result;
    if (root->type() != "footprint") {
        return std::unexpected(
            util::Error::parse("expected (footprint ...), got (" +
                               root->type() + ")"));
    }

    core::Footprint fp;

    // Footprint name — check properties first
    auto name_opt = root->property("name");
    if (name_opt) {
        fp.set_name(std::string(*name_opt));
    } else if (!root->children().empty() && root->children()[0]->is_atom()) {
        fp.set_name(root->children()[0]->atom_value());
    }

    // Properties
    auto desc_opt = root->property("descr");
    if (desc_opt) fp.set_description(std::string(*desc_opt));
    auto tags_opt = root->property("tags");
    if (tags_opt) fp.set_tags(std::string(*tags_opt));

    // Parse children: (pad ...), (model ...), etc.
    for (const auto& child : root->children()) {
        if (child->type() == "pad") {
            parse_pad(fp, *child);
        } else if (child->type() == "model" || child->type() == "model_3d") {
            parse_model_3d(fp, *child);
        } else if (child->type() == "attr") {
            auto type_opt = child->property("type");
            if (type_opt) fp.set_property("attr_type", std::string(*type_opt));
        }
    }

    return fp;
}

void FootprintParser::parse_pad(core::Footprint& fp,
                                const sexpr::DomNode& node) {
    core::Pad pad;

    // Pad number from property
    auto num = node.property("number");
    if (num) pad.number = *num;
    // Fallback: first atom/string child
    if (pad.number.empty() && !node.children().empty() &&
        node.children()[0]->is_atom()) {
        pad.number = node.children()[0]->atom_value();
    }

    // Pad type from property
    auto type = node.property("type");
    if (type) pad.type = *type;

    // Pad shape from property
    auto shape = node.property("shape");
    if (shape) pad.shape = *shape;

    // Position: (at x y [rot])
    auto* at_node = node.find_child("at");
    if (at_node && at_node->children().size() >= 2) {
        if (at_node->children()[0]->is_atom()) {
            try { pad.x = std::stod(at_node->children()[0]->atom_value()); }
            catch (...) {}
        }
        if (at_node->children()[1]->is_atom()) {
            try { pad.y = std::stod(at_node->children()[1]->atom_value()); }
            catch (...) {}
        }
    }

    // Size: (size w h)
    auto* size_node = node.find_child("size");
    if (size_node && size_node->children().size() >= 2) {
        if (size_node->children()[0]->is_atom()) {
            try { pad.width = std::stod(size_node->children()[0]->atom_value()); }
            catch (...) {}
        }
        if (size_node->children()[1]->is_atom()) {
            try { pad.height = std::stod(size_node->children()[1]->atom_value()); }
            catch (...) {}
        }
    }

    // Drill: (drill diameter) or (drill oval w h)
    auto* drill_node = node.find_child("drill");
    if (drill_node && !drill_node->children().empty() &&
        drill_node->children()[0]->is_atom()) {
        try { pad.drill = std::stod(drill_node->children()[0]->atom_value()); }
        catch (...) {}
    }

    if (!pad.number.empty()) {
        fp.add_pad(std::move(pad));
    }
}

void FootprintParser::parse_model_3d(core::Footprint& fp,
                                     const sexpr::DomNode& node) {
    core::Model3DRef model;

    // Model path — first atom/string child
    if (!node.children().empty() && node.children()[0]->is_atom()) {
        model.path = node.children()[0]->atom_value();
    } else {
        // Check properties
        auto path_opt = node.property("path");
        if (path_opt) model.path = *path_opt;
    }

    // Offset: (offset x y z)
    auto* off_node = node.find_child("offset");
    if (off_node && off_node->children().size() >= 3) {
        try {
            model.offset_x = std::stod(off_node->children()[0]->atom_value());
            model.offset_y = std::stod(off_node->children()[1]->atom_value());
            model.offset_z = std::stod(off_node->children()[2]->atom_value());
        } catch (...) {}
    }

    // Scale: (scale x y z)
    auto* scale_node = node.find_child("scale");
    if (scale_node && scale_node->children().size() >= 3) {
        try {
            model.scale_x = std::stod(scale_node->children()[0]->atom_value());
            model.scale_y = std::stod(scale_node->children()[1]->atom_value());
            model.scale_z = std::stod(scale_node->children()[2]->atom_value());
        } catch (...) {}
    }

    if (!model.path.empty()) {
        fp.add_model_3d(std::move(model));
    }
}

}  // namespace kforge::parser
