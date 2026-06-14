#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config.hpp"
#include "focus_ring.hpp"
#include "policy.hpp"

#include <string>
#include <vector>

// Pure-core tests — the heart of this unit. No kernel, no wlroots (xkbcommon is
// used by the combo parser as the brief sanctions). Four cores: combo parser,
// toml loader, matcher + tap state machine, focus ring.

namespace pol = unbox::ext_keybindings::policy;
namespace cfg = unbox::ext_keybindings::config;

using pol::Action;
using pol::Binding;
using pol::Combo;
using pol::Matcher;
using pol::parse_combo;

// xkb keysyms used across tests (stable XKB_KEY_* numeric values).
static constexpr std::uint32_t kTab = 0xff09;       // XKB_KEY_Tab
static constexpr std::uint32_t kF1 = 0xffbe;        // XKB_KEY_F1
static constexpr std::uint32_t kBackSpace = 0xff08; // XKB_KEY_BackSpace
static constexpr std::uint32_t kD = 0x064;          // XKB_KEY_d

// ============================================================================
// combo parser
// ============================================================================

TEST_CASE("bare modifier parses as a TAP") {
    auto c = parse_combo("Super");
    REQUIRE(c.has_value());
    CHECK(c->is_tap);
    CHECK(c->modifiers == pol::mod_logo);

    CHECK(parse_combo("alt")->is_tap);
    CHECK(parse_combo("CTRL")->is_tap);
    CHECK(parse_combo("Shift")->is_tap);
    CHECK(parse_combo("logo")->modifiers == pol::mod_logo); // Super synonym
}

TEST_CASE("each modifier name maps to its WLR bit (case-insensitive)") {
    CHECK(parse_combo("Alt+Tab")->modifiers == pol::mod_alt);
    CHECK(parse_combo("ctrl+Tab")->modifiers == pol::mod_ctrl);
    CHECK(parse_combo("CONTROL+Tab")->modifiers == pol::mod_ctrl); // synonym
    CHECK(parse_combo("shift+Tab")->modifiers == pol::mod_shift);
    CHECK(parse_combo("super+d")->modifiers == pol::mod_logo);
}

TEST_CASE("multi-modifier chord ORs the bits, last token is the key") {
    auto c = parse_combo("Alt+Shift+Tab");
    REQUIRE(c.has_value());
    CHECK_FALSE(c->is_tap);
    CHECK(c->modifiers == (pol::mod_alt | pol::mod_shift));
    CHECK(c->keysym == kTab);

    auto q = parse_combo("Ctrl+Alt+BackSpace");
    REQUIRE(q.has_value());
    CHECK(q->modifiers == (pol::mod_ctrl | pol::mod_alt));
    CHECK(q->keysym == kBackSpace);
}

TEST_CASE("final keysym resolves case-insensitively") {
    CHECK(parse_combo("Alt+tab")->keysym == kTab);
    CHECK(parse_combo("Alt+TAB")->keysym == kTab);
    CHECK(parse_combo("Alt+F1")->keysym == kF1);
    CHECK(parse_combo("Super+d")->keysym == kD);
}

TEST_CASE("malformed combos return nullopt") {
    CHECK_FALSE(parse_combo("").has_value());          // empty
    CHECK_FALSE(parse_combo("Alt+").has_value());      // trailing +
    CHECK_FALSE(parse_combo("+Tab").has_value());      // leading +
    CHECK_FALSE(parse_combo("Alt++Tab").has_value());  // double +
    CHECK_FALSE(parse_combo("Alt+Boguskey").has_value()); // unknown keysym
    CHECK_FALSE(parse_combo("Alt+Shift").has_value());    // modifier as final key
    CHECK_FALSE(parse_combo("Nope+Tab").has_value());     // unknown modifier
}

// ============================================================================
// toml loader
// ============================================================================

