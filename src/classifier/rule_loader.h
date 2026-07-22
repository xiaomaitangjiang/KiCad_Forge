#pragma once

#include <filesystem>
#include <vector>

#include "classifier/rule.h"
#include "util/result.h"

namespace kforge::classifier {

/// Loads classification rules from TOML configuration files.
///
/// Example TOML:
///   [[rule]]
///   name = "Resistors to Resistor_SMD"
///   priority = 10
///   target = "Resistor_SMD"
///   confidence = 95
///   condition = { type = "Resistor", package = "SMD_*" }
class RuleLoader {
public:
    /// Load all .toml rule files from a directory.
    static util::Result<std::vector<Rule>> load_directory(
        const std::filesystem::path& rules_dir);

    /// Load a single .toml rule file.
    static util::Result<std::vector<Rule>> load_file(
        const std::filesystem::path& path);

    /// Get built-in default rules (hardcoded fallback).
    static std::vector<Rule> default_rules();
};

}  // namespace kforge::classifier
