# unbox — Constitution (root AGENTS.md)

> Loaded every session. Non-obvious, project-specific rules only — if a fresh
> frontier model could infer it from the code, it does NOT belong here.
> Full design + rationale: `notes/plan.md`. Workflow: `ORCHESTRATOR.md`.

## What this project is
An experimental Wayland desktop environment for the Panasonic CF-AX3
(touchscreen, 4-core i5-4300U Haswell, 3.7 GiB RAM, Artix Linux + s6).
A **monolithic kernel + in-process extensions**: ONE process containing the
wlroots compositor core and an embedded RMLUi UI substrate; every feature
(taskbar, launcher, tiling, OSK, …) is an extension. Tiers:
**kernel → core → standard**. Ergonomic for touch AND keyboard AND mouse.

## Stack
C++23 (gcc), Meson + Ninja (+ ccache), wlroots **0.20** (system, pinned,
`WLR_USE_UNSTABLE`), `wlr_scene` + wlr GLES2 renderer for compositing,
RMLUi (vendored Meson subproject via the cmake module) on a sibling
**GLES 3.2** EGL context (hardware-verified native), toml++ config
(`unbox.toml`), doctest for pure-core tests, wlr headless backend for
integration tests. Development runs NESTED under the live labwc session.

## The non-negotiable architecture rules
- **The kernel names NO concrete feature.** It owns: contracts (the ABI),
  the extension host, the event/hook/service bus, backend/output/seat/scene
  glue, and the UI substrate. Policies (tiling, bar contents, focus rules)
  are extensions.
- **Contracts are the only cross-unit surface.** A unit's contract is its
  PUBLIC headers: `packages/<unit>/include/unbox/<unit>/`. Private headers
  live in `src/` and are invisible to everyone else. If you must read
  another unit's `src/` to do your job, the contract is incomplete — report
  up, never peek.
- **One owner per unit.** You may ONLY edit files in the unit you were
  assigned. Cross-unit changes are change-requests reported up.
- **Lifetime is part of the contract type system.** `unique_ptr` = ownership
  transfer; raw pointer/reference = non-owning borrow valid only for the
  call; every hook subscription returns an RAII handle that unsubscribes on
  destruction. Never a bare `wl_listener` across units.
- **Effects at the edges, pure decision cores.** Layout math, gesture
  recognition, config parsing: pure input→output, heavily unit-tested.
  Only thin glue touches wlroots/GL.
- **Hooks are error-isolated.** Events: fire-and-forget, N listeners,
  exceptions caught at the boundary (a throwing extension gets disabled,
  never the session). Filters: ordered value-in→value-out chains.
- **Cross-extension coupling anchors to exported TYPED symbols** (hook
  descriptors, service handles). String-keyed lookups are forbidden — a
  missing dependency must be a compile/link error.
- **Never block the event loop.** Single-threaded `wl_event_loop`; anything
  slow is deferred/async. Frame budget on this GPU is precious.

## Commands
- `meson setup build` (once) · `ninja -C build` — build everything
- `ninja -C build <unit-target>` — build ONE unit (prefer this; the i5 is slow)
- `meson test -C build --suite <unit>` — that unit's tests
- Sanitizer dev build lives in `build-asan/` (separate dir, don't thrash ccache)

## Don'ts (each is a settled decision — see notes/plan.md §2)
- NO ANGLE, NO Vulkan, no GL translation layers (native GLES 3.2 verified).
- NO direct `#include <wlr/...>` — only via the kernel's extern-"C" wrapper.
- NO new third-party deps without surfacing to the user first.
- NO synonym coinage: check `GLOSSARY.md`; new terms need user sign-off.

## Testing (asymmetric — strict core, lenient shell)
Pure cores: doctest, zero mocks of our own modules (mocking `unbox::` is a
design bug — inject the effect instead). Glue/shell: a few integration tests
on the wlr headless backend; do not chase unit coverage there.

## Reports
Finish a task → write `reports/<unit>.md`: what you built, the public
surface, test output, contract gaps / change-requests. Reply tiny.

## Vocabulary
`GLOSSARY.md` is canonical (wlroots' own names preferred). Never invent a
synonym for an existing concept.
