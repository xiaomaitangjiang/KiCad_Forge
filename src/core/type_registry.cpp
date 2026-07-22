#include "core/type_registry.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace kforge::core {

// --------------- built-in defaults ---------------

static const char* DEFAULT_COMPONENT_TYPES = R"JSON({
  "types": [
    {"name":"Resistor","icon":"⊟","color":"#34C759"},
    {"name":"Capacitor","icon":"⊞","color":"#007AFF"},
    {"name":"Inductor","icon":"◠","color":"#AF52DE"},
    {"name":"Diode","icon":"▷","color":"#FF9500"},
    {"name":"LED","icon":"☀","color":"#FFCC00"},
    {"name":"Transistor","icon":"△","color":"#FF3B30"},
    {"name":"MOSFET","icon":"△","color":"#FF3B30"},
    {"name":"JFET","icon":"△","color":"#FF3B30"},
    {"name":"VoltageRegulator","icon":"▦","color":"#5856D6"},
    {"name":"OpAmp","icon":"▷","color":"#FF9500"},
    {"name":"Comparator","icon":"≽","color":"#FF9500"},
    {"name":"LogicGate","icon":"⊡","color":"#5856D6"},
    {"name":"Microcontroller","icon":"⬡","color":"#007AFF"},
    {"name":"FPGA","icon":"◰","color":"#007AFF"},
    {"name":"Memory","icon":"▤","color":"#5856D6"},
    {"name":"Connector","icon":"▣","color":"#8E8E93"},
    {"name":"Switch","icon":"⏣","color":"#8E8E93"},
    {"name":"Fuse","icon":"◎","color":"#FF9500"},
    {"name":"Crystal","icon":"◇","color":"#AF52DE"},
    {"name":"Oscillator","icon":"◆","color":"#AF52DE"},
    {"name":"Transformer","icon":"◎","color":"#FF9500"},
    {"name":"Relay","icon":"⏻","color":"#FF9500"},
    {"name":"Jumper","icon":"⌸","color":"#8E8E93"},
    {"name":"TestPoint","icon":"◎","color":"#8E8E93"},
    {"name":"MountingHole","icon":"◉","color":"#8E8E93"},
    {"name":"FerriteBead","icon":"◎","color":"#34C759"},
    {"name":"Varistor","icon":"◎","color":"#FF9500"},
    {"name":"Thermistor","icon":"◎","color":"#FF9500"},
    {"name":"Photodiode","icon":"☀","color":"#FFCC00"},
    {"name":"Phototransistor","icon":"☀","color":"#FFCC00"},
    {"name":"TRIAC","icon":"△","color":"#FF3B30"},
    {"name":"SCR","icon":"△","color":"#FF3B30"},
    {"name":"Unknown","icon":"○","color":"#8E8E93"}
  ]}
)JSON";

static const char* DEFAULT_PACKAGE_TYPES = R"JSON({
  "types": [
    {"name":"SMD_0201","category":"SMD Chip"},
    {"name":"SMD_0402","category":"SMD Chip"},
    {"name":"SMD_0603","category":"SMD Chip"},
    {"name":"SMD_0805","category":"SMD Chip"},
    {"name":"SMD_1206","category":"SMD Chip"},
    {"name":"SMD_1210","category":"SMD Chip"},
    {"name":"SMD_1812","category":"SMD Chip"},
    {"name":"SMD_2010","category":"SMD Chip"},
    {"name":"SMD_2512","category":"SMD Chip"},
    {"name":"SOT_23","category":"SMD IC"},
    {"name":"SOT_223","category":"SMD IC"},
    {"name":"SOT_363","category":"SMD IC"},
    {"name":"SOIC_8","category":"SMD IC"},
    {"name":"SOIC_14","category":"SMD IC"},
    {"name":"SOIC_16","category":"SMD IC"},
    {"name":"SOIC_20","category":"SMD IC"},
    {"name":"SOIC_24","category":"SMD IC"},
    {"name":"SOIC_28","category":"SMD IC"},
    {"name":"SSOP","category":"SMD IC"},
    {"name":"TSSOP","category":"SMD IC"},
    {"name":"MSOP","category":"SMD IC"},
    {"name":"QFP_32","category":"SMD IC"},
    {"name":"QFP_44","category":"SMD IC"},
    {"name":"QFP_64","category":"SMD IC"},
    {"name":"QFP_100","category":"SMD IC"},
    {"name":"QFP_144","category":"SMD IC"},
    {"name":"QFN_16","category":"SMD IC"},
    {"name":"QFN_20","category":"SMD IC"},
    {"name":"QFN_24","category":"SMD IC"},
    {"name":"QFN_32","category":"SMD IC"},
    {"name":"QFN_48","category":"SMD IC"},
    {"name":"QFN_64","category":"SMD IC"},
    {"name":"BGA","category":"SMD IC"},
    {"name":"LGA","category":"SMD IC"},
    {"name":"PLCC","category":"SMD IC"},
    {"name":"THT_Axial","category":"Through-hole"},
    {"name":"THT_Radial","category":"Through-hole"},
    {"name":"THT_DIP","category":"Through-hole"},
    {"name":"DIP_8","category":"Through-hole"},
    {"name":"DIP_14","category":"Through-hole"},
    {"name":"DIP_16","category":"Through-hole"},
    {"name":"DIP_20","category":"Through-hole"},
    {"name":"DIP_24","category":"Through-hole"},
    {"name":"DIP_28","category":"Through-hole"},
    {"name":"DIP_40","category":"Through-hole"},
    {"name":"USB_A","category":"Connector"},
    {"name":"USB_C","category":"Connector"},
    {"name":"USB_Micro","category":"Connector"},
    {"name":"USB_Mini","category":"Connector"},
    {"name":"RJ45","category":"Connector"},
    {"name":"RJ11","category":"Connector"},
    {"name":"PinHeader","category":"Connector"},
    {"name":"PinSocket","category":"Connector"},
    {"name":"TerminalBlock","category":"Connector"},
    {"name":"Custom","category":"Other"},
    {"name":"Unknown","category":"Other"}
  ]}
)JSON";

