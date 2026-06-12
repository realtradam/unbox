#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "policy.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Pure decision core — strict, no wlroots, no kernel running. Exercises the
// keybinding match, the cycle-next selection over a model list, and the
// interactive-grab state machine (the user-observed drag bug).

using namespace unbox::ext_xdg_shell::policy;

namespace {
constexpr std::uint32_t keysym_escape = 0xff1b; // XKB_KEY_Escape (now unbound)
constexpr std::uint32_t modifier_shift = 1;     // WLR_MODIFIER_SHIFT
} // namespace

TEST_CASE("Ctrl+Alt+Backspace on press maps to terminate") {
    CHECK(match_keybinding(keysym_backspace, modifier_ctrl | modifier_alt, /*pressed=*/true) ==
          KeyAction::terminate);
}

TEST_CASE("terminate needs BOTH Ctrl and Alt with Backspace") {
    CHECK(match_keybinding(keysym_backspace, modifier_alt, true) == KeyAction::none);
    CHECK(match_keybinding(keysym_backspace, modifier_ctrl, true) == KeyAction::none);
    CHECK(match_keybinding(keysym_backspace, /*modifiers=*/0, true) == KeyAction::none);
}

TEST_CASE("no Escape combo is bound any more (no parent-session overlap)") {
    // User veto: quitting must share NO keys with labwc. Both plain Alt+Escape
    // AND Alt+Shift+Escape must pass through unconsumed now.
    CHECK(match_keybinding(keysym_escape, modifier_alt, /*pressed=*/true) == KeyAction::none);
    CHECK(match_keybinding(keysym_escape, modifier_alt | modifier_shift, true) ==
          KeyAction::none);
    CHECK(match_keybinding(keysym_escape, modifier_ctrl | modifier_alt, true) ==
          KeyAction::none);
}

TEST_CASE("Alt+F1 on press maps to cycle_focus") {
    CHECK(match_keybinding(keysym_f1, modifier_alt, true) == KeyAction::cycle_focus);
}

TEST_CASE("cycle binding requires the Alt modifier") {
    CHECK(match_keybinding(keysym_f1, 0, true) == KeyAction::none);
    CHECK(match_keybinding(keysym_f1, modifier_ctrl, true) == KeyAction::none);
}

TEST_CASE("an unbound key passes through") {
    constexpr std::uint32_t keysym_a = 0x0061; // XKB_KEY_a
    CHECK(match_keybinding(keysym_a, modifier_alt, true) == KeyAction::none);
    CHECK(match_keybinding(keysym_a, modifier_ctrl | modifier_alt, true) == KeyAction::none);
}

TEST_CASE("releases never match a binding (press-only)") {
    CHECK(match_keybinding(keysym_backspace, modifier_ctrl | modifier_alt, /*pressed=*/false) ==
          KeyAction::none);
    CHECK(match_keybinding(keysym_f1, modifier_alt, false) == KeyAction::none);
}

TEST_CASE("extra modifiers alongside Alt still match cycle") {
    CHECK(match_keybinding(keysym_f1, modifier_alt | modifier_shift, true) ==
          KeyAction::cycle_focus);
}

// ---- interactive grab state machine (the user-observed move bug) -----------

TEST_CASE("the exact user scenario: press -> request_move -> motion follows; "
          "release -> motion is passthrough") {
    GrabMachine g;
    // Titlebar press: button goes down. No grab yet (client hasn't asked).
    CHECK(g.on_button(/*pressed=*/true) == GrabAction::none);
    CHECK_FALSE(g.grabbing());
    CHECK(g.on_motion() == GrabAction::none); // nothing requested: passthrough

    // Client responds to the press with xdg_toplevel.move while button HELD.
    CHECK(g.on_request_move());
    CHECK(g.grabbing());

    // Motion WHILE HELD must move the toplevel (the bug: it didn't).
    CHECK(g.on_motion() == GrabAction::move_toplevel);
    CHECK(g.on_motion() == GrabAction::move_toplevel); // every held motion

    // Release ends the grab.
    CHECK(g.on_button(/*pressed=*/false) == GrabAction::end_grab);
    CHECK_FALSE(g.grabbing());

    // Motion WITHOUT clicking after release must NOT move (the bug: it did).
    CHECK(g.on_motion() == GrabAction::none);
}

