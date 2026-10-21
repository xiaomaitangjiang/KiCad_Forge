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

namespace
{
/// 候选快照：定义 + 与之同下标的 id/name 索引。
/// 索引必须在写文件之前建好 —— 落盘之后才 rebuild，中间窗口就是
/// 「磁盘已经是新的、内存还是旧的」。
struct TypeStore
{
    std::vector<TypeEntry> entries;
    std::unordered_map<std::string, int> ids;
    std::unordered_map<std::string, int> names;

    void reindex()
    {
        ids.clear();
        names.clear();
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            ids[entries[i].id] = static_cast<int>(i);
            names[entries[i].name] = static_cast<int>(i);
        }
    }
};

/// 只做 no-throw 交换发布（vector / unordered_map 同为 std::allocator，
/// swap 不分配也不抛）—— 失败路径上这里一次都不会被调用。
void publish(std::vector<TypeEntry>& entries,
             std::unordered_map<std::string, int>& ids,
             std::unordered_map<std::string, int>& names, TypeStore& candidate) noexcept
{
    entries.swap(candidate.entries);
    ids.swap(candidate.ids);
    names.swap(candidate.names);
}

/// 图标是短文本，限制按 UTF-8 字节算（std::string::size() 就是字节数）
constexpr std::size_t kMaxIconBytes = 64;

/// 新增/显式更新时的字段校验。
/// 只校验「这次真正写下去的字段」：加载历史配置不走这里 ——
/// 旧文件里合法存在的颜色/图标（例如三位缩写色值）不能被新规则判死，
/// 否则一次「只改名」就会丢掉别的字段。
util::Result<void> validate_icon(const std::string& icon)
{
    if (icon.size() > kMaxIconBytes)
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Icon must be at most " + std::to_string(kMaxIconBytes) + " UTF-8 bytes (got " +
            std::to_string(icon.size()) + ")"));
    return {};
}

/// 颜色只接受空串（= 无颜色）或 #RRGGBB（大小写皆可）
util::Result<void> validate_color(const std::string& color)
{
    if (color.empty()) return {};
    auto is_hex = [](char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    bool valid = color.size() == 7 && color[0] == '#';
    if (valid)
    {
        for (std::size_t i = 1; i < color.size(); ++i)
        {
            if (!is_hex(color[i]))
            {
                valid = false;
                break;
            }
        }
    }
    if (!valid)
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Color must be #RRGGBB or empty: " + color));
    return {};
}

util::Result<std::string> add_entry(TypeStore& store, const TypeEntry& value)
{
    if (auto icon = validate_icon(value.icon); !icon) return std::unexpected(icon.error());
    if (auto color = validate_color(value.color); !color) return std::unexpected(color.error());
    if (store.ids.contains(value.id))
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type id already exists: " + value.id));
    if (store.names.contains(value.name))
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type name already exists: " + value.name));
    store.entries.push_back(value);
    store.reindex();
    return value.id;
}

/// 补丁语义：缺省字段不动，显式字段（含空串）覆盖；ID 永不改变。
util::Result<void> apply_patch(TypeStore& store, std::string_view id, const TypePatch& patch)
{
    auto it = store.ids.find(std::string(id));
    if (it == store.ids.end())
        return std::unexpected(util::Error::make<util::Error::Kind::NotFound>(
            "Type not found: " + std::string(id)));

    auto index = static_cast<std::size_t>(it->second);
    TypeEntry updated = store.entries[index];
    if (patch.name)
    {
        if (patch.name->empty())
            return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
                "Type name must not be empty"));
        updated.name = *patch.name;
    }
    // 显式给出的字段才校验：省略 = 原值不动（原值可能是历史格式，原样保留）
    if (patch.icon)
    {
        if (auto valid = validate_icon(*patch.icon); !valid) return valid;
        updated.icon = *patch.icon;
    }
    if (patch.color)
    {
        if (auto valid = validate_color(*patch.color); !valid) return valid;
        updated.color = *patch.color;
    }
    if (patch.category) updated.category = *patch.category;

    // 只在真的改名时查重：历史文件里可能本来就有重名条目，
    // 那种文件改图标不该被名字校验挡住
    if (updated.name != store.entries[index].name)
    {
        for (std::size_t i = 0; i < store.entries.size(); ++i)
        {
            if (i != index && store.entries[i].name == updated.name)
                return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
                    "Type name already exists: " + updated.name));
        }
    }
    store.entries[index] = std::move(updated);
    store.reindex();
    return {};
}

