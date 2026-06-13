#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "dock_layout.hpp"
#include "reveal.hpp"

// Pure-core tests — the heart of this b4 step. No kernel, no wlroots, no RMLUi.
// Two cores: the reveal recognizer (reversible edge swipe -> fraction + commit)
// and the dock layout geometry (reveal fraction + slot count -> rects).

namespace rv = unbox::ext_stage_dock::reveal;
namespace lay = unbox::ext_stage_dock::layout;

using rv::RevealCommit;
using rv::RevealConfig;
using rv::RevealRecognizer;

// ============================================================================
// reveal recognizer
// ============================================================================

// A config with round numbers so fractions are exact: 100px dock, threshold
// 0.5, fling 1.0 px/ms, 24px edge slop.
static auto cfg() -> RevealConfig {
    return RevealConfig{.dock_width = 100, .open_threshold = 0.5, .fling_velocity = 1.0, .edge_slop = 24};
}

TEST_CASE("begin: an edge-started press is a reveal; a non-edge press is not") {
    RevealRecognizer r(cfg());
    CHECK(r.begin(0.0, 200.0, 0));      // exactly the left edge
    RevealRecognizer r2(cfg());
    CHECK(r2.begin(24.0, 200.0, 0));    // exactly at edge_slop
    RevealRecognizer r3(cfg());
    CHECK_FALSE(r3.begin(25.0, 200.0, 0));  // just past the slop -> not a reveal
    RevealRecognizer r4(cfg());
    CHECK_FALSE(r4.begin(500.0, 200.0, 0)); // mid-screen -> not a reveal
}

TEST_CASE("fraction grows with inward drag and is clamped to [0,1]") {
    RevealRecognizer r(cfg());
    REQUIRE(r.begin(0.0, 0.0, 0));
    CHECK(r.fraction() == doctest::Approx(0.0));
    CHECK(r.update(50.0, 0.0, 100) == doctest::Approx(0.5)); // 50/100 dock_width
    CHECK(r.update(100.0, 0.0, 200) == doctest::Approx(1.0)); // full
    CHECK(r.update(180.0, 0.0, 300) == doctest::Approx(1.0)); // clamped at 1
}

TEST_CASE("fraction shrinks on reverse drag (reversible) and clamps at 0") {
    RevealRecognizer r(cfg());
    REQUIRE(r.begin(0.0, 0.0, 0));
    CHECK(r.update(80.0, 0.0, 100) == doctest::Approx(0.8));
    CHECK(r.update(30.0, 0.0, 200) == doctest::Approx(0.3)); // dragged back inward->edge
    CHECK(r.update(0.0, 0.0, 300) == doctest::Approx(0.0));  // back to the edge
    CHECK(r.update(-40.0, 0.0, 400) == doctest::Approx(0.0)); // past the edge -> clamp 0
}

TEST_CASE("slow drag past threshold commits open; below threshold commits close") {
    RevealRecognizer open(cfg());
    REQUIRE(open.begin(0.0, 0.0, 0));
    open.update(60.0, 0.0, 1000);   // fraction 0.6, 60px over 1000ms -> 0.06 px/ms (slow)
    CHECK(open.end(1000) == RevealCommit::open);

    RevealRecognizer close(cfg());
    REQUIRE(close.begin(0.0, 0.0, 0));
    close.update(40.0, 0.0, 1000);  // fraction 0.4 (slow) -> below threshold
    CHECK(close.end(1000) == RevealCommit::close);
}

TEST_CASE("at-threshold (==open_threshold) commits open") {
    RevealRecognizer r(cfg());
    REQUIRE(r.begin(0.0, 0.0, 0));
    r.update(50.0, 0.0, 1000); // exactly 0.5
    CHECK(r.end(1000) == RevealCommit::open);
}

TEST_CASE("a fast inward fling under threshold still commits open (velocity)") {
    RevealRecognizer r(cfg());
    REQUIRE(r.begin(0.0, 0.0, 0));
    // Only 30px (fraction 0.3, below 0.5) but in 10ms -> 3 px/ms >= 1.0 fling.
    r.update(30.0, 0.0, 10);
    CHECK(r.fraction() == doctest::Approx(0.3));
    CHECK(r.end(10) == RevealCommit::open);
}

TEST_CASE("a fast outward fling over threshold commits close (velocity)") {
    RevealRecognizer r(cfg());
    REQUIRE(r.begin(0.0, 0.0, 0));
    r.update(90.0, 0.0, 1000);    // fraction 0.9 (well past threshold), slow
    r.update(60.0, 0.0, 1005);    // yanked back 30px in 5ms -> -6 px/ms outward fling
    CHECK(r.fraction() == doctest::Approx(0.6)); // still past threshold by position
    CHECK(r.end(1005) == RevealCommit::close);   // but the fling closes it
}

TEST_CASE("symmetric CLOSE drag: seed start_fraction=1.0, drag toward edge to close") {
    // The already-open dock: begin at the dock's right edge with start_fraction
    // 1.0, then drag back toward the screen edge. The SAME recognizer drives it.
    RevealRecognizer r(cfg());
    REQUIRE(r.begin(0.0, 0.0, 0, /*start_fraction=*/1.0));
    CHECK(r.fraction() == doctest::Approx(1.0));
    // Drag inward->edge by 70px slowly: 1.0 + (-70/100) = 0.3, below threshold.
    r.update(-70.0, 0.0, 1000);
    CHECK(r.fraction() == doctest::Approx(0.3));
    CHECK(r.end(1000) == RevealCommit::close);
}

