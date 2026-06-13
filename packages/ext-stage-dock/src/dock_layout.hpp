#pragma once

#include <algorithm>

// Pure decision core 2 — DOCK LAYOUT: the geometry mapping a reveal fraction +
// slot count to on-screen rects for the stage dock. No wlroots / GL / RMLUi —
// plain ints in, plain rects out, doctest-covered in tests/test_policy.cpp.
//
// Fork B division of labour: the RML/RCSS inside the dock document does the
// in-dock slot FLOW (wrapping, styling); this core owns the dock FRAME (its
// on-screen rect as the reveal slides it in from the left), the reveal OFFSET,
// and the SCROLL/CAPACITY math (how many slots fit, total content height, and a
// per-slot rect for callers that want compositor-side placement). All glue (c2/
// d1) consumes these; this file calls nothing back.
//
// Single wl_event_loop thread throughout.

namespace unbox::ext_stage_dock::layout {

// A pixel rectangle in output layout coords. (Matches the kernel's int box
// conventions; the glue converts to wlr_box at the edge.)
struct Box {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    [[nodiscard]] auto operator==(const Box&) const -> bool = default;
};

// Static metrics of the dock on a given output. `output_w`/`output_h` are the
// output's pixel size. `dock_width` is the revealed dock's on-screen width.
// `slot_height` is one preview slot's height; `gap` the vertical space between
// slots; `pad` the inner margin at the top (and conceptually all edges) of the
// dock content. All in px.
struct DockMetrics {
    int output_w = 0;
    int output_h = 0;
    int dock_width = 320;
    int slot_height = 96;
    int gap = 8;
    int pad = 8;
};

// The dock's on-screen rect at reveal fraction f in [0,1]. The dock slides
// horizontally: at f=0 it is fully hidden off the left (x == -dock_width); at
// f=1 it is fully revealed (x == 0). x is monotonic non-decreasing in f. y/h
// cover the full output height; w is always dock_width (the dock keeps its
// width and translates — it does not grow). f is clamped to [0,1].
[[nodiscard]] inline auto dock_box(const DockMetrics& m, double fraction) -> Box {
    const double f = std::clamp(fraction, 0.0, 1.0);
    // x goes from -dock_width (f=0) to 0 (f=1): x = -dock_width * (1 - f).
    const int x = -static_cast<int>(static_cast<double>(m.dock_width) * (1.0 - f));
    return Box{.x = x, .y = 0, .w = m.dock_width, .h = m.output_h};
}

// One slot's full stride: its height plus the gap below it.
[[nodiscard]] inline auto slot_stride(const DockMetrics& m) -> int {
    return m.slot_height + m.gap;
}

// How many slots fit in the revealed dock WITHOUT scrolling. The usable height
// is the output height minus the top+bottom pad; each slot occupies
// slot_height and slots are separated by `gap` (no gap after the last). Returns
// 0 if nothing fits. Independent of `count` — this is pure capacity.
[[nodiscard]] inline auto visible_slots(const DockMetrics& m) -> int {
    const int usable = m.output_h - 2 * m.pad;
    if (usable < m.slot_height || m.slot_height <= 0) {
        return 0;
    }
    // usable >= slot_height + k*(slot_height+gap)  =>  k = (usable - slot_height) / stride
    const int stride = slot_stride(m);
    if (stride <= 0) {
        return 1; // degenerate gap+height; at least the one that fit above
    }
    return 1 + (usable - m.slot_height) / stride;
}

// The total content height needed to stack `count` slots: top pad + count slots
// with `gap` between them + bottom pad (no trailing gap). 0 slots -> 0 (an empty
// dock has no content). Used to clamp the scroll range.
[[nodiscard]] inline auto content_height(const DockMetrics& m, int count) -> int {
    if (count <= 0) {
        return 0;
    }
    return 2 * m.pad + count * m.slot_height + (count - 1) * m.gap;
}

// The POSITIVE surface height that hugs `count` cards: content_height(count)
// clamped to a strictly-positive minimum. The ui substrate REJECTS a surface
// with non-positive geometry (create_surface/set_size return nullptr + log an
// error), so the empty dock (count 0 -> content_height 0) must still be created
// / resized at a positive size and merely hidden (set_visible(false)), never at
// height 0. Returns max(content_height(count), 1): >= 1 for every count >= 0,
// equal to content_height once there is at least one card. (Width never hits 0
// in practice — dock_width is a fixed positive constant — but callers should
// likewise guard it; this helper covers the height, which is the count-driven
// dimension.)
[[nodiscard]] inline auto surface_height(const DockMetrics& m, int count) -> int {
    return std::max(1, content_height(m, count));
}

// The on-screen rect of slot `i` (0-based) within the dock content, given the
// current vertical `scroll` offset (px scrolled DOWN; 0 = top). The slot's
// content-space top is pad + i*(slot_height+gap); subtracting `scroll` yields
// its screen y. x is the inner pad; width is dock_width minus pad on both sides
// (clamped to >= 0). A negative or off-screen y is returned as-is (the caller /
// RCSS clips); this core does not cull.
[[nodiscard]] inline auto slot_box(const DockMetrics& m, int i, int scroll) -> Box {
    const int content_y = m.pad + i * slot_stride(m);
    const int w = std::max(0, m.dock_width - 2 * m.pad);
    return Box{.x = m.pad, .y = content_y - scroll, .w = w, .h = m.slot_height};
}

} // namespace unbox::ext_stage_dock::layout
