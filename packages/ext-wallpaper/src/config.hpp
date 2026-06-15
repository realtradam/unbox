#pragma once

#include <string>
#include <string_view>
#include <vector>

// Pure decision core: the wallpaper config loader. Parses an unbox.toml
// document (as TEXT — file discovery/reading is an effect kept in the glue)
// into a WallpaperConfig. toml++ is the only dependency; no wlroots, no
// kernel. Doctest-covered in tests/test_config.cpp.
//
// SCHEMA (the [wallpaper] table; every key optional — absent => default):
//   [wallpaper]
//   path  = "/home/me/pic.jpg"   # absolute path to an image file; empty/missing = use default
//   fit   = "cover"              # cover | contain | stretch | center  (default cover)
//   color = "#1e1e2e"            # solid fill behind/around the image (default "#000000")
//
// DEFAULT IMAGE FALLBACK: when path is absent or empty, the GLUE resolves the
// bundled default image: <asset_root>/ext-wallpaper/default.jpg, where
// asset_root = $UNBOX_ASSET_DIR (env) ?? UNBOX_ASSET_DIR_DEFAULT (compiled) ?? ".".
// If the resolved file does not exist, the surface shows only the solid color.
// The pure helper `default_image_path(asset_root)` builds the path from a given
// root string (no I/O; the glue reads the env and checks existence).
//
// fit semantics (maps to RmlUi image-decorator fit keywords):
//   cover    -> cover       (crop to fill the surface, no bars)
//   contain  -> contain     (letterbox, keeps aspect)
//   stretch  -> fill        (distort to fill)
//   center   -> scale-none  (natural size, centred)
// An unknown fit value falls back to cover with a warning.
// A bad color value falls back to "#000000" with a warning.

namespace unbox::ext_wallpaper::config {

// How the image is fitted into the surface.
enum class FitMode { cover, contain, stretch, center };

// The parsed wallpaper configuration. Defaults are the compiled-in fallback
// used when no config / no [wallpaper] table / a bad value is present.
struct WallpaperConfig {
    std::string path{};             // absolute path to the image; empty = no image
    FitMode fit = FitMode::cover;   // default: cover
    std::string color{"#000000"};   // background colour (CSS hex); default black
};

// The outcome of loading. `cfg` is always populated (defaults where absent or
// invalid). `warnings` holds one message per bad value or a single parse-error
// message. `parse_error` is true iff the document failed to parse (toml syntax
// error) — then `cfg` is the pure default. NEVER throws (toml parse error is
// caught here). An empty document / absent [wallpaper] is NOT an error — it
// yields defaults with parse_error=false and no warnings.
struct LoadResult {
    WallpaperConfig cfg;
    std::vector<std::string> warnings;
    bool parse_error = false;
};

// Parse `toml_text` and extract the [wallpaper] config per the schema above.
// Each key is validated independently: an unknown/non-string fit, a
// non-string path, a non-string/empty color falls back to that field's
// default with a warning and never aborts the rest.
[[nodiscard]] auto load_from_string(std::string_view toml_text) -> LoadResult;

// Map FitMode to the RmlUi image-decorator fit keyword.
//   cover   -> "cover"
//   contain -> "contain"
//   stretch -> "fill"
//   center  -> "scale-none"
[[nodiscard]] auto rmlui_fit_keyword(FitMode fit) -> std::string_view;

// Pure path-join: given the asset root directory (no trailing slash required),
// return the absolute path of the bundled default wallpaper image:
//   <asset_root>/ext-wallpaper/default.jpg
// No I/O; no env reads — the caller (glue) supplies the root and checks existence.
[[nodiscard]] auto default_image_path(std::string_view asset_root) -> std::string;

} // namespace unbox::ext_wallpaper::config
