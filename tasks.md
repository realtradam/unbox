# tasks.md — live status

> The orchestrator updates this after EVERY milestone. Keep it terse:
> slice status + the single next action. History lives in git.

## Now

**ACTIVE (core, user-driven) — Slice 13: RML COMPOSITING SPIKE.** Big direction
change: RMLUi becomes the content compositor — toplevels + layer-shell (incl.
wallpaper) + chrome are RML elements backed by LIVE, SHARED GL textures, with
layout/animation/3D effects in RCSS; wlroots stays foundation + cursor plane +
(deferred) fullscreen scanout bypass. Lost wlr_scene damage/scanout is mitigated
by OUR dirty-gated rendering (NOT a RMLUi built-in) + a deferred scanout bypass.
GATED BY A SPIKE before commit. Full spec + acceptance criteria:
`notes/rml-compositing.md`; decision row in `notes/plan.md` §2.
NEXT ACTION: write the spike brief (kernel/substrate) and summon it.
Tiling (slice 7) is DEFERRED behind this (becomes RCSS over surface elements;
pure layout core in `notes/tiling-spec.md` carries over). Stage dock (slice 10)
real-seat feel check is paused under this pivot.

**DEV WORKFLOW (RML/RCSS hot-reload — use this):** UI documents are external assets
under `assets/<unit>/` (e.g. `assets/ext-stage-dock/dock.rml` + `dock.rcss`), loaded
via `UiSurfaceSpec::rml_path`. Launch unbox with
`UNBOX_ASSET_DIR=<repo>/assets UNBOX_DEV=1` (reads the source tree + arms an inotify
watcher). Then editing a .rml/.rcss and saving HOT-RELOADS the live surface — NO
recompile, NO restart (bindings/geometry preserved; a broken file keeps the old doc).
Real-seat verified. Installed builds find assets via `-DUNBOX_ASSET_DIR_DEFAULT`.
(9c0c0bf kernel, f852141 dock+build)

**CONFIG HOT-RELOAD (always-on, user feature):** editing `~/.config/unbox/unbox.toml`
re-applies keybindings LIVE — no restart. Backed by a general kernel primitive
`Host::watch_file(path, cb) -> FileWatch` (RAII, coalesced, editor-save/create-safe,
error-isolated; ONE session inotify also backs the UI hot-reload above). A
malformed/mid-edit file keeps the current bindings + logs a warning (keys never
drop). Real-seat verified. (3c1bde9 kernel watch_file, 9f7dc09 ext-keybindings)

**Just landed — usability slice (user-driven, real-seat verified on the CF-AX3):**
`ext-keybindings` (new core ext) reads keybindings from `unbox.toml`: tap-Super →
spawn fuzzel, Alt+Tab / Alt+Shift+Tab → stable focus rotation over all toplevels,
Alt+F1, Ctrl+Alt+Backspace → quit. ext-xdg-shell's hardcoded keybinds migrated
out. Kernel now exports `WAYLAND_DISPLAY` so extension-spawned clients reach unbox
(was the fuzzel "no monitors" root cause). build + build-asan both green.
Follow-up: Ctrl+Alt+F1..F12 VT switching is now kernel-hardwired (the session
escape hatch).

**Active — Slice 10: stage dock** (user-driven; supersedes slice 6 as the next
UI work). The Stage-Manager-style left-edge dock of minimized-window **previews**,
revealed by a left-edge **swipe**. **Fork B** (see plan.md §2): previews are
toplevel snapshots imported as textures into the RMLUi context, shown as `<img>`
in one RML document. Waves (a number per wave runs in parallel = disjoint units):

**Landed a1–d1 (committed; code-complete, all green build + build-asan 10/10 suites;
real-seat feel pending):**
- a1 kernel SPIKE — Fork-B GO on crocus: `Preview` + `create_preview(wlr_scene_tree*)`,
  wlr pixels → dmabuf → EGLImage → sampled RMLUi texture → `<img>`. (7fed564)
- b1 ext-xdg-shell — `Toplevel::hide()/show()` (≠ unmap) + `geometry()` + `scene_tree()`. (bdce81a)
- b2 kernel — UiSurface list bindings (`bind_list`/`bind_list_string`/`bind_list_event`). (74c8071)
- b4 ext-stage-dock (new unit) — skeleton + pure cores (reveal recognizer, dock layout). (d6535e8)
- c2 ext-stage-dock + host-bin — Super+M minimize → preview slot → hide; tap → restore. (3376100)
- d1 ext-stage-dock — RCSS dock slide-in + per-slot settle; restore instant. (b578327)
- fix ext-stage-dock — dock previews were blank: `data-attr-src` (RmlUi binds
  attrs, not `{{}}`) + font `Noto Sans` + valid `transform-origin`. REAL-SEAT
  VERIFIED: minimizing 2 foot windows shows 2 live preview snapshots in the dock;
  Super+M repeats with >1 window. (5ebd45a)
