#pragma once

#include <unbox/kernel/server.hpp>
#include <unbox/kernel/wlr.hpp>

#include "listener.hpp"

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>

// Private kernel state. Entity structs mirror tinywl's, with Listener
// members replacing manual wl_list_remove bookkeeping (RAII unsubscribes).
// Definitions are split: server.cpp (lifecycle + outputs), toplevel.cpp
// (xdg-shell + focus + grabs), input.cpp (devices, cursor, touch, seat).

namespace unbox::kernel {

struct Toplevel;

enum class CursorMode { Passthrough, Move, Resize };

struct Output {
    Server::Impl* server = nullptr;
    wlr_output* output = nullptr;
    Listener frame;
    Listener request_state;
    Listener destroy;
};

struct Toplevel {
    Server::Impl* server = nullptr;
    wlr_xdg_toplevel* xdg_toplevel = nullptr;
    wlr_scene_tree* scene_tree = nullptr;
    Listener map;
    Listener unmap;
    Listener commit;
    Listener destroy;
    Listener request_move;
    Listener request_resize;
    Listener request_maximize;
    Listener request_fullscreen;
};

struct Popup {
    wlr_xdg_popup* xdg_popup = nullptr;
    Listener commit;
    Listener destroy;
};

struct Keyboard {
    Server::Impl* server = nullptr;
    wlr_keyboard* keyboard = nullptr;
    Listener modifiers;
    Listener key;
    Listener destroy;
};

struct TouchDevice {
    Server::Impl* server = nullptr;
    wlr_input_device* device = nullptr;
    Listener destroy;
};

struct Server::Impl {
    Options options;

    wl_display* display = nullptr;
    wlr_backend* backend = nullptr;
    wlr_renderer* renderer = nullptr;
    wlr_allocator* allocator = nullptr;
    wlr_scene* scene = nullptr;
    wlr_scene_output_layout* scene_layout = nullptr;
    wlr_output_layout* output_layout = nullptr;
    wlr_xdg_shell* xdg_shell = nullptr;
    wlr_cursor* cursor = nullptr;
    wlr_xcursor_manager* cursor_mgr = nullptr;
    wlr_seat* seat = nullptr;
    std::string socket;

    // Ownership (RAII teardown); drained naturally during shutdown by the
    // destroy events wl_display_destroy_clients / backend destroy fire.
    std::list<std::unique_ptr<Output>> outputs;
    std::unordered_map<wlr_xdg_toplevel*, std::unique_ptr<Toplevel>> toplevels;
    std::unordered_map<wlr_xdg_popup*, std::unique_ptr<Popup>> popups;
    std::list<std::unique_ptr<Keyboard>> keyboards;
    std::list<std::unique_ptr<TouchDevice>> touch_devices;

    // Focus order: front = focused. Contains MAPPED toplevels only.
    std::list<Toplevel*> mapped_toplevels;

    // Interactive move/resize grab state (one grab at a time).
    CursorMode cursor_mode = CursorMode::Passthrough;
    Toplevel* grabbed_toplevel = nullptr;
    double grab_x = 0.0;
    double grab_y = 0.0;
    wlr_box grab_geobox{};
    std::uint32_t resize_edges = 0;

    // Touch: Wayland implicitly grabs a touch point to the surface that
    // received down; we record the surface's layout origin at down time to
    // derive surface-local coords for motion. Assumes the surface doesn't
    // move mid-touch (true except during interactive grabs; slice 5 will
    // route input properly).
    struct TouchPoint {
        wlr_surface* surface = nullptr;
        double origin_x = 0.0;
        double origin_y = 0.0;
    };
    std::unordered_map<std::int32_t, TouchPoint> touch_points;

    // Server-level listeners (disconnected explicitly in shutdown() BEFORE
    // the wlr objects owning their signals are destroyed).
    Listener new_output;
    Listener new_input;
    Listener new_xdg_toplevel;
    Listener new_xdg_popup;
    Listener cursor_motion;
    Listener cursor_motion_absolute;
    Listener cursor_button;
    Listener cursor_axis;
    Listener cursor_frame;
    Listener cursor_touch_down;
    Listener cursor_touch_up;
    Listener cursor_touch_motion;
    Listener cursor_touch_cancel;
    Listener cursor_touch_frame;
    Listener seat_request_cursor;
    Listener seat_pointer_focus_change;
    Listener seat_request_set_selection;

    // server.cpp
    void init(); // throws std::runtime_error on any component failure
    void shutdown();
    void handle_new_output(wlr_output* output);

    // toplevel.cpp
    void handle_new_toplevel(wlr_xdg_toplevel* toplevel);
    void handle_new_popup(wlr_xdg_popup* popup);
    void focus_toplevel(Toplevel* toplevel);
    auto toplevel_at(double lx, double ly, wlr_surface** surface, double* sx, double* sy)
        -> Toplevel*;
    void begin_interactive(Toplevel* toplevel, CursorMode mode, std::uint32_t edges);
    void reset_cursor_mode();

    // input.cpp
    void handle_new_input(wlr_input_device* device);
    void new_keyboard(wlr_input_device* device);
    void new_pointer(wlr_input_device* device);
    void new_touch(wlr_input_device* device);
    void update_seat_capabilities();
    auto handle_keybinding(std::uint32_t keysym) -> bool;
    void attach_cursor_handlers();
    void attach_seat_handlers();
    void process_cursor_motion(std::uint32_t time_msec);
    void process_cursor_move();
    void process_cursor_resize();
};

} // namespace unbox::kernel
