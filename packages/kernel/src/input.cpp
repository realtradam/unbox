#include "server_impl.hpp"

#include "vt_core.hpp"

#include <xkbcommon/xkbcommon.h>

namespace unbox::kernel {

// Slice 4: the kernel owns generic input PLUMBING only. It moves the cursor,
// tracks seat capabilities, handles the seat's own protocol requests, runs the
// key_filter (consume-or-pass), and EMITS typed events. It routes NOTHING to
// client surfaces and makes NO focus decision — ext-xdg-shell (and others) do
// that from the bus + borrows. The only client forward the kernel still does
// is keyboard key passthrough AFTER the filter, because the seat already holds
// the focus an extension set via wlr_seat_keyboard_notify_enter.

// ---- Device hotplug -----------------------------------------------------------

void Server::Impl::handle_new_input(wlr_input_device* device) {
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        new_keyboard(device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        new_pointer(device);
        break;
    case WLR_INPUT_DEVICE_TOUCH:
        new_touch(device);
        break;
    default:
        break;
    }
    update_seat_capabilities();
}

void Server::Impl::update_seat_capabilities() {
    // Always advertise a pointer: we always draw a cursor.
    std::uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!keyboards.empty()) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    if (!touch_devices.empty()) {
        caps |= WL_SEAT_CAPABILITY_TOUCH;
    }
    wlr_seat_set_capabilities(seat, caps);
}

void Server::Impl::new_keyboard(wlr_input_device* device) {
    wlr_keyboard* wlr_kb = wlr_keyboard_from_input_device(device);

    auto owned = std::make_unique<Keyboard>();
    Keyboard* keyboard = owned.get();
    keyboard->server = this;
    keyboard->keyboard = wlr_kb;
    keyboards.push_back(std::move(owned));

    // Default XKB keymap; per-config keymap is a later slice.
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_kb, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_kb, 25, 600);

    keyboard->modifiers.connect(wlr_kb->events.modifiers, [this, keyboard](void*) {
        wlr_seat_set_keyboard(seat, keyboard->keyboard);
        wlr_seat_keyboard_notify_modifiers(seat, &keyboard->keyboard->modifiers);
    });
    keyboard->key.connect(wlr_kb->events.key, [this, keyboard](void* data) {
        const auto* event = static_cast<wlr_keyboard_key_event*>(data);

        // libinput keycode -> xkbcommon
        const std::uint32_t keycode = event->keycode + 8;
        const xkb_keysym_t* syms = nullptr;
        const int nsyms =
            xkb_state_key_get_syms(keyboard->keyboard->xkb_state, keycode, &syms);
        const std::uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->keyboard);
        const bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;

        // SESSION ESCAPE HATCH: Ctrl+Alt+Fn (XF86Switch_VT_1..12) switches the
        // Linux VT. Handled HERE, kernel-hardwired, BEFORE the key_filter, so no
        // extension can intercept, consume, or block the only escape from the
        // real DRM seat (user decision: not config-driven, not rebindable). On
        // PRESS we change the VT; we CONSUME both press and release (the matching
        // release carries the same keysym) so the filter never runs and the key
        // never reaches the focused client. No session (headless/nested =>
        // session is NULL) is a clean no-op — never crash.
        for (int i = 0; i < nsyms; ++i) {
            if (const std::optional<unsigned> vt = vt_for_keysym(syms[i])) {
                if (pressed && session != nullptr) {
                    wlr_session_change_vt(session, *vt);
                }
                return; // consume: no filter, no client forward (press or release)
            }
        }

        // Thread each resolved keysym through the key_filter; a filter link may
        // CONSUME the key (set handled=true) — that is how extensions implement
        // compositor shortcuts. If any resolution is consumed, suppress the
        // client forward for this key.
        bool consumed = false;
        for (int i = 0; i < nsyms; ++i) {
            KeyEvent ke{};
            ke.keysym = syms[i];
            ke.keycode = event->keycode;
            ke.modifiers = modifiers;
            ke.pressed = pressed;
            ke.time_msec = event->time_msec;
            ke.handled = false;
            ke = key_filter.apply(ke);
            consumed = consumed || ke.handled;
        }
        // With no resolved syms (e.g. modifier-only) the filter still runs once
        // so extensions can observe raw modifier keys if they wish.
        if (nsyms == 0) {
            KeyEvent ke{};
            ke.keysym = XKB_KEY_NoSymbol;
            ke.keycode = event->keycode;
            ke.modifiers = modifiers;
            ke.pressed = pressed;
            ke.time_msec = event->time_msec;
            ke = key_filter.apply(ke);
            consumed = consumed || ke.handled;
        }

        if (!consumed) {
            wlr_seat_set_keyboard(seat, keyboard->keyboard);
            wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
        }
    });
    keyboard->destroy.connect(device->events.destroy, [this, keyboard](void*) {
        update_seat_capabilities();
        // Last action: destroys `keyboard` (and these listeners with it).
        keyboards.remove_if([keyboard](const auto& owned) { return owned.get() == keyboard; });
    });

    wlr_seat_set_keyboard(seat, wlr_kb);
}

