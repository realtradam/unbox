#pragma once

#include <unbox/kernel/hooks.hpp>
#include <unbox/kernel/surface_registry.hpp>
#include <unbox/kernel/wlr.hpp>

#include <cstdint>
#include <typeindex>

namespace unbox::kernel {
class UiSubstrate; // <unbox/kernel/ui.hpp> — the ui-substrate facade Host::ui() returns
}

// The Host API: the typed facade an Extension receives in activate(). It is
// PER-EXTENSION — the kernel hands each extension its own Host so that when a
// hook callback throws, the bus knows which extension to disable. Never pass
// your Host to another extension.
//
// Everything a Host exposes is valid for the SESSION lifetime (until your
// extension is destroyed at shutdown), unless noted. The wlroots borrows
// (display/scene/seat/cursor/output_layout, and the scene-layer trees) are
// non-owning: the kernel owns them; you may attach nodes/listeners but never
// destroy them. wlroots types arrive via wlr.hpp (a public header), so an
// extension's own glue is first-class.
//
// Single wl_event_loop thread throughout.

namespace unbox::kernel {

// ---- Kernel event payloads --------------------------------------------------
//
// The kernel emits these for the generic glue it owns. Payloads carry the
// data an extension needs to implement policy WITHOUT reading kernel src.
// Pointers inside a payload are BORROWS valid only for the duration of the
// emit call — never store them; store the wlr object's own stable handle if
// you must track it, and drop it on its destroy event.

// An output was added (after init_render + enable + scene wiring) or is about
// to be removed (emitted from its destroy handler; the wlr_output is still
// valid for the call, gone after). Distinguish via the two separate events.
struct OutputEvent {
    wlr_output* output; // borrow, valid for the call only
};

// Pointer motion already applied to the cursor; layout coords are the cursor's
// post-move position. Emitted for both relative and absolute motion.
struct PointerMotionEvent {
    double lx;            // cursor layout x AFTER the move
    double ly;            // cursor layout y AFTER the move
    std::uint32_t time_msec;
};

// A pointer button. The kernel does NOT forward it to any client — it only
// moves the cursor and emits this. The single pointer-routing extension is
// responsible for calling wlr_seat_pointer_notify_button (exactly like
// enter/motion/frame): that is what lets an interactive move/resize grab
// suppress the forward by simply not notifying. Carries the cursor layout
// position so a listener can hit-test the scene (click-to-focus, begin
// interactive grab). `pressed` == button down.
struct PointerButtonEvent {
    std::uint32_t button;     // linux/input-event-codes BTN_*
    bool pressed;
    double lx;                // cursor layout x at the event
    double ly;                // cursor layout y at the event
    std::uint32_t time_msec;
};

// A pointer axis (scroll) event. The kernel does NOT forward it to any client
// — the single pointer-routing extension calls wlr_seat_pointer_notify_axis
// (exactly like button/enter/motion/frame). Kernel only emits.
struct PointerAxisEvent {
    wl_pointer_axis orientation;
    double delta;
    std::int32_t delta_discrete;
    wl_pointer_axis_source source;
    std::uint32_t time_msec;
};

// A keyboard key, BEFORE it is forwarded to the focused client. Threaded
// through the key_filter (see Host::key_filter): set `handled = true` to
// CONSUME it (the kernel will not forward it to the client) — this is how
// ext-keybindings/ext-xdg-shell implement compositor shortcuts. `keysym` is
// the resolved xkb keysym of the (modified) press; `modifiers` is the active
// modifier mask (WLR_MODIFIER_*).
struct KeyEvent {
    std::uint32_t keysym;       // xkb_keysym_t
    std::uint32_t keycode;      // raw libinput keycode (for notify_key passthrough)
    std::uint32_t modifiers;    // WLR_MODIFIER_* mask
    bool pressed;               // true on press, false on release
    std::uint32_t time_msec;
    bool handled = false;       // set true to consume (suppress client forward)
};

// A touch point went down / moved / up. `lx`/`ly` are layout coords (the
// cursor path's absolute-to-layout mapping); the extension hit-tests the scene
// itself to find the target surface (the kernel routes nothing to clients).
struct TouchDownEvent {
    std::int32_t touch_id;
    double lx;
    double ly;
    std::uint32_t time_msec;
};
struct TouchMotionEvent {
    std::int32_t touch_id;
    double lx;
    double ly;
    std::uint32_t time_msec;
};
struct TouchUpEvent {
    std::int32_t touch_id;
    std::uint32_t time_msec;
};
struct TouchCancelEvent {
    std::int32_t touch_id;
};

// ---- Scene layers -----------------------------------------------------------
//
// Ordered z-bands of the scene, so extensions never fight over node order.
// Names follow wlr-layer-shell (background/bottom/top/overlay) plus `normal`
// for application toplevels, which layer-shell lacks. Stacking is strictly
// background < bottom < normal < top < overlay. An extension attaches its
// nodes under the tree it gets from Host::scene_layer(); raising/lowering
// WITHIN a layer is the extension's business, crossing layers is not.
//
// GLOSSARY: "scene layer" / SceneLayer is a NEW term (flagged for sign-off in
// reports/kernel.md). It reuses wlr-layer-shell's band names verbatim.
enum class SceneLayer {
    background,
    bottom,
    normal,
    top,
    overlay,
};

// ---- Service registry keying (typed, never string) --------------------------
//
// A service is a single-responder request/response capability: one extension
// registers an implementation of an abstract interface I; others fetch it by
// the TYPE I. The public API is the templated provide_service<I>/service<I>
// below; the type identity is the only key (no strings — a missing provider is
// a nullptr the caller checks, and the INTERFACE TYPE is a compile/link
// dependency on the providing unit's public header). Re-registering a type
// replaces the previous provider.

class Host {
public:
    virtual ~Host() = default;
    Host(const Host&) = delete;
    auto operator=(const Host&) -> Host& = delete;

