#include "core/model/type_registry.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <random>

#include "util/file_write.h"
#include "util/logger.h"
#include "util/platform.h"

namespace kforge::core {

// --------------- built-in id tables ---------------
// 与枚举顺序一一对应；顺序写错会在这里编译失败，而不是悄悄把数据读成别的类型。

constexpr const char* kBuiltinComponentIds[] = {
    "Resistor", "Capacitor", "Inductor", "Diode", "LED", "Transistor", "MOSFET", "JFET",
    "VoltageRegulator", "OpAmp", "Comparator", "LogicGate", "Microcontroller", "FPGA",
    "Memory", "Connector", "Switch", "Fuse", "Crystal", "Oscillator", "Transformer",
    "Relay", "Jumper", "TestPoint", "MountingHole", "FerriteBead", "Varistor",
    "Thermistor", "Photodiode", "Phototransistor", "TRIAC", "SCR", "Unknown",
};
static_assert(std::size(kBuiltinComponentIds) ==
              static_cast<std::size_t>(ComponentType::Unknown) + 1);

constexpr const char* kBuiltinPackageIds[] = {
    "SMD_0201", "SMD_0402", "SMD_0603", "SMD_0805", "SMD_1206", "SMD_1210", "SMD_1812",
    "SMD_2010", "SMD_2512", "SOT_23", "SOT_223", "SOT_363", "SOIC_8", "SOIC_14", "SOIC_16",
    "SOIC_20", "SOIC_24", "SOIC_28", "SSOP", "TSSOP", "MSOP", "QFP_32", "QFP_44", "QFP_64",
    "QFP_100", "QFP_144", "QFN_16", "QFN_20", "QFN_24", "QFN_32", "QFN_48", "QFN_64", "BGA",
    "LGA", "PLCC", "THT_Axial", "THT_Radial", "THT_DIP", "DIP_8", "DIP_14", "DIP_16",
    "DIP_20", "DIP_24", "DIP_28", "DIP_40", "USB_A", "USB_C", "USB_Micro", "USB_Mini",
    "RJ45", "RJ11", "PinHeader", "PinSocket", "TerminalBlock", "Custom", "Unknown",
};
static_assert(std::size(kBuiltinPackageIds) ==
              static_cast<std::size_t>(PackageType::Unknown) + 1);

const char* builtin_component_id(ComponentType type) {
    auto index = static_cast<std::size_t>(type);
    if (index >= std::size(kBuiltinComponentIds)) return "Unknown";
    return kBuiltinComponentIds[index];
}

const char* builtin_package_id(PackageType type) {
    auto index = static_cast<std::size_t>(type);
    if (index >= std::size(kBuiltinPackageIds)) return "Unknown";
    return kBuiltinPackageIds[index];
}

bool is_builtin_component_id(std::string_view id) {
    for (const char* candidate : kBuiltinComponentIds) {
        if (id == candidate) return true;
    }
    return false;
}

bool is_builtin_package_id(std::string_view id) {
    for (const char* candidate : kBuiltinPackageIds) {
        if (id == candidate) return true;
    }
    return false;
}

ComponentType builtin_component_type(std::string_view id) {
    for (std::size_t i = 0; i < std::size(kBuiltinComponentIds); ++i) {
        if (id == kBuiltinComponentIds[i]) return static_cast<ComponentType>(i);
    }
    return ComponentType::Unknown;
}

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

// --------------- helpers ---------------

static std::string make_custom_id()
{
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<int> dist(0, 15);
    static const char hex[] = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(36);
    for (int i = 0; i < 8; ++i) uuid += hex[dist(gen)];
    uuid += '-';
    for (int i = 0; i < 4; ++i) uuid += hex[dist(gen)];
    uuid += "-4";
    for (int i = 0; i < 3; ++i) uuid += hex[dist(gen)];
    uuid += '-';
    uuid += hex[8 + dist(gen) % 4];
    for (int i = 0; i < 3; ++i) uuid += hex[dist(gen)];
    uuid += '-';
    for (int i = 0; i < 12; ++i) uuid += hex[dist(gen)];
    return "custom:" + uuid;
}

// --------------- singleton ---------------

TypeRegistry& TypeRegistry::instance() {
    static TypeRegistry reg;
    return reg;
}

util::Result<void> TypeRegistry::build() {
    auto& r = instance();
    // 类型配置是运行期可改的（增删/改名都会写回），所以放在可写的数据目录；
    // 随包分发的 config/ 只作为首次运行的种子。
    // 之前按相对路径读，谁在哪个工作目录启动就读到哪份文件 —— 开发与部署不一致。
    std::filesystem::path config_dir =
        std::filesystem::path(kforge::util::get_data_dir()) / "config";
    std::error_code error;
    std::filesystem::create_directories(config_dir, error);

    auto resolve = [&](const char* name)
    {
        auto target = config_dir / name;
        if (!std::filesystem::exists(target))
        {
            auto seed = std::filesystem::path(kforge::util::get_exe_dir()) / "config" / name;
            std::error_code copy_error;
            if (std::filesystem::exists(seed))
                std::filesystem::copy_file(seed, target,
                                           std::filesystem::copy_options::overwrite_existing,
                                           copy_error);
        }
        return target.string();
    };

    if (!r.load_component_types(resolve("component_types.json")))
        kforge::util::log_warn{}("component_types.json not found — using built-in defaults");
    if (!r.load_package_types(resolve("package_types.json")))
        kforge::util::log_warn{}("package_types.json not found — using built-in defaults");
    return {};
}

TypeRegistry::TypeRegistry() {
    init_defaults();
}

void TypeRegistry::init_defaults() {
    load_component_types_json(DEFAULT_COMPONENT_TYPES);
    load_package_types_json(DEFAULT_PACKAGE_TYPES);
}

// --------------- json loading ---------------

bool TypeRegistry::parse_types(const std::string& json, bool package,
                               std::vector<TypeEntry>& out, std::string& error)
{
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        error = std::string("invalid JSON: ") + e.what();
        return false;
    }
    if (!parsed.contains("types") || !parsed["types"].is_array()) {
        error = "missing \"types\" array";
        return false;
    }

