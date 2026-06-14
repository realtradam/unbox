#pragma once

#include "dock_layout.hpp"
#include "reveal.hpp"

#include <cstdint>
#include <optional>

// Pure decision core 3 — the GESTURE CONTROLLER: the event->state transition
// that turns a touch/drag STREAM into the dock's value-driven slide state
// (slide_px, dragging, open). No wlroots / GL / RMLUi — plain events in, plain
// state + a tiny "what the glue must do next" outcome out, doctest-covered in
// tests/test_policy.cpp with nothing running.
//
// WHY a pure core (extension-agent.md: pure decision core + thin glue): the
// e1 gesture has two input SOURCES that must drive ONE mechanism —
//   * OPEN: the kernel touch bus (Host::on_touch_down/motion/up/cancel), valid
//     because the dock is HIDDEN at touch-down so the implicit-grab contract
//     routes the whole stream to our bus subscription (host.hpp:242-244).
//   * CLOSE: UiSurface::bind_drag, because the OPEN dock is a visible ui surface
//     so the substrate captures its touches into our RMLUi document, NOT the bus.
// Both feed the SAME RevealRecognizer + dock_box; this controller is where they
// converge so the glue is a thin adapter and the headless test can drive the
// full down->motion->up -> (slide_px, dragging, open) mapping WITHOUT a GL
// substrate or synthetic touch injection (which the pixman headless host lacks).
//
// Single wl_event_loop thread throughout (no internal synchronization).

namespace unbox::ext_stage_dock::gesture {

// What the glue must do AFTER a controller call (the side effects the pure core
// cannot perform itself). The glue makes the surface visible on make_visible and
// uses dirty_slide to know the slide value changed. NOTE (d1 fix): the close-hide
// is now driven by the C++ SlideAnimator's COMPLETION (the glue's on_frame),
// NOT an RmlUi transitionend; and dirty_dragging is VESTIGIAL — the
// data-class-dragging machinery was removed (RmlUi never animated the inline
// transform), so the glue ignores it. The field stays for the controller's own
// pure-core tests + a record of the gesture's "now scrubbing" intent.
struct Outcome {
    bool make_visible = false;   // show the surface (open begins compositing)
    bool dirty_slide = false;    // the `slide` value changed -> re-render/animate
    bool dirty_dragging = false; // vestigial (see note above); glue ignores it
};

// The live gesture state, read by the open/close call sites + the slide getter.
// open == is the dock revealed (the glue's animator-completion close-hide is
// gated on !open). slide_px == the open/closed TARGET translateX px (closed =
// -dock_width, open = 0) the glue's SlideAnimator eases toward (the body's
// live translateX is the glue-owned slide_px_ the animator drives, NOT this).
// dragging == is a finger-follow scrub active (the glue scrubs slide directly
// during it rather than animating).
class Controller {
public:
    Controller(reveal::RevealConfig reveal_config, layout::DockMetrics metrics)
        : recognizer_(reveal_config), metrics_(metrics) {}

    // ---- state the glue's bound getters read ----
    [[nodiscard]] auto slide_px() const -> double { return slide_px_; }
    [[nodiscard]] auto dragging() const -> bool { return dragging_; }
    [[nodiscard]] auto open() const -> bool { return open_; }
    [[nodiscard]] auto gesturing() const -> bool { return active_touch_.has_value(); }

    // Refresh the output geometry (multi-output / resize). Only metrics_.output_h
    // and dock_width feed dock_box(...).x, so this keeps the slide math correct
    // after an output change. Does not touch live gesture state.
    void set_metrics(layout::DockMetrics metrics) { metrics_ = metrics; }

    // ---- OPEN path (kernel touch bus) --------------------------------------
    // A touch went down at layout x/y, time t (ms), point `id`. Begins an OPEN
    // reveal iff no gesture is active, the dock is currently closed, and the
    // recognizer accepts the press as edge-started (x <= edge_slop). On accept:
    // records the active touch_id, dragging_ = true, slide_px_ = the f=0 (fully
    // hidden) offset, and asks the glue to make the surface visible + dirty both
    // bindings. Otherwise returns an empty Outcome (ignored: not an edge swipe,
    // or the dock is already open / mid-gesture).
    auto touch_down(std::int32_t id, double lx, double ly, std::uint32_t t) -> Outcome {
        if (active_touch_.has_value() || open_) {
            return {};
        }
        if (!recognizer_.begin(lx, ly, t, /*start_fraction=*/0.0)) {
            return {}; // not an edge-started reveal
        }
        active_touch_ = id;
        dragging_ = true;
        slide_px_ = dock_box(0.0);
        return Outcome{.make_visible = true, .dirty_slide = true, .dirty_dragging = true};
    }

    // A touch moved. If it is the active OPEN gesture's point, advance the
    // recognizer and follow the finger (slide_px_ tracks the live fraction; the
    // transition is off via dragging_). Otherwise a no-op.
    auto touch_motion(std::int32_t id, double lx, double ly, std::uint32_t t) -> Outcome {
        if (!is_active(id)) {
            return {};
        }
        const double frac = recognizer_.update(lx, ly, t);
        slide_px_ = dock_box(frac);
        return Outcome{.dirty_slide = true};
    }

