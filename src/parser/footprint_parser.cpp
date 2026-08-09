#include "parser/footprint_parser.h"
#include "sexpr/dom.h"
#include "sexpr/dom_builder.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace kforge::parser
{
using Kind = util::Error::Kind;

static double sv_to_double(std::string_view sv)
{
    double v = 0;
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;  // invalid input → 0, from_chars never throws
}

util::Result<core::Footprint> FootprintParser::parse(const std::filesystem::path& path)
{
    std::ifstream file(path.string(), std::ios::binary);
    if (!file.is_open())
        return std::unexpected(util::Error::make<Kind::IoError>("Cannot open file: " + path.string()));
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse_buffer(buffer.str());
}

util::Result<core::Footprint> FootprintParser::parse_buffer(std::string_view buffer)
{
    sexpr::DomBuilder builder;

    auto root_result = builder.parse_one(buffer);
    if (!root_result)
        return std::unexpected(root_result.error());

    auto& root = *root_result;
    if (root->type() != "footprint")
        return std::unexpected(util::Error::make<Kind::ParseError>(
            std::string("expected (footprint ...), got (") + std::string(root->type()) + ")"));

    core::Footprint fp;

    auto name_opt = root->property("name");
    if (name_opt)
        fp.set_name(std::string(*name_opt));
    else if (root->is_atom())
        fp.set_name(std::string(root->atom_value()));
    else if (!root->children().empty() && root->children()[0]->is_atom())
        fp.set_name(std::string(root->children()[0]->atom_value()));

    auto desc_opt = root->property("descr");
    if (desc_opt)
        fp.set_description(std::string(*desc_opt));
    auto tags_opt = root->property("tags");
    if (tags_opt)
        fp.set_tags(std::string(*tags_opt));

    for (const auto& child : root->children())
    {
        if (child->type() == "pad")
            parse_pad(fp, *child);
        else if (child->type() == "model" || child->type() == "model_3d")
            parse_model_3d(fp, *child);
        else if (child->type() == "attr")
        {
            auto type_opt = child->property("type");
            if (type_opt)
                fp.set_property("attr_type", std::string(*type_opt));
        }
    }
    return fp;
}

void FootprintParser::parse_pad(core::Footprint& fp, const sexpr::DomNode& node)
{
    core::Pad pad;

    // Pad number: stored as node atom (has_name_atom includes "pad")
    if (node.is_atom())
        pad.number = std::string(node.atom_value());
    // Fallback: property "number" or first atom child
    if (pad.number.empty()) {
        auto num = node.property("number");
        if (num) pad.number = *num;
    }
    if (pad.number.empty() && !node.children().empty() && node.children()[0]->is_atom())
        pad.number = std::string(node.children()[0]->atom_value());
    auto type = node.property("type");
    if (type)
        pad.type = *type;
    auto shape = node.property("shape");
    if (shape)
        pad.shape = *shape;

    auto* at_node = node.find_child("at");
    if (at_node && at_node->children().size() >= 2)
    {
        if (at_node->children()[0]->is_atom())
            pad.x = sv_to_double(at_node->children()[0]->atom_value());
        if (at_node->children()[1]->is_atom())
            pad.y = sv_to_double(at_node->children()[1]->atom_value());
    }

    auto* size_node = node.find_child("size");
    if (size_node && size_node->children().size() >= 2)
    {
        if (size_node->children()[0]->is_atom())
            pad.width = sv_to_double(size_node->children()[0]->atom_value());
        if (size_node->children()[1]->is_atom())
            pad.height = sv_to_double(size_node->children()[1]->atom_value());
    }

    auto* drill_node = node.find_child("drill");
    if (drill_node && !drill_node->children().empty() && drill_node->children()[0]->is_atom())
        pad.drill = sv_to_double(drill_node->children()[0]->atom_value());

    if (!pad.number.empty())
        fp.add_pad(std::move(pad));
}

void FootprintParser::parse_model_3d(core::Footprint& fp, const sexpr::DomNode& node)
{
    core::Model3DRef model;

    if (!node.children().empty() && node.children()[0]->is_atom())
        model.path = std::string(node.children()[0]->atom_value());
    else
    {
        auto path_opt = node.property("path");
        if (path_opt)
            model.path = *path_opt;
    }

    auto* off_node = node.find_child("offset");
    if (off_node && off_node->children().size() >= 3)
    {
        model.offset_x = sv_to_double(off_node->children()[0]->atom_value());
        model.offset_y = sv_to_double(off_node->children()[1]->atom_value());
        model.offset_z = sv_to_double(off_node->children()[2]->atom_value());
    }

    auto* scale_node = node.find_child("scale");
    if (scale_node && scale_node->children().size() >= 3)
    {
        model.scale_x = sv_to_double(scale_node->children()[0]->atom_value());
        model.scale_y = sv_to_double(scale_node->children()[1]->atom_value());
        model.scale_z = sv_to_double(scale_node->children()[2]->atom_value());
    }

    if (!model.path.empty())
        fp.add_model_3d(std::move(model));
}

}  // namespace kforge::parser
