# RML compositing — Phase 1 architecture (the design doc)

> **Status: ACTIVE design, gated only by Phase-2 implementation.** Phase 0 (the
> spike) **PASSED GO** on the real CF-AX3 GPU class — see
> `reports/rml-compositing-spike.md`. This doc settles the architecture the
> direction note (`notes/rml-compositing.md`) deferred to Phase 1. Phase 2 then
> implements it wave-by-wave, behind a flag, per `ORCHESTRATOR.md`.

## 0. Inputs that are already settled (do not relitigate)
- **GO**: live zero-copy import, RCSS 3D transform on live pixels, input accurate
  through the transform, per-subsurface surface trees, idle dirty-gate, and the
  FBO→dmabuf→`wlr_scene_buffer` present path all proven on Haswell+crocus.
- **Perf reality (Phase 0 Stage-0 measurement)**: ~30fps under a 4-window load,
  **fill-bound** — the whole-output composite is ~10–15ms GPU, CPU work ~2ms,
  present dominated by the fence wait. Damage limiting is the recovery lever.
- **Surface-tree answer (spike §3)**: **per-subsurface elements** by default, with
  a **per-window render-to-texture (RTT) hook** for the one case that needs it
  (a single effect that must treat a whole window tree as one flat surface —
  genie warp, cross-seam blur).
- **Contract decision (user)**: **RCSS is the single source of truth for ALL
  layout and animation.** C++/extensions DRIVE the document (what exists, which
  data, which classes) through a **typed** substrate API, but never own placement
  geometry imperatively. Tiling, stage-manager, floating, effects = RCSS.