TEST_CASE("loader parses the canonical schema") {
    const std::string toml = R"(
[[keybind]]
keys    = "Super"
action  = "spawn"
command = "fuzzel"

[[keybind]]
keys   = "Alt+Tab"
action = "focus-next"

[[keybind]]
keys   = "Alt+Shift+Tab"
action = "focus-prev"
)";
    auto r = cfg::load_from_string(toml);
    CHECK_FALSE(r.parse_error);
    REQUIRE(r.bindings.size() == 3);

    CHECK(r.bindings[0].combo.is_tap);
    CHECK(r.bindings[0].action == Action::spawn);
    CHECK(r.bindings[0].command == "fuzzel");

    CHECK(r.bindings[1].action == Action::focus_next);
    CHECK(r.bindings[1].combo.keysym == kTab);

    CHECK(r.bindings[2].action == Action::focus_prev);
    CHECK(r.bindings[2].combo.modifiers == (pol::mod_alt | pol::mod_shift));
}

TEST_CASE("loader skips malformed entries but keeps the rest") {
    const std::string toml = R"(
[[keybind]]
keys   = "Alt+Tab"
action = "focus-next"

[[keybind]]
keys   = "Alt+Bogus"
action = "focus-next"

[[keybind]]
keys   = "Alt+F1"
action = "no-such-action"

[[keybind]]
keys   = "Super"
action = "spawn"

[[keybind]]
keys   = "Alt+F1"
action = "quit"
)";
    auto r = cfg::load_from_string(toml);
    CHECK_FALSE(r.parse_error);
    // kept: Alt+Tab and Alt+F1->quit. skipped: bad combo, bad action, spawn w/o command.
    REQUIRE(r.bindings.size() == 2);
    CHECK(r.bindings[0].action == Action::focus_next);
    CHECK(r.bindings[1].action == Action::quit);
    CHECK(r.warnings.size() == 3);
}

TEST_CASE("a toml syntax error is a parse_error with no bindings") {
    auto r = cfg::load_from_string("this is = = not valid toml [[[");
    CHECK(r.parse_error);
    CHECK(r.bindings.empty());
    CHECK_FALSE(r.warnings.empty());
}

TEST_CASE("empty / keybind-less document yields zero bindings, not an error") {
    auto empty = cfg::load_from_string("");
    CHECK_FALSE(empty.parse_error);
    CHECK(empty.bindings.empty());

    auto other = cfg::load_from_string("title = \"unrelated\"\n");
    CHECK_FALSE(other.parse_error);
    CHECK(other.bindings.empty());
}

TEST_CASE("compiled defaults match the documented out-of-the-box set") {
    auto d = pol::default_bindings();
    REQUIRE(d.size() == 6);
    CHECK(d[0].combo.is_tap);
    CHECK(d[0].combo.modifiers == pol::mod_logo);
    CHECK(d[0].action == Action::spawn);
    CHECK(d[0].command == "pkill -x fuzzel || fuzzel"); // tap-Super toggles fuzzel
    CHECK(d[1].combo == parse_combo("Alt+Tab").value());
    CHECK(d[1].action == Action::focus_next);
    CHECK(d[2].combo == parse_combo("Alt+Shift+Tab").value());
    CHECK(d[2].action == Action::focus_prev);
    CHECK(d[3].combo == parse_combo("Alt+F1").value());
    CHECK(d[3].action == Action::focus_next);
    CHECK(d[4].combo == parse_combo("Ctrl+Alt+BackSpace").value());
    CHECK(d[4].action == Action::quit);
    CHECK(d[5].combo == parse_combo("Super+d").value());
    CHECK(d[5].action == Action::dock_toggle_visible);
}

// ============================================================================
// config hot-reload semantics (the swap-or-keep-old reload decision)
// ============================================================================
// Pure proof of the live-reload logic WITHOUT inotify or the event loop: the
// kernel already tests the watcher; here we test only what reload_config() does
// with the file's TEXT once it has been read. config A -> bindings A; reload
// with config B -> bindings B (the swap); reload with MALFORMED text -> bindings
// unchanged (still B) and NO throw. This is exactly the keep-old-on-bad +
// swap-on-good contract the glue's reload_config() relies on.

