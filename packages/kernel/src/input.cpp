#include "server_impl.hpp"

#include <xkbcommon/xkbcommon.h>

namespace unbox::kernel {

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

    // Default XKB keymap (layout "us" etc.); unbox.toml takes over later.
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_kb, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_kb, 25, 600);

    keyboard->modifiers.connect(wlr_kb->events.modifiers, [this, keyboard](void*) {
        // The seat exposes one logical keyboard; swap the active device in.
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

        bool handled = false;
        const std::uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->keyboard);
        if ((modifiers & WLR_MODIFIER_ALT) != 0 &&
            event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            for (int i = 0; i < nsyms; i++) {
                handled = handle_keybinding(syms[i]);
            }
        }
        if (!handled) {
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
    // All pointer handling is proxied through wlr_cursor; per-device
    // libinput config (acceleration, tap…) is a later slice.
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

    // wlr_cursor aggregates touch devices too and emits layout-mapped
    // touch_* events (handled below).
    wlr_cursor_attach_input_device(cursor, device);
}

// ---- Compositor keybindings ------------------------------------------------------

auto Server::Impl::handle_keybinding(std::uint32_t keysym) -> bool {
    // Slice-2 placeholder bindings (Alt held), replaced by the keybinding
    // filter chain in slice 5.
    switch (keysym) {
    case XKB_KEY_Escape:
        wl_display_terminate(display);
        return true;
    case XKB_KEY_F1:
        if (mapped_toplevels.size() >= 2) {
            focus_toplevel(mapped_toplevels.back());
        }
        return true;
    default:
        return false;
    }
}

// ---- Pointer (via wlr_cursor) ------------------------------------------------------

void Server::Impl::process_cursor_move() {
    wlr_scene_node_set_position(&grabbed_toplevel->scene_tree->node,
                                static_cast<int>(cursor->x - grab_x),
                                static_cast<int>(cursor->y - grab_y));
}

void Server::Impl::process_cursor_resize() {
    // Resizing moves the node when dragging top/left edges; the client is
    // asked for the new size (it commits a matching buffer later).
    Toplevel* toplevel = grabbed_toplevel;
    const double border_x = cursor->x - grab_x;
    const double border_y = cursor->y - grab_y;
    int new_left = grab_geobox.x;
    int new_right = grab_geobox.x + grab_geobox.width;
    int new_top = grab_geobox.y;
    int new_bottom = grab_geobox.y + grab_geobox.height;

    if ((resize_edges & WLR_EDGE_TOP) != 0) {
        new_top = static_cast<int>(border_y);
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    } else if ((resize_edges & WLR_EDGE_BOTTOM) != 0) {
        new_bottom = static_cast<int>(border_y);
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if ((resize_edges & WLR_EDGE_LEFT) != 0) {
        new_left = static_cast<int>(border_x);
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    } else if ((resize_edges & WLR_EDGE_RIGHT) != 0) {
        new_right = static_cast<int>(border_x);
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    wlr_box* geo_box = &toplevel->xdg_toplevel->base->geometry;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left - geo_box->x,
                                new_top - geo_box->y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_right - new_left,
                              new_bottom - new_top);
}

void Server::Impl::process_cursor_motion(std::uint32_t time_msec) {
    if (cursor_mode == CursorMode::Move) {
        process_cursor_move();
        return;
    }
    if (cursor_mode == CursorMode::Resize) {
        process_cursor_resize();
        return;
    }

    // Slice-3 spike input proof (NOT the slice-5 routing contract): if the
    // cursor is over the spike node, forward surface-local coords to RmlUi so
    // the document's button reacts to hover. Crude and private.
    if (ui_spike != nullptr) {
        if (wlr_scene_node* spike = ui_spike->node()) {
            int nx = 0;
            int ny = 0;
            wlr_scene_node_coords(spike, &nx, &ny);
            const double sx = cursor->x - nx;
            const double sy = cursor->y - ny;
            ui_spike->on_pointer_motion(sx, sy);
        }
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    Toplevel* toplevel = toplevel_at(cursor->x, cursor->y, &surface, &sx, &sy);
    if (toplevel == nullptr) {
        // Over no toplevel: the compositor draws its own default cursor.
        wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
    }
    if (surface != nullptr) {
        // Enter gives the surface pointer focus; wlroots dedupes repeats.
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time_msec, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

void Server::Impl::attach_cursor_handlers() {
    cursor_motion.connect(cursor->events.motion, [this](void* data) {
        const auto* event = static_cast<wlr_pointer_motion_event*>(data);
        wlr_cursor_move(cursor, &event->pointer->base, event->delta_x, event->delta_y);
        process_cursor_motion(event->time_msec);
    });
    cursor_motion_absolute.connect(cursor->events.motion_absolute, [this](void* data) {
        const auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
        wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);
        process_cursor_motion(event->time_msec);
    });
    cursor_button.connect(cursor->events.button, [this](void* data) {
        const auto* event = static_cast<wlr_pointer_button_event*>(data);
        wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);

        // Slice-3 spike input proof: forward clicks over the spike node to
        // RmlUi so its button reacts to press/release. Crude and private.
        if (ui_spike != nullptr) {
            if (wlr_scene_node* spike = ui_spike->node()) {
                int nx = 0;
                int ny = 0;
                wlr_scene_node_coords(spike, &nx, &ny);
                if (wlr_scene_node_at(spike, cursor->x, cursor->y, nullptr, nullptr) != nullptr) {
                    ui_spike->on_pointer_button(event->state ==
                                                WL_POINTER_BUTTON_STATE_PRESSED);
                }
            }
        }

        if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
            reset_cursor_mode();
        } else {
            // Click-to-focus.
            double sx = 0;
            double sy = 0;
            wlr_surface* surface = nullptr;
            focus_toplevel(toplevel_at(cursor->x, cursor->y, &surface, &sx, &sy));
        }
    });
    cursor_axis.connect(cursor->events.axis, [this](void* data) {
        const auto* event = static_cast<wlr_pointer_axis_event*>(data);
        wlr_seat_pointer_notify_axis(seat, event->time_msec, event->orientation, event->delta,
                                     event->delta_discrete, event->source,
                                     event->relative_direction);
    });
    cursor_frame.connect(cursor->events.frame, [this](void*) {
        wlr_seat_pointer_notify_frame(seat);
    });

    // ---- Touch (tinywl doesn't have this; the CF-AX3 does) ----
    cursor_touch_down.connect(cursor->events.touch_down, [this](void* data) {
        const auto* event = static_cast<wlr_touch_down_event*>(data);
        double lx = 0;
        double ly = 0;
        wlr_cursor_absolute_to_layout_coords(cursor, &event->touch->base, event->x, event->y,
                                             &lx, &ly);
        double sx = 0;
        double sy = 0;
        wlr_surface* surface = nullptr;
        Toplevel* toplevel = toplevel_at(lx, ly, &surface, &sx, &sy);
        if (toplevel != nullptr) {
            focus_toplevel(toplevel); // tap raises + focuses
        }
        if (surface != nullptr) {
            touch_points.insert_or_assign(event->touch_id,
                                          TouchPoint{surface, lx - sx, ly - sy});
            wlr_seat_touch_notify_down(seat, surface, event->time_msec, event->touch_id, sx, sy);
        }
    });
    cursor_touch_motion.connect(cursor->events.touch_motion, [this](void* data) {
        const auto* event = static_cast<wlr_touch_motion_event*>(data);
        auto it = touch_points.find(event->touch_id);
        if (it == touch_points.end()) {
            return; // down landed on no surface; nothing is grabbed
        }
        double lx = 0;
        double ly = 0;
        wlr_cursor_absolute_to_layout_coords(cursor, &event->touch->base, event->x, event->y,
                                             &lx, &ly);
        wlr_seat_touch_notify_motion(seat, event->time_msec, event->touch_id,
                                     lx - it->second.origin_x, ly - it->second.origin_y);
    });
    cursor_touch_up.connect(cursor->events.touch_up, [this](void* data) {
        const auto* event = static_cast<wlr_touch_up_event*>(data);
        touch_points.erase(event->touch_id);
        wlr_seat_touch_notify_up(seat, event->time_msec, event->touch_id);
    });
    cursor_touch_cancel.connect(cursor->events.touch_cancel, [this](void* data) {
        const auto* event = static_cast<wlr_touch_cancel_event*>(data);
        if (wlr_touch_point* point = wlr_seat_touch_get_point(seat, event->touch_id)) {
            wlr_seat_touch_notify_cancel(seat, point->client);
        }
        touch_points.erase(event->touch_id);
    });
    cursor_touch_frame.connect(cursor->events.touch_frame, [this](void*) {
        wlr_seat_touch_notify_frame(seat);
    });
}

// ---- Seat requests -------------------------------------------------------------

void Server::Impl::attach_seat_handlers() {
    seat_request_cursor.connect(seat->events.request_set_cursor, [this](void* data) {
        const auto* event = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
        // Any client may send this; honor only the pointer-focused one.
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
