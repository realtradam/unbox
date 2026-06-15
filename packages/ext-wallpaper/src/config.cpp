#include "config.hpp"

#include <toml++/toml.hpp>

#include <optional>
#include <string>
#include <string_view>

// Pure toml loader for the [wallpaper] config. No wlroots, no kernel, no I/O
// (the glue reads the file and hands us the text). Per-key validation, defaults
// on absence/invalid, every problem recorded as a warning, never throws.

namespace unbox::ext_wallpaper::config {

namespace {

// Map a fit string to the enum. nullopt for an unknown value.
auto parse_fit(std::string_view s) -> std::optional<FitMode> {
    if (s == "cover") {
        return FitMode::cover;
    }
    if (s == "contain") {
        return FitMode::contain;
    }
    if (s == "stretch") {
        return FitMode::stretch;
    }
    if (s == "center") {
        return FitMode::center;
    }
    return std::nullopt;
}

// Very light CSS hex colour validation: must start with '#' and have 6 or 3
// hex digits following (e.g. "#1e1e2e" or "#fff"). Returns true if valid.
auto is_valid_color(std::string_view s) -> bool {
    if (s.empty() || s[0] != '#') {
        return false;
    }
    const std::string_view digits = s.substr(1);
    if (digits.size() != 6 && digits.size() != 3) {
        return false;
    }
    for (const char c : digits) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

} // namespace

auto load_from_string(std::string_view toml_text) -> LoadResult {
    LoadResult result; // cfg defaults already set

    toml::table tbl;
    try {
        tbl = toml::parse(toml_text);
    } catch (const toml::parse_error& e) {
        result.parse_error = true;
        result.warnings.emplace_back(std::string("[wallpaper] config parse error: ") +
                                     std::string(e.description()));
        return result; // defaults stand
    }

    const toml::node* section = tbl.get("wallpaper");
    if (section == nullptr) {
        return result; // no table => defaults, not an error
    }
    const toml::table* wp = section->as_table();
    if (wp == nullptr) {
        result.warnings.emplace_back("[wallpaper] is not a table; using defaults");
        return result;
    }

    // path (string; empty or absent = no image — not an error).
    if (const toml::node* p = wp->get("path"); p != nullptr) {
        if (const auto* s = p->as_string()) {
            result.cfg.path = s->get(); // may be empty — glue treats that as "no image"
        } else {
            result.warnings.emplace_back("[wallpaper] path must be a string; using empty (no image)");
        }
    }

    // fit (string enum; unknown -> cover + warn).
    if (const toml::node* f = wp->get("fit"); f != nullptr) {
        if (const auto* s = f->as_string()) {
            if (const auto parsed = parse_fit(s->get())) {
                result.cfg.fit = *parsed;
            } else {
                result.warnings.emplace_back(
                    "[wallpaper] fit '" + s->get() +
                    "' is not one of cover|contain|stretch|center; using 'cover'");
            }
        } else {
            result.warnings.emplace_back(
                "[wallpaper] fit must be a string; using 'cover'");
        }
    }

    // color (CSS hex string; bad value -> default + warn).
    if (const toml::node* c = wp->get("color"); c != nullptr) {
        if (const auto* s = c->as_string()) {
            if (is_valid_color(s->get())) {
                result.cfg.color = s->get();
            } else {
                result.warnings.emplace_back(
                    "[wallpaper] color '" + s->get() +
                    "' is not a valid CSS hex colour (e.g. \"#1e1e2e\"); using '#000000'");
            }
        } else {
            result.warnings.emplace_back(
                "[wallpaper] color must be a string; using '#000000'");
        }
    }

    return result;
}

auto rmlui_fit_keyword(FitMode fit) -> std::string_view {
    switch (fit) {
        case FitMode::cover:   return "cover";
        case FitMode::contain: return "contain";
        case FitMode::stretch: return "fill";
        case FitMode::center:  return "scale-none";
    }
    return "cover"; // unreachable but keeps the compiler happy
}

auto default_image_path(std::string_view asset_root) -> std::string {
    // Strip a trailing slash from asset_root (if any) for clean joining.
    std::string root{asset_root};
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }
    return root + "/ext-wallpaper/default.jpg";
}

} // namespace unbox::ext_wallpaper::config