void Server::Impl::new_pointer(wlr_input_device* device) {
    // All pointer handling is proxied through wlr_cursor; per-device libinput
    // config is a later slice.
    wlr_cursor_attach_input_device(cursor, device);
}

void Server::Impl::new_touch(wlr_input_device* device) {
    auto owned = std::make_unique<TouchDevice>();
    TouchDevice* touch = owned.get();
    touch->server = this;
    touch->device = device;
    touch_devices.push_back(std::move(owned));

    touch->destroy.connect(device->events.destroy, [this, touch](void*) {
        update_seat_capabilities();
        // Last action: destroys `touch` (and this listener with it).
        touch_devices.remove_if([touch](const auto& owned) { return owned.get() == touch; });
    });

    wlr_cursor_attach_input_device(cursor, device);
}

// ---- Pointer (via wlr_cursor): move cursor + emit, route nothing ------------

void Server::Impl::emit_pointer_motion(std::uint32_t time_msec) {
    // Motion is ALWAYS observed by both the substrate (hover/leave on ui
    // surfaces) and the bus (extensions hit-test the scene themselves). A ui-
    // surface node is not a client surface, so a routing extension naturally
    // finds "no client here" over a ui surface and clears stale client hover.
    if (substrate != nullptr) {
        substrate->route_pointer_motion(cursor->x, cursor->y, time_msec);
    }
    const PointerMotionEvent ev{cursor->x, cursor->y, time_msec};
    ev_pointer_motion.emit(ev);
}

