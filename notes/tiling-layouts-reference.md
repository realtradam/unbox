# Tiling layouts — dwm reference (for slice 7, ext-window-tiling)

> **Status: REFERENCE / research only.** A catalogue of dwm's tiling layouts to
> draw from when designing `ext-window-tiling` (slice 7). Revisit once we have a
> baseline tiling core to work off. These are X11/dwm patches — NOT something we
> apply; they're a design reference. Each maps cleanly onto the slice-7 goal of a
> **pure layout core**: `(client count, master count, mfact, area) -> list of
> rects`, with zero wlroots types. Source: dwm.suckless.org/patches + the
> bakkeby/dwm-flexipatch bundle (which ships these as git-applicable diffs).

## Built-in dwm layouts (no patch)

| Layout | Description |
|---|---|
| **tile** | Default master/stack: `nmaster` windows in a left master column, the rest stacked vertically on the right. `mfact` sets the master/stack split. |
| **monocle** | Every window maximized to the full area; one shown at a time. |
| **floating** | Windows moved/resized freely (no tiling). |

## Layout patches

### Master / stack variants
| Layout | Description | Patch |
|---|---|---|
| **bstack** (bottomstack) | Master on **top** (full width); stack tiled left-to-right in a row beneath. | https://dwm.suckless.org/patches/bottomstack/ |
| **bstackhoriz** | Bottom-stack variant: stack clients stacked top-to-bottom beneath the master. | (same patch as bstack) |
| **columns** (col) | Like `tile`, but master-area clients are arranged in columns (left-to-right) instead of a single column. | https://dwm.suckless.org/patches/columns/ |
| **deck** | Master area normal; stack clients "decked" — piled on top of each other, only the focused one visible (monocle for the stack). | https://dwm.suckless.org/patches/deck/ |

### Centered master
| Layout | Description | Patch |
|---|---|---|
| **centeredmaster** | Master column centered horizontally; stack split to the **left and right** of it. | https://dwm.suckless.org/patches/centeredmaster/ |
| **centeredfloatingmaster** | Master floats centered **on top** of the full-width tiled stack. | (same patch) |

### Grids
| Layout | Description | Patch |
|---|---|---|
| **gridmode** (grid) | All windows in an even grid. | https://dwm.suckless.org/patches/gridmode/ |
| **gaplessgrid** | Grid with balanced columns; the last column absorbs the remainder so there are no empty cells. | https://dwm.suckless.org/patches/gaplessgrid/ |
| **horizgrid** | Splits into a top and bottom row (horizontal grid). | https://dwm.suckless.org/patches/horizgrid/ |
| **nrowgrid** | Grid whose number of rows is controlled by `nmaster`. | https://dwm.suckless.org/patches/nrowgrid/ |

### Spiral
| Layout | Description | Patch |
|---|---|---|
| **fibonacci → spiral** | Fibonacci tiling: each window takes half the remaining space, spiraling inward. | https://dwm.suckless.org/patches/fibonacci/ |
| **fibonacci → dwindle** | Same split, but windows dwindle toward the bottom-right instead of spiraling. | (same patch) |

### Meta / configurable
| Layout | Description | Patch |
|---|---|---|
| **flextile-deluxe** (supersedes **flextile**) | Configurable meta-layout: pick a split mode (horizontal / vertical / centered / floating / fixed) and a per-split tile arrangement (stack h/v, grids, fibonacci). Can reproduce tile, deck, monocle, centeredmaster, bstack/bstackhoriz, gapplessgrid, and more. | https://github.com/bakkeby/patches/wiki/flextile-deluxe/ (orig: https://dwm.suckless.org/patches/flextile/) |

### Also on the suckless site (not in the flexipatch bundle)
| Layout | Description | Patch |
|---|---|---|
| **tatami** | Symmetric "tatami mat" arrangement. | https://dwm.suckless.org/patches/tatami/ |
| **three-column / tcl** | Master in a center column with stacks on both sides; sized for wide screens. | https://dwm.suckless.org/patches/tcl/ |

## Adjacent (not layouts, but pair with them)
| Patch | Description | Patch |
|---|---|---|
| **vanitygaps** | Configurable inner/outer gaps between tiled windows and screen edges. | https://dwm.suckless.org/patches/vanitygaps/ |
| **cfacts** | Per-client size weights within the stack (give some stack windows more space). | https://dwm.suckless.org/patches/cfacts/ |

## Notes for the unbox design (to flesh out later)
- The whole set reduces to one pure function shape:
  `arrange(area: Box, clients: int, nmaster: int, mfact: float, params...) -> [Box]`.
  That's the slice-7 core (100% doctest-coverable, zero wlroots types).
- Touchscreen / iPad direction (CF-AX3): not all of these make sense for touch.
  Likely-relevant shortlist to decide on: tile, monocle, deck, bstack,
  centeredmaster, spiral/dwindle. Grids and flextile-deluxe are powerful but
  keyboard-heavy. Decide the starter set when we have a baseline.
- `flextile-deluxe` is the "one layout to rule them all" approach — worth
  studying as a model for a single configurable engine vs. many discrete
  layouts.
- Gaps (vanitygaps) and per-client weights (cfacts) are orthogonal modifiers;
  design the core so they're parameters, not separate layouts.
