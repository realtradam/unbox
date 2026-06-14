# RML compositing — architecture direction (gated by a spike)

> **Status: DIRECTION CHOSEN, gated by a GO/NO-GO spike.** This reopens the
> compositing half of plan.md §2 row 51 (wlr_scene for compositing). Nothing is
> committed until the spike (Phase 0) passes on the real CF-AX3. Naming below is
> PROPOSED — needs user sign-off before it lands in GLOSSARY.md.

## Thesis
The kernel's UI substrate (RMLUi) becomes the **content compositor**: application
toplevels, layer-shell clients (wallpaper, panels), and the existing UI chrome
are all RMLUi elements backed by **live, shared GL textures** — so window
**layout, animation, and 3D effects are expressed in RCSS**, with no per-frame
texture copies. Tiling, stage-manager, effects, etc. become RCSS + extension
policy on top of this.

## Why this is viable (the de-risking already done)
- **Shared EGLDisplay already exists** (plan.md §2 row 51): the wlr renderer and
  RMLUi's GLES 3.2 context share an EGLDisplay. Slice 3 + the stage dock already
  move client/scene pixels into RMLUi as textures via dmabuf/EGLImage on this
  exact Haswell/crocus HW — the texture handoff is proven; the spike's job is to
  make it **live + zero-copy + shared-handle** rather than snapshot.
- **RmlUi does transform-aware hit-testing for us.** `Context::ProcessMouse*` /
  `ProcessTouch*` pick the element under a point THROUGH `transform`/3D
  `perspective` and dispatch DOM-style events + `:hover` (auto-updated in
  `Context::Update`). So the hard geometry of routing input through a 3D-tilted
  window is handled upstream — we only translate "element X at local (lx,ly)" to
  `wl_seat` (surface-local coords + implicit grab), which the kernel already
  models.
