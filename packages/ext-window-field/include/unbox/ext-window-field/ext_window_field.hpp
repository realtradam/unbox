#pragma once

#include <unbox/kernel/extension.hpp>

#include <memory>
#include <optional>
#include <string>

// ext-window-field — RML compositing's window manager (GLOSSARY: "window
// field"). The CORE extension that composites application toplevels as RCSS
// SURFACE ELEMENTS inside ONE ui surface, rather than as wlr_scene nodes. It:
//   - subscribes to ext-xdg-shell's toplevel map/unmap/focus (typed Service),
//   - turns each toplevel's root wl_surface (Toplevel::wl_surface()) into a
//     kernel SurfaceElement (UiSubstrate::create_surface_element),
//   - lists the windows into its own ui surface's RML document (a data-bound
//     list of live source_uri()s) and lets RCSS lay them out + animate,
//   - drives focus policy (click-to-focus -> SurfaceElement::focus_keyboard() +
//     Toplevel::focus()), and takes each toplevel OUT of the wlr_scene composite
//     (Toplevel::hide()) while it owns the window field.
//
// Layout and animation live entirely in RCSS (the user's contract decision);
// this extension only DRIVES the document (which windows exist + their data) and
// focus — it never computes on-screen geometry imperatively.
//
// This header is the unit's WHOLE cross-extension contract: a factory only
// (a leaf — exports no hooks/services yet; a future tiling/stage policy is RCSS
// + bound data on the same field). host-bin installs it ONLY when RML
// compositing is enabled. Manifest: { id "window-field", tier core,
// depends_on "xdg-shell" }. Single wl_event_loop thread throughout.

namespace unbox::ext_window_field {

// Construct the extension. Cheap and side-effect free (per the Extension
// contract); ALL wiring happens in activate(). Ownership transfers to the
// caller (host-bin installs it into the Server).
//
// config_path: the explicit unbox.toml path (host-bin --config). If nullopt,
// activate() discovers $XDG_CONFIG_HOME/unbox/unbox.toml then
// ~/.config/unbox/unbox.toml. The [window-field] table tunes how a window is
// resized to its tile (resize_mode / resize_debounce_ms — see src/config.hpp);
// a missing/malformed config falls back to the compiled defaults (resize_mode
// "settle"). The effective file is watched for live hot-reload.
[[nodiscard]] auto create(std::optional<std::string> config_path = std::nullopt)
    -> std::unique_ptr<kernel::Extension>;

} // namespace unbox::ext_window_field
