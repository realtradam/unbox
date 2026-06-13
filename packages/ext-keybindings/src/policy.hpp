#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <xkbcommon/xkbcommon.h>

// Pure decision core (no wlroots / GL / RMLUi; xkbcommon is used only for the
// keysym-name resolution the brief sanctions for the combo parser). The glue
// translates kernel KeyEvents into these calls and acts on the results. Heavily
// doctest-covered in tests/test_policy.cpp with nothing running. This file calls
// nothing in the glue — it only parses and decides.

namespace unbox::ext_keybindings::policy {

// ---- WLR_MODIFIER_* bits ----------------------------------------------------
//
// Mirrored here as plain constants so the core does not pull wlr.hpp; the glue
// masks the live modifier state against the same WLR_MODIFIER_* values (the
// kernel's KeyEvent::modifiers is a WLR_MODIFIER_* mask).
inline constexpr std::uint32_t mod_shift = 1u << 0; // WLR_MODIFIER_SHIFT
inline constexpr std::uint32_t mod_ctrl = 1u << 2;  // WLR_MODIFIER_CTRL
inline constexpr std::uint32_t mod_alt = 1u << 3;   // WLR_MODIFIER_ALT
inline constexpr std::uint32_t mod_logo = 1u << 6;  // WLR_MODIFIER_LOGO (Super)

// The modifier bits a combo match is allowed to require. We match these EXACTLY
// (no stray Caps/Num lock affects the decision: those bits are outside this
// mask and ignored).
inline constexpr std::uint32_t mod_relevant = mod_shift | mod_ctrl | mod_alt | mod_logo;

// The xkb keysyms for the two logo (Super) keys, so the tap state machine can
// recognize the bare-modifier press/release without an xkbcommon include in the
// glue's hot path. Stable XKB_KEY_* numeric values.
inline constexpr std::uint32_t keysym_super_l = 0xffeb; // XKB_KEY_Super_L
inline constexpr std::uint32_t keysym_super_r = 0xffec; // XKB_KEY_Super_R

[[nodiscard]] inline auto is_super_keysym(std::uint32_t keysym) -> bool {
    return keysym == keysym_super_l || keysym == keysym_super_r;
}

// ---- Action vocabulary ------------------------------------------------------

enum class Action {
    spawn,        // run `command` via `sh -c`
    focus_next,   // rotate focus forward across mapped windows (wrapping)
    focus_prev,   // rotate focus backward (wrapping)
    close_active, // close the focused toplevel (no-op if none)
    quit,         // wl_display_terminate
};

// Map an action token (lowercased) to the enum; nullopt = unknown action.
[[nodiscard]] inline auto action_from_string(std::string_view s) -> std::optional<Action> {
    if (s == "spawn") {
        return Action::spawn;
    }
    if (s == "focus-next") {
        return Action::focus_next;
    }
    if (s == "focus-prev") {
        return Action::focus_prev;
    }
    if (s == "close-active") {
        return Action::close_active;
    }
    if (s == "quit") {
        return Action::quit;
    }
    return std::nullopt;
}

// ---- Combo ------------------------------------------------------------------
//
// A parsed `keys` string. Either a normal modifier+key chord, or a bare-modifier
// TAP (is_tap == true; `modifiers` holds the single tapped modifier mask, and
// `keysym` is unused). `modifiers` is a WLR_MODIFIER_* mask; `keysym` is the xkb
// keysym of the final key token.
struct Combo {
    std::uint32_t modifiers = 0;
    std::uint32_t keysym = 0;
    bool is_tap = false;

    [[nodiscard]] auto operator==(const Combo&) const -> bool = default;
};

// Map a single modifier token (case-insensitive) to its WLR_MODIFIER_* bit;
// nullopt = not a known modifier name.
[[nodiscard]] inline auto modifier_bit(std::string_view tok) -> std::optional<std::uint32_t> {
    // Lowercase compare (tokens are short; avoid allocating).
    auto eq = [tok](std::string_view name) {
        if (tok.size() != name.size()) {
            return false;
        }
        for (std::size_t i = 0; i < tok.size(); ++i) {
            char c = tok[i];
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != name[i]) {
                return false;
            }
        }
        return true;
    };
    if (eq("super") || eq("logo")) {
        return mod_logo;
    }
    if (eq("alt")) {
        return mod_alt;
    }
    if (eq("ctrl") || eq("control")) {
        return mod_ctrl;
    }
    if (eq("shift")) {
        return mod_shift;
    }
    return std::nullopt;
}