TEST_CASE("a move request that arrives with NO button down does not grab") {
    // The deferred-request race: if request_move lands after the release, the
    // button is already up, so no unclicked drag may start.
    GrabMachine g;
    g.on_button(true);
    g.on_button(false);             // pressed then released, no request yet
    CHECK_FALSE(g.on_request_move()); // late request: button is up
    CHECK_FALSE(g.grabbing());
    CHECK(g.on_motion() == GrabAction::none);
}

TEST_CASE("resize grab: held motion resizes, release ends it") {
    GrabMachine g;
    g.on_button(true);
    CHECK(g.on_request_resize());
    CHECK(g.mode() == GrabMode::resize);
    CHECK(g.on_motion() == GrabAction::resize_toplevel);
    CHECK(g.on_button(false) == GrabAction::end_grab);
    CHECK(g.on_motion() == GrabAction::none);
}

TEST_CASE("grab target lost (unmap/destroy mid-drag) drops the grab silently") {
    GrabMachine g;
    g.on_button(true);
    g.on_request_move();
    CHECK(g.grabbing());
    g.on_grab_target_lost();
    CHECK_FALSE(g.grabbing());
    CHECK(g.on_motion() == GrabAction::none);
    // A later release with no active grab is a no-op (not an end_grab).
    CHECK(g.on_button(false) == GrabAction::none);
}

TEST_CASE("button-down state tracks across presses without a grab") {
    GrabMachine g;
    CHECK_FALSE(g.button_down());
    g.on_button(true);
    CHECK(g.button_down());
    g.on_button(false);
    CHECK_FALSE(g.button_down());
}

// ---- touch-initiated grab (the user-found gap) -----------------------------

TEST_CASE("touch titlebar drag: down -> request_move -> touch motion follows -> "
          "up ends") {
    GrabMachine g;
    constexpr std::int32_t id = 7;
    // Touch lands on the client's CSD titlebar.
    g.on_touch_down(id);
    CHECK_FALSE(g.grabbing());
    // Client responds with xdg_toplevel.move (no POINTER button down).
    CHECK(g.on_request_move());
    CHECK(g.grabbing());
    CHECK(g.touch_driven());
    // Pointer motion must NOT drive a touch grab (source isolation).
    CHECK(g.on_motion() == GrabAction::none);
    // Motion of the originating touch point moves the toplevel.
    CHECK(g.on_touch_motion(id) == GrabAction::move_toplevel);
    CHECK(g.on_touch_motion(id) == GrabAction::move_toplevel);
    // The originating point lifting ends the grab.
    CHECK(g.on_touch_up(id) == GrabAction::end_grab);
    CHECK_FALSE(g.grabbing());
    CHECK(g.on_touch_motion(id) == GrabAction::none);
}

TEST_CASE("touch resize grab follows the originating point and ends on its up") {
    GrabMachine g;
    constexpr std::int32_t id = 3;
    g.on_touch_down(id);
    CHECK(g.on_request_resize());
    CHECK(g.mode() == GrabMode::resize);
    CHECK(g.on_touch_motion(id) == GrabAction::resize_toplevel);
    CHECK(g.on_touch_up(id) == GrabAction::end_grab);
}

TEST_CASE("a move request after the touch point lifted is ignored (no late drag)") {
    GrabMachine g;
    constexpr std::int32_t id = 1;
    g.on_touch_down(id);
    g.on_touch_up(id);                 // lifted before the request arrives
    CHECK_FALSE(g.on_request_move());  // nothing down -> no grab
    CHECK_FALSE(g.grabbing());
    CHECK(g.on_touch_motion(id) == GrabAction::none);
}

TEST_CASE("a second touch point does not steer or end a touch-driven grab") {
    GrabMachine g;
    constexpr std::int32_t origin = 10;
    constexpr std::int32_t other = 20;
    g.on_touch_down(origin);
    CHECK(g.on_request_move());
    CHECK(g.touch_driven());
    // A second finger goes down and moves: it must NOT drive the grab.
    g.on_touch_down(other);
    CHECK(g.on_touch_motion(other) == GrabAction::none);
    // The originating point still drives it.
    CHECK(g.on_touch_motion(origin) == GrabAction::move_toplevel);
    // The second point lifting must NOT end the grab.
    CHECK(g.on_touch_up(other) == GrabAction::none);
    CHECK(g.grabbing());
    // Only the originating point's up ends it.
    CHECK(g.on_touch_up(origin) == GrabAction::end_grab);
    CHECK_FALSE(g.grabbing());
}

