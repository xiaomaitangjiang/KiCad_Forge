#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "util/error.h"

namespace kforge::services
{
enum class LibrarySourceKind { Symbols, Footprints, Models };

// Extensions are ASCII identifiers; case folding must not alter path identity.
inline bool has_library_extension(const std::filesystem::path& path, std::string_view expected)
{
    const auto extension = path.extension().native();
    if (extension.size() != expected.size()) return false;
    for (std::size_t i = 0; i < extension.size(); ++i)
    {
        const auto c = extension[i];
        if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != expected[i]) return false;
    }
    return true;
}

struct LibrarySourcePath
{
    std::filesystem::path path;
    bool single_file = false;
};

/// Configuration may name directories/files that will be created by LCSC later.
/// An import requires the source to exist. Both entry points share the same
/// distinction: a symbol source can be one .kicad_sym; footprints/models need a directory.
inline util::Result<LibrarySourcePath> validate_library_source_path(
    const std::string& root, LibrarySourceKind kind, bool allow_missing = false)
{
    namespace fs = std::filesystem;
    using Kind = util::Error::Kind;
    const std::string label = kind == LibrarySourceKind::Symbols ? "Symbol"
                           : kind == LibrarySourceKind::Footprints ? "Footprint" : "3D model";
    auto invalid = [&](const std::string& message) -> util::Result<LibrarySourcePath>
    {
        return std::unexpected(util::Error::make<Kind::ValidationError>(message + ": " + root));
    };
    if (root.empty()) return invalid(label + " path is empty");
    const fs::path path(std::u8string(root.begin(), root.end()));
    const bool symbol_file = has_library_extension(path, ".kicad_sym");
    const bool footprint_file = has_library_extension(path, ".kicad_mod");
    const std::string footprint_error = symbol_file
        ? "Footprint path points to a .kicad_sym symbol file; put it in Symbol path. "
          "Choose a .pretty directory or its parent directory for footprints"
        : "Footprint path must be a .pretty directory or a directory containing footprint libraries";
    std::error_code error;
    const auto status = fs::status(path, error);
    const bool missing = status.type() == fs::file_type::not_found;
    if (error && !(missing && error == std::errc::no_such_file_or_directory))
        return std::unexpected(util::Error::make<Kind::IoError>(
            "Cannot inspect " + label + " path (filesystem error "
            + std::to_string(error.value()) + "): " + root));
    if (missing)
    {
        if (kind == LibrarySourceKind::Footprints &&
            (symbol_file || footprint_file))
            return invalid(footprint_error);
        if (kind == LibrarySourceKind::Models
            && (symbol_file || footprint_file || has_library_extension(path, ".step")
                || has_library_extension(path, ".stp") || has_library_extension(path, ".wrl")
                || has_library_extension(path, ".iges") || has_library_extension(path, ".igs")))
            return invalid("3D model path must be a directory");
        if (!allow_missing) return invalid(label + " import source not found");
        return LibrarySourcePath{path, kind == LibrarySourceKind::Symbols && symbol_file};
    }
    if (fs::is_directory(status)) return LibrarySourcePath{path, false};
    if (kind == LibrarySourceKind::Footprints) return invalid(footprint_error);
    if (kind == LibrarySourceKind::Models) return invalid("3D model path must be a directory");
    if (fs::is_regular_file(status) && symbol_file)
        return LibrarySourcePath{path, true};
    return invalid("Symbol path must be a .kicad_sym file or a directory");
}
}  // namespace kforge::services