TEST_CASE("reload: config A -> A, reload B -> B (swap), reload malformed -> unchanged") {
    // --- Initial load: config A (one binding: Alt+Tab -> focus-next). ---
    const std::string config_a = R"(
[[keybind]]
keys   = "Alt+Tab"
action = "focus-next"
)";
    auto a = cfg::load_from_string(config_a);
    REQUIRE_FALSE(a.parse_error);
    REQUIRE(a.bindings.size() == 1);
    std::vector<Binding> live = a.bindings; // the live table the glue holds

    // --- Reload with config B (two bindings: Super tap + Ctrl+Alt+BackSpace). ---
    const std::string config_b = R"(
[[keybind]]
keys    = "Super"
action  = "spawn"
command = "wofi"

[[keybind]]
keys   = "Ctrl+Alt+BackSpace"
action = "quit"
)";
    auto dec_b = cfg::reload_bindings(live, config_b);
    REQUIRE(dec_b.swapped);                 // a usable parse -> SWAP
    REQUIRE(dec_b.bindings.size() == 2);
    CHECK(dec_b.bindings[0].combo.is_tap);
    CHECK(dec_b.bindings[0].action == Action::spawn);
    CHECK(dec_b.bindings[0].command == "wofi"); // command change rides the swap
    CHECK(dec_b.bindings[1].action == Action::quit);
    live = dec_b.bindings; // glue installs the new table

    // --- Reload with MALFORMED text: keep-old, swapped == false, no throw. ---
    cfg::ReloadDecision dec_bad; // (default-constructed; assigned below)
    CHECK_NOTHROW(dec_bad = cfg::reload_bindings(live, "this = = not valid toml [[["));
    CHECK_FALSE(dec_bad.swapped);                // parse error -> KEEP OLD
    CHECK_FALSE(dec_bad.warnings.empty());       // and a diagnostic is surfaced
    REQUIRE(dec_bad.bindings.size() == 2);       // STILL config B, untouched
    CHECK(dec_bad.bindings == live);             // byte-for-byte the working keys
}

TEST_CASE("reload: a valid-but-empty doc keeps the old table (never drops working keys)") {
    // A file that parses cleanly but yields ZERO usable bindings (e.g. saved
    // mid-edit with the [[keybind]] block deleted, or every entry malformed)
    // must NOT swap — the user's live keys survive.
    std::vector<Binding> live = pol::default_bindings();
    REQUIRE(live.size() == 6);

    auto empty = cfg::reload_bindings(live, "title = \"unrelated\"\n");
    CHECK_FALSE(empty.swapped);
    REQUIRE(empty.bindings.size() == 6);
    CHECK(empty.bindings == live); // kept

    // Same for a doc where every entry is individually skipped (all malformed).
    const std::string all_bad = R"(
[[keybind]]
keys   = "Alt+Bogus"
action = "focus-next"

[[keybind]]
keys   = "Super"
action = "spawn"
)";
    auto bad = cfg::reload_bindings(live, all_bad); // bad combo + spawn w/o command
    CHECK_FALSE(bad.swapped);
    CHECK(bad.bindings == live); // STILL the defaults, no throw
}

TEST_CASE("reload: a partial save with at least ONE usable binding swaps to it") {
    // The reload contract is "swap on >=1 usable binding": even if some entries
    // are skipped, as long as one survives, that becomes the new live table.
    std::vector<Binding> live = pol::default_bindings();
    const std::string partial = R"(
[[keybind]]
keys   = "Alt+Bogus"
action = "focus-next"

[[keybind]]
keys   = "Alt+Tab"
action = "focus-next"
)";
    auto dec = cfg::reload_bindings(live, partial);
    REQUIRE(dec.swapped);
    REQUIRE(dec.bindings.size() == 1); // the one usable binding
    CHECK(dec.bindings[0].action == Action::focus_next);
    CHECK_FALSE(dec.warnings.empty()); // the skipped entry was reported
}

// ============================================================================
// matcher + tap state machine
// ============================================================================

static auto make_matcher() -> Matcher {
    return Matcher(pol::default_bindings());
}