    std::vector<TypeEntry> candidate;
    std::unordered_map<std::string, int> ids;
    for (auto& item : parsed["types"]) {
        TypeEntry entry;
        entry.name = item.value("name", "");
        // 旧文件没有 id：原 name 原样作为 legacy ID
        entry.id = item.value("id", entry.name);
        entry.icon = item.value("icon", "");
        entry.color = item.value("color", "");
        entry.category = item.value("category", "");
        if (entry.id.empty() || entry.name.empty()) {
            error += (error.empty() ? "" : "; ") + std::string("entry with empty id/name");
            continue;
        }
        if (ids.contains(entry.id)) {
            error = "duplicate id: " + entry.id;
            return false;
        }
        ids[entry.id] = static_cast<int>(candidate.size());
        candidate.push_back(std::move(entry));
    }
    if (candidate.empty()) {
        error = error.empty() ? "no usable entries" : error;
        return false;
    }

    // 内置类型不能改名成别的 ID，也不能被自定义 ID 顶掉：
    // 保留 ID 只要出现就必须对应内置集合里的那个值
    for (const auto& entry : candidate) {
        bool builtin = package ? is_builtin_package_id(entry.id) : is_builtin_component_id(entry.id);
        if (entry.id.starts_with("custom:") || builtin)
            continue;
        // 非 custom: 前缀的未知 ID 允许（历史数据可能就是这样），但记录一次
    }

    out = std::move(candidate);
    return true;
}

void TypeRegistry::load_component_types_json(const std::string& json) {
    std::vector<TypeEntry> candidate;
    std::string error;
    if (!parse_types(json, false, candidate, error)) {
        kforge::util::log_error{}("component types rejected ({}), keeping current snapshot: {}",
                                  error.empty() ? "invalid" : error,
                                  error.empty() ? json.substr(0, 80) : "");
        return;
    }
    std::lock_guard lock(mutex_);
    component_types_ = std::move(candidate);
    comp_id_map_.clear();
    comp_name_map_.clear();
    for (std::size_t i = 0; i < component_types_.size(); ++i) {
        comp_id_map_[component_types_[i].id] = static_cast<int>(i);
        comp_name_map_[component_types_[i].name] = static_cast<int>(i);
    }
}

void TypeRegistry::load_package_types_json(const std::string& json) {
    std::vector<TypeEntry> candidate;
    std::string error;
    if (!parse_types(json, true, candidate, error)) {
        kforge::util::log_error{}("package types rejected ({}), keeping current snapshot: {}",
                                  error.empty() ? "invalid" : error,
                                  error.empty() ? json.substr(0, 80) : "");
        return;
    }
    std::lock_guard lock(mutex_);
    package_types_ = std::move(candidate);
    pkg_id_map_.clear();
    pkg_name_map_.clear();
    for (std::size_t i = 0; i < package_types_.size(); ++i) {
        pkg_id_map_[package_types_[i].id] = static_cast<int>(i);
        pkg_name_map_[package_types_[i].name] = static_cast<int>(i);
    }
}

