# ext-stage-dock

The **stage dock** (slice 10): the left-edge ui surface holding minimized-window
**previews**, **revealed** by a left-edge **swipe** (GLOSSARY; notes/plan.md §2).
A **standard**-tier extension (`id "stage-dock"`, `depends_on {"xdg-shell"}`). It
is a LEAF consumer for now: its whole contract is the `create()` factory in
`include/unbox/ext-stage-dock/ext_stage_dock.hpp` — no exported hooks/services
yet.

## Status (b4 — skeleton + pure cores)
This step ships ONLY the unit skeleton and the two pure decision cores, doctest-
hard with no kernel/wlroots running. `activate()` is a deliberate no-op (one log
line). Runtime wiring comes later: c2 = static integration (consume
ext-xdg-shell's Service, build the RML document), d1 = reveal animation, e1 =
the edge-swipe gesture feeding the recognizer.

## Pure cores
- `src/reveal.hpp` — the **reveal** recognizer: a reversible, finger-following
  recognizer for the left-edge swipe. `begin()` accepts only edge-started
  presses (`x <= edge_slop`); `update()` returns the reveal **fraction** in
  [0,1] (inward travel / `dock_width`, clamped, reversible); `end()` commits
  open/close from final fraction + recent velocity (a fast fling overrides the
  position threshold). A `start_fraction` arg lets the SAME recognizer drive the
  symmetric CLOSE drag (seed 1.0, drag back toward the edge).
- `src/dock_layout.hpp` — the dock **frame** + reveal **offset** + scroll/
  capacity math. `dock_box(f)` slides the dock from `x = -dock_width` (f=0,
  hidden) to `x = 0` (f=1, revealed), covering output height. `visible_slots`,
  `content_height`, `slot_box(i, scroll)` give the slot stacking + scroll range.
  Fork B: RML/RCSS does the in-dock slot FLOW; this core does the frame/offset.

## Layout
- `include/unbox/ext-stage-dock/ext_stage_dock.hpp` — the factory (contract).
- `src/reveal.hpp` — reveal recognizer (pure; header-only).
- `src/dock_layout.hpp` — dock geometry (pure; header-only).
- `src/extension.cpp` — glue: minimal manifest + no-op `activate()` (skeleton).
- `tests/test_policy.cpp` — both cores, doctest-hard.

## Vocabulary
All terms used (stage dock, reveal, swipe, preview, slot) are canonical in
GLOSSARY.md — no new coinage.