// --------------- singleton ---------------

TypeRegistry& TypeRegistry::instance() {
    static TypeRegistry reg;
    return reg;
}

TypeRegistry::TypeRegistry() {
    init_defaults();
}

void TypeRegistry::init_defaults() {
    load_component_types_json(DEFAULT_COMPONENT_TYPES);
    load_package_types_json(DEFAULT_PACKAGE_TYPES);
}

// --------------- json loading ---------------

void TypeRegistry::load_component_types_json(const std::string& json) {
    try {
        auto j = nlohmann::json::parse(json);
        if (!j.contains("types") || !j["types"].is_array()) return;

        std::lock_guard lock(mutex_);
        component_types_.clear();
        comp_name_map_.clear();

        for (auto& item : j["types"]) {
            TypeEntry e;
            e.name     = item.value("name", "");
            e.icon     = item.value("icon", "");
            e.color    = item.value("color", "");
            e.category = item.value("category", "");
            if (!e.name.empty()) {
                comp_name_map_[e.name] = static_cast<int>(component_types_.size());
                component_types_.push_back(std::move(e));
            }
        }
    } catch (...) {
        // keep defaults
    }
}

void TypeRegistry::load_package_types_json(const std::string& json) {
    try {
        auto j = nlohmann::json::parse(json);
        if (!j.contains("types") || !j["types"].is_array()) return;

        std::lock_guard lock(mutex_);
        package_types_.clear();
        pkg_name_map_.clear();

        for (auto& item : j["types"]) {
            TypeEntry e;
            e.name     = item.value("name", "");
            e.icon     = item.value("icon", "");
            e.color    = item.value("color", "");
            e.category = item.value("category", "");
            if (!e.name.empty()) {
                pkg_name_map_[e.name] = static_cast<int>(package_types_.size());
                package_types_.push_back(std::move(e));
            }
        }
    } catch (...) {
        // keep defaults
    }
}

