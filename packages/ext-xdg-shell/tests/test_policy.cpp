#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "policy.hpp"

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
