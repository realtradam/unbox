# tasks.md — live status

> The orchestrator updates this after EVERY milestone. Keep it terse:
> slice status + the single next action. History lives in git.

## Now

**Next action:** Slice 3 — THE SPIKE: RMLUi→scene bridge (notes/plan.md
§4). Plan A: GLES 3.2 sibling context → dmabuf-backed FBO → wlr_buffer →
wlr_scene_buffer. Go/no-go gate for the whole UI design.

## Slices

| # | Slice | Status | Acceptance |
|---|---|---|---|
| 0 | Harness skeleton | **DONE** 2026-06-12 | All harness md files in place |
| 1 | Bootstrap: toolchain, Meson skeleton, RMLUi subproject compiles, empty kernel links wlroots-0.20 from C++ via the extern-"C" wrapper | **DONE** 2026-06-12 | met: build green; tests 1/1; binary prints wlroots 0.20.1 + RmlUi 6.2, exits 0 |
| 2 | tinywl port: kernel skeleton runs nested under labwc | **DONE** 2026-06-12 | met: nested output WL-1, foot toplevel mapped+focused, GLES2 renderer; touch handlers added (tinywl lacks them); headless boot test green |
| 3 | **THE SPIKE:** RMLUi→scene bridge | pending | a hello-world RML document composited as a scene node with damage tracking; go/no-go gate |
| 4 | Extension host + contracts: bus, manifests, static registration; xdg-shell/layer-shell refactored OUT of kernel into core extensions | pending | kernel names no feature; ext-xdg-shell + ext-layer-shell pass suite |
| 5 | Input routing + ergonomics contract: unified pointer/touch→RMLUi events, keybinding filter chain, touch-mode RCSS variables | pending | same ui surface usable by mouse and finger |
| 6 | First standard extensions: ext-taskbar + ext-launcher | pending | proves the ui-substrate contract is complete (friction = bad contract) |
| 7 | ext-window-tiling: pure layout core + thin scene glue | pending | layout math 100% doctest-covered, zero wlroots types in core |
| 8 | ext-osk: RML keyboard ui surface injecting via wlr_seat | pending | type into foot via touch only; auto-show on text-input focus |
| 9 | Session hardening: s6 user service, TTY launch on seat0, layout persistence (append-only state + pure reconcile on boot) | pending | survives `kill -9` + s6 restart with workspaces restored |

## Deferred decisions (decide when reached — see notes/plan.md §7)

dlopen extensions · remote builds on builder · xwayland default ·
OSK virtual-keyboard protocol vs direct seat injection · workspace model ·
clang-format style details