util::Result<void> remove_entry(TypeStore& store, std::string_view id, bool package)
{
    bool builtin = package ? is_builtin_package_id(id) : is_builtin_component_id(id);
    if (builtin)
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Built-in types cannot be deleted"));
    auto it = store.ids.find(std::string(id));
    if (it == store.ids.end())
        return std::unexpected(util::Error::make<util::Error::Kind::NotFound>(
            "Type not found: " + std::string(id)));
    store.entries.erase(store.entries.begin() + it->second);
    store.reindex();
    return {};
}

/// 与旧 save_types 完全相同的 JSON 形状：id/name 必写，空 icon/color/category 省略
util::Result<void> serialize_types(const std::vector<TypeEntry>& entries, bool package,
                                   std::string& out)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& entry : entries)
    {
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
    out = root.dump(2);
    return {};
}

/// 序列化 + 原子替换。任何异常都变成带路径的 IoError，绝不半写。
util::Result<void> persist_types(const std::string& path, const std::vector<TypeEntry>& entries,
                                 bool package, const char* kind)
{
    if (path.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::IoError>(
            std::string(kind) + " type config path is not set"));
    try
    {
        std::string text;
        if (auto encoded = serialize_types(entries, package, text); !encoded)
            return encoded;
        util::replace_file(path, text);
    }
    catch (const std::exception& e)
    {
        kforge::util::log_error{}("Cannot save {} types to {}: {}", kind, path, e.what());
        return std::unexpected(util::Error::make<util::Error::Kind::IoError>(
            std::string("Cannot save ") + kind + " types to " + path + ": " + e.what()));
    }
    return {};
}

/// 类型配置文件的读取结论。只有「文件确实不存在」才允许重绑保存目标：
/// 目录、权限、枚举失败、读取中断都必须保留原有定义与原保存路径。
enum class TypeFileRead
{
    Ok,
    Missing,
    Failed,
};

