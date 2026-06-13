#pragma once

#include <algorithm>
#include <cstdint>

// Pure decision core 1 — the REVEAL recognizer (GLOSSARY "reveal"/"swipe"): the
// reversible, finger-following recognizer for the left-edge swipe that shows the
// stage dock. No wlroots / GL / RMLUi — input is a stream of drag samples (layout
// x/y + time ms), output is a reveal FRACTION in [0,1] and, on release, a commit
// decision (snap fully open or fully closed). Heavily doctest-covered in
// tests/test_policy.cpp with nothing running; the glue (e1) feeds kernel touch/
// pointer events into these calls and animates from the result.
//
// Reversibility: dragging inward (rightward, away from the left edge) GROWS the
// fraction; dragging back toward the edge SHRINKS it. So the same recognizer
// drives the close gesture too — see RevealRecognizer::begin's start_fraction.
//
// Single wl_event_loop thread throughout (no internal synchronization).

namespace unbox::ext_stage_dock::reveal {

// Tunables for the recognizer. `dock_width` is the inward travel (px) that maps
// to a full reveal (fraction 1). `open_threshold` is the release fraction at or
// above which a slow drag commits OPEN. `fling_velocity` (px/ms, inward
// positive) is the release speed at or above which a fast flick commits OPEN
// even below the threshold (and, symmetrically, an outward flick at or below
// -fling_velocity commits CLOSE even above the threshold). `edge_slop` is how
// far from the left edge (px) a press may start and still count as a reveal.
struct RevealConfig {
    int dock_width = 320;
    double open_threshold = 0.5;
    double fling_velocity = 1.0; // px/ms
    int edge_slop = 24;          // px from the left edge
};

// The release decision: snap the dock fully open or fully closed.
enum class RevealCommit {
    open,
    close,
};

// A reversible finger-following recognizer for ONE drag. Construct with config,
// begin() at the down sample, update() on each move (returns the live fraction),
// end() at the up sample (returns the commit). Reusable across drags: a fresh
// begin() resets all per-drag state.
//
// Geometry note: only the X axis matters (the dock slides horizontally); Y is
// accepted for symmetry with the kernel touch/pointer payloads and ignored. All
// fractions are clamped to [0,1].
class RevealRecognizer {
public:
    explicit RevealRecognizer(RevealConfig config) : config_(config) {}

    // Begin a drag at the down sample (layout x/y, time ms). Returns true iff
    // this is a reveal gesture — i.e. it started within `edge_slop` of the left
    // edge (x <= edge_slop). When false, the caller should ignore the gesture
    // (it is not an edge swipe) and NOT feed update()/end().
    //
    // start_fraction seeds the fraction this drag begins from, so a CLOSE drag
    // (starting from the already-open dock) reuses the SAME recognizer: pass
    // start_fraction = 1.0 and a start x at the dock's right edge, then dragging
    // back toward the screen edge shrinks the fraction toward 0 and end() can
    // commit close. For the normal OPEN-from-hidden gesture, leave it 0.0. The
    // anchor x (origin_x_) is the down x; the fraction tracks inward travel from
    // there, biased by start_fraction.
    auto begin(double x, double y, std::uint32_t t, double start_fraction = 0.0) -> bool {
        (void)y;
        origin_x_ = x;
        start_fraction_ = clamp_fraction(start_fraction);
        fraction_ = start_fraction_;
        last_x_ = x;
        last_t_ = t;
        velocity_ = 0.0;
        active_ = x <= static_cast<double>(config_.edge_slop);
        return active_;
    }

    // Update with the current sample; returns the live reveal fraction in [0,1].
    // The fraction is start_fraction + (inward travel from origin / dock_width),
    // clamped. Inward = rightward (x increasing) for the open gesture; dragging
    // back toward the edge decreases it (fully reversible). Also tracks a
    // smoothed-enough instantaneous velocity (px/ms, inward positive) for the
    // fling decision in end(). A non-active recognizer (begin returned false)
    // returns its current fraction unchanged.
    auto update(double x, double y, std::uint32_t t) -> double {
        (void)y;
        if (!active_) {
            return fraction_;
        }
        const double inward = x - origin_x_;
        const double width = config_.dock_width > 0 ? static_cast<double>(config_.dock_width) : 1.0;
        fraction_ = clamp_fraction(start_fraction_ + inward / width);

        // Instantaneous velocity from the last sample. dt==0 (same timestamp)
        // keeps the previous velocity rather than dividing by zero.
        const double dt = static_cast<double>(t) - static_cast<double>(last_t_);
        if (dt > 0.0) {
            velocity_ = (x - last_x_) / dt;
        }
        last_x_ = x;
        last_t_ = t;
        return fraction_;
    }

    // Release; decide open vs close from the final fraction + recent velocity:
    //   * a fast INWARD fling (velocity >= fling_velocity) commits OPEN even
    //     below the threshold;
    //   * a fast OUTWARD fling (velocity <= -fling_velocity) commits CLOSE even
    //     above the threshold;
    //   * otherwise a slow release commits by position: OPEN iff
    //     fraction >= open_threshold, else CLOSE.
    // `t` is accepted for symmetry / a final velocity refresh but the decision
    // uses the velocity accumulated across update()s. A non-active recognizer
    // commits close (nothing was revealed).
    auto end(std::uint32_t t) -> RevealCommit {
        (void)t;
        if (!active_) {
            return RevealCommit::close;
        }
        if (velocity_ >= config_.fling_velocity) {
            return RevealCommit::open;
        }
        if (velocity_ <= -config_.fling_velocity) {
            return RevealCommit::close;
        }
        return fraction_ >= config_.open_threshold ? RevealCommit::open : RevealCommit::close;
    }

    // The live fraction (same value the last update() returned). Read-only.
    [[nodiscard]] auto fraction() const -> double { return fraction_; }

    // Whether begin() accepted this drag as an edge-started reveal.
    [[nodiscard]] auto active() const -> bool { return active_; }

    // The last measured instantaneous velocity (px/ms, inward positive).
    [[nodiscard]] auto velocity() const -> double { return velocity_; }

private:
    [[nodiscard]] static auto clamp_fraction(double f) -> double {
        return std::clamp(f, 0.0, 1.0);
    }

    RevealConfig config_;
    double origin_x_ = 0.0;
    double start_fraction_ = 0.0;
    double fraction_ = 0.0;
    double last_x_ = 0.0;
    std::uint32_t last_t_ = 0;
    double velocity_ = 0.0;
    bool active_ = false;
};

} // namespace unbox::ext_stage_dock::reveal
