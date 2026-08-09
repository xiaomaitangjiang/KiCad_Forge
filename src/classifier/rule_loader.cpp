#include "classifier/rule_loader.h"

#include <fstream>
#include <sstream>

#include <toml++/toml.h>

namespace kforge::classifier {
using Kind = util::Error::Kind;

util::Result<std::vector<Rule>> RuleLoader::load_directory(
    const std::filesystem::path& rules_dir) {
    std::vector<Rule> all_rules;

    if (!std::filesystem::exists(rules_dir)) {
        return all_rules;  // No rules directory — use defaults
    }

    for (const auto& entry : std::filesystem::directory_iterator(rules_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".toml") continue;

        auto result = load_file(entry.path());
        if (result) {
            auto& loaded = *result;
            all_rules.insert(all_rules.end(),
                             std::make_move_iterator(loaded.begin()),
                             std::make_move_iterator(loaded.end()));
        }
    }

    // If no rules loaded, use defaults
    if (all_rules.empty()) {
        all_rules = default_rules();
    }

    return all_rules;
}

util::Result<std::vector<Rule>> RuleLoader::load_file(
    const std::filesystem::path& path) {
    std::vector<Rule> rules;

    try {
        auto table = toml::parse_file(path.string());

        auto rule_array = table["rule"].as_array();
        if (!rule_array) {
            // Try [[rules]] (plural)
            rule_array = table["rules"].as_array();
        }

        if (rule_array) {
            for (const auto& node : *rule_array) {
                auto* tbl = node.as_table();
                if (!tbl) continue;

                Rule rule;
                rule.name = tbl->get("name")->value_or("Unnamed Rule");
                rule.priority = tbl->get("priority")->value_or(100);
                rule.target_library = tbl->get("target")->value_or("");
                rule.confidence = tbl->get("confidence")->value_or(80);
                rule.enabled = tbl->get("enabled")->value_or(true);

                if (rule.target_library.empty()) continue;

                // Build condition from table
                Condition cond = PropertyExists{"name"};  // default: match all

                auto* cond_tbl = tbl->get("condition")->as_table();
                if (cond_tbl) {
                    // Check for type match
                    auto type_opt = cond_tbl->get("type")->value<std::string>();
                    if (type_opt) {
                        cond = MatchType{*type_opt};
                    }
                    // Check for package match
                    auto pkg_opt = cond_tbl->get("package")->value<std::string>();
                    if (pkg_opt) {
                        cond = MatchPackage{*pkg_opt};
                    }
                    // Pin count range
                    auto* pc = cond_tbl->get("min_pins");
                    if (pc) {
                        PinCountRange pcr;
                        pcr.min = pc->value_or(0);
                        pcr.max = cond_tbl->get("max_pins")->value_or(9999);
                        cond = pcr;
                    }
                    // Property exists
                    auto prop_opt = cond_tbl->get("has_property")->value<std::string>();
                    if (prop_opt) {
                        cond = PropertyExists{*prop_opt};
                    }
                }

                rule.condition = std::move(cond);
                rules.push_back(std::move(rule));
            }
        }
    } catch (const toml::parse_error& err) {
        return std::unexpected(util::Error::make<Kind::ParseError>(
            "TOML parse error in " + path.string() + ": " +
            std::string(err.what())));
    }

    return rules;
}

std::vector<Rule> RuleLoader::default_rules() {
    std::vector<Rule> rules;

    // Passive components
    rules.emplace_back("Resistors → Resistor_SMD", 10, "Resistor_SMD",
                       MatchType{"Resistor"}, 95);
    rules.emplace_back("Capacitors → Capacitor_SMD", 10, "Capacitor_SMD",
                       MatchType{"Capacitor"}, 95);
    rules.emplace_back("Inductors → Inductor_SMD", 10, "Inductor_SMD",
                       MatchType{"Inductor"}, 95);
    rules.emplace_back("Ferrite Beads → FerriteBead_SMD", 10, "FerriteBead_SMD",
                       MatchType{"FerriteBead"}, 90);
    rules.emplace_back("Diodes → Diode_SMD", 10, "Diode_SMD",
                       MatchType{"Diode"}, 90);
    rules.emplace_back("LEDs → LED_SMD", 10, "LED_SMD",
                       MatchType{"LED"}, 90);

    // ICs by pin count
    rules.emplace_back("Small ICs (2-8 pins) → IC_Small", 20,
                       "IC_Small_SMD",
                       PinCountRange{2, 8}, 80);
    rules.emplace_back("Medium ICs (9-28 pins) → IC_Medium", 20,
                       "IC_Medium_SMD",
                       PinCountRange{9, 28}, 80);
    rules.emplace_back("Large ICs (29+ pins) → IC_Large", 20,
                       "IC_Large_SMD",
                       PinCountRange{29, 9999}, 80);

    // Connectors
    rules.emplace_back("Connectors → Connector", 15, "Connector",
                       MatchType{"Connector"}, 85);
    rules.emplace_back("Switches → Switch", 15, "Switch",
                       MatchType{"Switch"}, 85);

    // Power
    rules.emplace_back("Voltage Regulators → Regulator", 15, "Regulator_SMD",
                       MatchType{"VoltageRegulator"}, 90);

    // Crystals/Oscillators
    rules.emplace_back("Crystals → Crystal_SMD", 15, "Crystal_SMD",
                       MatchType{"Crystal"}, 90);

    return rules;
}

}  // namespace kforge::classifier