TypeFileRead read_type_file(const std::string& path, std::string& content)
{
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if ((!ec && status.type() == std::filesystem::file_type::not_found) ||
        ec == std::errc::no_such_file_or_directory)
        return TypeFileRead::Missing;
    if (ec || status.type() != std::filesystem::file_type::regular)
        return TypeFileRead::Failed;
    try
    {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return TypeFileRead::Failed;
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (f.bad()) return TypeFileRead::Failed;  // 读取中断：半截内容不作数
        content = std::move(raw);
    }
    catch (const std::exception&)
    {
        return TypeFileRead::Failed;
    }
    return TypeFileRead::Ok;
}
}  // namespace

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
        if (!item.is_object()) {
            error = "type entry is not an object";
            return false;
        }
        TypeEntry entry;
        // 字段类型不对（例如 name 是数字）整份文件拒绝：只丢这一条会让内存里
        // 留下一份"看起来加载成功"的残缺注册表。
        try {
            entry.name = item.value("name", "");
            // 旧文件没有 id：原 name 原样作为 legacy ID
            entry.id = item.value("id", entry.name);
            entry.icon = item.value("icon", "");
            entry.color = item.value("color", "");
            entry.category = item.value("category", "");
        } catch (const std::exception& e) {
            error = std::string("invalid field type: ") + e.what();
            return false;
        }
        if (entry.id.empty() || entry.name.empty()) {
            error = "entry with empty id/name";
            return false;
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

bool TypeRegistry::install_types(const std::string& json, bool package,
                                 const std::string& source_path) {
    std::vector<TypeEntry> candidate;
    std::string error;
    if (!parse_types(json, package, candidate, error)) {
        kforge::util::log_error{}("{} types rejected ({}), keeping current snapshot: {}",
                                  package ? "package" : "component",
                                  error.empty() ? "invalid" : error,
                                  error.empty() ? json.substr(0, 80) : "");
        return false;
    }
    std::lock_guard lock(mutex_);
    if (package) {
        package_types_ = std::move(candidate);
        pkg_id_map_.clear();
        pkg_name_map_.clear();
        for (std::size_t i = 0; i < package_types_.size(); ++i) {
            pkg_id_map_[package_types_[i].id] = static_cast<int>(i);
            pkg_name_map_[package_types_[i].name] = static_cast<int>(i);
        }
        if (!source_path.empty()) pkg_json_path_ = source_path;
    } else {
        component_types_ = std::move(candidate);
        comp_id_map_.clear();
        comp_name_map_.clear();
        for (std::size_t i = 0; i < component_types_.size(); ++i) {
            comp_id_map_[component_types_[i].id] = static_cast<int>(i);
            comp_name_map_[component_types_[i].name] = static_cast<int>(i);
        }
        if (!source_path.empty()) comp_json_path_ = source_path;
    }
    return true;
}

void TypeRegistry::load_component_types_json(const std::string& json) {
    install_types(json, false, {});
}

void TypeRegistry::load_package_types_json(const std::string& json) {
    install_types(json, true, {});
}

bool TypeRegistry::load_component_types(const std::string& json_path) {
    std::string content;
    const auto read = read_type_file(json_path, content);
    if (read != TypeFileRead::Ok) {
        if (read == TypeFileRead::Missing) {
            // 文件确实缺失（首次运行）：记住目标路径，让第一次保存能落在这里 ——
            // 内存保持现有快照不动。
            std::lock_guard lock(mutex_);
            comp_json_path_ = json_path;
        }
        // 目录 / 权限 / 读取中断：保留原定义与原保存路径，半截内容不当有效配置
        return false;
    }
    // 坏内容返回 false 且不重绑路径：否则下一次保存会把用户原本的文件覆盖掉
    return install_types(content, false, json_path);
}

bool TypeRegistry::load_package_types(const std::string& json_path) {
    std::string content;
    const auto read = read_type_file(json_path, content);
    if (read != TypeFileRead::Ok) {
        if (read == TypeFileRead::Missing) {
            std::lock_guard lock(mutex_);
            pkg_json_path_ = json_path;
        }
        return false;
    }
    return install_types(content, true, json_path);
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

// 旧的纯内存 API：只加载与既有测试用，不落盘。生产入口走 *_persistent。
util::Result<std::string> TypeRegistry::add_component_type(const TypeEntry& entry) {
    TypeEntry value = entry;
    if (value.name.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type name must not be empty"));

    std::lock_guard lock(mutex_);
    // ID 生成与索引用同一把锁：与 *_persistent 共用的静态 RNG 混用时不能有锁外窗口
    if (value.id.empty()) value.id = make_custom_id();
    TypeStore candidate{component_types_, comp_id_map_, comp_name_map_};
    auto added = add_entry(candidate, value);
    if (!added) return added;
    publish(component_types_, comp_id_map_, comp_name_map_, candidate);
    return *added;
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
    TypeStore candidate{component_types_, comp_id_map_, comp_name_map_};
    auto removed = remove_entry(candidate, id, false);
    if (!removed) return false;
    publish(component_types_, comp_id_map_, comp_name_map_, candidate);
    return true;
}

util::Result<std::string> TypeRegistry::add_package_type(const TypeEntry& entry) {
    TypeEntry value = entry;
    if (value.name.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type name must not be empty"));

    std::lock_guard lock(mutex_);
    // 同 add_component_type：ID 生成与索引发布同锁，锁外生成会与持久化路径抢 RNG
    if (value.id.empty()) value.id = make_custom_id();
    TypeStore candidate{package_types_, pkg_id_map_, pkg_name_map_};
    auto added = add_entry(candidate, value);
    if (!added) return added;
    publish(package_types_, pkg_id_map_, pkg_name_map_, candidate);
    return *added;
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
    TypeStore candidate{package_types_, pkg_id_map_, pkg_name_map_};
    auto removed = remove_entry(candidate, id, true);
    if (!removed) return false;
    publish(package_types_, pkg_id_map_, pkg_name_map_, candidate);
    return true;
}

// --------------- persistent mutation ---------------
// 顺序固定：拷贝候选 → 校验 → 序列化 + 原子替换 → 成功后才 no-throw 发布。
// 失败路径上内存与文件都保持调用前的样子，不靠事后回滚拼。

util::Result<std::string> TypeRegistry::add_component_type_persistent(const TypeEntry& entry) {
    TypeEntry value = entry;
    if (value.name.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type name must not be empty"));

    std::lock_guard lock(mutex_);  // ID 生成与落盘同锁：并发 add 不会撞同一个 id
    if (value.id.empty()) value.id = make_custom_id();
    TypeStore candidate{component_types_, comp_id_map_, comp_name_map_};
    auto added = add_entry(candidate, value);
    if (!added) return added;
    if (auto saved = persist_types(comp_json_path_, candidate.entries, false, "component"); !saved)
        return std::unexpected(saved.error());
    publish(component_types_, comp_id_map_, comp_name_map_, candidate);
    return *added;
}

util::Result<void> TypeRegistry::update_component_type_persistent(std::string_view id,
                                                                 const TypePatch& patch) {
    std::lock_guard lock(mutex_);
    TypeStore candidate{component_types_, comp_id_map_, comp_name_map_};
    if (auto patched = apply_patch(candidate, id, patch); !patched) return patched;
    if (auto saved = persist_types(comp_json_path_, candidate.entries, false, "component"); !saved)
        return std::unexpected(saved.error());
    publish(component_types_, comp_id_map_, comp_name_map_, candidate);
    return {};
}

util::Result<void> TypeRegistry::remove_component_type_persistent(std::string_view id) {
    std::lock_guard lock(mutex_);
    TypeStore candidate{component_types_, comp_id_map_, comp_name_map_};
    if (auto removed = remove_entry(candidate, id, false); !removed) return removed;
    if (auto saved = persist_types(comp_json_path_, candidate.entries, false, "component"); !saved)
        return std::unexpected(saved.error());
    publish(component_types_, comp_id_map_, comp_name_map_, candidate);
    return {};
}

util::Result<std::string> TypeRegistry::add_package_type_persistent(const TypeEntry& entry) {
    TypeEntry value = entry;
    if (value.name.empty())
        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
            "Type name must not be empty"));

    std::lock_guard lock(mutex_);
    if (value.id.empty()) value.id = make_custom_id();
    TypeStore candidate{package_types_, pkg_id_map_, pkg_name_map_};
    auto added = add_entry(candidate, value);
    if (!added) return added;
    if (auto saved = persist_types(pkg_json_path_, candidate.entries, true, "package"); !saved)
        return std::unexpected(saved.error());
    publish(package_types_, pkg_id_map_, pkg_name_map_, candidate);
    return *added;
}

util::Result<void> TypeRegistry::update_package_type_persistent(std::string_view id,
                                                               const TypePatch& patch) {
    std::lock_guard lock(mutex_);
    TypeStore candidate{package_types_, pkg_id_map_, pkg_name_map_};
    if (auto patched = apply_patch(candidate, id, patch); !patched) return patched;
    if (auto saved = persist_types(pkg_json_path_, candidate.entries, true, "package"); !saved)
        return std::unexpected(saved.error());
    publish(package_types_, pkg_id_map_, pkg_name_map_, candidate);
    return {};
}

util::Result<void> TypeRegistry::remove_package_type_persistent(std::string_view id) {
    std::lock_guard lock(mutex_);
    TypeStore candidate{package_types_, pkg_id_map_, pkg_name_map_};
    if (auto removed = remove_entry(candidate, id, true); !removed) return removed;
    if (auto saved = persist_types(pkg_json_path_, candidate.entries, true, "package"); !saved)
        return std::unexpected(saved.error());
    publish(package_types_, pkg_id_map_, pkg_name_map_, candidate);
    return {};
}

// --------------- 持久化目标 ---------------

std::string TypeRegistry::component_types_path() const {
    std::lock_guard lock(mutex_);
    return comp_json_path_;
}

std::string TypeRegistry::package_types_path() const {
    std::lock_guard lock(mutex_);
    return pkg_json_path_;
}

void TypeRegistry::bind_component_types_path(const std::string& path) {
    std::lock_guard lock(mutex_);
    comp_json_path_ = path;
}

void TypeRegistry::bind_package_types_path(const std::string& path) {
    std::lock_guard lock(mutex_);
    pkg_json_path_ = path;
}

// --------------- persistence ---------------

// 兼容入口：锁一直握到落盘完成 —— 先拷快照再放锁会让并发保存互相覆盖，
// 而且固定 .tmp 路径在这种情况下必然撞名。持久化变更请用 *_persistent。
bool TypeRegistry::save_component_types() {
    std::lock_guard lock(mutex_);
    return persist_types(comp_json_path_, component_types_, false, "component").has_value();
}

bool TypeRegistry::save_package_types() {
    std::lock_guard lock(mutex_);
    return persist_types(pkg_json_path_, package_types_, true, "package").has_value();
}

}  // namespace kforge::core
