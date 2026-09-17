// Shared 3D-model path resolution — used by ImportPipeline and the binding
// manager (auto-linking a footprint's 3D models after a re-bind).
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kforge::sexpr
{

/// Map a 3D model file extension to the models_3d.format() string
/// ("step" | "wrl" | "iges", or "" for unknown extensions).
inline std::string model_format_of(const std::string& ext_in)
{
    std::string ext = ext_in;
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c)
                           {
                               return std::tolower(c);
                           });
    if (ext == ".step" || ext == ".stp")
    {
        return "step";
    }
    if (ext == ".wrl")
    {
        return "wrl";
    }
    if (ext == ".iges" || ext == ".igs")
    {
        return "iges";
    }
    return {};
}

/// Resolve a KiCad 3D model reference (from a .kicad_mod (model ...) node) to
/// an existing absolute path, or {} when unresolvable.
/// Resolution order:
///   1. absolute path as-is
///   2. relative to the footprint file's own directory
///   3. "${VAR}/..." placeholder — VAR replaced by each model root
inline std::filesystem::path resolve_model_path(
    std::string_view ref, const std::filesystem::path& fp_dir,
    const std::vector<std::string>& model_roots)
{
    namespace fs = std::filesystem;

    auto found = [](const fs::path& cand) -> fs::path
    {
        std::error_code ec;
        if (!fs::exists(cand, ec) || ec)
        {
            return {};
        }
        auto canon = fs::weakly_canonical(cand, ec);
        return ec ? fs::absolute(cand) : canon;
    };

    // 1. Absolute path as-is
    if (fs::path(ref).is_absolute())
    {
        if (auto p = found(ref); !p.empty())
        {
            return p;
        }
    }

    // 2. Relative to the footprint file's own directory
    {
        auto p = found(fp_dir / ref);
        if (!p.empty())
        {
            return p;
        }
    }

    // 3. ${VAR} placeholder → replace with each model root
    auto var_begin = ref.find("${");
    if (var_begin != std::string_view::npos)
    {
        auto var_end = ref.find('}', var_begin);
        if (var_end != std::string_view::npos)
        {
            for (const auto& root : model_roots)
            {
                if (root.empty())
                {
                    continue;
                }
                std::string replaced = std::string(ref.substr(0, var_begin)) + root +
                                       std::string(ref.substr(var_end + 1));
                if (auto p = found(replaced); !p.empty())
                {
                    return p;
                }
            }
        }
    }
    return {};
}

}  // namespace kforge::sexpr
