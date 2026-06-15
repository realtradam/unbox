#pragma once

// Pure decision core: floating-window geometry math. No wlroots, no kernel, no
// RmlUi — just integers in, integers out, so it is heavily doctest-covered
// (tests/test_geometry.cpp). The glue (extension.cpp) holds the per-window Box
// as bound STATE and feeds drag deltas through these functions; RCSS APPLIES the
// result as style (the contract: C++ owns interactive geometry STATE, RCSS owns
// rendering/animation; "gesture recognition: pure input->output" per AGENTS.md).

namespace unbox::ext_window_field::geom {

// A window's on-screen box in field (= output) px. Top-left origin.
struct Box {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

inline auto operator==(const Box& a, const Box& b) -> bool {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// Which chrome control is being dragged. `move` = the titlebar (translate the
// whole box); the two resize grips live at the BOTTOM corners (the top edge is
// fixed because the titlebar lives there):
//   resize_bl — bottom-LEFT grip: the left edge + bottom edge follow the pointer
//               (x and w change, the right edge x+w stays put; h changes).
//   resize_br — bottom-RIGHT grip: the right edge + bottom edge follow (w and h
//               change; x and y stay put).
enum class Handle { move, resize_bl, resize_br };

// Minimum window size (px). A resize never shrinks below this; the opposite edge
// stays anchored so the window does not jump when it hits the floor.
struct Limits {
    int min_w = 240;
    int min_h = 160;
};

// Apply a drag to the box captured at drag-START. `dx,dy` is the CUMULATIVE
// pointer delta since the drag began (pointer_now - pointer_at_start). Pure: the
// same inputs always give the same Box, so the glue just records the start box +
// start pointer once and calls this every move/end phase. Honors `lim` (min
// size with the anchored-opposite-edge rule); does NOT clamp to the field — see
// clamp_to_field for that (kept separate so move and resize share one min-size
// core and the field bound is applied once at the end).
[[nodiscard]] auto apply_drag(Box start, Handle handle, int dx, int dy, Limits lim) -> Box;

// Clamp a box's ORIGIN so the whole window stays within a field of field_w ×
// field_h px (a window larger than the field is pinned to the top-left). Keeps
// the titlebar reachable — a window can never be dragged entirely off-screen.
// Size is never changed here. A non-positive field dimension is treated as "no
// bound on that axis" (returns the box unclamped on that axis).
[[nodiscard]] auto clamp_to_field(Box b, int field_w, int field_h) -> Box;

} // namespace unbox::ext_window_field::geom