TEST_CASE("touch cancel of the originating point ends the grab") {
    GrabMachine g;
    constexpr std::int32_t id = 5;
    g.on_touch_down(id);
    g.on_request_move();
    CHECK(g.grabbing());
    CHECK(g.on_touch_cancel(id) == GrabAction::end_grab);
    CHECK_FALSE(g.grabbing());
}

TEST_CASE("source isolation: a pointer release does not end a touch-driven grab") {
    GrabMachine g;
    constexpr std::int32_t id = 9;
    // Pointer happens to be down too, but touch is preferred and pins the grab.
    g.on_button(true);
    g.on_touch_down(id);
    CHECK(g.on_request_move());
    CHECK(g.touch_driven());
    // Releasing the pointer button must NOT end a touch-driven grab.
    CHECK(g.on_button(false) == GrabAction::none);
    CHECK(g.grabbing());
    // The touch point's up ends it.
    CHECK(g.on_touch_up(id) == GrabAction::end_grab);
}

TEST_CASE("pointer grab still works and touch motion does not drive it") {
    GrabMachine g;
    constexpr std::int32_t id = 2;
    g.on_button(true);
    CHECK(g.on_request_move());
    CHECK_FALSE(g.touch_driven());
    // A stray touch point's motion must not steer a pointer-driven grab.
    g.on_touch_down(id);
    CHECK(g.on_touch_motion(id) == GrabAction::none);
    CHECK(g.on_motion() == GrabAction::move_toplevel);
    CHECK(g.on_button(false) == GrabAction::end_grab);
}

// ---- the regression: a grab must never poison the NEXT grab ----------------
// (The user repro lived in the GLUE — a missing wlr_seat button-release left
// the seat's implicit pointer grab stuck, swallowing later touch-downs. The
// MACHINE-level sequence below proves the pure state carries nothing forward;
// the glue fix is regression-noted in the package doc.)

namespace {
// Helper: one full grab cycle of each kind, asserting it engages and ends and
// leaves the machine idle.
void touch_grab_cycle(GrabMachine& g, std::int32_t id) {
    g.on_touch_down(id);
    REQUIRE(g.on_request_move());
    REQUIRE(g.touch_driven());
    REQUIRE(g.on_touch_motion(id) == GrabAction::move_toplevel);
    REQUIRE(g.on_touch_up(id) == GrabAction::end_grab);
    REQUIRE_FALSE(g.grabbing());
}
void pointer_grab_cycle(GrabMachine& g) {
    g.on_button(true);
    REQUIRE(g.on_request_move());
    REQUIRE_FALSE(g.touch_driven());
    REQUIRE(g.on_motion() == GrabAction::move_toplevel);
    REQUIRE(g.on_button(false) == GrabAction::end_grab);
    REQUIRE_FALSE(g.grabbing());
}
} // namespace

TEST_CASE("EXACT user repro: touch grab -> pointer grab -> touch grab engages again") {
    GrabMachine g;
    touch_grab_cycle(g, 1);
    pointer_grab_cycle(g);
    // The third grab — a touch grab again — MUST engage (the regression).
    g.on_touch_down(2);
    CHECK(g.on_request_move());
    CHECK(g.touch_driven());
    CHECK(g.on_touch_motion(2) == GrabAction::move_toplevel);
    CHECK(g.on_touch_up(2) == GrabAction::end_grab);
}

TEST_CASE("mirrored order: pointer grab -> touch grab -> pointer grab engages again") {
    GrabMachine g;
    pointer_grab_cycle(g);
    touch_grab_cycle(g, 5);
    g.on_button(true);
    CHECK(g.on_request_move());
    CHECK_FALSE(g.touch_driven());
    CHECK(g.on_motion() == GrabAction::move_toplevel);
    CHECK(g.on_button(false) == GrabAction::end_grab);
}

