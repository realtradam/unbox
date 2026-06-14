#pragma once

#include <algorithm>
#include <functional>
#include <utility>

// Pure decision core 4 — the SLIDE ANIMATOR: a one-dimensional, interruptible
// easing animator that drives the dock's `slide_px` value over wall-clock time.
// No wlroots / GL / RMLUi — `start()` a run, `tick(dt)` it forward each frame,
// read `value()`; doctest-covered in tests/test_policy.cpp with nothing running.
//
// WHY this lives in C++ (the d1-fix design): RmlUi only starts a CSS transition
// on a class/definition change, NEVER on the inline `data-style-transform` the
// dock uses to render `slide`. So the keyboard / minimize / restore open/close
// paths "snapped" — there was no class flip for RmlUi to animate. We OWN the
// animation here instead: one interruptible animator that every non-finger path
// (keyboard, minimize, restore, drag-RELEASE) flows through, while the duration
// + easing are READ FROM RCSS (UiSurface::transition_timing) so they stay
// hot-reloadable. The finger-driven drag-MOVE bypasses the animator entirely
// (the finger IS the clock — see the glue's drag-move path).
//
// The animator is value-space agnostic: it interpolates `from`->`to` in whatever
// units the caller passes (the glue passes translateX px). The tween is RmlUi's
// own named-transition evaluator wrapped as a pure std::function<float(float)>
// (normalized progress [0,1] -> eased progress), so the on-seat feel matches the
// authored RCSS `transition` exactly.
//
// Single wl_event_loop thread throughout (no internal synchronization).

namespace unbox::ext_stage_dock::anim {

// The largest per-frame dt (seconds) the animator will honour. A frame gap (a
// stall, a tab-out, the first frame after the kernel starts scheduling) can hand
// us a huge dt; clamping it keeps a single tick from teleporting past the whole
// animation (it just advances at most one clamp-worth and the next frame
// continues), so the motion stays visible rather than snapping after a hitch.
inline constexpr double kMaxTickDt = 0.1; // 100 ms

class SlideAnimator {
public:
    // Begin (or REPLACE — interruptible) a run from `from` to `to` over
    // `duration` seconds, eased by `ease` (normalized progress [0,1] -> eased
    // progress in roughly [0,1]). A non-positive duration snaps immediately to
    // `to` (done at once). A null `ease` falls back to linear (identity). The
    // run is active until elapsed >= duration; interrupting mid-run (calling
    // start() again, e.g. a drag-release reversing a keyboard open) simply
    // re-anchors from the new `from` you pass (the glue passes the CURRENT
    // value), so motion is continuous.
    void start(double from, double to, double duration, std::function<float(float)> ease) {
        from_ = from;
        to_ = to;
        duration_ = duration;
        ease_ = std::move(ease);
        elapsed_ = 0.0;
        if (duration_ <= 0.0) {
            // Degenerate: nothing to animate, land on the target immediately.
            value_ = to_;
            active_ = false;
            return;
        }
        value_ = from_;
        active_ = true;
    }

    // Snap the current value to `value` with NO run (the drag-scrub: the finger
    // sets the position directly). Cancels any active run and leaves the animator
    // idle, so a subsequent tick() is inert until the next start().
    void set_immediate(double value) {
        value_ = value;
        active_ = false;
        elapsed_ = 0.0;
        duration_ = 0.0;
    }

    // Advance the run by `dt` seconds (clamped to kMaxTickDt to survive frame
    // gaps) and return the new value. When inactive, returns the held value
    // unchanged. Marks the run DONE the instant elapsed >= duration, pinning the
    // value EXACTLY to `to` (no float drift at the end) and clearing active().
    auto tick(double dt) -> double {
        if (!active_) {
            return value_;
        }
        elapsed_ += std::clamp(dt, 0.0, kMaxTickDt);
        if (elapsed_ >= duration_) {
            value_ = to_;
            active_ = false;
            return value_;
        }
        const double progress = elapsed_ / duration_; // (0,1) here (endpoints handled above)
        const double eased = ease_ ? static_cast<double>(ease_(static_cast<float>(progress)))
                                   : progress; // null ease == linear
        value_ = from_ + (to_ - from_) * eased;
        return value_;
    }

    // The current value (same as the last tick()/start()/set_immediate result).
    [[nodiscard]] auto value() const -> double { return value_; }

    // Whether a run is in progress (start()ed, not yet done, not set_immediate'd).
    [[nodiscard]] auto active() const -> bool { return active_; }

    // The run's target (the value tick() converges to). Useful to the glue to
    // know which DIRECTION it animated (open vs closed) when a run completes.
    [[nodiscard]] auto target() const -> double { return to_; }

private:
    double from_ = 0.0;
    double to_ = 0.0;
    double duration_ = 0.0;
    double elapsed_ = 0.0;
    double value_ = 0.0;
    std::function<float(float)> ease_;
    bool active_ = false;
};

} // namespace unbox::ext_stage_dock::anim
