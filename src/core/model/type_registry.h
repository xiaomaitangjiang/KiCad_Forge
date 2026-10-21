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

/// update 的补丁：字段缺省 = 保持原值，显式给值（含空串）= 覆盖。
/// 没有这个区分，「只改名」会顺手把图标/颜色清空 —— 用户看到的是丢数据。
/// 显式给出的 icon/color 会被校验（图标 ≤ 64 UTF-8 字节；颜色为空串或 #RRGGBB，
/// 大小写皆可），失败整次拒绝、内存与文件都不动。加载历史配置不走这套校验：
/// 旧文件里已有的非该格式颜色照样能读，也不会被一次改名改写掉。
struct TypePatch {
    std::optional<std::string> name;
    std::optional<std::string> icon;
    std::optional<std::string> color;
    std::optional<std::string> category;
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

    // --- 持久化变更：内存与文件一起提交，任何一步失败两边都不动 ---
    // 上面那组内存 API 只服务加载与既有测试；生产入口必须走这一组，
    // 否则就是「界面报成功、重启丢类型」。
    // 实现顺序：锁内拷贝候选 → 校验 → 序列化 + 原子替换 → 成功后 no-throw 发布。
    util::Result<std::string> add_component_type_persistent(const TypeEntry& entry);
    util::Result<void> update_component_type_persistent(std::string_view id, const TypePatch& patch);
    util::Result<void> remove_component_type_persistent(std::string_view id);
    util::Result<std::string> add_package_type_persistent(const TypeEntry& entry);
    util::Result<void> update_package_type_persistent(std::string_view id, const TypePatch& patch);
    util::Result<void> remove_package_type_persistent(std::string_view id);

    /// 当前绑定的持久化目标（未绑定 = 空串）。绑定只改「下次保存写到哪」，
    /// 不动内存定义 —— 供启动装配与测试保存/恢复单例状态。
    std::string component_types_path() const;
    std::string package_types_path() const;
    void bind_component_types_path(const std::string& path);
    void bind_package_types_path(const std::string& path);

private:
    TypeRegistry();
    void init_defaults();

    /// 校验并整体安装一份定义；失败不改内存、也不重绑持久化目标
    /// （坏文件绝不能被下次保存覆盖）。`source_path` 为空 = 不改变绑定。
    bool install_types(const std::string& json, bool package, const std::string& source_path);

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