bool TypeRegistry::load_component_types(const std::string& json_path) {
    std::ifstream f(json_path);
    if (!f.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    load_component_types_json(content);
    comp_json_path_ = json_path;
    return true;
}

bool TypeRegistry::load_package_types(const std::string& json_path) {
    std::ifstream f(json_path);
    if (!f.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    load_package_types_json(content);
    pkg_json_path_ = json_path;
    return true;
}

// --------------- accessors ---------------

const std::vector<TypeEntry>& TypeRegistry::component_types() const { return component_types_; }
const std::vector<TypeEntry>& TypeRegistry::package_types() const   { return package_types_; }

const TypeEntry* TypeRegistry::find_component(std::string_view name) const {
    std::lock_guard lock(mutex_);
    auto it = comp_name_map_.find(std::string(name));
    if (it != comp_name_map_.end()) return &component_types_[it->second];
    return nullptr;
}

int TypeRegistry::component_index(std::string_view name) const {
    std::lock_guard lock(mutex_);
    auto it = comp_name_map_.find(std::string(name));
    return (it != comp_name_map_.end()) ? it->second : -1;
}

std::string TypeRegistry::component_name_at(int idx) const {
    std::lock_guard lock(mutex_);
    if (idx >= 0 && idx < static_cast<int>(component_types_.size()))
        return component_types_[idx].name;
    return "Unknown";
}

const TypeEntry* TypeRegistry::find_package(std::string_view name) const {
    std::lock_guard lock(mutex_);
    auto it = pkg_name_map_.find(std::string(name));
    if (it != pkg_name_map_.end()) return &package_types_[it->second];
    return nullptr;
}

int TypeRegistry::package_index(std::string_view name) const {
    std::lock_guard lock(mutex_);
    auto it = pkg_name_map_.find(std::string(name));
    return (it != pkg_name_map_.end()) ? it->second : -1;
}

std::string TypeRegistry::package_name_at(int idx) const {
    std::lock_guard lock(mutex_);
    if (idx >= 0 && idx < static_cast<int>(package_types_.size()))
        return package_types_[idx].name;
    return "Unknown";
}

// --------------- mutation ---------------

bool TypeRegistry::add_component_type(const TypeEntry& entry) {
    if (entry.name.empty()) return false;
    std::lock_guard lock(mutex_);
    if (comp_name_map_.count(entry.name)) return false;
    int idx = static_cast<int>(component_types_.size());
    comp_name_map_[entry.name] = idx;
    component_types_.push_back(entry);
    return true;
}

bool TypeRegistry::remove_component_type(std::string_view name) {
    std::lock_guard lock(mutex_);
    auto it = comp_name_map_.find(std::string(name));
    if (it == comp_name_map_.end()) return false;
    int idx = it->second;
    // Move "Unknown" marker — keep the entry count stable by leaving a hole.
    // Better: swap with last, then pop.
    int last = static_cast<int>(component_types_.size()) - 1;
    if (idx != last) {
        comp_name_map_[component_types_[last].name] = idx;
        component_types_[idx] = std::move(component_types_[last]);
    }
    component_types_.pop_back();
    comp_name_map_.erase(it);
    // fixup map entry for the swapped item
    if (idx != last) {
        comp_name_map_[component_types_[idx].name] = idx;
    }
    return true;
}

bool TypeRegistry::add_package_type(const TypeEntry& entry) {
    if (entry.name.empty()) return false;
    std::lock_guard lock(mutex_);
    if (pkg_name_map_.count(entry.name)) return false;
    int idx = static_cast<int>(package_types_.size());
    pkg_name_map_[entry.name] = idx;
    package_types_.push_back(entry);
    return true;
}

bool TypeRegistry::remove_package_type(std::string_view name) {
    std::lock_guard lock(mutex_);
    auto it = pkg_name_map_.find(std::string(name));
    if (it == pkg_name_map_.end()) return false;
    int idx = it->second;
    int last = static_cast<int>(package_types_.size()) - 1;
    if (idx != last) {
        pkg_name_map_[package_types_[last].name] = idx;
        package_types_[idx] = std::move(package_types_[last]);
    }
    package_types_.pop_back();
    pkg_name_map_.erase(it);
    if (idx != last) {
        pkg_name_map_[package_types_[idx].name] = idx;
    }
    return true;
}

// --------------- persistence ---------------

bool TypeRegistry::save_component_types() {
    if (comp_json_path_.empty()) return false;
    try {
        nlohmann::json j;
        nlohmann::json arr = nlohmann::json::array();
        for (auto& e : component_types_) {
            nlohmann::json item;
            item["name"] = e.name;
            if (!e.icon.empty())  item["icon"] = e.icon;
            if (!e.color.empty()) item["color"] = e.color;
            arr.push_back(item);
        }
        j["types"] = arr;
        std::ofstream f(comp_json_path_);
        if (!f.is_open()) return false;
        f << j.dump(2);
        return true;
    } catch (...) { return false; }
}

bool TypeRegistry::save_package_types() {
    if (pkg_json_path_.empty()) return false;
    try {
        nlohmann::json j;
        nlohmann::json arr = nlohmann::json::array();
        for (auto& e : package_types_) {
            nlohmann::json item;
            item["name"] = e.name;
            if (!e.category.empty()) item["category"] = e.category;
            arr.push_back(item);
        }
        j["types"] = arr;
        std::ofstream f(pkg_json_path_);
        if (!f.is_open()) return false;
        f << j.dump(2);
        return true;
    } catch (...) { return false; }
}

// --------------- free functions ---------------

int component_type_index(std::string_view name) {
    return TypeRegistry::instance().component_index(name);
}

std::string component_type_name(int idx) {
    return TypeRegistry::instance().component_name_at(idx);
}

int package_type_index(std::string_view name) {
    return TypeRegistry::instance().package_index(name);
}

std::string package_type_name(int idx) {
    return TypeRegistry::instance().package_name_at(idx);
}

}  // namespace kforge::core
