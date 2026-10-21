#pragma once

#include <string>
#include <vector>
#include <unordered_map>

#include "core/model/types.h"

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

    // --- Source ---
    const std::string& library_path() const { return library_path_; }
    void set_library_path(std::string p) { library_path_ = std::move(p); }

    /// 来源规范路径标识（见 core/db/source_key.h）；未落盘时为空
    const std::string& source_key() const { return source_key_; }
    void set_source_key(std::string k) { source_key_ = std::move(k); }

    /// .pretty 库名（父目录名），用于解析 "Pkg:Name" 引用
    const std::string& footprint_lib_name() const { return footprint_lib_name_; }
    void set_footprint_lib_name(std::string n) { footprint_lib_name_ = std::move(n); }

    // --- Description ---
    const std::string& description() const { return description_; }
    void set_description(std::string d) { description_ = std::move(d); }

    const std::string& tags() const { return tags_; }
    void set_tags(std::string t) { tags_ = std::move(t); }

    // --- Pads ---
    const std::vector<Pad>& pads() const { return pads_; }
    std::vector<Pad>& pads() { return pads_; }
    void add_pad(Pad pad) { pads_.push_back(std::move(pad)); }
    int pad_count() const {
        if (!pads_.empty()) return static_cast<int>(pads_.size());
        // 回读时 pads_ 没有被还原（仓储行不重建 pads 序列化）：
        // 用库存的 _pad_count 保持计数，别让 pad_count 又变回 0。
        // 与 Symbol::pin_count 同一套回退思路。
        auto pc = property("_pad_count");
        if (pc) { try { return std::stoi(*pc); } catch (...) {} }
        return 0;
    }

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
    /// 封装类型 ID（见 core/model/type_registry.h）；未分类为空
    std::string package_type_id;

private:
    Uuid id_;
    std::string name_;
    std::string library_path_;
    std::string source_key_;          // 规范路径标识
    std::string footprint_lib_name_;  // .pretty 库名
    std::string description_;
    std::string tags_;
    std::vector<Pad> pads_;
    std::vector<Model3DRef> models_3d_;
    std::unordered_map<std::string, std::string> properties_;
};

}  // namespace kforge::core
