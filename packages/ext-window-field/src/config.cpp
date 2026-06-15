#include "config.hpp"

#include <toml++/toml.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Pure toml loader for the [window-field] policy. No wlroots, no kernel, no I/O
// (the glue reads the file and hands us the text). Mirrors ext-keybindings'
// config.cpp posture: per-key validation, defaults on absence/invalid, every
// problem recorded as a warning, never throws.

namespace unbox::ext_window_field::config {

namespace {

// Map a resize_mode string to the enum. nullopt for an unknown value (the caller
// warns + keeps the default).
auto parse_mode(std::string_view s) -> std::optional<ResizeMode> {
    if (s == "off") {
        return ResizeMode::off;
    }
    if (s == "settle") {
        return ResizeMode::settle;
    }
    if (s == "continuous") {
        return ResizeMode::continuous;
    }
    if (s == "debounced") {
        return ResizeMode::debounced;
    }
    return std::nullopt;
}

} // namespace

auto load_from_string(std::string_view toml_text) -> LoadResult {
    LoadResult result; // policy defaults already set

    toml::table tbl;
    try {
        tbl = toml::parse(toml_text);
    } catch (const toml::parse_error& e) {
        result.parse_error = true;
        result.warnings.emplace_back(std::string("[window-field] config parse error: ") +
                                     std::string(e.description()));
        return result; // defaults stand
    }

    const toml::node* section = tbl.get("window-field");
    if (section == nullptr) {
        return result; // no table => defaults, not an error
    }
    const toml::table* wf = section->as_table();
    if (wf == nullptr) {
        result.warnings.emplace_back("[window-field] is not a table; using defaults");
        return result;
    }

    // resize_mode (string enum).
    if (const toml::node* m = wf->get("resize_mode"); m != nullptr) {
        if (const auto* s = m->as_string()) {
            if (const auto parsed = parse_mode(s->get())) {
                result.policy.mode = *parsed;
            } else {
                result.warnings.emplace_back(
                    "[window-field] resize_mode '" + s->get() +
                    "' is not one of off|settle|continuous|debounced; using 'settle'");
            }
        } else {
            result.warnings.emplace_back(
                "[window-field] resize_mode must be a string; using 'settle'");
        }
    }

    // resize_debounce_ms (non-negative integer).
    if (const toml::node* d = wf->get("resize_debounce_ms"); d != nullptr) {
        if (const auto* v = d->as_integer()) {
            const std::int64_t ms = v->get();
            if (ms < 0) {
                result.warnings.emplace_back(
                    "[window-field] resize_debounce_ms must be >= 0; using 100");
            } else {
                result.policy.debounce_ms = static_cast<int>(ms);
            }
        } else {
            result.warnings.emplace_back(
                "[window-field] resize_debounce_ms must be an integer; using 100");
        }
    }

    return result;
}

} // namespace unbox::ext_window_field::config