TEST_CASE("chord fires on press and consumes, exact-modifier match") {
    auto m = make_matcher();
    // Alt+Tab press -> focus-next, consumed.
    auto out = m.feed(kTab, pol::mod_alt, true);
    REQUIRE(out.fired != Matcher::npos);
    CHECK(m.bindings()[out.fired].action == Action::focus_next);
    CHECK(out.consume);
}

TEST_CASE("chord does not fire on release") {
    auto m = make_matcher();
    auto out = m.feed(kTab, pol::mod_alt, false);
    CHECK(out.fired == Matcher::npos);
}

TEST_CASE("exact-modifier: extra modifier bits do not match a narrower combo") {
    auto m = make_matcher();
    // Ctrl+Alt+Tab must NOT fire Alt+Tab (relevant mods differ).
    auto out = m.feed(kTab, pol::mod_alt | pol::mod_ctrl, true);
    CHECK(out.fired == Matcher::npos);
}

TEST_CASE("Alt+Shift+Tab fires focus-prev, not Alt+Tab") {
    auto m = make_matcher();
    auto out = m.feed(kTab, pol::mod_alt | pol::mod_shift, true);
    REQUIRE(out.fired != Matcher::npos);
    CHECK(m.bindings()[out.fired].action == Action::focus_prev);
}

TEST_CASE("tap-Super: press then release with nothing between FIRES on release") {
    auto m = make_matcher();
    // Super press: nothing fires, not consumed.
    auto down = m.feed(pol::keysym_super_l, pol::mod_logo, true);
    CHECK(down.fired == Matcher::npos);
    CHECK_FALSE(down.consume);
    // Super release: tap fires (spawn fuzzel), NOT consumed.
    auto up = m.feed(pol::keysym_super_l, 0, false);
    REQUIRE(up.fired != Matcher::npos);
    CHECK(m.bindings()[up.fired].action == Action::spawn);
    CHECK_FALSE(up.consume);
}

// ---- REAL-SEAT decider (DEBUG brief) ---------------------------------------
// Hardware capture (/tmp/keycap.log) proved BOTH the keyboard Super and the
// tablet Super emit evdev 125 -> xkb keysym Super_L (0xffeb). This is the EXACT
// press->release sequence the real seat produces, fed through the matcher with
// nothing between. It MUST yield the spawn "fuzzel" action. The kernel does not
// promise whether KeyEvent::modifiers carries WLR_MODIFIER_LOGO on a lone Super
// press/release, so we assert the tap fires for BOTH possible modifier masks.

TEST_CASE("REAL-SEAT: lone Super_L press->release fires spawn (modifiers == 0 both edges)") {
    // The pessimistic case: the kernel reports NO modifier bits on the lone
    // Super press AND release (the modifier mask is computed pre-/post- the key
    // itself, depending on the kernel's ordering). The tap must still fire,
    // because the matcher keys the tap on the KEYSYM, never on modifiers == 0.
    auto m = make_matcher();
    auto down = m.feed(0xffeb /*Super_L*/, 0, true);
    CHECK(down.fired == Matcher::npos);
    CHECK_FALSE(down.consume);
    auto up = m.feed(0xffeb /*Super_L*/, 0, false);
    REQUIRE(up.fired != Matcher::npos);
    CHECK(m.bindings()[up.fired].action == Action::spawn);
    CHECK(m.bindings()[up.fired].command == "pkill -x fuzzel || fuzzel");
    CHECK_FALSE(up.consume);
}

TEST_CASE("REAL-SEAT: lone Super_L press->release fires spawn (WLR_MODIFIER_LOGO set)") {
    // The optimistic case: the kernel reports WLR_MODIFIER_LOGO on the press
    // (and possibly the release). Must ALSO fire — the LOGO bit on the lone
    // Super press must NOT be treated as a Super-carrying chord that marks the
    // tap "used" (the keysym IS the Super key, so it arms rather than gates).
    auto m = make_matcher();
    auto down = m.feed(0xffeb /*Super_L*/, pol::mod_logo, true);
    CHECK(down.fired == Matcher::npos);
    auto up = m.feed(0xffeb /*Super_L*/, pol::mod_logo, false);
    REQUIRE(up.fired != Matcher::npos);
    CHECK(m.bindings()[up.fired].action == Action::spawn);
    CHECK(m.bindings()[up.fired].command == "pkill -x fuzzel || fuzzel");
    CHECK_FALSE(up.consume);
}