    // ---- Session borrows (kernel-owned; never destroy) ----
    [[nodiscard]] virtual auto display() -> wl_display* = 0;
    [[nodiscard]] virtual auto scene() -> wlr_scene* = 0;
    [[nodiscard]] virtual auto seat() -> wlr_seat* = 0;
    [[nodiscard]] virtual auto cursor() -> wlr_cursor* = 0;
    // The kernel's shared xcursor theme manager (kernel-owned; never destroy).
    // For an extension that sets the cursor image itself — e.g. ext-xdg-shell
    // drawing the default cursor on passthrough and resize/move cursors during
    // an interactive grab (wlr_cursor_set_xcursor(cursor(), cursor_manager(),
    // name)). Loaded at 24px; the kernel only touches it on seat focus-clear.
    [[nodiscard]] virtual auto cursor_manager() -> wlr_xcursor_manager* = 0;
    [[nodiscard]] virtual auto output_layout() -> wlr_output_layout* = 0;

    // The scene-tree for a z-band. Attach your nodes here. Stable for the
    // session; never destroy it. The trees are created once in stacking order.
    [[nodiscard]] virtual auto scene_layer(SceneLayer layer) -> wlr_scene_tree* = 0;

    // ---- Typed surface -> scene-tree association ----
    // Register that `surface` is hosted in `tree` (which YOUR extension owns).
    // Returns a move-only RAII SurfaceRegistration; keep it as a member of the
    // hosting entity so the association dies with the node. Re-registering the
    // same surface replaces the mapping (the older handle becomes a no-op).
    // This is the typed replacement for the old wlr_surface.data convention —
    // cross-unit surface->tree coupling routes through here, never .data.
    [[nodiscard]] auto host_surface(wlr_surface* surface, wlr_scene_tree* tree)
        -> SurfaceRegistration {
        const auto token = surface_store().set(surface, tree);
        return SurfaceRegistration(&surface_store(), surface, token);
    }
    // Resolve `surface` to the scene tree it is hosted in, or null if no
    // extension has registered it. The returned tree is a BORROW owned by the
    // registering extension, valid only while that registration handle lives —
    // never cache it across events; re-resolve each time.
    [[nodiscard]] auto scene_tree_for(wlr_surface* surface) -> wlr_scene_tree* {
        return static_cast<wlr_scene_tree*>(surface_store().get(surface));
    }