TEST_CASE("many alternating grabs leave no residue (count never poisons engage)") {
    GrabMachine g;
    for (int i = 0; i < 5; ++i) {
        touch_grab_cycle(g, 100 + i);
        pointer_grab_cycle(g);
    }
    // Still works after the loop.
    touch_grab_cycle(g, 999);
}

// ---- interleaving rule: the OTHER input pressed during an active grab -------
// Rule (defined here): an input event from the NON-driving source while a grab
// is active never changes the grab — the grab stays pinned to its originator
// and ends only on the originator's release/up. The newly-pressed other input
// becomes available to drive the NEXT grab once this one ends.

TEST_CASE("pointer press DURING an active touch grab does not hijack or end it") {
    GrabMachine g;
    constexpr std::int32_t id = 7;
    g.on_touch_down(id);
    REQUIRE(g.on_request_move());
    REQUIRE(g.touch_driven());

    // Pointer goes down mid-touch-grab: must not change the grab.
    CHECK(g.on_button(true) == GrabAction::none);
    CHECK(g.touch_driven());
    CHECK(g.on_touch_motion(id) == GrabAction::move_toplevel); // touch still drives
    CHECK(g.on_motion() == GrabAction::none);                  // pointer does not

    // Pointer release mid-touch-grab must NOT end the touch grab.
    CHECK(g.on_button(false) == GrabAction::none);
    CHECK(g.grabbing());

    // Only the originating touch up ends it.
    CHECK(g.on_touch_up(id) == GrabAction::end_grab);
    CHECK_FALSE(g.grabbing());
}

TEST_CASE("touch down DURING an active pointer grab does not hijack or end it") {
    GrabMachine g;
    constexpr std::int32_t id = 8;
    g.on_button(true);
    REQUIRE(g.on_request_move());
    REQUIRE_FALSE(g.touch_driven());

    // A finger touches down mid-pointer-grab: must not change the grab.
    g.on_touch_down(id);
    CHECK_FALSE(g.touch_driven());
    CHECK(g.on_motion() == GrabAction::move_toplevel);          // pointer still drives
    CHECK(g.on_touch_motion(id) == GrabAction::none);           // touch does not

    // That touch lifting must NOT end the pointer grab.
    CHECK(g.on_touch_up(id) == GrabAction::none);
    CHECK(g.grabbing());

    // Only the pointer release ends it.
    CHECK(g.on_button(false) == GrabAction::end_grab);
    CHECK_FALSE(g.grabbing());

    // And a fresh touch grab engages right after.
    touch_grab_cycle(g, id + 1);
}

TEST_CASE("cycle_next picks the back of the focus order when >= 2 windows") {
    CHECK(cycle_next(std::size_t{2}) == 1);
    CHECK(cycle_next(std::size_t{3}) == 2);
    CHECK(cycle_next(std::size_t{5}) == 4);
}

TEST_CASE("cycle_next does nothing with fewer than two windows") {
    CHECK(cycle_next(std::size_t{0}) == no_selection);
    CHECK(cycle_next(std::size_t{1}) == no_selection);
}

TEST_CASE("cycle_next list overload mirrors the count overload") {
    std::vector<std::string> none;
    std::vector<std::string> one{"a"};
    std::vector<std::string> three{"a", "b", "c"};
    CHECK(cycle_next(none) == no_selection);
    CHECK(cycle_next(one) == no_selection);
    CHECK(cycle_next(three) == 2); // index of "c", the least-recently focused
}

TEST_CASE("repeated cycling walks the stack (model simulation)") {
    // Model the glue's focus_order as a front=focused list; cycling focuses
    // the back, which then moves to the front. Two windows ping-pong; three
    // walk in a stable rotation.
    std::vector<std::string> order{"top", "mid", "bot"};
    auto cycle = [&] {
        const std::size_t idx = cycle_next(order.size());
        REQUIRE(idx != no_selection);
        std::string picked = order[idx];
        order.erase(order.begin() + static_cast<long>(idx));
        order.insert(order.begin(), picked);
        return picked;
    };
    CHECK(cycle() == "bot"); // order -> bot, top, mid
    CHECK(order == std::vector<std::string>{"bot", "top", "mid"});
    CHECK(cycle() == "mid"); // order -> mid, bot, top
    CHECK(order == std::vector<std::string>{"mid", "bot", "top"});
}
