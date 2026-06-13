#include "config.hpp"

#include <string>

// toml++ is consumed via its PREBUILT subproject library, which is compiled with
// exceptions enabled (toml::v3::ex::parse). We therefore use the throwing parse
// and catch toml::parse_error HERE so a syntax error becomes a clean
// parse_error result, never an exception escaping into activate() (the brief's
// hard rule). Matching the lib's exception mode avoids the toml::v3::noex link
// mismatch that TOML_EXCEPTIONS=0 in this TU would cause.
#include <toml++/toml.hpp>

namespace unbox::ext_keybindings::config {

auto load_from_string(std::string_view toml_text) -> LoadResult {
    LoadResult result;

    toml::table root;
    try {
        root = toml::parse(toml_text);
    } catch (const toml::parse_error& err) {
        result.parse_error = true;
        std::string msg = "unbox.toml parse error: ";
        msg += std::string(err.description());
        result.warnings.push_back(std::move(msg));
        return result;
    }

    const toml::node* keybind_node = root.get("keybind");
    if (keybind_node == nullptr) {
        // No [[keybind]] at all: not an error, just zero bindings. The glue
        // falls back to defaults.
        result.warnings.emplace_back("unbox.toml has no [[keybind]] entries");
        return result;
    }

    const toml::array* entries = keybind_node->as_array();
    if (entries == nullptr) {
        result.warnings.emplace_back("'keybind' must be an array of tables ([[keybind]])");
        return result;
    }

    std::size_t idx = 0;
    for (const toml::node& node : *entries) {
        const std::string where = "keybind #" + std::to_string(idx);
        ++idx;

        const toml::table* entry = node.as_table();
        if (entry == nullptr) {
            result.warnings.push_back(where + ": not a table");
            continue;
        }

        // keys (required, string).
        const toml::node* keys_node = entry->get("keys");
        if (keys_node == nullptr || !keys_node->is_string()) {
            result.warnings.push_back(where + ": missing or non-string 'keys'");
            continue;
        }
        const std::string keys = keys_node->value<std::string>().value();

        // action (required, string).
        const toml::node* action_node = entry->get("action");
        if (action_node == nullptr || !action_node->is_string()) {
            result.warnings.push_back(where + ": missing or non-string 'action'");
            continue;
        }
        const std::string action_str = action_node->value<std::string>().value();
        const auto action = policy::action_from_string(action_str);
        if (!action) {
            result.warnings.push_back(where + ": unknown action '" + action_str + "'");
            continue;
        }

        // command (required iff action == spawn; string when present).
        std::string command;
        const toml::node* command_node = entry->get("command");
        if (command_node != nullptr) {
            if (!command_node->is_string()) {
                result.warnings.push_back(where + ": 'command' must be a string");
                continue;
            }
            command = command_node->value<std::string>().value();
        }
        if (*action == policy::Action::spawn && command.empty()) {
            result.warnings.push_back(where + ": action 'spawn' requires a non-empty 'command'");
            continue;
        }

        // combo (validated last so a bad combo skips with a clear message).
        const auto combo = policy::parse_combo(keys);
        if (!combo) {
            result.warnings.push_back(where + ": malformed key combo '" + keys + "'");
            continue;
        }

        result.bindings.push_back(policy::Binding{
            .combo = *combo, .action = *action, .command = std::move(command)});
    }

    return result;
}

} // namespace unbox::ext_keybindings::config
