#pragma once

#include <string>
#include <filesystem>

#include "core/types.h"

namespace kforge::core {

/// Metadata for a 3D model file (.step, .wrl, .iges).
class Model3D {
public:
    Uuid id() const { return id_; }
    void set_id(Uuid id) { id_ = std::move(id); }

    const std::filesystem::path& file_path() const { return file_path_; }
    void set_file_path(std::filesystem::path p) { file_path_ = std::move(p); }

    const std::string& format() const { return format_; }
    void set_format(std::string f) { format_ = std::move(f); }

    /// Description from STEP header or filename
    const std::string& description() const { return description_; }
    void set_description(std::string d) { description_ = std::move(d); }

    /// Bounding box (mm) — may be unset for non-STEP files
    double width{0.0};
    double height{0.0};
    double depth{0.0};

    /// Source of the model (e.g. "LCSC", "manufacturer", "user")
    const std::string& source() const { return source_; }
    void set_source(std::string s) { source_ = std::move(s); }

private:
    Uuid id_;
    std::filesystem::path file_path_;
    std::string format_;  // "step", "wrl", "iges"
    std::string description_;
    std::string source_;
};

}  // namespace kforge::core
