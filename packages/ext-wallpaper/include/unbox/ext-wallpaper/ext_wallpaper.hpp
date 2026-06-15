#pragma once

#include <unbox/kernel/extension.hpp>

#include <memory>
#include <optional>
#include <string>

// ext-wallpaper — config-driven desktop background image (GLOSSARY: "wallpaper").
// A standard-tier extension that composites a solid colour and/or an image file
// at SceneLayer::background, below every application window. The image and its
// fit mode are driven by the [wallpaper] section of unbox.toml; the effective
// config file is hot-watched so edits take effect without a restart.
//
// DEFAULT IMAGE FALLBACK: when [wallpaper] path is absent or empty, the
// extension uses the bundled default image:
//   <asset_root>/ext-wallpaper/default.jpg
// where asset_root = $UNBOX_ASSET_DIR ?? UNBOX_ASSET_DIR_DEFAULT ?? ".".
// If that file does not exist (e.g. running uninstalled with no UNBOX_ASSET_DIR),
// the surface shows only the solid color — no crash, one log message.
//
// The surface is ALWAYS input_transparent (never steals clicks from windows
// above it) and sized to the primary output.
//
// This header is the unit's ENTIRE cross-extension contract: a factory only
// (a leaf — exports no hooks or services). Manifest:
//   { id "wallpaper", tier standard, depends_on {} }
//
// Single wl_event_loop thread throughout.

namespace unbox::ext_wallpaper {

// Construct the extension. Cheap and side-effect free (per the Extension
// contract); ALL wiring happens in activate().
//
// config_path: the explicit unbox.toml path (host-bin --config). If nullopt,
// activate() discovers $XDG_CONFIG_HOME/unbox/unbox.toml then
// ~/.config/unbox/unbox.toml (same discovery as ext-window-field). The
// [wallpaper] table sets image path, fit mode, and background colour; a
// missing/malformed config falls back to compiled defaults (no image, black
// background, fit=cover). The effective file is watched for live hot-reload
// (drop + recreate the ui surface with baked values on change).
[[nodiscard]] auto create(std::optional<std::string> config_path = std::nullopt)
    -> std::unique_ptr<kernel::Extension>;

} // namespace unbox::ext_wallpaper
