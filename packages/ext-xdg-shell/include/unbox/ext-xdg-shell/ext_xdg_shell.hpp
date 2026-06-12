#pragma once

#include <unbox/kernel/extension.hpp>
#include <unbox/kernel/hooks.hpp>

#include <memory>
#include <string_view>

// ext-xdg-shell — window management as a CORE extension.
//
// Recreates the kernel's former tinywl-shape shell against the kernel ABI
// alone: the wlr_xdg_shell v3 global, toplevel/popup lifecycle, click/tap-to-
// focus, pointer/touch routing to clients, Alt+F1 focus-cycle, Alt+Escape
// terminate, client-requested interactive move/resize, and maximize/
// fullscreen configure replies. Tier: core. depends_on: none.
//
// This header is the unit's CONTRACT — the only surface downstream slices
// (taskbar, tiling) couple to. It is intentionally minimal: future consumers
// change-request exactly what they need. Everything here runs on the single
// wl_event_loop thread.

namespace unbox::ext_xdg_shell {

// ---- Toplevel: the opaque window handle carried by this extension's events --
//
// A borrow of one managed application window (xdg_toplevel). The pointer you
// receive in an event payload is a BORROW valid ONLY until the matching
// on_toplevel_unmapped fires for it (or, equivalently, until the underlying
// xdg_toplevel is destroyed) — never store the raw Toplevel*; if you must
// track a window across events, key your own map on identity captured during
// on_toplevel_mapped and drop it on on_toplevel_unmapped. All methods are
// valid only while the borrow is live, on the event-loop thread.
class Toplevel {
public:
    virtual ~Toplevel() = default;
    Toplevel(const Toplevel&) = delete;
    auto operator=(const Toplevel&) -> Toplevel& = delete;

    // The window's current xdg title, or "" if the client has set none. The
    // returned view is valid only for the duration of the call (it aliases the
    // client's title buffer); copy it if you need to keep it.
    [[nodiscard]] virtual auto title() const -> std::string_view = 0;

    // The window's xdg app_id (its application identity), or "" if unset. Same
    // call-only lifetime as title().
    [[nodiscard]] virtual auto app_id() const -> std::string_view = 0;

    // Give this window keyboard focus and raise it within the normal layer
    // (the click/tap-to-focus action, exposed so a taskbar entry can focus a
    // window). No-op if it is no longer mapped.
    virtual void focus() = 0;

    // Ask the client to close this window (xdg toplevel close request). The
    // client decides when/whether to honor it; the window stays valid until
    // its own unmap/destroy then fires normally.
    virtual void close() = 0;

protected:
    Toplevel() = default;
};

// ---- Exported event payload -------------------------------------------------
//
// Carries a borrow of the Toplevel the event concerns. Borrow lifetime is the
// event-callback duration for *_focused, and "until the matching unmapped"
// for *_mapped (see Toplevel above). Subscribe via your Host::subscribe so
// your id is injected for error isolation.
struct ToplevelEvent {
    Toplevel* toplevel; // borrow; see Toplevel lifetime notes
};

// ---- The extension's exported hooks -----------------------------------------
//
// Fetch these through the service handle below (the typed cross-extension
// coupling: a missing ext-xdg-shell is a link error on Service, never a string
// lookup). The Events are adopt()ed by this extension in activate(), so a
// throwing subscriber on one of them disables the SUBSCRIBER, not us.
class Service {
public:
    virtual ~Service() = default;

    // A toplevel just mapped (became visible) and was focused. The payload
    // borrow stays valid until the matching on_toplevel_unmapped for it.
    [[nodiscard]] virtual auto on_toplevel_mapped()
        -> unbox::kernel::Event<const ToplevelEvent&>& = 0;

    // A toplevel is unmapping (about to become invisible / be destroyed). The
    // payload borrow is valid for THIS call only — drop any tracking of it now.
    [[nodiscard]] virtual auto on_toplevel_unmapped()
        -> unbox::kernel::Event<const ToplevelEvent&>& = 0;

    // Keyboard focus moved to this toplevel (map-focus, click/tap-to-focus, or
    // Alt+F1 cycle). Payload borrow valid for the call.
    [[nodiscard]] virtual auto on_toplevel_focused()
        -> unbox::kernel::Event<const ToplevelEvent&>& = 0;

protected:
    Service() = default;
};

// ---- Factory ----------------------------------------------------------------
//
// Construct the extension; install() it into the Server (ownership transfer).
// Construction is side-effect free — all wiring happens in activate(). The
// manifest is { id: "xdg-shell", tier: core, depends_on: {} }.
[[nodiscard]] auto make_extension() -> std::unique_ptr<unbox::kernel::Extension>;

} // namespace unbox::ext_xdg_shell
