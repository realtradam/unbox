#pragma once

#include <cstdint>

// Pure decision core (no wlroots / GL / RMLUi). The glue translates wlroots
// input into these calls and acts on the results. Heavily doctest-covered in
// tests/test_policy.cpp without the kernel present. This file calls nothing in
// the glue — it only computes.
//
// Compositor keybindings (Ctrl+Alt+Backspace terminate, Alt+F1 focus-cycle)
// have been removed from this unit; they now live in ext-keybindings.

namespace unbox::ext_xdg_shell::policy {

// ---- Interactive move/resize grab state machine (pure) ----------------------
//
// Root-causes the user-observed bug: "dragging a titlebar doesn't move the
// window while the button is held, but after releasing, unclicked motion drags
// it." That symptom is a grab whose lifetime is decoupled from the button: the
// grab outlives the press (so post-release motion still moves) and/or is not
// actually engaged while the button is down.
//
// The fix is to make the grab a deterministic function of two facts the glue
// feeds in — whether the pointer button is currently DOWN, and whether the
// client has requested an interactive move/resize — and to gate every action on
// them. Invariants this machine guarantees:
//   * A grab is ACTIVE iff a move/resize was requested AND the button is still
//     down. Motion moves/resizes the toplevel ONLY while active (and the glue
//     suppresses the client pointer notify then).
//   * A button RELEASE always ends the grab and restores passthrough — no
//     matter when the request arrived relative to the press/release.
//   * A move/resize request that arrives while NO button is down does NOT
//     engage a grab (so a stale/late request can never start an unclicked drag).
// This is pure logic; the glue calls process_cursor_move/resize and the
// seat/cursor effects when ask() says so.

enum class GrabMode { none, move, resize };

// What the glue should DO after feeding an event in.
enum class GrabAction {
    none,            // nothing to do
    move_toplevel,   // run process_cursor_move (motion while move-grabbing)
    resize_toplevel, // run process_cursor_resize (motion while resize-grabbing)
    end_grab,        // grab ended: reset cursor mode + restore default cursor
};

// A grab is driven by EITHER the pointer button OR a single touch point. The
// machine tracks which inputs are currently down, and — once a client
// move/resize request engages a grab — pins the grab to ONE originating source:
//   * pointer: ended by the pointer button release;
//   * a specific touch id: ended by THAT point's up/cancel only.
// A second simultaneous touch point never steers or ends the grab (the grab
// follows only its originating point — the simplest honest rule). Pointer and
// touch are isolated: pointer motion never moves a touch-driven grab and vice
// versa.
class GrabMachine {
public:
    [[nodiscard]] auto mode() const -> GrabMode { return mode_; }
    [[nodiscard]] auto grabbing() const -> bool { return mode_ != GrabMode::none; }
    // True iff the pointer button is currently held (preserved pointer API).
    [[nodiscard]] auto button_down() const -> bool { return pointer_down_; }
    // True iff the grab is currently driven by a touch point (else pointer or
    // not grabbing).
    [[nodiscard]] auto touch_driven() const -> bool {
        return mode_ != GrabMode::none && source_ == Source::touch;
    }

    // ---- pointer source ----
    // The pointer button went down/up. A release ALWAYS tears down a grab that
    // the POINTER is driving (a touch-driven grab is unaffected).
    auto on_button(bool pressed) -> GrabAction {
        pointer_down_ = pressed;
        if (!pressed && mode_ != GrabMode::none && source_ == Source::pointer) {
            return reset();
        }
        return GrabAction::none;
    }

    // ---- touch source ----
    // A touch point went down / up / cancelled. Up or cancel of the
    // grab-ORIGINATING point ends the grab; any other point is ignored by the
    // grab machine.
    void on_touch_down(std::int32_t touch_id) {
        ++touch_down_count_;
        last_touch_id_ = touch_id;
        any_touch_id_valid_ = true;
    }
    auto on_touch_up(std::int32_t touch_id) -> GrabAction { return touch_lifted(touch_id); }
    auto on_touch_cancel(std::int32_t touch_id) -> GrabAction { return touch_lifted(touch_id); }

    // ---- engage ----
    // The client requested an interactive move/resize. Engages only while an
    // input is actually down, pinning the grab to that source (touch preferred
    // when a touch point is down — touch CSD drag has no pointer button; a
    // request with NOTHING down is ignored, preventing an unclicked drag).
    auto on_request_move() -> bool { return engage(GrabMode::move); }
    auto on_request_resize() -> bool { return engage(GrabMode::resize); }

    // ---- motion ----
    // Pointer motion: acts only on a pointer-driven grab.
    [[nodiscard]] auto on_motion() const -> GrabAction {
        return source_ == Source::pointer ? motion_action() : GrabAction::none;
    }
    // Touch motion of `touch_id`: acts only on a grab driven by THAT point.
    [[nodiscard]] auto on_touch_motion(std::int32_t touch_id) const -> GrabAction {
        return (source_ == Source::touch && mode_ != GrabMode::none && touch_id == origin_touch_id_)
                   ? motion_action()
                   : GrabAction::none;
    }

    // The grabbed toplevel went away (unmap/destroy): drop the grab silently.
    void on_grab_target_lost() {
        mode_ = GrabMode::none;
        source_ = Source::pointer;
    }

private:
    enum class Source { pointer, touch };

    [[nodiscard]] auto motion_action() const -> GrabAction {
        switch (mode_) {
        case GrabMode::move:
            return GrabAction::move_toplevel;
        case GrabMode::resize:
            return GrabAction::resize_toplevel;
        case GrabMode::none:
            return GrabAction::none;
        }
        return GrabAction::none;
    }

    auto engage(GrabMode mode) -> bool {
        // Touch preferred when a point is down (CSD touch drag has no pointer
        // button); else the pointer; else nothing-down -> ignore the request.
        if (touch_down_count_ > 0 && any_touch_id_valid_) {
            source_ = Source::touch;
            origin_touch_id_ = last_touch_id_;
        } else if (pointer_down_) {
            source_ = Source::pointer;
        } else {
            return false;
        }
        mode_ = mode;
        return true;
    }

    auto touch_lifted(std::int32_t touch_id) -> GrabAction {
        if (touch_down_count_ > 0) {
            --touch_down_count_;
        }
        if (mode_ != GrabMode::none && source_ == Source::touch && touch_id == origin_touch_id_) {
            return reset();
        }
        return GrabAction::none;
    }

    auto reset() -> GrabAction {
        mode_ = GrabMode::none;
        source_ = Source::pointer;
        return GrabAction::end_grab;
    }

    GrabMode mode_ = GrabMode::none;
    Source source_ = Source::pointer;

    // Pointer input state.
    bool pointer_down_ = false;

    // Touch input state. We only need to know whether ANY point is down (to
    // gate engage) and the originating point's id (to steer/end the grab); the
    // brief's "second point must not confuse the grab" is satisfied by pinning
    // to origin_touch_id_ and ignoring all others.
    int touch_down_count_ = 0;
    bool any_touch_id_valid_ = false;
    std::int32_t last_touch_id_ = 0;
    std::int32_t origin_touch_id_ = 0;
};

} // namespace unbox::ext_xdg_shell::policy
