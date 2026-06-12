# unbox

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
- **Status:** planning / harness complete — see `tasks.md` for the live
  slice status and `notes/plan.md` for the full design + rationale.

## Building (slice 1 will make this real)

```sh
sudo pacman -S meson ninja cmake ccache   # one-time toolchain
meson setup build
ninja -C build
./build/packages/host-bin/unbox           # runs nested under labwc
```

## Documentation

- **Design & rationale:** `notes/plan.md`
- **Agent constitution (build rules):** `AGENTS.md`
- **Orchestration workflow:** `ORCHESTRATOR.md`
- **Canonical vocabulary:** `GLOSSARY.md`
- **Live status / task log:** `tasks.md`
