#pragma once

#include <unbox/kernel/extension.hpp>

#include <memory>
#include <optional>
#include <string>

// ext-keybindings — config-driven compositor keybindings as a CORE extension.
//
// The first step to a usable DE. Two user-facing features plus the compositor
// shortcuts ext-xdg-shell used to hardcode:
//   * Tap the Super (logo) key alone -> spawn an external launcher (fuzzel).
//   * Alt+Tab / Alt+Shift+Tab -> rotate keyboard focus across ALL mapped
//     windows (forward / back, wrapping), in STABLE mapping order — not MRU.
//   * Alt+F1 -> focus-next, Ctrl+Alt+Backspace -> quit (the shortcuts
//     ext-xdg-shell drops this wave; preserved here as compiled defaults).
//
// Tier: core. Manifest id "keybindings", depends_on {"xdg-shell"} — it consumes
// ext-xdg-shell's Service (window focus + toplevel lifecycle events) for the
// focus ring, fetched in activate() via host.service<ext_xdg_shell::Service>().
// Input arrives through the kernel's key_filter (a Filter<KeyEvent>): a matched
// binding sets handled=true to consume the key before it reaches the client.
//
// This header is the unit's WHOLE cross-extension contract: a factory only
// (mirroring ext-layer-shell). It exports NO hooks or services — it is a leaf
// consumer. Single wl_event_loop thread throughout.

namespace unbox::ext_keybindings {

// Construct the extension (ownership transfer to the caller; host-bin installs
// it via Server::install). Construction is cheap and side-effect free per the
// Extension contract; ALL wiring — config load, key_filter subscription,
// xdg-shell event subscriptions — happens in activate().
//
// config_path: the explicit unbox.toml path (host-bin --config). If nullopt,
// activate() discovers $XDG_CONFIG_HOME/unbox/unbox.toml then
// ~/.config/unbox/unbox.toml. If no file is found OR it fails to parse OR it
// contains no valid bindings, the extension logs and degrades to the compiled-in
// DEFAULTS (Super->spawn fuzzel, Alt+Tab/Alt+Shift+Tab focus rotation,
// Alt+F1->focus-next, Ctrl+Alt+Backspace->quit). A bad/missing config NEVER
// throws out of activate(); the ONLY fatal (a thrown exception) is a missing
// ext-xdg-shell Service, which is a broken core session.
[[nodiscard]] auto create(std::optional<std::string> config_path = std::nullopt)
    -> std::unique_ptr<kernel::Extension>;

} // namespace unbox::ext_keybindings
