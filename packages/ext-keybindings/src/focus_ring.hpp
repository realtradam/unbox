#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

// Pure decision core: the focus ring (stable mapping-order rotation). Operates
// over OPAQUE tokens — the glue passes ext_xdg_shell::Toplevel* as a void-ish
// token and NEVER dereferences it here; this core only compares identity and
// computes the next/previous token. No wlroots, no kernel; doctest-covered in
// tests/test_policy.cpp.
//
// "Rotate across all" means STABLE mapping-order rotation, not MRU: windows keep
// their insertion (map) order, so repeated focus-next walks all N windows in a
// fixed cycle instead of ping-ponging the two most-recently-used. `current_` is
// just a cursor into that stable order; noting an external focus only MOVES the
// cursor, it never reorders the ring.

namespace unbox::ext_keybindings::policy {

template <typename Token>
class FocusRing {
public:
    // A window mapped: append in map order (stable). Ignored if already present
    // (defensive — the glue keys add on on_toplevel_mapped, which is once).
    void add(Token t) {
        if (index_of(t) == npos) {
            order_.push_back(t);
        }
    }

    // A window unmapped: remove it. If it was the current cursor target, the
    // cursor is cleared (next focus-next starts from the front, focus-prev from
    // the back) — the glue must never deref the removed token again.
    void remove(Token t) {
        const std::size_t i = index_of(t);
        if (i == npos) {
            return;
        }
        order_.erase(order_.begin() + static_cast<std::ptrdiff_t>(i));
        if (has_current_ && current_ == t) {
            has_current_ = false;
        }
    }

    // Note that focus actually moved to `t` (map-focus, click/tap-to-focus, or
    // our own rotation echoing back). Moves the cursor WITHOUT reordering the
    // ring. A token not in the ring is ignored (it has no slot to rotate from).
    void note_focused(Token t) {
        if (index_of(t) != npos) {
            current_ = t;
            has_current_ = true;
        }
    }

    [[nodiscard]] auto size() const -> std::size_t { return order_.size(); }
    [[nodiscard]] auto empty() const -> bool { return order_.empty(); }

    // The token focus-next should move to, or no value if there is nothing to
    // do (0 windows). With 1 window, returns that window (re-focus / no-op at
    // the glue). With an unknown/cleared cursor, starts at the FRONT.
    [[nodiscard]] auto next() const -> const Token* {
        if (order_.empty()) {
            return nullptr;
        }
        const std::size_t cur = current_index();
        if (cur == npos) {
            return &order_.front();
        }
        const std::size_t nxt = (cur + 1) % order_.size();
        return &order_[nxt];
    }

    // The token focus-prev should move to, or no value (0 windows). Unknown
    // cursor starts at the BACK.
    [[nodiscard]] auto prev() const -> const Token* {
        if (order_.empty()) {
            return nullptr;
        }
        const std::size_t cur = current_index();
        if (cur == npos) {
            return &order_.back();
        }
        const std::size_t prv = (cur + order_.size() - 1) % order_.size();
        return &order_[prv];
    }

    // The glue calls this AFTER it has driven focus to `t` (the brief: set
    // current yourself, don't depend solely on the focused event echoing back).
    void set_current(Token t) { note_focused(t); }

    // For tests/diagnostics: the current cursor token, or nullptr if none.
    [[nodiscard]] auto current() const -> const Token* {
        return has_current_ ? &current_ : nullptr;
    }

private:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    [[nodiscard]] auto index_of(Token t) const -> std::size_t {
        for (std::size_t i = 0; i < order_.size(); ++i) {
            if (order_[i] == t) {
                return i;
            }
        }
        return npos;
    }

    [[nodiscard]] auto current_index() const -> std::size_t {
        return has_current_ ? index_of(current_) : npos;
    }

    std::vector<Token> order_; // stable map order
    Token current_{};
    bool has_current_ = false;
};

} // namespace unbox::ext_keybindings::policy