// Resolve a final key token to an xkb keysym (case-insensitive). Returns 0
// (XKB_KEY_NoSymbol) if the name does not resolve.
[[nodiscard]] inline auto keysym_from_token(const std::string& tok) -> std::uint32_t {
    return xkb_keysym_from_name(tok.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
}

// Parse a `keys` string ("Super", "Alt+Tab", "Ctrl+Alt+BackSpace", "Super+d")
// into a Combo. Rules (brief schema):
//   * `Mod(+Mod...)+Key` -> a chord: modifier bits OR'd, final token a keysym.
//   * a SINGLE bare modifier token ("Super") -> a TAP binding.
//   * Returns nullopt for: an empty string, an empty token (leading/trailing/
//     double '+'), an unknown final keysym, a modifier name used as the final
//     key, or a non-modifier token used where a modifier belongs.
[[nodiscard]] inline auto parse_combo(std::string_view keys) -> std::optional<Combo> {
    if (keys.empty()) {
        return std::nullopt;
    }

    // Split on '+'.
    std::vector<std::string_view> tokens;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= keys.size(); ++i) {
        if (i == keys.size() || keys[i] == '+') {
            tokens.push_back(keys.substr(start, i - start));
            start = i + 1;
        }
    }
    for (std::string_view t : tokens) {
        if (t.empty()) {
            return std::nullopt; // leading/trailing/double '+'
        }
    }

    // Bare single modifier -> TAP.
    if (tokens.size() == 1) {
        if (auto m = modifier_bit(tokens.front())) {
            return Combo{.modifiers = *m, .keysym = 0, .is_tap = true};
        }
        // A single non-modifier token is a key with no modifiers — fall through
        // to the chord path so e.g. "F1" alone (if ever configured) resolves.
    }

    // Chord: every token but the last is a modifier; the last is the key.
    Combo combo{};
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        auto m = modifier_bit(tokens[i]);
        if (!m) {
            return std::nullopt; // a non-modifier where a modifier belongs
        }
        combo.modifiers |= *m;
    }
    const std::string final_tok(tokens.back());
    // A modifier name as the FINAL token (and not the bare-tap case above) is
    // malformed — e.g. "Alt+Shift" with no key.
    if (modifier_bit(final_tok)) {
        return std::nullopt;
    }
    combo.keysym = keysym_from_token(final_tok);
    if (combo.keysym == 0) {
        return std::nullopt; // unknown keysym name
    }
    return combo;
}

// ---- Binding ----------------------------------------------------------------
//
// One [[keybind]]: a parsed combo + the action it triggers (+ the spawn command
// for Action::spawn). Produced by the toml loader (config.hpp); consumed by the
// Matcher.
struct Binding {
    Combo combo;
    Action action = Action::quit;
    std::string command; // only meaningful for Action::spawn

    [[nodiscard]] auto operator==(const Binding&) const -> bool = default;
};

// ---- Matcher + tap state machine --------------------------------------------
//
// The decision core for the input path: fed a sequence of (keysym, modifiers,
// pressed) it reports which Binding (if any) fires for each event, so the glue
// can consume the key and run the action. Holds the bare-Super TAP state.
//
// Semantics (brief):
//   * CHORD bindings fire on PRESS when the keysym matches and the relevant
//     modifier bits match EXACTLY (so Alt+Tab does not fire for Ctrl+Alt+Tab).
//     A chord that USES Super marks the tap as "used" so a tap does not also
//     fire on the eventual Super release.
//   * TAP bindings fire on the modifier's RELEASE iff it was pressed and
//     released with nothing in between (no other key press, no chord use).
//   * The modifier press/release themselves are NEVER consumed (other combos
//     need the modifier held); only a firing tap is an effect (and the glue
//     consumes nothing on a tap either — the modifier already passed through).
class Matcher {
public:
    // Result of feeding one event. `fired` is the index into the bindings list
    // of the binding that should run, or npos for "nothing fires". `consume` is
    // true iff the glue should set KeyEvent::handled (suppress client forward):
    // true for a fired CHORD press, false otherwise (taps consume nothing).
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    struct Outcome {
        std::size_t fired = npos;
        bool consume = false;
    };

