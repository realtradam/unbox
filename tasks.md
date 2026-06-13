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

**Next action:** Slice 6 — ext-taskbar + ext-launcher (first standard
extensions; prove the ui-substrate contract). Queued into it from
slice 5: list/container data bindings (taskbar will change-request the
shape), keyboard-into-ui-surfaces (launcher text input), kernel removes
the deprecated no-op `Options::ui_spike` field, host-bin's demo ui
extension retires (replaced by the real consumers).

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

## Deferred decisions (decide when reached — see notes/plan.md §7)

dlopen extensions · remote builds on builder · xwayland default ·
OSK virtual-keyboard protocol vs direct seat injection · workspace model ·
clang-format style details
