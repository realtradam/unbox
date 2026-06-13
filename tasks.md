# tasks.md — live status

> The orchestrator updates this after EVERY milestone. Keep it terse:
> slice status + the single next action. History lives in git.

## Now

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

Slice 6 (ext-taskbar + ext-launcher) is paused; the stage dock matures the same
ui-substrate gaps it would have (list bindings, first real interactive consumer).
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
| 6 | First standard extensions: ext-taskbar + ext-launcher | pending | proves the ui-substrate contract is complete (friction = bad contract) |
| 7 | ext-window-tiling: pure layout core + thin scene glue | pending | layout math 100% doctest-covered, zero wlroots types in core |
| 8 | ext-osk: RML keyboard ui surface injecting via wlr_seat | pending | type into foot via touch only; auto-show on text-input focus |
| 9 | Session hardening: s6 user service, TTY launch on seat0, layout persistence (append-only state + pure reconcile on boot) | pending | survives `kill -9` + s6 restart with workspaces restored |
| 10 | **Stage dock** (ext-stage-dock): minimized-window previews on a left-edge swipe (Fork B) | **a1–d1 landed** (real-seat pending) | DONE: Super+M minimize→RMLUi-imported preview snapshot→dock slot→hide; tap→restore; RCSS dock slide-in + slot settle. NEXT: real-seat feel + 1 boundary call (input-transparent UiSurface flag) → e1 gesture reveal/drag-out |

## Deferred decisions (decide when reached — see notes/plan.md §7)

dlopen extensions · remote builds on builder · xwayland default ·
OSK virtual-keyboard protocol vs direct seat injection · workspace model ·
clang-format style details
