#pragma once

#include <string>
#include <vector>
#include <unordered_map>

#include "core/types.h"

namespace kforge::core {

/// Pad in a footprint.
struct Pad {
    std::string number;
    std::string type;   // smd, thru_hole, np_thru_hole, connect
    std::string shape;  // rect, oval, circle, roundrect
    double x{0.0};
    double y{0.0};
    double width{0.0};
    double height{0.0};
    double drill{0.0};  // 0 for SMD pads
};

/// 3D model reference within a footprint.
struct Model3DRef {
    std::string path;       // relative or absolute path to .step/.wrl file
    double offset_x{0.0};
    double offset_y{0.0};
    double offset_z{0.0};
    double scale_x{1.0};
    double scale_y{1.0};
    double scale_z{1.0};
    double rotate_x{0.0};
    double rotate_y{0.0};
    double rotate_z{0.0};
};

/// Represents a single PCB footprint from a .kicad_mod file.
class Footprint {
public:
    // --- Identity ---
    Uuid id() const { return id_; }
    void set_id(Uuid id) { id_ = std::move(id); }

    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    // --- Description ---
    const std::string& description() const { return description_; }
    void set_description(std::string d) { description_ = std::move(d); }

    const std::string& tags() const { return tags_; }
    void set_tags(std::string t) { tags_ = std::move(t); }

    // --- Pads ---
    const std::vector<Pad>& pads() const { return pads_; }
    std::vector<Pad>& pads() { return pads_; }
    void add_pad(Pad pad) { pads_.push_back(std::move(pad)); }
    int pad_count() const { return static_cast<int>(pads_.size()); }

    // --- 3D Models ---
    const std::vector<Model3DRef>& models_3d() const { return models_3d_; }
    std::vector<Model3DRef>& models_3d() { return models_3d_; }
    void add_model_3d(Model3DRef m) { models_3d_.push_back(std::move(m)); }
    bool has_3d_model() const { return !models_3d_.empty(); }

    // --- Custom properties ---
    std::optional<std::string> property(const std::string& key) const {
        auto it = properties_.find(key);
        if (it != properties_.end()) return it->second;
        return std::nullopt;
    }
    void set_property(std::string key, std::string value) {
        properties_[std::move(key)] = std::move(value);
    }
    const std::unordered_map<std::string, std::string>& properties() const {
        return properties_;
    }

    // --- Courtyard ---
    double courtyard_width{0.0};
    double courtyard_height{0.0};

    // --- Classification ---
    PackageType package_type{PackageType::Unknown};

private:
    Uuid id_;
    std::string name_;
    std::string description_;
    std::string tags_;
    std::vector<Pad> pads_;
    std::vector<Model3DRef> models_3d_;
    std::unordered_map<std::string, std::string> properties_;
};

}  // namespace kforge::core
