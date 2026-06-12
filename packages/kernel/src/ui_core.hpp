#pragma once

#include <cstdint>

// Pure decision cores for the ui substrate — NO wlroots / GL / RMLUi types, so
// they are doctest-able with nothing running (AGENTS.md: effects at the edges,
// pure cores tested hard). The substrate glue (ui_substrate.cpp) injects the
// effects around these.
//
// Everything runs on the single wl_event_loop thread; no synchronization here.

namespace unbox::kernel {

// touch-mode: the substrate-level theme state that scales hit targets for
// finger input. It flips automatically by last-input kind — a touch event
// turns it ON, pointer motion turns it OFF — but with debounce so a stray
// pointer jitter during a touch interaction (palm, accidental trackpad brush)
// does not flicker it. A manual override pins it for tests/config.
//
// Pure state machine: feed it input-kind events + a monotonic timestamp; it
// returns whether the effective mode CHANGED so the caller can re-theme only
// on a transition. The debounce rule: after a touch event, pointer motion is
// ignored for `debounce_ms`; a touch event always wins immediately.
class TouchModeTracker {
public:
    enum class Mode { pointer, touch };
    enum class Override { none, force_pointer, force_touch };

    explicit TouchModeTracker(std::uint32_t debounce_ms = 700) : debounce_ms_(debounce_ms) {}

    // A touch event happened at time_msec. Returns true if the EFFECTIVE mode
    // changed. Touch always wins immediately and arms the debounce window.
    auto on_touch(std::uint32_t time_msec) -> bool {
        last_touch_msec_ = time_msec;
        have_touch_ = true;
        return set_auto(Mode::touch);
    }

    // Pointer motion at time_msec. Ignored (does NOT flip to pointer) while
    // within debounce_ms of the last touch — that suppresses palm/jitter.
    // Returns true if the effective mode changed.
    auto on_pointer_motion(std::uint32_t time_msec) -> bool {
        if (have_touch_ && time_msec - last_touch_msec_ < debounce_ms_) {
            return false; // inside the debounce shadow of a touch
        }
        return set_auto(Mode::pointer);
    }

    // Pin the mode regardless of input (tests/config). Override::none returns
    // to automatic, adopting the current auto-derived mode. Returns true if the
    // effective mode changed.
    auto set_override(Override ov) -> bool {
        const Mode before = effective();
        override_ = ov;
        return effective() != before;
    }

    [[nodiscard]] auto effective() const -> Mode {
        switch (override_) {
        case Override::force_pointer: return Mode::pointer;
        case Override::force_touch: return Mode::touch;
        case Override::none: break;
        }
        return auto_mode_;
    }

    [[nodiscard]] auto is_touch() const -> bool { return effective() == Mode::touch; }

private:
    auto set_auto(Mode m) -> bool {
        const Mode before = effective();
        auto_mode_ = m;
        return effective() != before;
    }

    std::uint32_t debounce_ms_;
    std::uint32_t last_touch_msec_ = 0;
    bool have_touch_ = false;
    Mode auto_mode_ = Mode::pointer;
    Override override_ = Override::none;
};

// NOTE (user decision, slice-5 hands-on): touch-mode causes NO automatic visual
// scaling. The dp-ratio knob is retired — the substrate leaves every context at
// RmlUi's default 1.0 permanently, so `dp` behaves like `px` in practice. The
// touch-mode STATE (auto-flip + debounce + on_touch_mode_changed notification)
// stays meaningful for invisible affordances and later slices (OSK auto-show,
// spacing); an extension that wants to adapt does so itself via the
// notification. (Earlier slices applied a 1.0/1.25 ratio here.)

// Who owns an in-flight pointer/touch grab — the consumer of the initiating
// press/down owns the whole stream until it ends (standard seat implicit-grab
// behavior). `none` = no grab active.
enum class GrabOwner { none, substrate, bus };

// Pure implicit-grab state for the pointer button stream. A grab begins on the
// FIRST button press (when no button was down) and ends when the LAST button
// is released; the owner is decided ONCE at grab start by whether the press
// landed over a ui surface, and EVERY event until the grab ends routes to that
// owner — regardless of what the cursor is over later. This is what makes a
// release land on the same party as its press (the slice-5 stuck-drag bug:
// press→extensions, release-over-ui-surface must still reach extensions).
class PointerButtonGrab {
public:
    // A button press landed; `over_surface` is the hit-test AT PRESS TIME.
    // Returns the owner this press (and the rest of the grab) routes to.
    auto press(bool over_surface) -> GrabOwner {
        if (down_count_ == 0) {
            owner_ = over_surface ? GrabOwner::substrate : GrabOwner::bus;
        }
        ++down_count_;
        return owner_;
    }
    // A button release. Returns the owner it routes to (the grab's owner). The
    // grab ends (owner -> none) when the last button comes up.
    auto release() -> GrabOwner {
        const GrabOwner who = owner_;
        if (down_count_ > 0 && --down_count_ == 0) {
            owner_ = GrabOwner::none;
        }
        return who;
    }
    [[nodiscard]] auto owner() const -> GrabOwner { return owner_; }
    [[nodiscard]] auto active() const -> bool { return down_count_ > 0; }

private:
    int down_count_ = 0;
    GrabOwner owner_ = GrabOwner::none;
};

// Axis-aligned hit test in layout coordinates: is (lx,ly) inside the rect at
// (x,y) of size w×h? Half-open on the far edges (matches scene node bounds).
[[nodiscard]] constexpr auto point_in_rect(double lx, double ly, int x, int y, int w, int h)
    -> bool {
    return lx >= x && ly >= y && lx < static_cast<double>(x) + w &&
           ly < static_cast<double>(y) + h;
}

} // namespace unbox::kernel
