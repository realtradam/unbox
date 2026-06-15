#pragma once

#include <unbox/kernel/extension.hpp>
#include <unbox/kernel/hooks.hpp>
#include <unbox/kernel/wlr.hpp> // wlr_box, wlr_scene_tree (the mechanism types below)

#include <memory>
#include <string_view>

// ext-xdg-shell — window management as a CORE extension.
//
// Recreates the kernel's former tinywl-shape shell against the kernel ABI
// alone: the wlr_xdg_shell v3 global, toplevel/popup lifecycle, click/tap-to-
// focus, pointer/touch routing to clients, client-requested interactive
// move/resize, and maximize/fullscreen configure replies. Tier: core.
// depends_on: none.
//
// Compositor keybindings (focus-cycle, terminate) are NOT handled here; they
// live in ext-keybindings, which subscribes to the kernel's key_filter and
// calls Toplevel::focus() on this extension's Service.
//
// This header is the unit's CONTRACT — the only surface downstream slices
// (taskbar, tiling, ext-keybindings) couple to. It is intentionally minimal:
// future consumers change-request exactly what they need. Everything here runs
// on the single wl_event_loop thread.

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

    // ---- Minimize mechanism (slice 10 / stage dock) ----------------------
    //
    // Neutral compositor-side mechanism the stage dock drives to minimize a
    // window. The "minimized" STATE and dock placement are ext-stage-dock
    // policy — none of it is tracked here; this surface only hides/shows the
    // scene node and reports geometry.

    // The window's current on-screen box in LAYOUT coordinates: its scene-node
    // position plus the size of its current xdg window geometry. Valid only for
    // the call. The dock uses it to size the preview snapshot and to restore
    // the window to where it was. Returns sane values only for a MAPPED
    // toplevel; for an unmapped one the box reflects the last committed
    // geometry at the node's last position and should not be relied upon (call
    // it while mapped, e.g. from on_toplevel_mapped onward and before unmap).
    [[nodiscard]] virtual auto geometry() const -> wlr_box = 0;

    // The scene tree hosting this toplevel's surfaces — the SAME tree this
    // extension created and registered via Host::host_surface (so it equals
    // Host::scene_tree_for(this toplevel's wl_surface)). A BORROW owned by THIS
    // extension; valid only while the Toplevel borrow is live (drop it on
    // unmap). Never destroy it. The dock feeds it to
    // UiSubstrate::create_preview() to snapshot the window, and it is the node
    // hide()/show() enable/disable.
    [[nodiscard]] virtual auto scene_tree() -> wlr_scene_tree* = 0;

    // The toplevel's ROOT wl_surface — the surface whose tree scene_tree()
    // hosts (i.e. xdg_toplevel->base->surface). This is the handle a
    // compositor / window-field passes to UiSubstrate::create_surface_element()
    // to composite the window as a live RCSS surface element (RML compositing,
    // Phase 2): the kernel then manages the toplevel's whole subsurface/popup
    // tree itself, so this extension does NOT expose popup/subsurface handles.
    // A non-owning BORROW with the SAME lifetime as the rest of Toplevel: valid
    // only while the Toplevel borrow is live (i.e. until the matching
    // on_toplevel_unmapped fires for it). Never store it; never destroy it.
    // Returns nullptr only in the degenerate case of an already-destroyed
    // underlying xdg_toplevel (never for a live, mapped toplevel).
    [[nodiscard]] virtual auto wl_surface() -> wlr_surface* = 0;

    // Compositor-side HIDE / SHOW: disable / enable the toplevel's scene node
    // so it is not composited and the client stops receiving frame callbacks
    // (wlr_scene withholds them from a non-visible node), WITHOUT unmapping it
    // — the client stays mapped and NO on_toplevel_unmapped fires. Idempotent
    // (double hide/show is fine). Does NOT change keyboard focus or raise: if
    // the focused window is hidden, the caller drives focus to whatever should
    // be focused next (via focus() on that toplevel). "minimized" is the
    // caller's concept, not tracked here. A hidden toplevel still unmaps /
    // destroys normally (on_toplevel_unmapped fires) when its client closes it.
    virtual void hide() = 0;
    virtual void show() = 0;

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
    // programmatic Toplevel::focus() call). Payload borrow valid for the call.
    // NOTE: ext-keybindings' Alt+Tab cycle calls Toplevel::focus(), which
    // produces this event — callers may rely on that guarantee.
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
[[nodiscard]] auto create() -> std::unique_ptr<unbox::kernel::Extension>;

} // namespace unbox::ext_xdg_shell
