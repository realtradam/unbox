# ext-xdg-shell — package notes

**Tier:** core. **Manifest:** `{ id: "xdg-shell", tier: core, depends_on: {} }`.
Recreates the kernel's former tinywl-shape window management as an extension,
against the Host ABI alone. The xdg-shell v3 global is created HERE (the
extension-creates-the-global split), not by the kernel.

## Why it exists
The slice-4 kernel boots featureless: it owns input/output/scene/seat glue and
emits a typed catalogue, but names no shell policy. This unit is the minimal
shell that makes a session usable: toplevels appear, focus follows
click/tap/cycle, pointers and touch route to clients, windows move/resize on
request, and the dev keybindings (Ctrl+Alt+Backspace / Alt+F1) live here.

## Side-effect graph
- **Creates:** the `wlr_xdg_shell` v3 global on `host.display()`.
- **Subscribes (kernel catalogue):** `on_pointer_motion` (hit-test → seat
  enter/motion, default xcursor over nothing, drives grabs), `on_pointer_button`
  (forward `notify_button` + click-to-focus + grab begin/reset),
  `on_pointer_axis` (forward `notify_axis`), `on_pointer_frame` (notify_frame),
  `on_touch_down/motion/up` (tap-to-focus + down/up/motion notify),
  `on_touch_frame` (notify_frame), and `key_filter` (consume
  Ctrl+Alt+Backspace→terminate / Alt+F1→cycle, pass everything else).
- **Binds (raw xdg-shell signals, RAII `Listener`):** `new_toplevel`,
  `new_popup`, and per-entity map/unmap/commit/destroy/request_move/
  request_resize/request_maximize/request_fullscreen.
- **Emits (exported `Event`s, adopt()ed):** `on_toplevel_mapped`,
  `on_toplevel_unmapped`, `on_toplevel_focused`, each carrying a `Toplevel`
  borrow.
- **Provides (service):** `Service` (typed handle to the three Events).
- **Registers (typed surface→tree contract):** each toplevel's and popup's
  `wlr_surface` → its scene tree via `Host::host_surface()` (RAII handle held
  in the entity); resolves popup parents via `Host::scene_tree_for()`.

## Surface→scene-tree association (typed kernel contract)
The old cross-unit `wlr_surface.data` / `wlr_xdg_surface.data` convention is
DEAD. Cross-unit surface→tree coupling now routes through the kernel's typed
`Host::host_surface()` / `Host::scene_tree_for()` registry. `new_popup`
resolves its parent (our toplevel/ancestor popup OR a layer surface registered
by ext-layer-shell) uniformly through `scene_tree_for()`. We still set
`wlr_scene_tree.node.data = ToplevelEntry*` PRIVATELY — that is an intra-unit
back-pointer for `toplevel_at`, which the registry contract explicitly permits;
it is never read by another unit.

## Gotchas the headers can't express
- **All pointer forwarding is OURS.** The kernel forwards NOTHING to clients
  (corrected host.hpp): this extension calls
  `wlr_seat_pointer_notify_enter/motion/button/axis/frame` itself. Grabs
  suppress the forward by simply not notifying while a move/resize is in flight.
  (Forwarding button + axis was a real bug found hands-on in a nested session —
  click-drag selection and wheel scroll in foot were dead without it.)
- **Touch layout-origin-during-grab skew** (slice-2 parity): a touch point's
  surface origin is captured at down-time and assumed stationary; if the
  surface moves mid-touch (interactive grab) motion coords skew. Accepted until
  slice 5.
- **Terminate is `Ctrl+Alt+Backspace`** (the canonical X11 kill-the-server
  chord). It deliberately shares NO key with labwc's defaults — no Escape at all
  — after the user vetoed any overlap (even Alt+Shift+Escape was too close to
  labwc's `A-Escape` "Exit labwc"). Every Escape combo now passes THROUGH
  unconsumed; pure-core tests guard both that and the new chord.
- **Interactive move/resize grab is a pure state machine** (`policy::
  GrabMachine`), NOT an ad-hoc cursor-mode flag. The grab is a deterministic
  function of (button-down, client-requested-move/resize): it engages ONLY
  while the button is held, every held motion moves/resizes (suppressing the
  client pointer notify), and a button RELEASE always ends it. This kills the
  user-observed bug where a titlebar drag didn't move while held but then
  followed the cursor unclicked after release (the grab's lifetime had been
  decoupled from the button; a late `request_move` could engage post-release).
  The glue feeds press/release/request/motion in and executes the returned
  action; the geometry (grab origin, resize box/edges) lives in the glue.
- **Alt+F1 cycle focuses the back of the focus order** (least-recently
  focused), matching the former kernel; the picked window then moves to front,
  so repeated presses walk the stack. The binding key is consumed even with
  fewer than two windows (no client should see a half-handled compositor combo).
- Teardown is pure RAII (reverse declaration order); there is no manual
  cleanup. The `wlr_xdg_shell` global and scene nodes are display/scene-owned
  and outlive nothing of ours improperly.
