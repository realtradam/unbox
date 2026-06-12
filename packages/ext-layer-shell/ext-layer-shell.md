# ext-layer-shell

wlr-layer-shell-unstable-v1 (protocol **version 5**, the wlroots-0.20 cap) for
**external** clients: panels, launchers, wallpapers, on-screen keyboards, and
the crash-isolation escape hatch from `notes/plan.md` §2. unbox's own RMLUi ui
substrate does **not** go through layer-shell — this protocol exists so foreign
processes can paint the desktop's edges.

Tier `core`, manifest id `layer-shell`, no dependencies. `activate(Host&)`
creates the `wlr_layer_shell_v1` global on `host.display()`.

## Why it exists
The kernel names no concrete protocol. Layer-shell is shell *policy* (which
edge a panel reserves, which z-band it lands in), so it is an extension. The
extension-creates-the-global split keeps the kernel featureless.

## Side-effect graph
- **Creates:** the `wlr_layer_shell_v1` global (one, at activation).
- **Subscribes (kernel events):** `on_output_added` / `on_output_removed` —
  to track the output set (assign one to outputless surfaces; re-arrange and
  evict on output loss). Plus a one-shot enumeration of already-existing outputs
  (`host.output_layout()->outputs`) at activate, since outputs predate
  activation (see Gotchas).
- **Binds (wlroots signals, via RAII `Listener`):** shell `new_surface`; per
  surface its `wlr_surface.commit`, layer-surface `destroy`, and `new_popup`.
- **Drives:** `wlr_scene_layer_surface_v1_configure` on every commit and output
  change, attaching each surface's scene node under the kernel `SceneLayer`
  band matching its protocol layer (background/bottom/top/overlay map 1:1;
  `normal` is toplevels-only and never used here).
- **Emits hooks:** none yet (see *Deferred*).

## Surface → scene-tree association (typed kernel contract)
For each layer surface we register its `wlr_surface` → our `wlr_scene_tree` via
`Host::host_surface()`, holding the move-only `SurfaceRegistration` as a member
of the `LayerSurface` (it unregisters on destruction). ext-xdg-shell resolves a
popup's parent surface to our tree via `Host::scene_tree_for()`, so xdg popups
parented to a layer surface attach correctly. This is the kernel-owned **typed**
replacement for the old `wlr_surface.data` convention (now dead) — cross-unit
surface→tree coupling routes through this contract, never through `.data`.

## Pure core
`include/unbox/ext-layer-shell/arrangement.hpp` — `Box`, `SurfaceState`,
`exclusive_edge()`, `apply_exclusive()`. Zero wlroots types; the independent,
doctest-hard mirror of the usable-area bookkeeping that
`wlr_scene_layer_surface_v1_configure` performs. It is what tiling (slice 7)
will read for per-output usable area. The glue keeps a per-output `Box` updated
from the helper's `usable_area` out-param using this model's coordinate
convention.

## What was deferred (intentional)
- **`on_demand` keyboard interactivity:** only `exclusive` (focus on map) and
  `none` (leave alone) are honored. `on_demand` needs slice 5's input routing
  (click-to-focus a layer surface) and is a documented TODO.
- **A typed usable-area service / `usable-area-changed` Event:** not exported.
  The per-output `Box` is computed and held internally; publishing it is left
  to the consumer that actually needs it (tiling) so the contract is shaped by
  a real caller, not guessed. Noted as a deliberate deferral.
- **Popup glue beyond the registration:** `new_popup` is bound but does no
  extra work; wlroots' scene helper wires popup nodes once a consumer resolves
  the parent via `Host::scene_tree_for()` against our `host_surface()`
  registration.

## Gotchas
- **Seed outputs at activate, do not rely on events alone.** `Server::create()`
  starts the backend, so outputs exist BEFORE extensions activate; their
  `on_output_added` already fired. We enumerate `host.output_layout()->outputs`
  in `activate()` to catch them — events-only tracking left `outputs_` empty and
  silently broke every output-less client (the fuzzel "no configure" bug). The
  underlying Host contract gap (late subscribers miss state) is a standing
  change-request in `reports/ext-layer-shell.md`.
- **Never destroy the scene node in the layer-surface destroy handler.**
  `wlr_scene_layer_surface_v1` installs its own internal destroy listener that
  frees the scene tree; calling `wlr_scene_node_destroy` ourselves is a
  use-after-free (signal-emit order between the two listeners is unspecified).
  Our destroy handler only reclaims the usable area and erases the
  `LayerSurface`.
- An output-less surface arriving when **no** output exists yet is **parked**
  (`pending_`, destroy-listener only) and placed once an output appears — not
  closed. We only close on a hard failure (`wlr_scene_layer_surface_v1_create`
  returning null).
- The destroy handler's **last** action is `owner.erase(this)`, which deletes
  the `LayerSurface`; copy any needed value (output, owner ref) into locals
  first — nothing may touch members afterwards (listener-lifetime).
