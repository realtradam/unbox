#pragma once

#include <optional>

#include <xkbcommon/xkbcommon.h>

// Pure decision core for the kernel's VT-switch escape hatch — NO wlroots / GL,
// so it is doctest-able with nothing running (AGENTS.md: effects at the edges,
// pure cores tested hard). The keyboard glue (input.cpp) injects the effect
// (wlr_session_change_vt) around this.
//
// xkbcommon is allowed here: it is a pure value library (a keysym is an
// integer), not a wlroots/GL/wayland effect surface — the wlroots-include rule
// targets <wlr/...> + <wayland-server*.h>, and input.cpp already includes
// <xkbcommon/xkbcommon.h> for keysym resolution.

namespace unbox::kernel {

// Map a resolved keysym to the Linux VT it requests, or nullopt if it is not a
// VT-switch keysym. Ctrl+Alt+Fn is delivered by xkb as the contiguous range
// XKB_KEY_XF86Switch_VT_1 (0x1008FE01) .. XKB_KEY_XF86Switch_VT_12
// (0x1008FE0C); the returned value is the 1-based VT number (1..12). These
// keysyms are ONLY produced with Ctrl+Alt held, so matching the keysym is
// sufficient — no separate modifier-mask check. Plain F1..F12 resolve to the
// ordinary XKB_KEY_F1.. keysyms and are NOT in this range, so they pass through.
[[nodiscard]] constexpr auto vt_for_keysym(xkb_keysym_t keysym) -> std::optional<unsigned> {
    if (keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
        return static_cast<unsigned>(keysym - XKB_KEY_XF86Switch_VT_1) + 1U;
    }
    return std::nullopt;
}

} // namespace unbox::kernel
