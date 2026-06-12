#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unbox/ext-layer-shell/arrangement.hpp>

using unbox::ext_layer_shell::apply_exclusive;
using unbox::ext_layer_shell::Box;
using unbox::ext_layer_shell::Edge;
using unbox::ext_layer_shell::exclusive_edge;
using unbox::ext_layer_shell::SurfaceState;
namespace anchor = unbox::ext_layer_shell::anchor;

namespace {

// A 1920x1080 output at the layout origin, the canonical test stage.
constexpr Box kOutput{0, 0, 1920, 1080};

// Anchor a strip to a single edge plus both perpendicular edges (the common
// full-width panel anchoring).
constexpr std::uint32_t kTopStrip = anchor::top | anchor::left | anchor::right;
constexpr std::uint32_t kBottomStrip = anchor::bottom | anchor::left | anchor::right;
constexpr std::uint32_t kLeftStrip = anchor::left | anchor::top | anchor::bottom;
constexpr std::uint32_t kRightStrip = anchor::right | anchor::top | anchor::bottom;

} // namespace

// ---- exclusive_edge: anchoring -> reserved edge -----------------------------

TEST_CASE("non-positive exclusive zone reserves nothing") {
    CHECK(exclusive_edge({.anchor = kTopStrip, .exclusive_zone = 0}) == Edge::none);
    CHECK(exclusive_edge({.anchor = kTopStrip, .exclusive_zone = -1}) == Edge::none);
}

TEST_CASE("a full-width top strip reserves the top edge") {
    CHECK(exclusive_edge({.anchor = kTopStrip, .exclusive_zone = 30}) == Edge::top);
}

TEST_CASE("each single-edge strip deduces its own edge") {
    CHECK(exclusive_edge({.anchor = kTopStrip, .exclusive_zone = 1}) == Edge::top);
    CHECK(exclusive_edge({.anchor = kBottomStrip, .exclusive_zone = 1}) == Edge::bottom);
    CHECK(exclusive_edge({.anchor = kLeftStrip, .exclusive_zone = 1}) == Edge::left);
    CHECK(exclusive_edge({.anchor = kRightStrip, .exclusive_zone = 1}) == Edge::right);
}

TEST_CASE("anchoring to a single bare edge reserves that edge") {
    // Anchored only to top (no perpendicular edges) is still a meaningful strip.
    CHECK(exclusive_edge({.anchor = anchor::top, .exclusive_zone = 5}) == Edge::top);
    CHECK(exclusive_edge({.anchor = anchor::left, .exclusive_zone = 5}) == Edge::left);
}

TEST_CASE("anchoring to a bare corner reserves nothing (protocol rule)") {
    CHECK(exclusive_edge({.anchor = anchor::top | anchor::left, .exclusive_zone = 9}) ==
          Edge::none);
    CHECK(exclusive_edge({.anchor = anchor::bottom | anchor::right, .exclusive_zone = 9}) ==
          Edge::none);
}

TEST_CASE("anchoring to two PARALLEL edges reserves nothing") {
    CHECK(exclusive_edge({.anchor = anchor::top | anchor::bottom, .exclusive_zone = 9}) ==
          Edge::none);
    CHECK(exclusive_edge({.anchor = anchor::left | anchor::right, .exclusive_zone = 9}) ==
          Edge::none);
}

TEST_CASE("anchoring to all four edges reserves nothing") {
    const std::uint32_t all = anchor::top | anchor::bottom | anchor::left | anchor::right;
    CHECK(exclusive_edge({.anchor = all, .exclusive_zone = 9}) == Edge::none);
}

TEST_CASE("unanchored surface reserves nothing even with positive zone") {
    CHECK(exclusive_edge({.anchor = 0, .exclusive_zone = 9}) == Edge::none);
}

// ---- exclusive_edge: explicit override (v5) ---------------------------------

TEST_CASE("explicit exclusive_edge disambiguates a corner anchor") {
    // Bottom-right corner: ambiguous by deduction, but an explicit edge the
    // surface is anchored to resolves it.
    SurfaceState s{.anchor = anchor::bottom | anchor::right,
                   .exclusive_zone = 12,
                   .exclusive_edge = Edge::bottom};
    CHECK(exclusive_edge(s) == Edge::bottom);
    s.exclusive_edge = Edge::right;
    CHECK(exclusive_edge(s) == Edge::right);
}

TEST_CASE("explicit exclusive_edge not among anchored edges is ignored") {
    // Anchored top-left only; asking to reserve the bottom edge is invalid.
    SurfaceState s{.anchor = anchor::top | anchor::left,
                   .exclusive_zone = 12,
                   .exclusive_edge = Edge::bottom};
    CHECK(exclusive_edge(s) == Edge::none);
}

// ---- apply_exclusive: single surface ----------------------------------------

TEST_CASE("a non-reserving surface leaves the usable area untouched") {
    const SurfaceState s{.anchor = kTopStrip, .exclusive_zone = 0};
    CHECK(apply_exclusive(kOutput, s) == kOutput);
}

