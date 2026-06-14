# Status bar + Home screen — early ideas (DRAFT, NOT yet designed)

> **Status: IDEAS ONLY — needs fleshing out.** Nothing here is a committed
> design. Every section below has open questions that must be resolved (and
> user-signed-off) before implementation. These two features **replace the cut
> window-list taskbar** (old slice 6) and are **sequenced AFTER slice 7
> (tiling)** — see `tasks.md` slices 11–12.
>
> Unit names (`ext-statusbar`, `ext-home`) are **tentative**; a canonical name
> needs a `GLOSSARY.md` entry with user sign-off (no synonym coinage rule).
>
> Rationale for replacing the taskbar: this is a touchscreen CF-AX3 with an
> iPad-Stage-Manager north star. A conventional window-list taskbar overlaps the
> stage dock and costs scarce screen + GPU. What's genuinely missing is (1)
> system status and (2) an iOS-style app launch surface — these two items.

## 1. Status bar (tentative: `ext-statusbar`)

An iPad/iPhone-style bar pinned to the **top edge** of the screen.

- **Left:** clock.
- **Right:** a system **tray** — wifi, volume, battery.
- **Left / middle / right** sections are **configurable** (which items appear and
  in what order), presumably via `unbox.toml`.

### Open questions / to flesh out
- Item model: a fixed set vs a plugin/registry of "status items" extensions can
  contribute. How does config reference them (typed, not string-keyed)?
- Data sources: battery (sysfs/UPower?), wifi (NetworkManager/iwd via D-Bus?),
  volume (PipeWire/WirePlumber — already in the session per `start-unbox`),
  clock (local). Each is an effect at the edge; keep pure formatting cores.
- Surface: a `wlr-layer-shell` top-anchored surface with an **exclusive zone**
  (reserves top space; tiling/usable-area must account for it — ties into the
  ext-layer-shell usable-area model already built).
- Tap behavior of tray items (popovers for volume slider, wifi list, etc.).
- Touch target sizing (touch-mode is state-only, no auto-scale — extension
  adapts affordances explicitly via `on_touch_mode_changed`).
- Theming/RCSS; per-output (one bar per output? primary only?).

## 2. Home screen (iPad "Springboard"-style; tentative: `ext-home`)

A full-screen surface that shows **app icons**; the iOS home screen analogue.

- **Tap/click an app:**
  - if the app is **not open** → launch it.
  - if it **is open** (single instance) → raise/focus that window.
  - if it is open with **multiple instances** → show all of them as options and
    let the user pick which to bring forward.
- **Add / remove** apps from the home screen (some management flow/UI).
- **Swipe up from the bottom edge** → enter the home screen.

### Open questions / to flesh out
- App catalog source: XDG `.desktop` entries (Exec/Name/Icon) for the
  add-app picker? How is the on-screen set stored/persisted (append-only state,
  reconcile-on-boot — mirrors slice 9's durability model)?
- Icons: needs an XDG **icon-theme** dependency — overlaps `favicon-spec.md`
  (the dock favicon work). Resolve once, share.
- **Instance tracking (the hard part):** "is this app already open, and which
  windows are its instances?" requires a window↔app association (app-id /
  `.desktop` ↔ `xdg_toplevel` app_id). Likely a kernel/core capability the
  home screen + stage dock + a future taskbar-tray all reuse. Define the
  contract once.
- The multi-instance picker UI — could reuse the stage-dock preview-snapshot
  pipeline (live thumbnails of the candidate windows).
- Gesture: swipe-up-from-bottom mirrors the stage dock's left-edge reveal
  recognizer; reuse the gesture-CLAIM input path / recognizer cores.
- Layout: grid, pages/folders, reorder (drag). How it coexists with tiling and
  the stage dock (is the home screen a workspace? a layer? what has focus?).
- Launch reuses the existing `spawn` mechanism + `WAYLAND_DISPLAY` export.

## Shared primitives these two (probably) need
- **Window↔app association / instance enumeration** (home screen; reusable).
- **XDG icon-theme lookup** (home screen icons; shared with favicon-spec).
- **Top-edge exclusive-zone layer surface + usable-area accounting** (status bar).
- **Bottom-edge gesture reveal** (home screen; same family as stage dock's
  left-edge reveal).
- Keep "kernel names no feature": snapshots, gesture-claim, list bindings,
  instance enumeration are generic; status-item set and home-screen contents are
  the policy and live in the standard extensions.