    // A touch lifted. If it is the active OPEN gesture's point, release the
    // recognizer and SNAP: commit decides open vs close (distance >= threshold OR
    // a fast inward fling -> open; else close). See snap() for the shared release.
    auto touch_up(std::int32_t id, std::uint32_t t) -> Outcome {
        if (!is_active(id)) {
            return {};
        }
        const reveal::RevealCommit commit = recognizer_.end(t);
        active_touch_.reset();
        return snap(commit);
    }

    // A touch was cancelled (e.g. palm reject). Treat like a release that reverts
    // to CLOSE (nothing committed), if it is the active gesture's point.
    auto touch_cancel(std::int32_t id) -> Outcome {
        if (!is_active(id)) {
            return {};
        }
        active_touch_.reset();
        return snap(reveal::RevealCommit::close);
    }

    // ---- CLOSE path (UiSurface::bind_drag, surface-LOCAL coords) -----------
    // The open dock captured a drag. x/y are surface-local document px (origin
    // top-left), good for the recognizer directly (no layout-origin subtract).
    // We seed the SAME recognizer for a close: start_fraction 1.0, force_active
    // (the finger lands anywhere on the dock, not at the edge), so dragging back
    // toward the edge drops the fraction from 1.0 and end() can commit close.
    // `t` is a caller-supplied monotonic ms (bind_drag has no time_msec).
    auto drag_start(double x, double y, std::uint32_t t) -> Outcome {
        // Seed the recognizer at the dock's CURRENT fraction, not a hardcoded
        // value: the surface captures the touch (so we arrive here via bind_drag)
        // whether the dock is open (drag to CLOSE, fraction ~1) OR closed-but-
        // -visible (drag to OPEN, fraction ~0). fraction = 1 + slide_px_/width
        // inverts dock_box(); begin() clamps it. force_active because the finger
        // lands anywhere on the dock, not at the screen edge.
        const double w = static_cast<double>(metrics_.dock_width);
        const double cur = (w > 0.0) ? (1.0 + slide_px_ / w) : 1.0;
        recognizer_.begin(x, y, t, /*start_fraction=*/cur, /*force_active=*/true);
        dragging_ = true;
        // Transition off (dragging_) so the body follows the finger 1:1; the first
        // drag_move updates slide_px_ from the live fraction.
        return Outcome{.dirty_dragging = true};
    }

    // A drag moved. Follow the finger back toward the edge (frac drops from 1.0).
    auto drag_move(double x, double y, std::uint32_t t) -> Outcome {
        const double frac = recognizer_.update(x, y, t);
        slide_px_ = dock_box(frac);
        return Outcome{.dirty_slide = true};
    }

    // A drag ended. Release + snap exactly like a touch_up (shared snap()).
    auto drag_end(std::uint32_t t) -> Outcome {
        const reveal::RevealCommit commit = recognizer_.end(t);
        return snap(commit);
    }

    // ---- non-gesture open/close call sites (set the open/closed TARGET) ----
    // Super+M reveal, do_restore reveal, refresh_slots reveal, toggle_visible
    // open: set the OPEN target (slide_px == translateX 0). dragging_ = false. The
    // glue makes the surface visible then ANIMATES slide_px_ to this target via
    // the C++ SlideAnimator (d1 fix). No-op shape if already open (still returns
    // the outcome so the glue's set_visible(true) is idempotent / safe).
    auto open_now() -> Outcome {
        open_ = true;
        dragging_ = false;
        slide_px_ = dock_box(1.0); // == 0
        return Outcome{.make_visible = true, .dirty_slide = true, .dirty_dragging = true};
    }

    // refresh_slots conceal, toggle_visible close: set the CLOSED target. The
    // glue ANIMATES slide_px_ back out via the SlideAnimator; its completion
    // (on_frame) hides the surface once the slide-out finishes (d1 fix — this
    // replaced the old RmlUi transitionend dock_settled hide).
    auto close_now() -> Outcome {
        open_ = false;
        dragging_ = false;
        slide_px_ = dock_box(0.0); // == -dock_width
        return Outcome{.dirty_slide = true, .dirty_dragging = true};
    }

private:
    [[nodiscard]] auto is_active(std::int32_t id) const -> bool {
        return active_touch_.has_value() && *active_touch_ == id;
    }

    // The body translateX px for a reveal fraction (only .x matters here).
    [[nodiscard]] auto dock_box(double fraction) const -> double {
        return static_cast<double>(layout::dock_box(metrics_, fraction).x);
    }

    // Shared release for both paths: stop dragging, set the open/closed state +
    // the snap TARGET px. The glue RESUMES the C++ SlideAnimator from the current
    // finger value to this target (apply_release), easing with the RCSS-read
    // tween; on a CLOSE the animator's completion hides the surface.
    auto snap(reveal::RevealCommit commit) -> Outcome {
        dragging_ = false;
        if (commit == reveal::RevealCommit::open) {
            open_ = true;
            slide_px_ = dock_box(1.0); // 0
            return Outcome{.make_visible = true, .dirty_slide = true, .dirty_dragging = true};
        }
        open_ = false;
        slide_px_ = dock_box(0.0); // -dock_width
        return Outcome{.dirty_slide = true, .dirty_dragging = true};
    }

    reveal::RevealRecognizer recognizer_;
    layout::DockMetrics metrics_;
    std::optional<std::int32_t> active_touch_;
    double slide_px_ = 0.0; // set to the closed offset by the glue at create
    bool dragging_ = false;
    bool open_ = false;
};

} // namespace unbox::ext_stage_dock::gesture