TEST_CASE("REAL-SEAT: the tablet Super (a Super_R name) also fires the same tap") {
    // The user wants both Super keys treated identically. On this hardware both
    // emit Super_L, but bind portability: a Super_R event must fire the same
    // bare-"Super" tap binding.
    auto m = make_matcher();
    m.feed(0xffec /*Super_R*/, 0, true);
    auto up = m.feed(0xffec /*Super_R*/, 0, false);
    REQUIRE(up.fired != Matcher::npos);
    CHECK(m.bindings()[up.fired].action == Action::spawn);
}

TEST_CASE("REAL-SEAT suppression: Super down, a down, a up, Super up -> NO tap") {
    // The brief's explicit suppression case. A key pressed while Super is held
    // marks the tap used; the eventual Super release must NOT fire.
    auto m = make_matcher();
    m.feed(0xffeb /*Super_L*/, pol::mod_logo, true);
    m.feed(kD, pol::mod_logo, true);  // 'a'/'d' down while Super held
    m.feed(kD, pol::mod_logo, false); // 'a'/'d' up (release never fires anyway)
    auto up = m.feed(0xffeb /*Super_L*/, pol::mod_logo, false);
    CHECK(up.fired == Matcher::npos); // tap suppressed
}

TEST_CASE("tap-Super resync: a dropped Super release does not eat the next tap") {
    // Regression: "Super sometimes takes several presses to open fuzzel."
    // Simulate a LOST release: Super goes down, but its release event never
    // arrives (dropped on the real seat / swallowed by the parent compositor in
    // nested dev). Then the user types a normal key, which under the old guard
    // latched super_used_ while super_down_ was still stuck true. A subsequent
    // genuine Super tap MUST still fire on its first press->release.
    auto m = make_matcher();
    m.feed(pol::keysym_super_l, pol::mod_logo, true); // Super down
    // ... release LOST (never fed) ...
    m.feed(kD, 0, true);  // user types a key while SM still thinks Super is held
    m.feed(kD, 0, false);
    // Fresh, clean Super tap:
    auto down = m.feed(pol::keysym_super_l, pol::mod_logo, true);
    CHECK(down.fired == Matcher::npos);
    auto up = m.feed(pol::keysym_super_l, 0, false);
    REQUIRE(up.fired != Matcher::npos); // tap fires on the FIRST clean attempt
    CHECK(m.bindings()[up.fired].action == Action::spawn);
    CHECK(m.bindings()[up.fired].command == "pkill -x fuzzel || fuzzel");
}

TEST_CASE("tap-Super gated: another key pressed while held suppresses the tap") {
    auto m = make_matcher();
    m.feed(pol::keysym_super_l, pol::mod_logo, true);
    // A different key goes down while Super is held -> tap used.
    m.feed(kD, pol::mod_logo, true); // Super+d (no binding for it -> npos)
    auto up = m.feed(pol::keysym_super_l, 0, false);
    CHECK(up.fired == Matcher::npos); // tap suppressed
}

TEST_CASE("tap-Super gated: a Super-carrying chord suppresses the tap") {
    auto m = make_matcher();
    m.feed(pol::keysym_super_l, pol::mod_logo, true);
    // Even a key whose modifier mask includes logo marks the tap used.
    m.feed(kTab, pol::mod_logo, true);
    auto up = m.feed(pol::keysym_super_l, 0, false);
    CHECK(up.fired == Matcher::npos);
}

