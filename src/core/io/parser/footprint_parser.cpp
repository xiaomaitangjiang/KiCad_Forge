#include "core/io/parser/footprint_parser.h"
#include "core/io/sexpr/dom/dom.h"
#include "core/io/sexpr/dom/dom_builder.h"

#include <charconv>
#include <fstream>
#include <sstream>
#ifdef __APPLE__
#include <fast_float/fast_float.h>
#endif

namespace kforge::parser
{
using Kind = util::Error::Kind;

static double sv_to_double(std::string_view sv)
{
    double v = 0;
#ifdef __APPLE__
    fast_float::from_chars(sv.data(), sv.data() + sv.size(), v);
#else
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
#endif
    return v;  // invalid input → 0, from_chars never throws
}

/// KiCad 的简单参数节点（at/size/drill）没有子节点：DOM 把顺序参数存成
/// property 对，最后落单的参数放在 atom_value。按源码顺序展开，坐标/尺寸
/// 才不会被当成「属性键」而丢掉。
static std::vector<std::string_view> positional_values(const sexpr::DomNode& node)
{
    std::vector<std::string_view> values;
    for (const auto& [key, value] : node.properties())
    {
        values.push_back(key);
        if (!value.empty())
            values.push_back(value);
    }
    if (node.is_atom())
        values.push_back(node.atom_value());
    return values;
}

static bool is_pad_type(std::string_view text)
{
    return text == "smd" || text == "thru_hole" || text == "np_thru_hole" ||
           text == "connect" || text == "mixed";
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
    // KiCad 5 的 (module ...) 与 v6+ 的 (footprint ...) 结构一致；
    // 老库文件（以及部分第三方转换器输出）仍是 module，直接兼容
    if (root->type() != "footprint" && root->type() != "module")
        return std::unexpected(util::Error::make<Kind::ParseError>(
            std::string("expected (footprint ...), got (") + std::string(root->type()) + ")"));

    core::Footprint fp;

    auto name_opt = root->property("name");
    if (name_opt)
    {
        std::string raw(*name_opt);
        // 旧格式把库名一起写在名字里："easyeda2kicad:SOT-23" → "SOT-23"
        if (auto colon = raw.find(':'); colon != std::string::npos)
            raw = raw.substr(colon + 1);
        fp.set_name(std::move(raw));
    }
    else if (root->is_atom())
        fp.set_name(std::string(root->atom_value()));
    else if (!root->children().empty() && root->children()[0]->is_atom())
    {
        std::string raw(root->children()[0]->atom_value());
        if (auto colon = raw.find(':'); colon != std::string::npos)
            raw = raw.substr(colon + 1);
        fp.set_name(std::move(raw));
    }

    auto desc_opt = root->property("descr");
    if (desc_opt)
        fp.set_description(std::string(*desc_opt));
    auto tags_opt = root->property("tags");
    if (tags_opt)
        fp.set_tags(std::string(*tags_opt));

    for (const auto& child : root->children())
    {
        if (child->type() == "pad")
        {
            parse_pad(fp, *child);
        }
        else if (child->type() == "model" || child->type() == "model_3d")
        {
            parse_model_3d(fp, *child);
        }
        else if (child->type() == "attr")
        {
            auto type_opt = child->property("type");
            if (type_opt)
            {
                fp.set_property("attr_type", std::string(*type_opt));
            }
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
    if (pad.number.empty())
    {
        auto num = node.property("number");
        if (num)
            pad.number = *num;
    }
    if (pad.number.empty() && !node.children().empty() && node.children()[0]->is_atom())
        pad.number = std::string(node.children()[0]->atom_value());
    auto type = node.property("type");
    if (type)
        pad.type = *type;
    auto shape = node.property("shape");
    if (shape)
        pad.shape = *shape;
    // v6+ 的焊盘类型/形状是紧跟编号的位置参数：(pad "1" smd roundrect ...)。
    // DOM 把它们存成第一组 property 对，不会出现在 property("type") 里。
    if ((pad.type.empty() || pad.shape.empty()) && !node.properties().empty())
    {
        const auto& [first, second] = node.properties().front();
        if (is_pad_type(first))
        {
            if (pad.type.empty())
                pad.type = std::string(first);
            if (pad.shape.empty())
                pad.shape = std::string(second);
        }
    }

    if (auto* at_node = node.find_child("at"))
    {
        const auto values = positional_values(*at_node);
        if (values.size() >= 2)
        {
            pad.x = sv_to_double(values[0]);
            pad.y = sv_to_double(values[1]);
        }
    }

    if (auto* size_node = node.find_child("size"))
    {
        const auto values = positional_values(*size_node);
        if (values.size() >= 2)
        {
            pad.width = sv_to_double(values[0]);
            pad.height = sv_to_double(values[1]);
        }
    }

    if (auto* drill_node = node.find_child("drill"))
    {
        for (auto value : positional_values(*drill_node))
        {
            if (value.empty() || value == "oval" || value == "round" || value == "circle")
                continue;
            pad.drill = sv_to_double(value);
            break;
        }
    }

    if (!pad.number.empty())
        fp.add_pad(std::move(pad));
}

void FootprintParser::parse_model_3d(core::Footprint& fp, const sexpr::DomNode& node)
{
    core::Model3DRef model;

    // Path is the first string in the model node — may be atom, child atom, or property
    if (node.is_atom())
    {
        model.path = std::string(node.atom_value());
    }
    else if (!node.children().empty() && node.children()[0]->is_atom())
    {
        model.path = std::string(node.children()[0]->atom_value());
    }
    else if (auto path_opt = node.property("path"))
    {
        model.path = *path_opt;
    }

    const auto* off_node = node.find_child("offset");
    if ((off_node != nullptr) && off_node->children().size() >= 3)
    {
        model.offset_x = sv_to_double(off_node->children()[0]->atom_value());
        model.offset_y = sv_to_double(off_node->children()[1]->atom_value());
        model.offset_z = sv_to_double(off_node->children()[2]->atom_value());
    }

    const auto* scale_node = node.find_child("scale");
    if ((scale_node != nullptr) && scale_node->children().size() >= 3)
    {
        model.scale_x = sv_to_double(scale_node->children()[0]->atom_value());
        model.scale_y = sv_to_double(scale_node->children()[1]->atom_value());
        model.scale_z = sv_to_double(scale_node->children()[2]->atom_value());
    }

    if (!model.path.empty())
    {
        fp.add_model_3d(std::move(model));
    }
}

}  // namespace kforge::parser