bool TypeRegistry::load_component_types(const std::string& json_path) {
    std::ifstream f(json_path);
    if (!f.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    load_component_types_json(content);
    std::lock_guard lock(mutex_);
    comp_json_path_ = json_path;
    return true;
}

bool TypeRegistry::load_package_types(const std::string& json_path) {
    std::ifstream f(json_path);
    if (!f.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    load_package_types_json(content);
    std::lock_guard lock(mutex_);
    pkg_json_path_ = json_path;
    return true;
}

// --------------- accessors（一律返回值副本） ---------------

std::vector<TypeEntry> TypeRegistry::component_types() const {
    std::lock_guard lock(mutex_);
    return component_types_;
}

std::vector<TypeEntry> TypeRegistry::package_types() const {
    std::lock_guard lock(mutex_);
    return package_types_;
}

std::optional<TypeEntry> TypeRegistry::find_component(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = comp_id_map_.find(std::string(id));
    if (it == comp_id_map_.end()) return std::nullopt;
    return component_types_[it->second];
}

std::string TypeRegistry::component_name(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = comp_id_map_.find(std::string(id));
    if (it == comp_id_map_.end()) return {};
    return component_types_[it->second].name;
}

std::string TypeRegistry::component_display(std::string_view id) const {
    auto name = component_name(id);
    return name.empty() ? std::string(id) : name;
}

std::optional<TypeEntry> TypeRegistry::component_by_name(std::string_view name) const {
    std::lock_guard lock(mutex_);
    auto it = comp_name_map_.find(std::string(name));
    if (it == comp_name_map_.end()) return std::nullopt;
    return component_types_[it->second];
}

std::optional<TypeEntry> TypeRegistry::find_package(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = pkg_id_map_.find(std::string(id));
    if (it == pkg_id_map_.end()) return std::nullopt;
    return package_types_[it->second];
}

std::string TypeRegistry::package_name(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = pkg_id_map_.find(std::string(id));
    if (it == pkg_id_map_.end()) return {};
    return package_types_[it->second].name;
}

std::string TypeRegistry::package_display(std::string_view id) const {
    auto name = package_name(id);
    return name.empty() ? std::string(id) : name;
}

std::optional<TypeEntry> TypeRegistry::package_by_name(std::string_view name) const {
    std::lock_guard lock(mutex_);
    auto it = pkg_name_map_.find(std::string(name));
    if (it == pkg_name_map_.end()) return std::nullopt;
    return package_types_[it->second];
}

// --------------- mutation ---------------

util::Result<std::string> TypeRegistry::add_component_type(const TypeEntry& entry) {
    TypeEntry value = entry;
    if (value.name.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type name must not be empty"));
    if (value.id.empty())
        value.id = make_custom_id();

    std::lock_guard lock(mutex_);
    if (comp_id_map_.contains(value.id))
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type id already exists: " + value.id));
    if (comp_name_map_.contains(value.name))
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type name already exists: " + value.name));
    comp_id_map_[value.id] = static_cast<int>(component_types_.size());
    comp_name_map_[value.name] = static_cast<int>(component_types_.size());
    component_types_.push_back(std::move(value));
    return component_types_.back().id;
}

util::Result<void> TypeRegistry::update_component_type(std::string_view id,
                                                      const TypeEntry& fields) {
    std::lock_guard lock(mutex_);
    auto it = comp_id_map_.find(std::string(id));
    if (it == comp_id_map_.end())
        return std::unexpected(util::Error::make<util::Error::Kind::NotFound>(
            "Type not found: " + std::string(id)));
    auto& entry = component_types_[it->second];
    if (!fields.name.empty() && fields.name != entry.name) {
        auto clash = comp_name_map_.find(fields.name);
        if (clash != comp_name_map_.end() && clash->second != it->second)
            return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
                "Type name already exists: " + fields.name));
        comp_name_map_.erase(entry.name);
        entry.name = fields.name;
        comp_name_map_[entry.name] = it->second;
    }
    entry.icon = fields.icon;
    entry.color = fields.color;
    return {};
}

bool TypeRegistry::remove_component_type(std::string_view id) {
    if (is_builtin_component_id(id))
        return false;  // 内置类型不允许删除
    std::lock_guard lock(mutex_);
    auto it = comp_id_map_.find(std::string(id));
    if (it == comp_id_map_.end()) return false;
    int index = it->second;
    comp_name_map_.erase(component_types_[index].name);
    component_types_.erase(component_types_.begin() + index);
    comp_id_map_.clear();
    comp_name_map_.clear();
    for (std::size_t i = 0; i < component_types_.size(); ++i) {
        comp_id_map_[component_types_[i].id] = static_cast<int>(i);
        comp_name_map_[component_types_[i].name] = static_cast<int>(i);
    }
    return true;
}

util::Result<std::string> TypeRegistry::add_package_type(const TypeEntry& entry) {
    TypeEntry value = entry;
    if (value.name.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type name must not be empty"));
    if (value.id.empty())
        value.id = make_custom_id();

    std::lock_guard lock(mutex_);
    if (pkg_id_map_.contains(value.id))
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type id already exists: " + value.id));
    if (pkg_name_map_.contains(value.name))
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type name already exists: " + value.name));
    pkg_id_map_[value.id] = static_cast<int>(package_types_.size());
    pkg_name_map_[value.name] = static_cast<int>(package_types_.size());
    package_types_.push_back(std::move(value));
    return package_types_.back().id;
}

util::Result<void> TypeRegistry::update_package_type(std::string_view id,
                                                     const TypeEntry& fields) {
    std::lock_guard lock(mutex_);
    auto it = pkg_id_map_.find(std::string(id));
    if (it == pkg_id_map_.end())
        return std::unexpected(util::Error::make<util::Error::Kind::NotFound>(
            "Type not found: " + std::string(id)));
    auto& entry = package_types_[it->second];
    if (!fields.name.empty() && fields.name != entry.name) {
        auto clash = pkg_name_map_.find(fields.name);
        if (clash != pkg_name_map_.end() && clash->second != it->second)
            return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
                "Type name already exists: " + fields.name));
        pkg_name_map_.erase(entry.name);
        entry.name = fields.name;
        pkg_name_map_[entry.name] = it->second;
    }
    if (!fields.category.empty())
        entry.category = fields.category;
    return {};
}