- **Naming (GLOSSARY)**: *RML compositing* (the approach), *surface element* (an
  RML element backed by a live client surface's shared texture).

## 1. The contract principle (Option 2), reconciled with the constitution
AGENTS.md forbids **string-keyed cross-extension dependency discovery** ("a
missing dependency must be a compile/link error"). RCSS-as-layout does **not**
violate this: that rule governs how *units find each other*, not the styling
vocabulary. So:

- **Units couple through typed C++ symbols** (service handles, hook descriptors,
  the substrate API) — unchanged. A missing `ext-xdg-shell` is still a link error.
- **Within a surface, layout/animation is RCSS.** C++ pushes *data* (typed
  `bind_*`/`bind_list_*`) and *intent* (set a class, dirty a binding); the
  document decides geometry and tweens. This is already how `UiSurface` works
  (`notes`/the `ui.hpp` contract) — Phase 1 does not invent a paradigm, it adds a
  **live** surface primitive and **input-back** to the existing one.

Litmus: an extension may never read another extension's state by string name, and
may never compute a window's on-screen rectangle and command "draw it there." It
provides the window list + per-window data; RCSS lays them out and animates.

## 2. What already exists and is REUSED verbatim
The `kernel` `UiSubstrate`/`UiSurface` contract (`packages/kernel/include/unbox/
kernel/ui.hpp`) already provides everything Option 2 needs **except live windows**:
- `create_surface(UiSurfaceSpec)` → a `UiSurface` = one RML document = one
  composited node; per-pixel alpha; `SceneLayer`.
- Typed data bindings: `bind_int/double/bool/string`, **`bind_list` +
  `bind_list_string/int/...` + `bind_list_event`** (the list pattern), `dirty()`.
- Interaction: `bind_event`, **`bind_drag`** (captured drag stream in surface-local
  px), `on_touch_mode_changed`.
- **`transition_timing(element_id, property)`** — read RCSS-authored
  duration/delay/easing from C++ so animators reuse hot-reloadable RCSS values.
- **`Preview`** = a FROZEN toplevel snapshot imported as a texture, shown via
  `<img src=source_uri()>` in any ui surface. `create_preview(wlr_scene_tree*)`.
- Dev **hot-reload** of RML/RCSS, error-isolated.

**Phase 1 = make `Preview` LIVE + route input back into it.** That is the whole
new mechanism; the layout/animation/contract machinery is already shipped.

## 3. New kernel primitive: the live surface element
A live analogue of `Preview`. Proposed contract (in `ui.hpp`, kernel-owned):

    class SurfaceElement {                 // GLOSSARY: "surface element"
    public:
      // The <img src> URI resolving to this surface's LIVE shared texture inside
      // any ui surface of this substrate (e.g. "unbox-surface://7"). Stable for life.
      virtual auto source_uri() const -> std::string = 0;
      virtual auto width()  const -> int = 0;   // current surface px (tracks commits)
      virtual auto height() const -> int = 0;
      // NO refresh(): unlike Preview, this updates itself every client commit
      // (seq-gated re-import) and drives the client's frame callbacks.
      virtual ~SurfaceElement() = default;
    };

    // On UiSubstrate:
    virtual auto create_surface_element(wlr_surface* client)   // a BORROW
        -> std::unique_ptr<SurfaceElement> = 0;

Semantics (all proven in the spike, generalized from `spike_gl.hpp`):
- **Zero-copy, seq-gated**: re-imports the client's current buffer only when
  `wlr_surface_state.seq` advances (pool-reuse-proof); double-buffered
  `wlr_buffer_lock`/unlock; idle client ⇒ zero work.
- **Drives the client loop**: the substrate sends `wl_surface` frame callbacks
  each composited frame (a live element, unlike `Preview`, is responsible for the
  client's progress — the spike's "stuck-frame" fix).
- **Surface tree** (see §6): one `create_surface_element(toplevel root)` manages
  the toplevel + its subsurfaces + popups as **child elements**, each its own live
  texture at its tree offset.
- **Lifetime**: owned by the contributing extension via `unique_ptr`; destroying
  it drops the import + frame-callback duty. The `wlr_surface*` is a borrow valid
  until the owner drops the element (extensions already track map/unmap).

Wallpaper/layer-shell surfaces use the **same** `create_surface_element` (spike
criterion 5).

## 4. The compositor document + window-layout model (RCSS-driven)
This is the heart of Option 2. Windows are not per-window scene nodes; they are
**surface elements inside a ui surface document**, laid out by RCSS.

- A window-management extension (today `ext-xdg-shell`; later a tiling/stage
  extension) owns **one ui surface** at `SceneLayer` for app content (call it the
  *window field*). It does NOT compute geometry.
- It feeds windows through the **existing list binding**: `bind_list("wins", …)`
  with per-row fields — crucially a `live_uri` string field returning each
  window's `SurfaceElement::source_uri()`, plus whatever the RCSS layout keys off
  (focused bool, app_id string, a layout-slot int/percent, z-order, etc.).
- The RML authors the layout:

      <div data-model="wm">
        <div class="field tiling">                 <!-- class chosen by C++ intent -->
          <div data-for="w : wins" class="win"
               data-class-focused="w.focused"
               style="--slot: {{ w.slot }};">
            <img src="{{ w.live_uri }}"/>
          </div>
        </div>
      </div>

  Tiling = RCSS flex/grid keyed on `--slot`; stage-manager = the same list under a
  `.stage` class with `transform`/perspective per card; floating = absolutely
  positioned from bound `x/y`. **Switching layout = swapping a class / changing
  bound data**, animated by RCSS `transition` — the user's "everything is laid out
  and animated in RCSS."
- **Animation timing** comes from RCSS; C++ that must coordinate (e.g. a gesture)
  reads it via `transition_timing()` (already shipped) and drives progress with
  bound values — never hand-rolled geometry.

This makes tiling/stage/effects *policies expressed as RML+RCSS + a window list*,
exactly the constitution's "kernel names no feature."

## 5. Unified input (pick → wl_seat), folded into the substrate
The substrate already routes `data-event*`/`bind_drag` for ui surfaces. Phase 1
adds **client input-back** for surface elements:
- On pointer/touch, the substrate feeds the screen point to `Context::Process*`
  (transform-aware pick). If the hovered element is a surface element, it maps the
  point to surface-local via **`Element::Project()`** (the spike's fix — projects
  through the element's real 3D transform, no-op when untransformed) and forwards
  via `wlr_seat_pointer/touch_notify_*`.
- Keyboard focus follows the focused window (the wm extension calls a focus path;
  `wlr_seat_keyboard_notify_*`).
- **Cursor stays a wlr hardware plane**, never drawn in RMLUi (recompose-on-move
  would be fatal).
- Implicit grab / click-to-focus stays wm-extension policy; the substrate only
  provides the pick→local→seat translation as a typed primitive.

Contract sketch (kernel): a surface element created from a `wlr_surface` is
**automatically** input-routed by the substrate (it knows the element↔surface
map); the wm extension does not wire seat calls itself. This subsumes
`ext-xdg-shell`'s current pointer/touch routing.

## 6. Surface trees: per-subsurface + RTT hook
- Default: `create_surface_element(root)` builds **one child element per
  subsurface/popup**, positioned at its tree offset; DOM order = composite order;
  popups are not parent-clipped (own absolutely-positioned elements). This is the
  spike's criterion-4 result.
- A small **"place child relative to parent's resolved box"** layout helper is
  needed so a moving/transformed parent drags its children (spike §3 edge note) —
  pure layout glue.
- **RTT hook** (do not build until an effect needs it): a per-element opt-in that
  flattens a window's whole tree to one texture (RmlUi `SaveLayerAsTexture`) so a
  tree-spanning effect transforms/filters one surface. Element-level policy, not a
  global mode.

## 7. Present + performance posture
- **Present path**: reuse Phase-0 Plan A — RMLUi composites into an FBO on a
  `wlr_swapchain` dmabuf, handed to a `wlr_scene_buffer`; EGL fence, no `glFinish`.
  `wlr_scene` is reduced to **presenter of one full-output buffer + the cursor
  plane + (later) scanout bypass**.
- **Dirty-gate (ours)**: schedule + `Render()` only on a real signal — a client
  commit (wlroots), an active RCSS animation (`GetNextUpdateDelay()` finite), or an
  input-driven state change. Idle ≈ no GPU. Proven in the spike.
- **Damage-limited compositing — Option B (build here, correctly).** Now that we
  own the real compositor (not a throwaway), do it the production way:
  1. Per-element dirt → screen-space damage region: project each changed surface's
     `wl_surface` damage rect through its element transform (forward of the spike's
     `project_to_screen`) → AABB → union (cap; fall back to full-frame when it
     explodes); static-transform elements use AABB, animating ones are full-damage
     for the animation.
  2. Render damage into the swapchain with **buffer-age accumulation**
     (`wlr_damage_ring` keyed on the presented buffer; repaint the union over the
     buffer's age) and a **scissor** on RmlUi's draw; redraw all elements
     intersecting the region in z-order (blending-correct).
  3. Feed the region to **`wlr_scene_buffer_set_buffer_with_damage`** so output +
     KMS partial-update benefit (battery/thermal on a 15W fanless panel).
  - A **damage-debug tint** toggle (same trick as the spike's click crosshair) to
    watch the reshaded region shrink and catch buffer-age staleness.
- **Fullscreen-video scanout bypass (deferred, separate from damage).** When one
  opaque, untransformed, fullscreen surface has nothing composited on top, pull it
  out of the RMLUi composite and hand it to `wlr_scene`/scanout directly (RMLUi
  draws nothing that frame). Trigger = the fullscreen STATE. Damage limiting can't
  help a full-rate video; this can. Size it by measurement; not a blocker.

## 8. Cross-unit contract changes (what Phase 2 touches)
| Unit | Change | Contract impact |
|---|---|---|
| **kernel** (`ui.hpp`, present/frames, input) | add `SurfaceElement` + `create_surface_element`; auto input-back for surface elements; damage-limited present; dirty-gate as the scheduler | NEW public surface in `ui.hpp`; present internals private |
| **ext-window-field** (NEW core unit; user decision §10.1) | owns the window-field ui surface + the window list (each row a `SurfaceElement` `live_uri` + layout data) + layout policy (tiling/stage/floating as RCSS); subscribes to `ext-xdg-shell`'s map/unmap/focus; drives focus + click-to-focus policy | NEW contract: the layout/tiling service (typed); consumes `ext-xdg-shell::Service` + kernel `SurfaceElement` |
| **ext-xdg-shell** | stop owning a `wlr_scene_tree` per toplevel for COMPOSITING; expose each toplevel's **root `wlr_surface`** so `ext-window-field` can make a `SurfaceElement`; `hide()/show()` become list membership / a hidden class, not scene-node toggles; pointer/touch routing moves to the substrate | `Toplevel::scene_tree()` **retired**; add `Toplevel::wl_surface()` (typed borrow); `geometry()` becomes the RCSS-resolved element box (read-back) — a real change-request |
| **ext-layer-shell** | layer surfaces become surface elements at the right `SceneLayer`; wallpaper via the identical path | analogous to xdg-shell |
| **ext-stage-dock** | minimize/restore re-expressed as **RCSS over the live window list** (a `.minimized` class / a dock list of live URIs); drop the frozen-`Preview` snapshot path (previews can stay live now) | consumes the new list/`SurfaceElement`; coordinates with `ext-window-field`; `Preview` may remain for thumbnails of hidden windows |
| **host-bin** (orchestrator-owned) | composition-root wiring (+ the NEW `ext-window-field` unit) + the **Phase-2 feature flag** to switch compositing path | — |

The biggest contract churn is **`ext-xdg-shell`** losing window compositing
(`scene_tree()`/`hide()`/`show()`/`geometry()` redesigned around exposing the root
`wlr_surface`) **and the new `ext-window-field` unit** owning layout. Settle both
contracts before Wave 2 fans out.

## 9. Unit/ownership map + Phase 2 wave plan
Topological, disjoint-where-possible (per `ORCHESTRATOR.md` §2). Behind a flag so
the session stays usable each wave.

1. **Wave 1 — kernel substrate.** `SurfaceElement` + `create_surface_element`
   (live import, seq-gate, frame-callback duty, surface-tree children) + auto
   input-back. Damage-limited present + dirty-gate scheduler. (Pure-core damage
   math is doctested; glue tested on the headless backend.) *opus agent.*
2. **Wave 2 — ext-xdg-shell + ext-layer-shell** (disjoint): retire per-window
   scene-tree compositing; `ext-xdg-shell` exposes `Toplevel::wl_surface()` + the
   new focus/geometry contract; `ext-layer-shell` exposes its surfaces likewise.
   Depends on Wave 1's contract. (These two are disjoint and can summon together.)
3. **Wave 3 — ext-window-field** (NEW): owns the window-field ui surface, the
   `bind_list` of live windows, and RCSS layout (tiling/stage/floating); subscribes
   to ext-xdg-shell map/unmap/focus; drives focus policy. Depends on Wave 2.
4. **Wave 4 — ext-stage-dock**: minimize/restore as RCSS over the live window
   list; coordinate with ext-window-field; thumbnails via live elements or
   `Preview`. Depends on Wave 3.
5. **Wave 5 — perf hardening**: damage-debug tooling, scanout bypass, real-seat
   numbers; refine tiling (now an RCSS layout over the window field).

## 10. Open sub-decisions (USER — boundary calls before/within Phase 2)
1. **RESOLVED (user): a NEW `ext-window-field` / `ext-tiling` core extension owns
   the window-field ui surface, the window list, and layout policy** (tiling /
   stage / floating as RCSS). `ext-xdg-shell` keeps the xdg protocol and only
   supplies toplevel handles + their root `wlr_surface`s; it no longer owns window
   compositing. This is the new unit added in Wave 2.
2. **One window-field document vs one per output/workspace?** Recommendation: one
   per output to start; workspaces = bound class/data on it.
3. **`Preview` retirement vs coexistence.** Keep `Preview` for thumbnails of
   *hidden* windows (no live buffer), use live `SurfaceElement` everywhere else?
   Recommendation: coexist.
4. **Flag strategy for Phase 2** (config `unbox.toml` key vs build flag) to run
   old `wlr_scene` compositing and new RML compositing side-by-side during the
   migration. Recommendation: `unbox.toml` runtime key.

## 11. Risks & fallback
- **Perf after damage limiting + scanout bypass still below budget on the real
  panel** → fall back to `wlr_scene` compositing with transient
  snapshot-through-RMLUi effects (the stage-dock `Preview` path already proves that
  half). This is the row-71 reopen trigger.
- **RCSS-only layout proves unworkable for a real tiling/effects extension**
  (e.g. needs imperative geometry RCSS can't express) → revisit the contract
  decision with a typed-placement-service escape hatch for that one case (kept out
  unless earned, per the rules discipline).
- **`ext-xdg-shell` contract churn** is the integration risk; settle its new
  window-list/geometry contract with its owner-agent before Wave 2 fans out.
