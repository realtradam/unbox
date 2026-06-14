# ext-window-tiling — baseline design (slice 7)

> **Status: BASELINE DECIDED — build off this.** The agreed starting point for
> the tiling extension. Layout catalogue to grow into later lives in
> `notes/tiling-layouts-reference.md`. Open questions at the bottom are to be
> resolved as we build.

## Naming (replaces dwm's "master")

dwm's "master/stack" → we use:

- **primary** — the window that gets the most space / most attention.
- **stack** — every other tiled window.

Rationale: "master" is unclear (and carries master/slave baggage). "primary" is
plain and position-agnostic (it works whether the big window is left or right).
Considered alternatives: `hero`, `main` (fine), `focus` (REJECTED — collides
with keyboard focus), `stage` (REJECTED — taken by the stage dock).

**Pending: final user sign-off + `GLOSSARY.md` entry** for `primary` / `stack`
(no-synonym-coinage rule).

## The baseline layout

A single default tiling layout. Mirror of dwm's `tile`; the primary column sits
on the **right by default but the side is configurable** (`primary_side`), so it
can be flipped to a left primary + right stack.

- **New windows auto-tile** — opening a window drops it into the layout
  automatically; no manual placement step. Where it lands in the stack (top or
  bottom) is **configurable** (`new_window`).
- Window-count behaviour:
  - **1 window** → fills the whole area (fullscreen).
  - **2 windows** → split 50/50: primary right half, the one stack window left
    half.
  - **3+ windows** → primary takes the right portion (`split_ratio` of the
    width); the remaining windows share the LEFT portion, stacked vertically and
    sized evenly.

```
   1 window            2 windows           3+ windows
 ┌──────────┐        ┌─────┬─────┐        ┌────┬────────┐
 │          │        │     │     │        │ S1 │        │
 │    P     │        │  S  │  P  │        ├────┤   P    │
 │          │        │     │     │        │ S2 │        │
 └──────────┘        └─────┴─────┘        ├────┤        │
                                          │ S3 │        │
                                          └────┴────────┘
   P = primary (right)   S = stack (left, vertical)
```

## The ordering model (decided)

One **ordered list** of tiled windows. The **head of the list is the primary**;
the remaining elements are the **stack**, rendered top-to-bottom in list order.

- **New windows enter at the BOTTOM of the stack** (append to the list end) —
  they do NOT steal the primary slot.
- **The top of the stack is promoted to primary** when the primary slot is
  vacated (the primary window closes or is moved out). Because the primary is
  just the head of the list, removing the head makes the next element (the old
  top-of-stack) the new primary automatically.
- Net effect of the two defaults: opening windows never disturbs your current
  primary; closing the primary hands the big slot to the longest-waiting stack
  window (the one at the top).

## Config (`unbox.toml`, `[tiling]` table)

```toml
[tiling]
primary_side  = "right"   # "right" (default) or "left"
new_window    = "bottom"  # "bottom" (default) or "top" of the stack
split_ratio   = 0.55      # fraction of width given to the primary column
primary_count = 1         # windows in the primary area (baseline 1)
```

- **`primary_side`** — which side the primary column sits on. Default **right**.
- **`new_window`** — where a newly-opened window enters the stack. Default
  **bottom**. (`top` makes the newest window the first in the stack and thus the
  next in line for promotion to primary.)
- **`split_ratio`** (dwm `mfact`) — primary column width fraction. **Static
  config for now** (no runtime keybind/drag yet — add when friction demands).
- **`primary_count`** (dwm `nmaster`) — baseline **1**; model allows >1 later
  without redesign.

### Hot-reloadable (all four)

All four values **live-reload** — edit `unbox.toml`, save, and the change applies
with no restart. This reuses the existing kernel primitive
`Host::watch_file(path, cb) -> FileWatch` that already backs ext-keybindings'
config reload (RAII, coalesced, editor-save/create-safe, error-isolated; one
session inotify is shared). ext-window-tiling watches the same `unbox.toml` and
re-parses its own `[tiling]` table.

On a save:
- Re-read + re-parse `[tiling]` (pure). **Keep-old-on-bad:** a malformed /
  mid-edit file keeps the current values and logs one warning — never drops a
  working layout (same contract as the keybindings reload).
- **`primary_side`, `split_ratio`, `primary_count`** take effect immediately:
  swap the live values and **re-arrange every output's tiled set** that frame.
- **`new_window`** updates the stored value but only affects windows opened
  *after* the change (it governs insertion order, not existing windows) — no
  re-arrange needed.

The watcher holds no tiling state; it just hands new parsed values to the same
pure core, so reload is a value swap + a re-arrange call (no re-subscribe, no
window churn).

## The pure core (slice-7 contract shape)

Two pure pieces, both wlroots-free and 100% doctest-coverable (the slice-7 "pure
decision core" rule):

```
# geometry: ordered window list -> one Box each, in list order
arrange(area: Box, n: int, primary_count: int, split_ratio: float,
        primary_side: Side) -> [Box]

# list management: where a new window is inserted (top|bottom) and primary
# promotion on removal — pure operations over the ordered list of opaque tokens
```

- `area` = the usable region (already minus the status bar / any reserved zones).
- `arrange` returns one `Box` per window in list order (head = primary).
- `new_window` (top/bottom) and promotion-on-remove govern the LIST order, not
  the geometry — keep them separate from `arrange`.
- Effects (assigning rects to real toplevels via the scene) live in thin glue.

## Decided
- **Primary side:** configurable (`primary_side`), default **right**.
- **New-window insertion:** configurable (`new_window`), default **bottom** of
  the stack.
- **Primary promotion:** when the primary leaves, the **top of the stack** is
  promoted to primary (automatic — it's the new list head).

## Scope discipline

Keep the baseline minimal and **fully automatic**: **NO new keybindings or
gestures for tiling at this stage.** Windows auto-tile on open; the primary is
the list head; that's it. Everything below is **deferred until real-use friction
makes it the next thing to fix** — the WM is being dogfooded on the CF-AX3 and
features get added incrementally, largest friction first.

## Deferred until needed (NOT in baseline)

- **Manual primary swap / move-in-stack** — promoting an arbitrary stack window
  or reordering. (Needs a keybind/gesture → out of scope for now.)
- **Runtime `split_ratio` / `primary_count` changes** — config-only for now.
- **Focus + cycling across primary/stack** — existing ext-keybindings focus ring
  (Alt+Tab) already moves focus; no tiling-specific focus controls added now.
- **Floating exceptions** — dialogs / fixed-size / transient windows staying
  floating. Confirm + implement when a real app needs it.
- **Gaps** (dwm `vanitygaps`) — inner/outer gaps as a parameter (not a layout).
- **Touch ergonomics (CF-AX3):** many stack windows → thin left column; cap stack
  count or collapse to deck/monocle past N (a future second layout, not baseline).
- **Interaction with the rest of unbox:** minimize-to-stage-dock removing a
  window from the tiled set and restoring re-inserting it; home-screen /
  fullscreen-app vs the tiled set.
- **Per-output / per-workspace scope** — what tiling state is keyed to (workspace
  model is itself a deferred decision — see plan.md §7).

## Not in baseline (build off this later)
See `notes/tiling-layouts-reference.md` — deck, monocle, bstack, centeredmaster,
spiral/dwindle, grids, flextile-deluxe-style configurability, cfacts weights.