    // The ui substrate (RMLUi behind a typed facade). Contribute ui surfaces +
    // data bindings through this; never touch GL or RMLUi types. Borrow valid
    // for your extension's lifetime; carries your id for error isolation. See
    // <unbox/kernel/ui.hpp>. (Returns a reference even when no GL backend is
    // present — UiSubstrate::available()/create_surface report that.)
    [[nodiscard]] virtual auto ui() -> UiSubstrate& = 0;

    // ---- Kernel event catalogue ----
    // Subscribe through these to react to kernel-owned input/output. Each
    // returns an Event/Filter you subscribe to with YOUR extension id (the
    // Host supplies it; see subscribe helpers below). The kernel emits; you
    // never emit on these.
    //
    // INPUT CONSUMPTION ORDER + IMPLICIT GRAB (contract-docs: this is true,
    // verified by the kernel suite). Before emitting a pointer-button /
    // pointer-axis / touch event on the bus, the kernel offers it to the ui
    // substrate FIRST. Consumption follows STANDARD SEAT IMPLICIT-GRAB rules,
    // not the cursor's current position:
    //   - Pointer buttons: the grab is decided at the FIRST button press (when
    //     no button was down) by whether that press was over a visible ui
    //     surface. If yes, the substrate owns the WHOLE press..last-release
    //     stream and consumes it (no bus emit); if no, the bus owns it and
    //     EVERY event of the stream — including a release that happens to be
    //     over a ui surface — is emitted on the bus. This is what lets an
    //     ext-xdg-shell interactive move/resize grab (press on a titlebar)
    //     receive its release even when the cursor ends over a ui surface.
    //   - Touch: each touch point's owner is decided at its down (over a ui
    //     surface vs not); that point's motion/up/cancel route to the same
    //     owner regardless of where the point travels.
    //   - A ui surface destroyed mid-grab does not strand the stream: a
    //     substrate-owned tail stays consumed (delivered nowhere), never
    //     leaking onto the bus mid-grab.
    // Pointer MOTION is always emitted on the bus (extensions hit-test the
    // scene themselves); the substrate also gets motion for hover/leave (and,
    // during a substrate-owned button grab, the grabbed surface keeps the
    // moves). Because a ui-surface node is not a client surface, a routing
    // extension that hit-tests motion finds "no client here" over a ui surface
    // and clears stale client hover. Keyboard keys are NOT consumed by the
    // substrate this slice (keyboard-into-ui is deferred).
    [[nodiscard]] virtual auto on_output_added() -> Event<const OutputEvent&>& = 0;
    [[nodiscard]] virtual auto on_output_removed() -> Event<const OutputEvent&>& = 0;
    [[nodiscard]] virtual auto on_pointer_motion() -> Event<const PointerMotionEvent&>& = 0;
    [[nodiscard]] virtual auto on_pointer_button() -> Event<const PointerButtonEvent&>& = 0;
    [[nodiscard]] virtual auto on_pointer_axis() -> Event<const PointerAxisEvent&>& = 0;
    // Pointer frame: emitted once per input frame after motion/button/axis;
    // the extension routing pointer events to clients calls
    // wlr_seat_pointer_notify_frame here. No payload.
    [[nodiscard]] virtual auto on_pointer_frame() -> Event<>& = 0;
    [[nodiscard]] virtual auto on_touch_down() -> Event<const TouchDownEvent&>& = 0;
    [[nodiscard]] virtual auto on_touch_motion() -> Event<const TouchMotionEvent&>& = 0;
    [[nodiscard]] virtual auto on_touch_up() -> Event<const TouchUpEvent&>& = 0;
    [[nodiscard]] virtual auto on_touch_cancel() -> Event<const TouchCancelEvent&>& = 0;
    // Touch frame: emitted once per touch input frame; route via
    // wlr_seat_touch_notify_frame. No payload.
    [[nodiscard]] virtual auto on_touch_frame() -> Event<>& = 0;