void Server::Impl::attach_cursor_handlers() {
    cursor_motion.connect(cursor->events.motion, [this](void* data) {
        const auto* event = static_cast<wlr_pointer_motion_event*>(data);
        wlr_cursor_move(cursor, &event->pointer->base, event->delta_x, event->delta_y);
        emit_pointer_motion(event->time_msec);
    });
    cursor_motion_absolute.connect(cursor->events.motion_absolute, [this](void* data) {
        const auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
        wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);
        emit_pointer_motion(event->time_msec);
    });
    cursor_button.connect(cursor->events.button, [this](void* data) {
        const auto* event = static_cast<wlr_pointer_button_event*>(data);
        const bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;

        // Consumption order: the substrate gets first refusal. If the click is
        // over a visible ui surface it consumes it (drives the document) and we
        // do NOT emit on the bus — no click-through to clients beneath.
        if (substrate != nullptr &&
            substrate->route_pointer_button(cursor->x, cursor->y, pressed, event->time_msec)) {
            return;
        }
        const PointerButtonEvent ev{event->button, pressed, cursor->x, cursor->y,
                                    event->time_msec};
        ev_pointer_button.emit(ev);
    });
    cursor_axis.connect(cursor->events.axis, [this](void* data) {
        const auto* event = static_cast<wlr_pointer_axis_event*>(data);
        if (substrate != nullptr &&
            substrate->route_pointer_axis(cursor->x, cursor->y, event->delta, event->time_msec)) {
            return; // consumed by a ui surface
        }
        const PointerAxisEvent ev{event->orientation, event->delta, event->delta_discrete,
                                  event->source, event->time_msec};
        ev_pointer_axis.emit(ev);
    });
    cursor_frame.connect(cursor->events.frame, [this](void*) { ev_pointer_frame.emit(); });

    // ---- Touch (tinywl lacks this; the CF-AX3 has it). Convert to layout
    // coords + emit; extensions route to surfaces. ----
    cursor_touch_down.connect(cursor->events.touch_down, [this](void* data) {
        const auto* event = static_cast<wlr_touch_down_event*>(data);
        double lx = 0;
        double ly = 0;
        wlr_cursor_absolute_to_layout_coords(cursor, &event->touch->base, event->x, event->y,
                                             &lx, &ly);
        // Substrate first refusal (consume-or-pass). A down over a ui surface
        // is captured by the substrate (tap = click) and not emitted on the bus.
        if (substrate != nullptr &&
            substrate->route_touch_down(event->touch_id, lx, ly, event->time_msec)) {
            return;
        }
        const TouchDownEvent ev{event->touch_id, lx, ly, event->time_msec};
        ev_touch_down.emit(ev);
    });
    cursor_touch_motion.connect(cursor->events.touch_motion, [this](void* data) {
        const auto* event = static_cast<wlr_touch_motion_event*>(data);
        double lx = 0;
        double ly = 0;
        wlr_cursor_absolute_to_layout_coords(cursor, &event->touch->base, event->x, event->y,
                                             &lx, &ly);
        // If this touch id was captured by a ui surface at down, the substrate
        // keeps it (and consumes the motion); otherwise it passes to the bus.
        if (substrate != nullptr &&
            substrate->route_touch_motion(event->touch_id, lx, ly, event->time_msec)) {
            return;
        }
        const TouchMotionEvent ev{event->touch_id, lx, ly, event->time_msec};
        ev_touch_motion.emit(ev);
    });
    cursor_touch_up.connect(cursor->events.touch_up, [this](void* data) {
        const auto* event = static_cast<wlr_touch_up_event*>(data);
        if (substrate != nullptr && substrate->route_touch_up(event->touch_id, event->time_msec)) {
            return; // a captured (ui-surface) touch ended
        }
        const TouchUpEvent ev{event->touch_id, event->time_msec};
        ev_touch_up.emit(ev);
    });
    cursor_touch_cancel.connect(cursor->events.touch_cancel, [this](void* data) {
        const auto* event = static_cast<wlr_touch_cancel_event*>(data);
        // A cancel of a substrate-captured touch releases the RML button and is
        // consumed; otherwise it passes to the bus.
        if (substrate != nullptr && substrate->route_touch_up(event->touch_id, event->time_msec)) {
            return;
        }
        const TouchCancelEvent ev{event->touch_id};
        ev_touch_cancel.emit(ev);
    });
    cursor_touch_frame.connect(cursor->events.touch_frame, [this](void*) {
        ev_touch_frame.emit();
    });
}

// ---- Seat requests (generic protocol glue) ----------------------------------

void Server::Impl::attach_seat_handlers() {
    seat_request_cursor.connect(seat->events.request_set_cursor, [this](void* data) {
        const auto* event = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
        if (seat->pointer_state.focused_client == event->seat_client) {
            wlr_cursor_set_surface(cursor, event->surface, event->hotspot_x, event->hotspot_y);
        }
    });
    seat_pointer_focus_change.connect(seat->pointer_state.events.focus_change, [this](void* data) {
        const auto* event = static_cast<wlr_seat_pointer_focus_change_event*>(data);
        if (event->new_surface == nullptr) {
            wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
        }
    });
    seat_request_set_selection.connect(seat->events.request_set_selection, [this](void* data) {
        const auto* event = static_cast<wlr_seat_request_set_selection_event*>(data);
        wlr_seat_set_selection(seat, event->source, event->serial);
    });
}

} // namespace unbox::kernel
