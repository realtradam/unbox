<img src="readme-assets/unbox_logo.png" alt="unbox logo" width="240">

An experimental Wayland desktop environment for the Panasonic CF-AX3 Let's
Note (touchscreen convertible, i5-4300U Haswell, 3.7 GiB RAM, Artix Linux +
s6) — lightweight, and ergonomic for **touch, keyboard, and mouse** equally.

**Architecture:** a monolithic **kernel + in-process extensions**. One
process holds the wlroots compositor core and an embedded RMLUi UI
substrate; every user-facing feature (taskbar, launcher, on-screen
keyboard, tiling, …) is an extension talking to the kernel through typed
C++ contracts. Tiers: `kernel → core → standard`.

- **Stack:** C++23 · Meson/Ninja · wlroots 0.20 (`wlr_scene`, GLES2
  renderer) · RMLUi on a sibling GLES 3.2 EGL context · toml++ · doctest
- **Status:** in active development. The kernel boots, extension host +
  typed bus are in place, and core extensions exist (xdg-shell, layer-shell,
  keybindings, stage-dock). Slices 0–5b are done; the Stage-Manager-style
  left-edge dock (slice 10) is landed and real-seat verified on the CF-AX3.
  See `tasks.md` for live slice status and `notes/plan.md` for the full
  design + rationale.

## Building

```sh
sudo pacman -S meson ninja cmake ccache   # one-time toolchain
meson setup build
ninja -C build
./build/packages/host-bin/unbox           # runs nested under labwc
```

For UI work, run with the source assets and hot-reload armed — editing a
`.rml`/`.rcss` under `assets/` reloads the live surface (no recompile/restart):

```sh
UNBOX_ASSET_DIR="$PWD/assets" UNBOX_DEV=1 ./build/packages/host-bin/unbox
```

Useful flags: `-s <cmd>` spawns a client at startup (e.g. `-s foot`),
`--config <path>` points at an explicit `unbox.toml`, `--ui-demo` loads the
throwaway demo surface. `UNBOX_HOT_RELOAD` is accepted as an alias for
`UNBOX_DEV`.

## Extensions

The kernel names **no** concrete feature. Everything user-facing is an
extension that talks to the kernel through typed C++ contracts (its public
header under `packages/<unit>/include/`). Each declares a tier and its
dependencies; `host-bin` is the composition root that installs them in
dependency order.

| Extension | Tier | Depends on | What it does |
|---|---|---|---|
| **ext-xdg-shell** | core | — | Window management: the `xdg_shell` global, toplevel/popup lifecycle, click/tap-to-focus, pointer/touch routing, interactive move/resize, and the neutral hide/show minimize mechanism. Exports a `Service` with toplevel mapped/unmapped/focused events. |
| **ext-layer-shell** | core | — | `wlr-layer-shell-unstable-v1` for external clients (panels, launchers, wallpapers, OSKs). Maps protocol layers onto scene bands and keeps each output's usable area current. The anchor/exclusive-zone math is a pure core other units (e.g. tiling) can reuse. |
| **ext-keybindings** | core | xdg-shell, stage-dock | Config-driven shortcuts from `unbox.toml`, hot-reloaded live. Defaults: tap **Super** → launcher (fuzzel), **Alt+Tab / Alt+Shift+Tab** → focus rotation over all windows, **Ctrl+Alt+Backspace** → quit. A leaf consumer — exports nothing. |
| **ext-stage-dock** | standard | xdg-shell | Stage-Manager-style left-edge dock of minimized-window previews, revealed by a left-edge swipe. Snapshots windows into RMLUi textures; exports a `Service::toggle_visible()`. |

Bindings live in `unbox.toml` (sample at the repo root; copy to
`~/.config/unbox/unbox.toml`). A missing or malformed config falls back to
the compiled-in defaults — keys never drop.

Planned but not yet built: taskbar, launcher, on-screen keyboard, window
tiling, wallpaper, notifications. See `tasks.md` for the live roadmap.

## Documentation

- **Design & rationale:** `notes/plan.md`
- **Agent constitution (build rules):** `AGENTS.md`
- **Orchestration workflow:** `ORCHESTRATOR.md`
- **Canonical vocabulary:** `GLOSSARY.md`
- **Live status / task log:** `tasks.md`
