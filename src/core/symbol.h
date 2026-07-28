#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

#include "core/types.h"

namespace kforge::core {

/// Represents a single schematic symbol from a .kicad_sym library.
class Symbol {
public:
    // --- Identity ---
    Uuid id() const { return id_; }
    void set_id(Uuid id) { id_ = std::move(id); }

    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    const std::string& lib_id() const { return lib_id_; }
    void set_lib_id(std::string id) { lib_id_ = std::move(id); }

    // --- Reference ---
    bool is_power() const { return is_power_; }
    void set_power(bool p) { is_power_ = p; }

    const std::string& reference_prefix() const { return ref_prefix_; }
    void set_reference_prefix(std::string p) { ref_prefix_ = std::move(p); }

    // --- Properties ---
    /// Default value shown on schematic (e.g. "100nF", "10k")
    const std::string& default_value() const { return default_value_; }
    void set_default_value(std::string v) { default_value_ = std::move(v); }

    /// Default footprint (can be empty)
    const std::string& footprint() const { return footprint_; }
    void set_footprint(std::string f) { footprint_ = std::move(f); }

    /// Datasheet URL
    const std::string& datasheet() const { return datasheet_; }
    void set_datasheet(std::string d) { datasheet_ = std::move(d); }

    /// Description
    const std::string& description() const { return description_; }
    void set_description(std::string d) { description_ = std::move(d); }

    /// Manufacturer part number
    const std::string& mpn() const { return mpn_; }
    void set_mpn(std::string m) { mpn_ = std::move(m); }

    // --- Kicad_Forge version tracking ---
    const std::string& Kicad_Forge_ID() const { return kf_id_; }
    void set_Kicad_Forge_ID(std::string id) { kf_id_ = std::move(id); }

    const std::string& Pre_Kicad_Forge_ID() const { return pre_kf_id_; }
    void set_Pre_Kicad_Forge_ID(std::string id) { pre_kf_id_ = std::move(id); }

    /// Move current Kicad_Forge_ID to Pre_Kicad_Forge_ID, compute new from attributes.
    void regenerate_Kicad_Forge_ID();

    /// Compute a stable hash from symbol attributes (name, pins, footprint, value).
    static std::string compute_hash(const Symbol& sym);

    // --- Pins ---
    const std::vector<PinDefinition>& pins() const { return pins_; }
    std::vector<PinDefinition>& pins() { return pins_; }
    void add_pin(PinDefinition pin) { pins_.push_back(std::move(pin)); }
    int pin_count() const {
        if (!pins_.empty()) return static_cast<int>(pins_.size());
        // Fallback: use stored _pin_count property when pins_ vector wasn't restored
        auto pc = property("_pin_count");
        if (pc) { try { return std::stoi(*pc); } catch (...) {} }
        return 0;
    }

    // --- Custom properties (key-value) ---
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
    std::unordered_map<std::string, std::string>& properties() {
        return properties_;
    }

    // --- Classification metadata ---
    ComponentType component_type{ComponentType::Unknown};
    PackageType package_type{PackageType::Unknown};
    std::optional<ValueRange> value_range;

    // --- Parent library reference ---
    Uuid library_id() const { return library_id_.empty() ? "default" : library_id_; }
    void set_library_id(Uuid id) { library_id_ = std::move(id); }

private:
    Uuid id_;
    Uuid library_id_{"default"};
    std::string name_;
    std::string lib_id_;
    bool is_power_{false};
    std::string ref_prefix_{"U"};
    std::string default_value_;
    std::string footprint_;
    std::string datasheet_;
    std::string description_;
    std::string mpn_;
    std::vector<PinDefinition> pins_;
    std::unordered_map<std::string, std::string> properties_;
    std::string kf_id_;
    std::string pre_kf_id_;
};

}  // namespace kforge::core
