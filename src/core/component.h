#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "core/footprint.h"
#include "core/model_3d.h"
#include "core/symbol.h"

namespace kforge::core {

/// Correspondence status for the symbol-footprint-3D triad.
enum class CorrespondenceStatus :std::uint8_t {
    Complete,           // All three linked
    MissingFootprint,   // Symbol has no footprint
    MissingModel3D,     // Footprint has no 3D model
    FootprintNotFound,  // Symbol references a footprint not in any library
    Model3DNotFound,    // Footprint references a 3D model file not found on disk
    Orphan,             // Not linked to anything
};

/// Unified component view combining symbol + footprint + 3D model.
/// This is the primary entity the GUI displays and the user interacts with.
class Component {
public:
    // --- Identity ---
    Uuid id() const { return id_; }
    void set_id(Uuid id) { id_ = std::move(id); }

    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    ComponentType type() const { return type_; }
    void set_type(ComponentType t) { type_ = t; }

    PackageType package() const { return package_; }
    void set_package(PackageType p) { package_ = p; }

    // --- Value ---
    const std::string& value() const { return value_; }
    void set_value(std::string v) { value_ = std::move(v); }

    std::optional<ValueRange> value_range() const { return value_range_; }
    void set_value_range(ValueRange r) { value_range_ = r; }

    // --- Pin/pad counts ---
    int pin_count() const { return pin_count_; }
    void set_pin_count(int n) { pin_count_ = n; }

    // --- Library info ---
    const std::string& library_name() const { return library_name_; }
    void set_library_name(std::string n) { library_name_ = std::move(n); }

    // --- Relationship tracking ---
    std::optional<Uuid> symbol_id() const { return symbol_id_; }
    void link_symbol(Uuid id) { symbol_id_ = std::move(id); }

    std::optional<Uuid> footprint_id() const { return footprint_id_; }
    void link_footprint(Uuid id) { footprint_id_ = std::move(id); }

    const std::vector<Uuid>& model_3d_ids() const { return model_3d_ids_; }
    void add_model_3d(Uuid id) { model_3d_ids_.push_back(std::move(id)); }

    // --- Status ---
    CorrespondenceStatus status() const { return status_; }
    void set_status(CorrespondenceStatus s) { status_ = s; }

    // --- Manufacturer info ---
    const std::string& mpn() const { return mpn_; }
    void set_mpn(std::string m) { mpn_ = std::move(m); }

    const std::string& manufacturer() const { return manufacturer_; }
    void set_manufacturer(std::string m) { manufacturer_ = std::move(m); }

    const std::string& lcsc_id() const { return lcsc_id_; }
    void set_lcsc_id(std::string id) { lcsc_id_ = std::move(id); }

    const std::string& datasheet_url() const { return datasheet_url_; }
    void set_datasheet_url(std::string url) { datasheet_url_ = std::move(url); }

    // --- Extra properties ---
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

private:
    Uuid id_;
    std::string name_;
    ComponentType type_{ComponentType::Unknown};
    PackageType package_{PackageType::Unknown};
    std::string value_;
    std::optional<ValueRange> value_range_;
    int pin_count_{0};
    std::string library_name_;

    std::optional<Uuid> symbol_id_;
    std::optional<Uuid> footprint_id_;
    std::vector<Uuid> model_3d_ids_;

    CorrespondenceStatus status_{CorrespondenceStatus::Orphan};

    std::string mpn_;
    std::string manufacturer_;
    std::string lcsc_id_;
    std::string datasheet_url_;

    std::unordered_map<std::string, std::string> properties_;
};

}  // namespace kforge::core