    // Key handling is a FILTER, not an Event: links run in order and may set
    // KeyEvent::handled to CONSUME the key before the kernel forwards it to the
    // focused client. The kernel applies this filter on every key, then
    // forwards to the client only if the result is not handled. (This is the
    // consume-or-pass channel the brief calls for; ext-keybindings lives here.)
    [[nodiscard]] virtual auto key_filter() -> Filter<KeyEvent>& = 0;

    // ---- Subscription helpers (tag with THIS extension's id) ----
    // Prefer these over event.subscribe(id, cb): the Host injects your id so a
    // throwing callback disables YOU and not someone else.
    template <typename... Args, typename Fn>
    [[nodiscard]] auto subscribe(Event<Args...>& ev, Fn&& fn) -> Subscription {
        return ev.subscribe(extension_id(), std::forward<Fn>(fn));
    }
    template <typename T, typename Fn>
    [[nodiscard]] auto subscribe(Filter<T>& flt, Fn&& fn) -> Subscription {
        return flt.subscribe(extension_id(), std::forward<Fn>(fn));
    }

    // ---- Exporting your own hooks (cross-extension coupling) ----
    // To expose a hook to OTHER extensions, declare an Event/Filter as a member
    // of your extension and adopt() it in activate(): adoption binds it to the
    // kernel's error-isolation registry (so a throwing subscriber on your hook
    // disables the SUBSCRIBER's extension, not yours) and tracks it for purge.
    // The hook is pinned (non-movable); keep it as a stable member and expose a
    // reference via your public header or a service. Subscribers pass their own
    // id (their Host::subscribe injects it). Adopt before anyone subscribes.
    void adopt(detail::HookBase& hook) { adopt_hook(hook); }

    // ---- Services (typed single-responder) ----
    // Register `impl` as the provider of interface I for the session. `impl`
    // is a NON-OWNING borrow: it must outlive every consumer — store it as a
    // member of your extension (so it dies last, in reverse-activation order).
    // Returns false if a provider for I was already registered (it is still
    // replaced; the bool lets a provider detect a collision). No strings.
    template <typename I>
    auto provide_service(I* impl) -> bool {
        return register_service(std::type_index(typeid(I)), static_cast<void*>(impl));
    }
    // Fetch the provider of interface I, or nullptr if none is registered yet.
    // Do not cache across activation: fetch in activate() AFTER the provider's
    // extension (declare it in your depends_on so it activates first).
    template <typename I>
    [[nodiscard]] auto service() -> I* {
        return static_cast<I*>(lookup_service(std::type_index(typeid(I))));
    }

protected:
    Host() = default;

    // The id the kernel assigned to THIS extension; subscriptions are tagged
    // with it for error isolation. Kernel-internal Hosts return
    // kernel_extension_id.
    [[nodiscard]] virtual auto extension_id() const -> ExtensionId = 0;

    // Non-template service core (the templates above are thin type-key shims).
    virtual auto register_service(std::type_index type, void* impl) -> bool = 0;
    [[nodiscard]] virtual auto lookup_service(std::type_index type) -> void* = 0;

    // Bind an extension-exported hook to the isolation registry (see adopt()).
    virtual void adopt_hook(detail::HookBase& hook) = 0;

    // The kernel-owned, session-wide surface->tree association store (shared by
    // ALL extensions; the host_surface/scene_tree_for shims above route here).
    [[nodiscard]] virtual auto surface_store() -> detail::PointerAssoc& = 0;
};

} // namespace unbox::kernel