TEST_CASE("a top panel shrinks the usable area from the top") {
    const SurfaceState s{.anchor = kTopStrip, .exclusive_zone = 30};
    CHECK(apply_exclusive(kOutput, s) == Box{0, 30, 1920, 1050});
}

TEST_CASE("a bottom panel shrinks height but not the origin") {
    const SurfaceState s{.anchor = kBottomStrip, .exclusive_zone = 40};
    CHECK(apply_exclusive(kOutput, s) == Box{0, 0, 1920, 1040});
}

TEST_CASE("a left bar shifts x and shrinks width") {
    const SurfaceState s{.anchor = kLeftStrip, .exclusive_zone = 50};
    CHECK(apply_exclusive(kOutput, s) == Box{50, 0, 1870, 1080});
}

TEST_CASE("a right bar shrinks width but not the origin") {
    const SurfaceState s{.anchor = kRightStrip, .exclusive_zone = 60};
    CHECK(apply_exclusive(kOutput, s) == Box{0, 0, 1860, 1080});
}

TEST_CASE("the margin on the reserved edge adds to the reserved space") {
    // Top panel with a 30px zone and a 10px top margin reserves 40px.
    const SurfaceState s{
        .anchor = kTopStrip, .exclusive_zone = 30, .margin_top = 10};
    CHECK(apply_exclusive(kOutput, s) == Box{0, 40, 1920, 1040});
}

TEST_CASE("margins on non-reserved edges do not affect the usable area") {
    const SurfaceState s{.anchor = kTopStrip,
                         .exclusive_zone = 30,
                         .margin_right = 100,
                         .margin_bottom = 100,
                         .margin_left = 100};
    CHECK(apply_exclusive(kOutput, s) == Box{0, 30, 1920, 1050});
}

// ---- apply_exclusive: stacking several surfaces -----------------------------

TEST_CASE("two perpendicular panels stack into a corner-reduced area") {
    Box usable = kOutput;
    usable = apply_exclusive(usable, {.anchor = kTopStrip, .exclusive_zone = 30});
    usable = apply_exclusive(usable, {.anchor = kLeftStrip, .exclusive_zone = 50});
    CHECK(usable == Box{50, 30, 1870, 1050});
}

TEST_CASE("two opposite panels reduce both ends of an axis") {
    Box usable = kOutput;
    usable = apply_exclusive(usable, {.anchor = kTopStrip, .exclusive_zone = 30});
    usable = apply_exclusive(usable, {.anchor = kBottomStrip, .exclusive_zone = 40});
    CHECK(usable == Box{0, 30, 1920, 1010});
}

TEST_CASE("four panels box in the usable area on every side") {
    Box usable = kOutput;
    usable = apply_exclusive(usable, {.anchor = kTopStrip, .exclusive_zone = 24});
    usable = apply_exclusive(usable, {.anchor = kBottomStrip, .exclusive_zone = 24});
    usable = apply_exclusive(usable, {.anchor = kLeftStrip, .exclusive_zone = 48});
    usable = apply_exclusive(usable, {.anchor = kRightStrip, .exclusive_zone = 48});
    CHECK(usable == Box{48, 24, 1824, 1032});
}

TEST_CASE("two stacked top panels each carve from the running usable area") {
    Box usable = kOutput;
    usable = apply_exclusive(usable, {.anchor = kTopStrip, .exclusive_zone = 30});
    usable = apply_exclusive(usable, {.anchor = kTopStrip, .exclusive_zone = 20});
    // Origin pushed down twice; height reduced by the sum.
    CHECK(usable == Box{0, 50, 1920, 1030});
}

// ---- apply_exclusive: clamping / degenerate cases ---------------------------

TEST_CASE("an over-large exclusive zone clamps the usable area to zero, not negative") {
    const SurfaceState s{.anchor = kTopStrip, .exclusive_zone = 5000};
    const Box out = apply_exclusive(kOutput, s);
    CHECK(out.height == 0);
    CHECK(out.width == 1920);
    // y advanced by the full requested distance even though height clamped.
    CHECK(out.y == 5000);
}

TEST_CASE("width clamps to zero on an over-large side reservation") {
    const SurfaceState s{.anchor = kLeftStrip, .exclusive_zone = 5000};
    const Box out = apply_exclusive(kOutput, s);
    CHECK(out.width == 0);
    CHECK(out.x == 5000);
}

TEST_CASE("a non-origin output box is reduced relative to its own origin") {
    // Second monitor at x=1920.
    const Box second{1920, 0, 1280, 1024};
    const SurfaceState s{.anchor = kTopStrip, .exclusive_zone = 25};
    CHECK(apply_exclusive(second, s) == Box{1920, 25, 1280, 999});
}

TEST_CASE("a -1 (stretch-over) surface never reduces the usable area") {
    // Wallpaper/lockscreen anchored to all four with stretch-over zone.
    const std::uint32_t all = anchor::top | anchor::bottom | anchor::left | anchor::right;
    const SurfaceState s{.anchor = all, .exclusive_zone = -1};
    CHECK(apply_exclusive(kOutput, s) == kOutput);
}