TEST_CASE("symmetric CLOSE drag that does not travel far stays open") {
    RevealRecognizer r(cfg());
    REQUIRE(r.begin(0.0, 0.0, 0, /*start_fraction=*/1.0));
    r.update(-20.0, 0.0, 1000); // 1.0 - 0.2 = 0.8, still past threshold, slow
    CHECK(r.end(1000) == RevealCommit::open);
}

TEST_CASE("dt==0 samples do not divide by zero and keep prior velocity") {
    RevealRecognizer r(cfg());
    REQUIRE(r.begin(0.0, 0.0, 0));
    r.update(30.0, 0.0, 10);          // 3 px/ms inward
    const double v = r.velocity();
    r.update(40.0, 0.0, 10);          // same timestamp -> velocity unchanged
    CHECK(r.velocity() == doctest::Approx(v));
    CHECK(r.fraction() == doctest::Approx(0.4)); // fraction still tracks position
}

TEST_CASE("an inactive (non-edge) recognizer is inert") {
    RevealRecognizer r(cfg());
    CHECK_FALSE(r.begin(500.0, 0.0, 0));
    CHECK_FALSE(r.active());
    CHECK(r.update(600.0, 0.0, 100) == doctest::Approx(0.0)); // no movement
    CHECK(r.end(100) == RevealCommit::close);
}

// ============================================================================
// dock layout
// ============================================================================

static auto metrics() -> lay::DockMetrics {
    return lay::DockMetrics{
        .output_w = 1920, .output_h = 1080, .dock_width = 300,
        .slot_height = 100, .gap = 10, .pad = 20};
}

TEST_CASE("dock_box: f=0 fully off-screen left, f=1 flush at x==0") {
    auto m = metrics();
    auto hidden = lay::dock_box(m, 0.0);
    CHECK(hidden.x == -300); // -dock_width
    CHECK(hidden.w == 300);
    CHECK(hidden.y == 0);
    CHECK(hidden.h == 1080); // covers the output

    auto shown = lay::dock_box(m, 1.0);
    CHECK(shown.x == 0);
    CHECK(shown.w == 300);
    CHECK(shown.h == 1080);
}

TEST_CASE("dock_box: x is monotonic non-decreasing in f and clamps outside [0,1]") {
    auto m = metrics();
    CHECK(lay::dock_box(m, 0.5).x == -150); // halfway
    int prev = lay::dock_box(m, 0.0).x;
    for (double f = 0.0; f <= 1.0; f += 0.1) {
        int x = lay::dock_box(m, f).x;
        CHECK(x >= prev);
        prev = x;
    }
    CHECK(lay::dock_box(m, -1.0).x == -300); // clamped to f=0
    CHECK(lay::dock_box(m, 2.0).x == 0);     // clamped to f=1
}

TEST_CASE("visible_slots: 0/1/many capacity") {
    // usable = 1080 - 40 = 1040; stride = 110; 1 + (1040-100)/110 = 1 + 8 = 9.
    CHECK(lay::visible_slots(metrics()) == 9);

    // Exactly one slot fits.
    lay::DockMetrics one{.output_w = 0, .output_h = 140, .dock_width = 300,
                         .slot_height = 100, .gap = 10, .pad = 20};
    CHECK(lay::visible_slots(one) == 1); // usable 100 == slot_height

    // Nothing fits (usable < slot_height).
    lay::DockMetrics none{.output_w = 0, .output_h = 100, .dock_width = 300,
                          .slot_height = 100, .gap = 10, .pad = 20};
    CHECK(lay::visible_slots(none) == 0); // usable 60 < 100
}

TEST_CASE("content_height: 0/1/many slots") {
    auto m = metrics(); // pad 20, slot 100, gap 10
    CHECK(lay::content_height(m, 0) == 0);
    CHECK(lay::content_height(m, 1) == 2 * 20 + 100);              // 140, no gap
    CHECK(lay::content_height(m, 3) == 2 * 20 + 3 * 100 + 2 * 10); // 360
}

TEST_CASE("slot_box: vertical stacking by stride, inset width") {
    auto m = metrics(); // pad 20, slot 100, gap 10, dock_width 300
    auto s0 = lay::slot_box(m, 0, 0);
    CHECK(s0.x == 20);                 // inner pad
    CHECK(s0.y == 20);                 // pad + 0*stride
    CHECK(s0.w == 300 - 2 * 20);       // dock_width minus pad both sides = 260
    CHECK(s0.h == 100);

    auto s1 = lay::slot_box(m, 1, 0);
    CHECK(s1.y == 20 + 110);           // pad + 1*stride(110) = 130
    auto s2 = lay::slot_box(m, 2, 0);
    CHECK(s2.y == 20 + 220);           // 240
}

TEST_CASE("slot_box: scroll offset shifts slots up") {
    auto m = metrics();
    auto s2_unscrolled = lay::slot_box(m, 2, 0);
    auto s2_scrolled = lay::slot_box(m, 2, 150);
    CHECK(s2_scrolled.y == s2_unscrolled.y - 150);
    CHECK(s2_scrolled.x == s2_unscrolled.x); // scroll is vertical only
}
