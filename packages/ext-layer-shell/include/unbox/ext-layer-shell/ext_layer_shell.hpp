#pragma once

#include <unbox/kernel/extension.hpp>

#include <memory>

// ext-layer-shell — wlr-layer-shell-unstable-v1 (version 5) for EXTERNAL
// clients: panels, launchers, wallpapers, on-screen keyboards, and the
// crash-isolation escape hatch (notes/plan.md §2). unbox's OWN ui substrate
// does NOT go through layer-shell.
//
// Tier: core. Manifest id "layer-shell", no dependencies. The extension owns
// the wlr_layer_shell_v1 global (created on host.display() in activate()), maps
// each protocol layer 1:1 onto a kernel SceneLayer band (background/bottom/top/
// overlay; `normal` is toplevels-only and never used here), and keeps each
// output's usable area up to date as anchored/exclusive surfaces come and go.
//
// This header is the unit's WHOLE cross-extension contract: a factory only. The
// arrangement math (anchors, margins, exclusive-zone accumulation) is the pure
// core in <unbox/ext-layer-shell/arrangement.hpp> — depend on THAT, not on this,
// if you only need the usable-area model (tiling, slice 7).
//
// Single wl_event_loop thread throughout. Ownership of the returned extension
// transfers to the caller (host-bin installs it into the Server).

namespace unbox::ext_layer_shell {

// Construct the extension. Cheap and side-effect free (per the Extension
// contract); ALL wiring — global creation, signal binding, output tracking —
// happens in activate(). host-bin installs the returned object via
// Server::install(); the kernel calls activate() in topological order.
//
// Ownership: unique_ptr = transfer to the caller. The object must outlive the
// session and is destroyed (RAII teardown of the global, listeners, and scene
// nodes) at shutdown.
[[nodiscard]] auto create() -> std::unique_ptr<kernel::Extension>;

} // namespace unbox::ext_layer_shell
