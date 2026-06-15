#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "geometry.hpp"

// Pure-core tests for floating-window drag/resize math. No kernel, no wlroots.

namespace geom = unbox::ext_window_field::geom;
using geom::Box;
using geom::Handle;
using geom::Limits;

namespace {
constexpr Limits kLim{.min_w = 240, .min_h = 160};
constexpr Box kStart{.x = 100, .y = 80, .w = 800, .h = 500};
} // namespace

TEST_CASE("move translates the whole box, size unchanged") {
    const Box r = geom::apply_drag(kStart, Handle::move, 30, -20, kLim);
    CHECK(r == Box{130, 60, 800, 500});
}

TEST_CASE("move with zero delta is identity") {
    CHECK(geom::apply_drag(kStart, Handle::move, 0, 0, kLim) == kStart);
}

TEST_CASE("resize_br grows width+height, anchors top-left") {
    const Box r = geom::apply_drag(kStart, Handle::resize_br, 50, 40, kLim);
    CHECK(r == Box{100, 80, 850, 540});
}

TEST_CASE("resize_br clamps to min size, top-left still anchored") {
    const Box r = geom::apply_drag(kStart, Handle::resize_br, -10000, -10000, kLim);
    CHECK(r == Box{100, 80, 240, 160});
}

TEST_CASE("resize_bl moves left edge, anchors right edge") {
    // Drag left by 60 (dx=-60): width grows 60, x moves left 60, right edge fixed.
    const Box r = geom::apply_drag(kStart, Handle::resize_bl, -60, 25, kLim);
    CHECK(r.w == 860);
    CHECK(r.x == 40);
    CHECK(r.x + r.w == kStart.x + kStart.w); // right edge unchanged
    CHECK(r.h == 525);
}

TEST_CASE("resize_bl clamps to min width with right edge anchored") {
    // Drag right far (dx huge positive): width floors at min_w, x = right - min_w.
    const Box r = geom::apply_drag(kStart, Handle::resize_bl, 100000, 0, kLim);
    CHECK(r.w == 240);
    CHECK(r.x + r.w == kStart.x + kStart.w);
    CHECK(r.x == kStart.x + kStart.w - 240);
}

TEST_CASE("clamp_to_field keeps the window inside") {
    CHECK(geom::clamp_to_field(Box{-50, -30, 400, 300}, 1920, 1080) ==
          Box{0, 0, 400, 300});
    CHECK(geom::clamp_to_field(Box{1800, 1000, 400, 300}, 1920, 1080) ==
          Box{1520, 780, 400, 300});
}

TEST_CASE("clamp_to_field pins an oversized window to the origin") {
    CHECK(geom::clamp_to_field(Box{200, 200, 3000, 2000}, 1920, 1080) ==
          Box{0, 0, 3000, 2000});
}

TEST_CASE("clamp_to_field with non-positive field is a no-op on that axis") {
    CHECK(geom::clamp_to_field(Box{-50, -30, 400, 300}, 0, 0) == Box{-50, -30, 400, 300});
}

TEST_CASE("a box already inside is unchanged") {
    CHECK(geom::clamp_to_field(Box{100, 100, 400, 300}, 1920, 1080) ==
          Box{100, 100, 400, 300});
}
