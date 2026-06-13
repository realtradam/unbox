# ext-keybindings

Config-driven compositor keybindings — the first step to a usable DE. A **core**
extension (`id "keybindings"`, `depends_on {"xdg-shell"}`). It is a LEAF
consumer: its whole contract is the `create()` factory in
`include/unbox/ext-keybindings/ext_keybindings.hpp` — it exports no hooks or
services.

## Why it exists
Compositor shortcuts must live in ONE policy unit, not be hardcoded in the
shell. This unit owns them and reads them from `unbox.toml` (toml++). It also
SUBSUMES the two shortcuts ext-xdg-shell used to hardcode (`Alt+F1` focus-cycle,
`Ctrl+Alt+Backspace` quit), preserved as compiled defaults.

## Action vocabulary (`action = ...`)
- `spawn` — run `command` via `/bin/sh -c` (requires a non-empty `command`).
- `focus-next` / `focus-prev` — rotate keyboard focus across ALL mapped windows.
- `close-active` — close the focused toplevel (no-op if none).
- `quit` — `wl_display_terminate`.

Combos are `Mod+...+Key` (mods: `Super`/`Logo`, `Alt`, `Ctrl`/`Control`,
`Shift`, case-insensitive; final token an xkb keysym name). A BARE modifier
(`"Super"`) is a TAP binding. Unknown action / malformed combo = log + skip that
entry; never abort. No config / parse error / zero valid bindings → compiled
defaults (out-of-the-box == the repo-root sample `unbox.toml`).

## Focus ring: STABLE map order, not MRU
`focus-next`/`focus-prev` rotate a list kept in **map order** (append on
`on_toplevel_mapped`, drop on `on_toplevel_unmapped`, move-cursor on
`on_toplevel_focused`). Repeated Alt+Tab therefore walks all N windows and
wraps — it does NOT ping-pong the two most-recently-used (MRU was rejected as
wrong). The `Toplevel*` is stored only between its mapped and unmapped events
(the supported borrow window) and never dereferenced inside the pure ring core.

## The tap-Super gotcha
A bare-modifier binding fires on the modifier's RELEASE only if it was pressed
and released with NOTHING in between. The matcher arms on Super-down, marks
"used" on any other key press (or any Super-carrying chord), and fires on
Super-up only if still unused. The modifier press/release are NEVER consumed
(other combos need Super held); a fired chord IS consumed (`handled = true`), a
fired tap consumes nothing (the modifier already passed through). Pointer
Super+click cannot mark the tap used (the pointer is not in the key_filter) —
accepted for now.

## labwc-nested caveat
In nested dev under the live labwc session the parent compositor may swallow
Alt+Tab and the Super tap before they reach unbox, so live FEEL cannot be
verified here — that needs the orchestrator's hands-on on the real seat. No
Escape combo is bound (an established decision keeps all Escape chords passing
through to the parent session).

## Layout
- `include/unbox/ext-keybindings/ext_keybindings.hpp` — the factory (contract).
- `src/policy.hpp` — combo parser + matcher/tap state machine (pure;
  xkbcommon-only).
- `src/focus_ring.hpp` — stable-rotation ring over opaque tokens (pure).
- `src/config.{hpp,cpp}` — toml++ loader, string → bindings + warnings (pure).
- `src/extension.cpp` — glue: key_filter link, xdg-shell event subscriptions,
  fork/exec spawn, focus/close/terminate effects.
- `tests/test_policy.cpp` — the four cores, doctest-hard.
- `tests/test_glue.cpp` — headless install/activate/dispatch/shutdown smoke.