- TRANSPARENCY + usability pass (REAL-SEAT VERIFIED): kernel — ui surfaces now
  composite per-pixel alpha (stray opaque `Clear()` removed) AND `set_size` resizes
  the render target (was logical-only; the slice-5 change-request) so a surface can
  grow. (f1e12a3). ext-stage-dock — strip background transparent (windows show
  through; cards keep their panel), surface hugs the card stack (no full-height
  input capture), and re-minimize-after-empty fixed (stale `focused_`: restore now
  sets it directly since a non-defocused window's `focus()` is a seat no-op). (661166a)
- CARD = ROUNDED THUMBNAIL (REAL-SEAT VERIFIED): the card IS the window preview,
  rounded on all four corners — a full-bleed `image(... cover center)` decorator on
  a child of a rounded `overflow:hidden` slot (RmlUi won't clip an element's OWN
  decorator to its OWN radius → decorator lives on the clipped child). First use of
  the substrate's RmlUi clipping path (scissor + stencil clip-mask); kernel verified
  it correct + added 4 regression tests (6519ebf). Title overlay parked
  (`display:none`, binding kept) for a later text redesign — user's call. (a743f44)

**NEXT (needs user):**
1. REAL-SEAT feel check (covers c2+d1): `~/start-unbox.sh -s foot`, Super+M minimizes
   foot → its preview card slides into the 240px left dock; tap the card → foot
   restores; minimizing the last window slides the dock out.
2. BOUNDARY DECISION — the full cross-screen "window flies into the dock" flight (and
   e1's drag-out grow-back) needs exactly ONE new kernel primitive: an
   **input-transparent UiSurface flag** on `UiSurfaceSpec` (d1 proved animation-end is
   already observable via `bind_event`; no timer/frame-tick needed). Approve it →
   then c1 gesture-claim + e1 (edge reveal + drag-out), the config-driven
   minimize-keybind migration (ext-keybindings action + a stage-dock Service), and
   favicon (needs an XDG icon-theme dep) follow.

Slice 6 re-scoped: the **window-list taskbar is CUT** (overlaps the stage dock,
conflicts with the touch/iPad direction; its contract-exercise purpose was met by
the stage dock). Replaced by two future, not-yet-designed features sequenced
AFTER slice 7 (tiling): **status bar** (slice 11) and **home screen** (slice 12)
— ideas + open questions captured in `notes/status-bar-home-screen.md`. Launching
is covered by fuzzel today (and the home screen later).
Still queued whenever UI work resumes: keyboard-into-ui-surfaces, removing the
deprecated no-op `Options::ui_spike`, retiring host-bin's demo ui.

## Slices

| # | Slice | Status | Acceptance |
|---|---|---|---|
| 0 | Harness skeleton | **DONE** 2026-06-12 | All harness md files in place |
| 1 | Bootstrap: toolchain, Meson skeleton, RMLUi subproject compiles, empty kernel links wlroots-0.20 from C++ via the extern-"C" wrapper | **DONE** 2026-06-12 | met: build green; tests 1/1; binary prints wlroots 0.20.1 + RmlUi 6.2, exits 0 |
| 2 | tinywl port: kernel skeleton runs nested under labwc | **DONE** 2026-06-12 | met: nested output WL-1, foot toplevel mapped+focused, GLES2 renderer; touch handlers added (tinywl lacks them); headless boot test green |
| 3 | **THE SPIKE:** RMLUi→scene bridge | **DONE — GO** 2026-06-12 | met: Plan A (dmabuf FBO→wlr_buffer→wlr_scene_buffer) verified nested+headless on HD 4400; Plan B fallback verified; orientation fixed + position-aware guard; input proof on-screen; RSS ≈83 MiB; ASan/UBSan clean in our code (known noise: Mesa leak reports + 2 benign UBSan downcasts inside vendored RMLUi). glFinish→fence and format negotiation deferred to the real substrate (slice 4+) |
| 4 | Extension host + contracts: bus, manifests, static registration; xdg-shell/layer-shell refactored OUT of kernel into core extensions | **DONE** 2026-06-12 | met: kernel boots featureless (names no feature); typed Event/Filter bus error-isolated + topo activation; ext-xdg-shell (toplevels, focus, grabs via pure GrabMachine, button/axis routing, Ctrl+Alt+Backspace quit) + ext-layer-shell (fuzzel verified, pure arrangement core) pass suites; typed surface→scene-tree registry replaced the data-field convention; first protocol codegen (wlr-layer-shell XML vendored); user hands-on: all input paths verified incl. touch; 68 cases green + ASan clean; idle RSS ≈73 MiB |
| 5 | Input routing + ergonomics contract: unified pointer/touch→RMLUi events, keybinding filter chain, touch-mode RCSS variables | **DONE** 2026-06-13 | met (user hands-on): real ui substrate (`Host::ui()` → UiSurface, scalar+event bindings, dmabuf+fence+swapchain); same demo surface driven by mouse AND finger; consume-or-pass with implicit-grab ownership (press owner gets release, per touch point too); touch-mode = state+notification only, NO visual scaling (user decision); touch-initiated grabs incl. pointer/touch alternation (seat release-leak fixed); keybinding chain satisfied by slice-4 Filter (ext-keybindings deferred); 113 doctest cases green, ASan clean, idle RSS ≈78 MiB |
| 5b | Usability: `ext-keybindings` (config-driven `unbox.toml`) — Super→fuzzel, Alt+Tab focus rotation; ext-xdg-shell keybinds migrated; kernel exports `WAYLAND_DISPLAY` for spawned clients | **DONE** 2026-06-13 | met (real-seat, user-confirmed): fuzzel opens on Super, Alt+Tab cycles all windows, quit works; build + build-asan both green (3rd-party Mesa/RmlUi sanitizer noise suppressed; a real libwayland leak in the layer-shell client test fixed) |
| 6 | ~~ext-taskbar~~ + ext-launcher | **taskbar CUT / re-scoped** | window-list taskbar dropped (overlaps the stage dock + conflicts with the touch/iPad direction). Replaced by slices 11–12. Launching is covered by fuzzel today and the home screen later. The contract-exercise purpose was met by the stage dock. |
| 7 | ext-window-tiling: pure layout core + thin scene glue | **DEFERRED pending slice 13** (baseline designed) | layout math 100% doctest-covered, zero wlroots types in core. Baseline = `primary` (right) + `stack` (left), auto-tile, 1=full/2=50-50/3+=stack-left; see `notes/tiling-spec.md` (+ `notes/tiling-layouts-reference.md`). Held until RML compositing (slice 13) lands — tiling then becomes RCSS layout over surface elements; the pure core is renderer-agnostic and carries over. (`Toplevel::set_box` prototype reverted; recreate from `prompts/ext-xdg-shell.md` when tiling resumes.) |
| 8 | ext-osk: RML keyboard ui surface injecting via wlr_seat | pending | type into foot via touch only; auto-show on text-input focus |
| 9 | Session hardening: s6 user service, TTY launch on seat0, layout persistence (append-only state + pure reconcile on boot) | pending | survives `kill -9` + s6 restart with workspaces restored |
| 10 | **Stage dock** (ext-stage-dock): minimized-window previews on a left-edge swipe (Fork B) | **a1–d1 landed; previews real-seat-verified** | DONE: Super+M minimize→RMLUi-imported preview snapshot→dock slot→hide (previews confirmed rendering on hardware); RCSS dock slide-in + slot settle. NEXT: confirm tap-to-restore + animation feel; 1 boundary call (input-transparent UiSurface flag) → c1 gesture-claim → e1 gesture reveal/drag-out; then config-driven minimize keybind + favicon (XDG icon dep) |
| 11 | **Status bar** (tent. ext-statusbar): iPad/iOS top bar — clock (left), configurable left/middle/right sections, tray (right) wifi/volume/battery | **IDEA — needs design** | sequenced AFTER slice 7 (tiling); replaces cut taskbar. Details + open questions: `notes/status-bar-home-screen.md` |
| 12 | **Home screen** (tent. ext-home, iPad springboard): app grid; tap = launch-or-raise (instance picker if >1 open); add/remove apps; swipe-up-from-bottom to enter | **IDEA — needs design** | sequenced AFTER slice 7 (tiling); replaces cut taskbar. Details + open questions: `notes/status-bar-home-screen.md` |
| 13 | **THE SPIKE: RML compositing** — RMLUi becomes the content compositor (toplevels + layer-shell incl. wallpaper + chrome = RML elements backed by LIVE, SHARED GL textures; layout/animation/3D effects in RCSS). wlroots = foundation + cursor plane + (deferred) fullscreen scanout bypass. | **ACTIVE (core) — spike** | GO/NO-GO on the CF-AX3: (1) live toplevel texture in RmlUi via shared context, ZERO per-frame copy; (2) RCSS 3D transform on it; (3) pointer+touch+keyboard routed back through RmlUi picking → wl_seat; (4) window w/ popup+subsurface composited (decides per-subsurface-elements vs per-window RTT); (5) wallpaper as an element; (6) perf ~4 windows@1080p + idle≈no-work (our dirty-gating) + video cost; (7) present via existing FBO→scene_buffer bridge. Full spec + decision row: `notes/rml-compositing.md`, plan.md §2. |

## Deferred decisions (decide when reached — see notes/plan.md §7)

dlopen extensions · remote builds on a fast box · xwayland default ·
OSK virtual-keyboard protocol vs direct seat injection · workspace model ·
clang-format style details
