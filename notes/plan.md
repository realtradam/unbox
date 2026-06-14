# unbox — Living Plan

> **Status:** harness complete; implementation not started (slice 1 next).
> **Purpose:** the full design + rationale so any agent or human picking
> this up has the complete picture. This is a *living* document — the
> orchestrator updates §2 when a decision lands and §7 when one is made.
> Live progress lives in `tasks.md`, not here.

## 0. The goal in one paragraph

A lightweight, experimental Wayland desktop environment for the Panasonic
CF-AX3 Let's Note, equally ergonomic for touchscreen, keyboard, and mouse.
One process — a **monolithic kernel** (wlroots compositor core + embedded
RMLUi UI substrate + extension host) — where **every feature is an
in-process extension** coupled through typed C++ contracts. The methodology
is imported from a separate in-house project: minimal kernel, contracts
as the only cross-unit surface,
one owner-agent per unit, functional core / imperative shell, glossary
discipline, and the harness-as-deliverable (its principles P1–P8 apply
here unchanged; see its `notes/restructure-plan.md` §1).

## 1. Verified target inventory (probed 2026-06-12, this machine)

| Fact | Value | How verified |
|---|---|---|
| Machine | Panasonic CF-AX3, touchscreen convertible | — |
| CPU | i5-4300U (Haswell-ULT), 4 threads, 1.9–2.9 GHz | `lscpu` |
| RAM | 3.7 GiB + 8 GiB swap | `free -h` |
| GPU | Intel HD 4400 (HSW GT2), Mesa 26.1.2 **crocus** | EGL probe |
| GL support | **GLES 3.2 native**, EGL 1.5 | 20-line EGL probe: created ES 3.2 context, `GL_VERSION: OpenGL ES 3.2 Mesa 26.1.2-arch1.1` |
| Vulkan | NOT viable: no driver installed (only vulkan-icd-loader); Haswell's `hasvk` deprecated upstream | `pacman -Q` |
| OS / init | Artix Linux, s6 + s6-rc, elogind 257.14 (seatd also present) | `pacman -Q` |
| Session | **labwc 0.20.0 running on Wayland** — proof wlroots 0.20 works on this exact GPU/seat stack; also our nested-dev host | env + `pacman -Q` |
| wlroots | `wlroots0.20` 0.20.1 and `wlroots0.19` 0.19.3 packaged | `pacman -Q` |
| Wayland | wayland 1.25, wayland-protocols 1.49, libinput 1.31.3, xorg-xwayland 24.1.12 | `pacman -Q` |
| Toolchain present | gcc 16.1.1 (C++23-capable), git, pkgconf, freetype2 2.14.3, glfw 3.4, foot | `pacman -Q` |
| Toolchain MISSING | meson, ninja, cmake, ccache — install at slice 1 | `pacman -Q` |
| RMLUi | NOT packaged in Artix/Arch repos → vendor as Meson subproject; only hard dep is FreeType (present) | `pacman -Si` |

## 2. Decisions (settled — relitigating one requires new evidence)

Each row: what we decided, why (the P4 test: the specific problem it
solves), and the trigger that would reopen it.

