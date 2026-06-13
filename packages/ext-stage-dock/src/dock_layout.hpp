#pragma once

#include <algorithm>

// Pure decision core 2 — DOCK LAYOUT: the geometry of the stage-dock FRAME.
// No wlroots / GL / RMLUi — plain ints in, plain rects out, doctest-covered in
// tests/test_policy.cpp.
//
// Fork B division of labour: the RML/RCSS inside the dock document does the
// in-dock slot FLOW (the full-height rail's flex centering, scrolling, card
// styling); this core owns ONLY the dock FRAME — its on-screen rect as the
// reveal slides it in from the left. (The card-stack capacity / content-height /
// per-slot-rect math the earlier hug-the-cards sizing used now lives in the
// RCSS, so those helpers were removed when the dock became a full-height rail.)
// All glue consumes this; this file calls nothing back.
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
// output's pixel size. `dock_width` is the revealed dock's on-screen width. All
// in px.
struct DockMetrics {
    int output_w = 0;
    int output_h = 0;
    int dock_width = 320;
};

// The dock's on-screen rect at reveal fraction f in [0,1]. The dock slides
// horizontally: at f=0 it is fully hidden off the left (x == -dock_width); at
// f=1 it is fully revealed (x == 0). x is monotonic non-decreasing in f. y/h
// cover the FULL output height (the dock is a full-height left rail); w is
// always dock_width (the dock keeps its width and translates — it does not
// grow). f is clamped to [0,1].
[[nodiscard]] inline auto dock_box(const DockMetrics& m, double fraction) -> Box {
    const double f = std::clamp(fraction, 0.0, 1.0);
    // x goes from -dock_width (f=0) to 0 (f=1): x = -dock_width * (1 - f).
    const int x = -static_cast<int>(static_cast<double>(m.dock_width) * (1.0 - f));
    return Box{.x = x, .y = 0, .w = m.dock_width, .h = m.output_h};
}

} // namespace unbox::ext_stage_dock::layout