- **RmlUi GL3 renderer supports** transforms, clip masks, **filters (blur,
  drop-shadow)**, **shaders**, and **render-to-texture** — and caches compiled
  geometry + effect passes (an unchanged blur isn't recomputed). Our custom
  RenderInterface must implement these hooks (the "custom is fine" work).

## Division of labour (the end state)
- **RMLUi = content compositor.** ALL on-screen content: toplevels, layer-shell
  clients (incl. wallpaper — user decision), UI chrome. Layout/animation/effects
  in RCSS.
- **wlroots = foundation + plane manager** (NOT optional; RMLUi can't talk to
  DRM):
  - backend/DRM, output management, modeset, vblank/frame scheduling, GLES
    renderer, buffer import (dmabuf→texture), allocation, `wl_seat`/input;
  - the **hardware cursor plane** (drawing the cursor in RMLUi would force a full
    recomposite on every move — keep it a wlr plane);
  - the **fullscreen-video scanout bypass** (deferred optimization — see below).
- This is NOT "hybrid compositing": in steady state RMLUi composites everything
  and `wlr_scene` is reduced to **present the RMLUi buffer + cursor plane +
  scanout bypass**. `wlr_scene` may stay as that thin presenter (it can even
  scan out the RMLUi buffer itself on the primary plane).

## Performance posture (replacing wlr_scene's damage/scanout)
What we give up by moving content off `wlr_scene`: per-surface damage, occlusion
culling, and direct scanout. Mitigations:
- **Dirty-gated rendering (OUR mechanism — NOT a RMLUi built-in).** RMLUi tracks
  internal dirty flags (so `Update()` is cheap at idle) and knows when animations
  are active, but it does NOT provide screen damage or a "skip this frame"
  signal — and the most important dirty source here (a client buffer updating) is
  OUR shared texture changing OUTSIDE RMLUi, which RMLUi can't see anyway. So WE
  gate: only schedule + `Render()` a frame when a signal we already own fires — a
  client buffer commit (wlroots), an active RCSS animation (RMLUi tells us), or an
  input-driven state change (hover/focus/drag). This keeps a static desktop at
  ~zero GPU — the big battery/thermal win on a 15 W fanless ultrabook — WITHOUT
  per-surface damage. (`request_frames` already stops scheduling at rest; we gate
  on dirtiness on top.) The spike must CONFIRM idle ≈ no work in practice.
- **Fullscreen-video scanout bypass (deferred).** When exactly one window is
  fullscreen with nothing composited on top, pull that surface OUT of the RMLUi
  composite and hand it to `wlr_scene`/scanout directly (RMLUi draws nothing that
  frame). Trigger = the fullscreen STATE (e.g. VLC's fullscreen button), not the
  click. Impact of NOT having it: more battery/heat during long fullscreen video,
  not breakage — so it's a later optimization, sized by a spike measurement.
- What we lose and accept for now: partial-region redraw when one small thing
  changes (minor; the HD4400 can repaint 1080p of simple quads within budget).

## Phase 0 — THE SPIKE (kernel/substrate; throwaway; one GO/NO-GO)
Acceptance criteria, measured on the real CF-AX3:
1. A **live** toplevel buffer sampled by RmlUi via a shared GL context — **zero
   per-frame copy** — drawn as an element.
2. An **RCSS 3D transform + transition** applied to it (visual proof of payoff).
3. **Pointer + touch + keyboard routed back** to that client through RmlUi
   picking → `wl_seat`, correct under a transform.
4. A toplevel **with a popup + subsurface** composited correctly → decides the
   surface-tree question: **per-subsurface elements** vs **per-window
   render-to-texture**.
5. A **layer-shell client (wallpaper)** also rendered as an element (proves the
   "wallpaper through RMLUi" decision; same mechanism as #1).
6. **Perf**: ~4 windows @1080p incl. one continuously updating (terminal/video);
   measure frame time AND confirm **idle = ~no work** with dirty-gating on; also
   measure the cost of pushing fullscreen video through RMLUi (to size the
   scanout bypass).
7. **Present path**: reuse the existing RMLUi-FBO → `wlr_scene_buffer` bridge
   (fastest to truth); cursor stays a wlr plane.

Output: report + GO/NO-GO + chosen answers to (4) and the present-path, + the
perf numbers.

## Phase 1 — Architecture (if GO): a design doc settling
- The **surface-element model** + surface-tree handling (from spike #4).
- The **shared-texture handoff** API in the substrate.
- **Unified input**: fold the kernel's pointer/touch routing into RmlUi picking
  (dovetails with the existing "substrate gets input first" implicit-grab path).
- The **extension-facing contract**: how a future tiling/effects/stage-dock
  extension places & animates windows — RCSS docs + data bindings (existing
  substrate contract) vs. a new typed window-layout service.
- Update plan.md §2 row 51 to the new compositing model.

## Phase 2 — Implementation (phased, behind a flag; session stays usable)
substrate: surface-element + shared-texture handoff → kernel: route toplevels +
layer-shell into the substrate instead of `wlr_scene`; switch input → port
focus/move/resize/fullscreen → re-express stage-dock minimize as RCSS → revisit
tiling (now trivial: RCSS layout over surface elements) → effects.

## Proposed naming (NEEDS SIGN-OFF before GLOSSARY.md)
- **RML compositing** — the approach: RMLUi composites all on-screen content.
- **surface element** — an RML element backed by a live client surface's shared
  texture (a toplevel OR a layer surface presented inside RMLUi). Uses the
  canonical "surface"; avoids the "window" alias.

## Open questions the spike resolves
- per-subsurface elements vs per-window render-to-texture (the #1 unknown);
- present path: reuse FBO→scene_buffer vs eventually render direct to output;
- real perf headroom on the HD4400 (frame time + idle + video).

## Relationship to other work
- **Tiling is deferred** and becomes much smaller on top of this (RCSS layout
  over surface elements; the pure layout core in `notes/tiling-spec.md` still
  applies — it's renderer-agnostic).
- The **stage dock** already prototypes the texture-import half (frozen
  previews); its minimize/restore becomes RCSS on the new path.
- Status bar / home screen (slices 11–12) are RMLUi chrome already — they fit
  natively.
