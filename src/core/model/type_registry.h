#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <mutex>

#include "core/model/types.h"
#include "util/error.h"

namespace kforge::core {

/// 一个类型条目。`id` 是不可变身份（落库的就是它），`name` 只用于展示 ——
/// 重命名、重排、改图标都不改变已有数据的含义。
struct TypeEntry {
    std::string id;
    std::string name;
    std::string icon;
    std::string color;
    std::string category;  // 封装类型用
};

// ============================================================
// 内置枚举 ↔ 内置 ID：静态、不可变映射。
// 存储一律用 ID；枚举只服务内置推断，不参与持久化，
// 所以注册表里增删/重排类型不会让已存的值改变含义。
// ============================================================

const char* builtin_component_id(ComponentType type);
const char* builtin_package_id(PackageType type);
ComponentType builtin_component_type(std::string_view id);  // 非内置 → Unknown
bool is_builtin_component_id(std::string_view id);
bool is_builtin_package_id(std::string_view id);

/// 动态类型注册表：JSON 装载，内置默认值兜底。
/// 读接口一律返回值副本（快照），不把可能失效的 vector 引用交给调用方。
class TypeRegistry {
public:
    static TypeRegistry& instance();

    // Launcher 服务 build：装载 component/package types（缺失仅 warn）。
    static kforge::util::Result<void> build();

    /// Load component types from a JSON file.
    /// JSON format: { "types": [ {"id":"...", "name":"...", "icon":"...", "color":"..."}, ... ] }
    /// 旧文件没有 "id" 时，原 name 原样作为 legacy ID。
    bool load_component_types(const std::string& json_path);
    bool load_package_types(const std::string& json_path);

    /// Direct load from JSON string (for defaults bundled in binary).
    void load_component_types_json(const std::string& json);
    void load_package_types_json(const std::string& json);

    // --- Component types ---
    std::vector<TypeEntry> component_types() const;
    std::optional<TypeEntry> find_component(std::string_view id) const;
    /// 展示名；ID 未登记时返回空串
    std::string component_name(std::string_view id) const;
    /// 展示用：登记过取 name，未登记原样返回 ID（不把未知值悄悄改成 Unknown）
    std::string component_display(std::string_view id) const;
    /// 旧接口的按名查找（仅在名字唯一匹配时用）
    std::optional<TypeEntry> component_by_name(std::string_view name) const;

    /// 新增类型；id 为空时生成 "custom:<UUID>"。
    /// 空名、重复 ID、与内置 ID 冲突都返回错误，由调用方上报。
    util::Result<std::string> add_component_type(const TypeEntry& entry);
    /// 改展示字段（名称/图标/颜色）；ID 不变，筛选结果不受影响
    util::Result<void> update_component_type(std::string_view id, const TypeEntry& fields);
    /// 删除类型；内置类型拒绝删除（返回 false）。引用检查由调用方负责
    bool remove_component_type(std::string_view id);

    // --- Package types ---
    std::vector<TypeEntry> package_types() const;
    std::optional<TypeEntry> find_package(std::string_view id) const;
    std::string package_name(std::string_view id) const;
    std::string package_display(std::string_view id) const;
    std::optional<TypeEntry> package_by_name(std::string_view name) const;

    util::Result<std::string> add_package_type(const TypeEntry& entry);
    util::Result<void> update_package_type(std::string_view id, const TypeEntry& fields);
    bool remove_package_type(std::string_view id);

    /// Save current types back to the file they were loaded from.
    bool save_component_types();
    bool save_package_types();

private:
    TypeRegistry();
    void init_defaults();

    /// 解析并校验一份候选快照；有问题返回 false 且不改动当前快照
    bool parse_types(const std::string& json, bool package, std::vector<TypeEntry>& out,
                     std::string& error);

    std::vector<TypeEntry> component_types_;
    std::vector<TypeEntry> package_types_;
    std::unordered_map<std::string, int> comp_id_map_;
    std::unordered_map<std::string, int> pkg_id_map_;
    std::unordered_map<std::string, int> comp_name_map_;  // 名字 → 下标（唯一匹配用）
    std::unordered_map<std::string, int> pkg_name_map_;

    std::string comp_json_path_;
    std::string pkg_json_path_;

    mutable std::mutex mutex_;
};

}  // namespace kforge::core