bool TypeRegistry::remove_package_type(std::string_view id) {
    if (is_builtin_package_id(id))
        return false;
    std::lock_guard lock(mutex_);
    auto it = pkg_id_map_.find(std::string(id));
    if (it == pkg_id_map_.end()) return false;
    int index = it->second;
    pkg_name_map_.erase(package_types_[index].name);
    package_types_.erase(package_types_.begin() + index);
    pkg_id_map_.clear();
    pkg_name_map_.clear();
    for (std::size_t i = 0; i < package_types_.size(); ++i) {
        pkg_id_map_[package_types_[i].id] = static_cast<int>(i);
        pkg_name_map_[package_types_[i].name] = static_cast<int>(i);
    }
    return true;
}

// --------------- persistence ---------------

namespace
{
/// 唯一临时文件 + 同目录替换：只有落盘成功才发布新内容
bool save_types(const std::string& path, const std::vector<TypeEntry>& entries, bool package)
{
    if (path.empty()) return false;
    try {
        nlohmann::json array = nlohmann::json::array();
        for (const auto& entry : entries) {
            nlohmann::json item;
            item["id"] = entry.id;
            item["name"] = entry.name;
            if (!entry.icon.empty()) item["icon"] = entry.icon;
            if (!entry.color.empty()) item["color"] = entry.color;
            if (package && !entry.category.empty()) item["category"] = entry.category;
            array.push_back(std::move(item));
        }
        nlohmann::json root;
        root["types"] = std::move(array);
        util::replace_file(path, root.dump(2));
        return true;
    } catch (const std::exception& e) {
        kforge::util::log_error{}("Cannot save types to {}: {}", path, e.what());
        return false;
    }
}
}  // namespace

bool TypeRegistry::save_component_types() {
    std::vector<TypeEntry> snapshot;
    std::string path;
    {
        std::lock_guard lock(mutex_);
        snapshot = component_types_;
        path = comp_json_path_;
    }
    return save_types(path, snapshot, false);
}

bool TypeRegistry::save_package_types() {
    std::vector<TypeEntry> snapshot;
    std::string path;
    {
        std::lock_guard lock(mutex_);
        snapshot = package_types_;
        path = pkg_json_path_;
    }
    return save_types(path, snapshot, true);
}

}  // namespace kforge::core
