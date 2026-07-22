#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace kforge::core {

/// Metadata for one component type entry.
struct TypeEntry {
    std::string name;
    std::string icon;
    std::string color;
    std::string category;  // for package types
};

/// Dynamic registry of ComponentType / PackageType values.
/// Loads from JSON files at startup; falls back to built-in defaults.
/// Thread-safe for reads; writes are serialized.
class TypeRegistry {
public:
    static TypeRegistry& instance();

    /// Load component types from a JSON file.
    /// JSON format: { "types": [ {"name":"...", "icon":"...", "color":"..."}, ... ] }
    bool load_component_types(const std::string& json_path);
    bool load_package_types(const std::string& json_path);

    /// Direct load from JSON string (for defaults bundled in binary).
    void load_component_types_json(const std::string& json);
    void load_package_types_json(const std::string& json);

    // --- Component types ---
    const std::vector<TypeEntry>& component_types() const;
    const TypeEntry* find_component(std::string_view name) const;
    int component_index(std::string_view name) const;  // -1 if not found
    std::string component_name_at(int idx) const;       // "" if out of range

    /// Add a new component type at runtime. Returns false if name already exists.
    bool add_component_type(const TypeEntry& entry);
    bool remove_component_type(std::string_view name);

    // --- Package types ---
    const std::vector<TypeEntry>& package_types() const;
    const TypeEntry* find_package(std::string_view name) const;
    int package_index(std::string_view name) const;     // -1 if not found
    std::string package_name_at(int idx) const;          // "" if out of range

    bool add_package_type(const TypeEntry& entry);
    bool remove_package_type(std::string_view name);

    /// Save current types back to the file they were loaded from.
    bool save_component_types();
    bool save_package_types();

private:
    TypeRegistry();
    void init_defaults();

    std::vector<TypeEntry> component_types_;
    std::vector<TypeEntry> package_types_;
    std::unordered_map<std::string, int, std::hash<std::string>,
                       std::equal_to<std::string>> comp_name_map_;
    std::unordered_map<std::string, int, std::hash<std::string>,
                       std::equal_to<std::string>> pkg_name_map_;

    std::string comp_json_path_;
    std::string pkg_json_path_;

    mutable std::mutex mutex_;
};

// ============================================================
// Convenience free functions that use the global registry.
// These replace the old hardcoded switch statements.
// ============================================================

/// Convert a stored type name string back to a ComponentType enum ordinal.
/// "Resistor" → 0, "Capacitor" → 1, "Unknown" → N-1,  "Bogus" → N-1 (Unknown)
int component_type_index(std::string_view name);
std::string component_type_name(int idx);

/// Convert a stored type name string to PackageType enum ordinal.
int package_type_index(std::string_view name);
std::string package_type_name(int idx);

}  // namespace kforge::core