| Decision | Rationale | Reopen if |
|---|---|---|
| **Monolithic kernel** (compositor + RMLUi in one process), features as in-process extensions | Faithful to dispatch (its extensions are in-process too); one GL context/font atlas/event loop on a 3.7 GiB machine; extensions get cheap superpowers (OSK injects via wlr_seat, tiling edits the scene directly); simple frame scheduling for touch latency | Crash-rate in practice makes session loss intolerable despite mitigations |
| Crash-isolation mitigations: exception-catching hook boundaries, pure extension cores, s6 restart, layer-shell escape hatch for external clients | A segfaulting extension kills the session and Wayland clients die with the compositor — accepted cost, bounded by keeping the segfault surface (kernel + thin glue) small | — |
| **Extension ABI designed as-if-dynamic, linked statically** | dlopen + C++ ABI instability is ceremony with zero current consumers (P1 stopping point) | A real third-party extension appears |
| **wlroots 0.20**, system package, pinned, via ONE extern-"C" wrapper header | Matches running labwc (proven on this hardware); wlroots API churns between minors — the pin + single wrapper contains upgrades to one file | 0.21+ needed for a feature; bump = one dedicated slice |
| **wlr GLES2 renderer** for compositing; **sibling GLES 3.2 EGL context** (shared EGLDisplay) for RMLUi | wlroots' renderer is its own battle-tested choice; RMLUi's maintained backend needs GL3/GLES3; GLES 3.2 verified native. Older GL API ≠ faster: same crocus driver either way, and ES3 features (UBOs, VAOs, instancing) REDUCE CPU-side driver overhead — the actual bottleneck on this CPU | — |
| **NO ANGLE / translation layers** | ANGLE's Linux backend is Vulkan (no driver here; hasvk deprecated); ANGLE-on-GL just stacks on the Mesa we'd bypass; Chromium-scale build; tens of MB RSS; wlroots speaks system EGL | — |
| **Meson + Ninja**, RMLUi via Meson's cmake subproject module; ccache; `build/` + `build-asan/` separate | Wayland-ecosystem native (wlroots/labwc/sway), first-class wayland-scanner codegen; RMLUi is CMake-only, the cmake module bridges it, built once. PROVEN at slice 1: RMLUi 6.2 configures + builds via the cmake module first try | cmake-module friction on a future RMLUi bump → fall back to prebuilding RMLUi as a system-installed lib |
| **Vendoring = Meson wrap-file tarballs, NEVER git submodules** (user decision) | Wraps pin by sha256, sources land in gitignored `subprojects/`, `packagecache/` allows pre-seeding; submodule UX rejected | — |
| **C++23, gcc 16** | Already installed; designated initializers ease wlroots struct setup; Hyprland proved years of C++-on-wlroots | — |
| **toml++** for `unbox.toml` config | Mirrors dispatch.toml convention; header-only | — |
| **doctest** for pure cores | Lightest compile cost of the mainstream frameworks — compile time IS the scarce resource here | — |
| **wlr headless backend** for integration tests | Tests without a display, CI-able, no nested session needed | — |
| **Header-contract workflow + READ RULE** (orchestrator reads only `include/**` + host-bin + harness) | C++'s .hpp/.cpp split makes dispatch's contract-only visibility *mechanical*, and the compiler enforces what prompts only suggest; headers are the densest token representation of the system | — |
| **Lifetime encoded in contract types** (unique_ptr / borrows / RAII subscription handles) | The workflow's blind spot is cross-unit use-after-free (`wl_listener` after owner death — THE wlroots bug); typed lifetime makes it visible in headers the orchestrator can read | — |
| ASan/UBSan dev build + read-only debugger-agent escape hatch | Sanitizer traces substitute for reading two units' sources when a lifetime bug spans units | — |
| **Develop nested under labwc**; real seat only via s6 service at slice 9+ | Never brick the live session; wlroots auto-nests | — |
| **xwayland: optional extension, OFF by default** | RAM; this is an experimental DE — X11 apps opt in | — |
| Vocabulary source: **wlroots' own names** | P8 + prefer training-baked terms; Wayland's synonym swamp (surface/view/window/toplevel, output/monitor/display) is severe | — |
| **touch-mode causes NO visual change** (state + typed notification only; dp-ratio stays 1.0) | User found any automatic scaling jarring on hardware (slice-5 hands-on, three iterations: 1.6→1.25→none); extensions adapt affordances explicitly via `on_touch_mode_changed` | Real-seat ergonomics (slice 9+) show finger targets genuinely too small |
| **Keybindings are config-driven via `unbox.toml`** — ext-keybindings (core) is the first `unbox.toml` consumer/parser; external fuzzel-on-Super stands in for an in-process launcher for now | Fastest path to a usable DE (Super→fuzzel, Alt+Tab); exercises the key_filter + ext-xdg-shell focus contract with no new UI | A bespoke in-process launcher/taskbar lands (slice 6) |
| **Kernel exports `WAYLAND_DISPLAY`** (setenv at startup) so any process an extension spawns connects to unbox, not the session that launched unbox | Spawned clients inherit the process env; without it fuzzel hit the parent labwc (`wayland-0`) → "no monitors" | — |
| **VT switching (Ctrl+Alt+Fn) is kernel-hardwired** before the key_filter (`wlr_session_change_vt`) | It is the session escape hatch — must work even if an extension throws or greedily consumes keys; not a rebindable feature (user decision) | — |
| **Stage dock** (ext-stage-dock, standard) = the Stage-Manager-style left-edge dock of minimized-window **previews**, revealed by a left-edge **swipe**. **Fork B**: previews are toplevel snapshots imported as textures INTO the ui substrate's RMLUi context and shown as `<img>` in ONE RML document | Closest to the iPad Stage Manager north star; one ui surface animates as a unit via RCSS; reuses the slice-3 dmabuf/EGLImage bridge in reverse (wlr pixels → RMLUi texture) instead of two-layer scene/RML lockstep | Cross-context texture import proves infeasible on crocus → fall back to Fork A (previews as `wlr_scene` snapshot nodes) |
| **Mechanism in kernel/core, policy in ext-stage-dock.** Kernel ui substrate gains: preview-snapshot, list/container bindings, a gesture-CLAIM input path. ext-xdg-shell gains: `Toplevel::hide()/show()` (≠ unmap), `geometry()`, `scene_tree()`. ext-stage-dock owns: the "minimized" set, dock layout, gesture recognition, easing | Keeps "kernel names no feature" — snapshot/claim/list-bindings are generic primitives; minimize-to-dock is the only policy and lives in one standard extension | — |

## 3. Architecture

```
┌──────────────────────────────────────────────────────────────┐
│ STANDARD extensions: ext-taskbar · ext-launcher · ext-osk ·  │
│ ext-window-tiling · ext-wallpaper · ext-notifications · …    │
├──────────────────────────────────────────────────────────────┤
│ CORE extensions (minimum usable session):                    │
│ ext-xdg-shell · ext-layer-shell · ext-output-config ·        │
│ ext-keybindings · (ext-xwayland, optional, off)              │
├──────────────────────────────────────────────────────────────┤
│ KERNEL (names no feature):                                   │
│  wl_event_loop · backend/output/seat/scene glue ·            │
│  UI substrate (RMLUi contexts, render-to-scene, input        │
│  routing, theme/touch-mode variables) · extension host ·     │
│  typed event/hook/service bus · contracts (the ABI)          │
└──────────────────────────────────────────────────────────────┘
packages/host-bin = composition root (orchestrator-owned): the ONLY
place that names every extension; loads unbox.toml; activates in
dependency order.
```