    explicit Matcher(std::vector<Binding> bindings) : bindings_(std::move(bindings)) {
        // Does any binding tap on Super? Cache so non-Super sessions skip SM.
        for (const Binding& b : bindings_) {
            if (b.combo.is_tap && (b.combo.modifiers & mod_logo) != 0) {
                super_tap_ = true;
                break;
            }
        }
    }

    [[nodiscard]] auto bindings() const -> const std::vector<Binding>& { return bindings_; }

    // Feed one key event. Updates the tap state machine and returns what fires.
    auto feed(std::uint32_t keysym, std::uint32_t modifiers, bool pressed) -> Outcome {
        // --- Tap state machine for Super ---
        if (is_super_keysym(keysym)) {
            if (pressed) {
                // A fresh Super press arms the tap (only if nothing else was
                // already held that would make this not a clean tap). Re-press
                // while armed (auto-repeat) keeps the armed state.
                if (!super_down_) {
                    super_down_ = true;
                    super_used_ = false;
                }
            } else {
                // Super release: fire the bare-Super tap iff it was a clean tap.
                Outcome out{};
                if (super_down_ && !super_used_) {
                    out.fired = find_super_tap();
                    out.consume = false; // taps never consume
                }
                super_down_ = false;
                super_used_ = false;
                return out;
            }
            return Outcome{}; // the Super press itself is never consumed
        }

        // --- Any non-Super key press while Super is armed marks it used ---
        if (pressed && super_down_) {
            super_used_ = true;
        }
        // A chord that explicitly carries Super in its modifier mask also marks
        // the tap used (covers Super+click-style chords routed as key presses,
        // and guards the case where the modifier bit is set even if the Super
        // keysym was not the one we tracked).
        if (pressed && (modifiers & mod_logo) != 0) {
            super_used_ = true;
        }

        // --- Chord matching (presses only) ---
        if (pressed) {
            const std::uint32_t rel = modifiers & mod_relevant;
            for (std::size_t i = 0; i < bindings_.size(); ++i) {
                const Binding& b = bindings_[i];
                if (b.combo.is_tap) {
                    continue;
                }
                if (b.combo.keysym == keysym && (b.combo.modifiers & mod_relevant) == rel) {
                    return Outcome{.fired = i, .consume = true};
                }
            }
        }
        return Outcome{};
    }

    // Whether the tap SM is even relevant (no Super tap configured -> the glue
    // could skip, but feed() is cheap; exposed for tests/diagnostics).
    [[nodiscard]] auto tracks_super_tap() const -> bool { return super_tap_; }

private:
    [[nodiscard]] auto find_super_tap() const -> std::size_t {
        if (!super_tap_) {
            return npos;
        }
        for (std::size_t i = 0; i < bindings_.size(); ++i) {
            const Binding& b = bindings_[i];
            if (b.combo.is_tap && (b.combo.modifiers & mod_logo) != 0) {
                return i;
            }
        }
        return npos;
    }

    std::vector<Binding> bindings_;
    bool super_tap_ = false; // any binding taps on Super?
    bool super_down_ = false;
    bool super_used_ = false;
};

// ---- Compiled-in DEFAULTS ---------------------------------------------------
//
// The out-of-the-box bindings, matching the sample unbox.toml the orchestrator
// commits at repo root EXACTLY (brief):
//   Super              -> spawn fuzzel
//   Alt+Tab            -> focus-next
//   Alt+Shift+Tab      -> focus-prev
//   Alt+F1             -> focus-next   (was ext-xdg-shell's Alt+F1 cycle)
//   Ctrl+Alt+Backspace -> quit         (was ext-xdg-shell's terminate)
// Built from parse_combo so the default keysyms resolve through the SAME path as
// configured ones (no hand-coded keysym numbers to drift).
[[nodiscard]] inline auto default_bindings() -> std::vector<Binding> {
    std::vector<Binding> out;
    auto add = [&out](std::string_view keys, Action action, std::string command) {
        if (auto c = parse_combo(keys)) {
            out.push_back(Binding{.combo = *c, .action = action, .command = std::move(command)});
        }
    };
    add("Super", Action::spawn, "fuzzel");
    add("Alt+Tab", Action::focus_next, {});
    add("Alt+Shift+Tab", Action::focus_prev, {});
    add("Alt+F1", Action::focus_next, {});
    add("Ctrl+Alt+BackSpace", Action::quit, {});
    return out;
}

} // namespace unbox::ext_keybindings::policy