TEST_CASE("tap-Super: a non-tap session never fires a tap") {
    Matcher m(std::vector<Binding>{
        Binding{.combo = parse_combo("Alt+Tab").value(), .action = Action::focus_next, .command = {}}});
    CHECK_FALSE(m.tracks_super_tap());
    m.feed(pol::keysym_super_l, pol::mod_logo, true);
    auto up = m.feed(pol::keysym_super_l, 0, false);
    CHECK(up.fired == Matcher::npos);
}

TEST_CASE("modifier press/release are never consumed") {
    auto m = make_matcher();
    CHECK_FALSE(m.feed(pol::keysym_super_l, pol::mod_logo, true).consume);
    CHECK_FALSE(m.feed(pol::keysym_super_l, 0, false).consume);
}

// ============================================================================
// focus ring
// ============================================================================

// Opaque tokens: integers stand in for Toplevel* (the ring never derefs them).
using Ring = unbox::ext_keybindings::policy::FocusRing<int>;

TEST_CASE("empty ring: next/prev yield nothing") {
    Ring r;
    CHECK(r.empty());
    CHECK(r.next() == nullptr);
    CHECK(r.prev() == nullptr);
}

TEST_CASE("single window: next/prev return that window") {
    Ring r;
    r.add(1);
    REQUIRE(r.next() != nullptr);
    CHECK(*r.next() == 1);
    REQUIRE(r.prev() != nullptr);
    CHECK(*r.prev() == 1);
}

TEST_CASE("rotation walks ALL N in stable map order, wrapping (not MRU ping-pong)") {
    Ring r;
    r.add(10);
    r.add(20);
    r.add(30);
    // No current set yet -> next starts at front.
    REQUIRE(*r.next() == 10);
    r.set_current(10);
    // Repeated next must visit 20, 30, then wrap to 10 — all three, in order.
    CHECK(*r.next() == 20);
    r.set_current(20);
    CHECK(*r.next() == 30);
    r.set_current(30);
    CHECK(*r.next() == 10); // wrap
    r.set_current(10);
    CHECK(*r.next() == 20); // and keeps walking, never ping-ponging 10<->30
}

TEST_CASE("prev walks backward and wraps to the back") {
    Ring r;
    r.add(10);
    r.add(20);
    r.add(30);
    r.set_current(10);
    CHECK(*r.prev() == 30); // wrap to back
    r.set_current(30);
    CHECK(*r.prev() == 20);
    r.set_current(20);
    CHECK(*r.prev() == 10);
}

TEST_CASE("unknown / cleared cursor: next starts at front, prev at back") {
    Ring r;
    r.add(10);
    r.add(20);
    r.add(30);
    CHECK(*r.next() == 10);
    CHECK(*r.prev() == 30);
}

TEST_CASE("removing the current window clears the cursor; next restarts at front") {
    Ring r;
    r.add(10);
    r.add(20);
    r.add(30);
    r.set_current(20);
    r.remove(20);
    CHECK(r.size() == 2);
    CHECK(r.current() == nullptr);
    CHECK(*r.next() == 10); // cursor cleared -> front
}

TEST_CASE("removing a non-current window preserves the cursor and order") {
    Ring r;
    r.add(10);
    r.add(20);
    r.add(30);
    r.set_current(30);
    r.remove(10);
    REQUIRE(r.current() != nullptr);
    CHECK(*r.current() == 30);
    // order now {20,30}; next from 30 wraps to 20.
    CHECK(*r.next() == 20);
}

TEST_CASE("external focus reposition: note_focused moves the cursor, not order") {
    Ring r;
    r.add(10);
    r.add(20);
    r.add(30);
    r.set_current(10);
    // User clicks window 30 (external focus) -> cursor jumps to 30.
    r.note_focused(30);
    REQUIRE(r.current() != nullptr);
    CHECK(*r.current() == 30);
    // Alt+Tab from there wraps to 10, proving order is unchanged.
    CHECK(*r.next() == 10);
}

TEST_CASE("note_focused on an unknown token is ignored") {
    Ring r;
    r.add(10);
    r.set_current(10);
    r.note_focused(999); // not in ring
    REQUIRE(r.current() != nullptr);
    CHECK(*r.current() == 10);
}