- "Window", "workspace", "ui surface" are **contract types**; every policy
  about them (tiling, focus rules, bar contents) is an extension.
- Extensions contribute UI as RML documents + data bindings via the ui
  substrate service — never GL, never raw RMLUi contexts.
- Input: ONE kernel routing path feeds pointer AND touch into ui surfaces;
  extensions never see device types unless they ask (gesture extensions).

## 4. THE SPIKE (slice 3) — RMLUi → wlr_scene bridge

The single biggest unknown; everything UI depends on it. Plan A: RMLUi
renders each ui surface into an offscreen GLES 3.2 FBO backed by a dmabuf
(shared EGLDisplay, separate context), wrapped as a `wlr_buffer`, attached
as a `wlr_scene_buffer` node — damage flows through the scene like any
client surface. Fallbacks, in order: (B) render into a shm `wlr_buffer`
(CPU copy, acceptable for low-churn surfaces like a bar), (C) import the
FBO texture via EGLImage into the wlr renderer. Acceptance: hello-world
RML document composited + damage-tracked + input-routed in the nested
session. If A–C all fail, the monolith decision (§2 row 1) gets reopened —
that is what a spike is for.

## 5. Roadmap

Slices 1–9 with acceptance criteria live in `tasks.md` (single source of
truth for status). Summary: bootstrap → tinywl port → THE SPIKE →
extension host → input ergonomics → taskbar+launcher → tiling → OSK →
s6 session hardening. Slice 9's durability model is dispatch's: persist
layout state append-only, pure `reconcile` on boot, status derived never
trusted.

## 6. Risks

| Risk | Mitigation |
|---|---|
| Spike fails (dmabuf path) | Fallbacks B/C in §4; worst case reopen monolith decision early, cheaply |
| wlroots 0.19→0.20→0.21 API churn | Version pinned; ONE wrapper header; upgrades are a dedicated slice |
| Cross-unit lifetime bugs | Typed lifetime contracts + ASan build + debugger-agent (ORCHESTRATOR.md §4) |
| Build times on the i5 | ccache, per-unit targets, RMLUi built once, doctest; remote-build escape hatch (§7) |
| RMLUi text-input/IME for OSK | OSK injects via wlr_seat directly (no protocol); text-input protocol only if external-client IME is ever wanted |
| Touch latency / frame budget | wlr_scene damage tracking; flat effect-light RCSS theme by default; never block the event loop |
| RAM pressure (3.7 GiB) | One process, one font atlas; xwayland off; measure RSS at every slice end |

## 7. Deferred decisions (decide when reached, record in §2)

| Decision | Default until then | Trigger |
|---|---|---|
| dlopen extension loading | static linking | third-party extension exists |
| Remote builds on a fast box | local + ccache | builds still bottleneck after ccache |
| OSK injection: wlr_seat direct vs virtual-keyboard protocol | direct injection | external-client IME need |
| Workspace model (per-output? tags?) | undecided — design at slice 7 | tiling slice starts |
| clang-format style | defer config to slice 1 | first formatting dispute |
| Catch2 vs doctest revisit | doctest | doctest blocks something real |
| dmabuf render-format negotiation (`wlr_renderer_get_render_formats` is private in wlroots 0.20) | hardcoded ARGB8888/LINEAR (verified on crocus) | wlroots bump slice or a GPU that rejects it |
| window placement policy (new toplevels overlap at origin) | tinywl parity: no placement | slice 7 tiling (or earlier if it blocks testing) |
| dock favicon decoders (vendor lunasvg+stb_image; wire RmlUi LoadTexture for PNG/SVG) | no favicon — preview+title only | favicon feature scheduled — full design in `notes/favicon-spec.md` |

## 8. References

- Methodology source: a private in-house reference repo
  (AGENTS.md, ORCHESTRATOR.md, GLOSSARY.md, notes/restructure-plan.md §1
  P1–P8)
- "The AI Harness" — https://dev.to/louaiboumediene/the-ai-harness-why-your-ai-coding-agent-is-only-as-smart-as-the-repo-you-put-it-in-cml
  (layer budgets: constitution <100 lines, rules ≤5, feature docs ~60 —
  ours run ~20–30 because contract headers carry the schema half)
- wlroots 0.20 docs + tinywl: https://gitlab.freedesktop.org/wlroots/wlroots
  (tinywl is the slice-2 seed)
- RMLUi: https://github.com/mikke89/RmlUi + https://mikke89.github.io/RmlUiDoc/
  (GL3/GLES3 renderer backend; custom render/system interfaces)
- Prior art for C++-on-wlroots: Hyprland (pre-0.36, wlroots era)
