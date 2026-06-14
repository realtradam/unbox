#pragma once

#include <unbox/kernel/host.hpp>
#include <unbox/kernel/server.hpp>
#include <unbox/kernel/ui.hpp>
#include <unbox/kernel/wlr.hpp>

#include "file_watcher.hpp"
#include "frame_driver.hpp"
#include "listener.hpp"
#include "ui_substrate.hpp"

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

// Private kernel state (slice 4). The kernel names no concrete feature: shell
// policy (xdg-shell toplevels/popups, focus, cycling, interactive move/resize,
// keybindings) was EXTRACTED to extensions. What remains is generic plumbing
// plus the extension host + typed bus.
//
// Definitions split: server.cpp (lifecycle, outputs, host/bus/activation),
// input.cpp (devices, cursor, touch, seat — now event-emitting, not routing).
//
// Everything runs on the single wl_event_loop thread.

namespace unbox::kernel {

struct Output {
    Server::Impl* server = nullptr;
    wlr_output* output = nullptr;
    Listener frame;
    Listener request_state;
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

// ---- Extension host bookkeeping ---------------------------------------------

class HostImpl; // per-extension Host facade (host.cpp)

// One installed extension's kernel-side record.
struct ExtensionSlot {
    std::unique_ptr<Extension> extension;
    std::unique_ptr<HostImpl> host;
    ExtensionId id{};
    bool activated = false;
    bool disabled = false; // tripped when a callback threw (error isolation)
};

struct Server::Impl : detail::DisableSink {
    Options options;

    wl_display* display = nullptr;
    wlr_backend* backend = nullptr;
    // The libseat/logind session, captured from wlr_backend_autocreate's
    // out-param at init. NULL under headless/nested backends (no real seat) —
    // the VT-switch escape hatch no-ops cleanly then. Owned by the backend.
    wlr_session* session = nullptr;
    wlr_renderer* renderer = nullptr;
    wlr_allocator* allocator = nullptr;
    wlr_scene* scene = nullptr;
    wlr_scene_output_layout* scene_layout = nullptr;
    wlr_output_layout* output_layout = nullptr;
    wlr_cursor* cursor = nullptr;
    wlr_xcursor_manager* cursor_mgr = nullptr;
    wlr_seat* seat = nullptr;
    std::string socket;

    // Ordered scene-tree z-bands (SceneLayer order). Created once over
    // scene->tree in stacking order so background < … < overlay. Extensions
    // attach nodes via Host::scene_layer(); the kernel owns them.
    std::array<wlr_scene_tree*, 5> scene_layers{};

    // The ui substrate (the kernel's RMLUi subsystem behind <unbox/kernel/ui.hpp>).
    // Kernel-owned; torn down in shutdown() BEFORE scene/renderer/allocator. Its
    // per-extension facades (PerExtensionUi, one per HostImpl) borrow it.
    std::unique_ptr<Substrate> substrate;

    // The ONE inotify-on-the-wl_event_loop file watcher for the session, shared
    // by Host::watch_file (config + extensions) AND the substrate's asset
    // hot-reload. Created lazily on the first watch (asset or watch_file) via
    // file_watcher(); torn down in shutdown() BEFORE the display/loop dies (so
    // its event source is removed while the loop is still alive).
    std::unique_ptr<FileWatcher> watcher;
    // Get-or-create the shared watcher (lazy). Returns nullptr only if there is
    // no wl_event_loop (never in practice — the display always has one).
    auto file_watcher() -> FileWatcher*;

    // The per-frame animation driver (Host::request_frames). Holds the live
    // per-frame callbacks; drained from the PRIMARY output's frame handler
    // (frame_driver_output) BEFORE substrate->tick_all()/commit. Created lazily
    // on the first request; outlives the extensions (their FrameRequest handles
    // unregister into it on destruction), so it is a plain Impl member torn down
    // after the extension slots in shutdown(). Has no loop/wlr resource itself.
    std::unique_ptr<FrameDriver> frames;
    auto frame_driver() -> FrameDriver*;
    // The output whose frame event drives request_frames: the FIRST output
    // added (the primary). One shared dt; secondary outputs do not drive the
    // callbacks. Cleared when that output is removed (a survivor is promoted in
    // the next frame handler that runs). Borrow; the kernel owns the output.
    wlr_output* frame_driver_output = nullptr;
    // Monotonic timestamp (seconds) of the previous driving frame; used to
    // compute dt. Reset (< 0) until the first driving frame establishes a base.
    double last_frame_time = -1.0;

    std::list<std::unique_ptr<Output>> outputs;
    std::list<std::unique_ptr<Keyboard>> keyboards;
    std::list<std::unique_ptr<TouchDevice>> touch_devices;

    // ---- The typed bus: kernel-emitted hooks (host.hpp catalogue) ----
    Event<const OutputEvent&> ev_output_added{this};
    Event<const OutputEvent&> ev_output_removed{this};
    Event<const PointerMotionEvent&> ev_pointer_motion{this};
    Event<const PointerButtonEvent&> ev_pointer_button{this};
    Event<const PointerAxisEvent&> ev_pointer_axis{this};
    Event<> ev_pointer_frame{this};
    Event<const TouchDownEvent&> ev_touch_down{this};
    Event<const TouchMotionEvent&> ev_touch_motion{this};
    Event<const TouchUpEvent&> ev_touch_up{this};
    Event<const TouchCancelEvent&> ev_touch_cancel{this};
    Event<> ev_touch_frame{this};
    Filter<KeyEvent> key_filter{this};

    // Every hook bound to the isolation registry (kernel hooks above + any
    // extension-exported hooks adopted via Host::adopt). disable() purges an
    // extension across ALL of them. Raw borrows; lifetimes are the hooks'.
    std::vector<detail::HookBase*> all_hooks;

    // Typed service registry (one provider per interface type; no strings).
    std::unordered_map<std::type_index, void*> services;

    // Kernel-owned surface -> scene-tree association (replaces the old untyped
    // wlr_surface.data cross-extension convention). Shared by all extensions
    // via Host::host_surface/scene_tree_for; the kernel just stores it.
    detail::PointerAssoc surface_assoc;

    // Installed extensions, in install order; activation order is computed
    // topologically in activate_extensions().
    std::vector<ExtensionSlot> extensions;
    bool extensions_activated = false;

    // Server-level listeners (disconnected in shutdown() BEFORE the wlr objects
    // owning their signals die).
    Listener new_output;
    Listener new_input;
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

    // detail::DisableSink
    void disable(ExtensionId who) noexcept override;

    // server.cpp — lifecycle + outputs
    void init(); // throws std::runtime_error on any component failure
    void shutdown();
    void handle_new_output(wlr_output* output);
    void start_substrate(); // builds the ui substrate; never throws, may be unavailable
    void register_hook(detail::HookBase& hook); // track for purge/disable
    // Schedule a frame on the driving output (if any) so the continuous-frame
    // loop keeps advancing while >=1 FrameRequest is alive. No-op if no output.
    void schedule_driver_frame();

    // server.cpp — extension host
    void install(std::unique_ptr<Extension> extension);
    void activate_extensions();

    // input.cpp — devices, cursor, touch, seat (event-emitting glue)
    void handle_new_input(wlr_input_device* device);
    void new_keyboard(wlr_input_device* device);
    void new_pointer(wlr_input_device* device);
    void new_touch(wlr_input_device* device);
    void update_seat_capabilities();
    void attach_cursor_handlers();
    void attach_seat_handlers();
    void emit_pointer_motion(std::uint32_t time_msec);
};

// ---- Per-extension ui-substrate facade --------------------------------------
//
// The public UiSubstrate an extension gets from Host::ui(). Injects the owning
// extension id (for error isolation) and resolves a UiSurfaceSpec's SceneLayer
// to the kernel's scene-layer tree, then delegates to the shared Substrate.
// Owned by its HostImpl; borrows the kernel-owned Substrate.

class PerExtensionUi final : public UiSubstrate {
public:
    PerExtensionUi(Server::Impl* server, ExtensionId id) : server_(server), id_(id) {}

    auto create_surface(const UiSurfaceSpec& spec) -> std::unique_ptr<UiSurface> override;
    auto create_preview(wlr_scene_tree* source) -> std::unique_ptr<Preview> override;
    auto available() const -> bool override;
    auto touch_mode() const -> bool override;
    void set_touch_mode_override(TouchModeOverride ov) override;

private:
    Server::Impl* server_;
    ExtensionId id_;
};

// ---- Per-extension Host facade ----------------------------------------------

class HostImpl final : public Host {
public:
    HostImpl(Server::Impl* server, ExtensionId id) : server_(server), id_(id), ui_(server, id) {}

    auto display() -> wl_display* override { return server_->display; }
    auto scene() -> wlr_scene* override { return server_->scene; }
    auto seat() -> wlr_seat* override { return server_->seat; }
    auto cursor() -> wlr_cursor* override { return server_->cursor; }
    auto cursor_manager() -> wlr_xcursor_manager* override { return server_->cursor_mgr; }
    auto output_layout() -> wlr_output_layout* override { return server_->output_layout; }
    auto scene_layer(SceneLayer layer) -> wlr_scene_tree* override {
        return server_->scene_layers[static_cast<std::size_t>(layer)];
    }
    auto ui() -> UiSubstrate& override { return ui_; }

    auto on_output_added() -> Event<const OutputEvent&>& override {
        return server_->ev_output_added;
    }
    auto on_output_removed() -> Event<const OutputEvent&>& override {
        return server_->ev_output_removed;
    }
    auto on_pointer_motion() -> Event<const PointerMotionEvent&>& override {
        return server_->ev_pointer_motion;
    }
    auto on_pointer_button() -> Event<const PointerButtonEvent&>& override {
        return server_->ev_pointer_button;
    }
    auto on_pointer_axis() -> Event<const PointerAxisEvent&>& override {
        return server_->ev_pointer_axis;
    }
    auto on_pointer_frame() -> Event<>& override { return server_->ev_pointer_frame; }
    auto on_touch_down() -> Event<const TouchDownEvent&>& override {
        return server_->ev_touch_down;
    }
    auto on_touch_motion() -> Event<const TouchMotionEvent&>& override {
        return server_->ev_touch_motion;
    }
    auto on_touch_up() -> Event<const TouchUpEvent&>& override { return server_->ev_touch_up; }
    auto on_touch_cancel() -> Event<const TouchCancelEvent&>& override {
        return server_->ev_touch_cancel;
    }
    auto on_touch_frame() -> Event<>& override { return server_->ev_touch_frame; }
    auto key_filter() -> Filter<KeyEvent>& override { return server_->key_filter; }

protected:
    auto extension_id() const -> ExtensionId override { return id_; }

    auto register_service(std::type_index type, void* impl) -> bool override {
        const bool fresh = server_->services.emplace(type, impl).second;
        if (!fresh) {
            server_->services[type] = impl; // replace
        }
        return fresh;
    }
    auto lookup_service(std::type_index type) -> void* override {
        auto it = server_->services.find(type);
        return it == server_->services.end() ? nullptr : it->second;
    }
    void adopt_hook(detail::HookBase& hook) override { server_->register_hook(hook); }
    auto surface_store() -> detail::PointerAssoc& override { return server_->surface_assoc; }
    auto register_file_watch(std::string path, std::function<void()> on_change)
        -> FileWatch override {
        FileWatcher* w = server_->file_watcher();
        if (w == nullptr) {
            return FileWatch{};
        }
        return w->add(path, std::move(on_change), id_);
    }
    auto register_frame_request(std::function<void(double)> on_frame) -> FrameRequest override {
        FrameDriver* d = server_->frame_driver();
        if (d == nullptr) {
            return FrameRequest{}; // no event loop: inert handle (mirror watch_file)
        }
        FrameRequest req = d->add(std::move(on_frame), id_);
        // Kick the driving output so the continuous-frame loop starts THIS turn
        // even if the scene was idle (the frame handler re-arms each frame while
        // requests live). Safe before any output exists (no-op then).
        server_->schedule_driver_frame();
        return req;
    }

private:
    Server::Impl* server_;
    ExtensionId id_;
    PerExtensionUi ui_;
};

} // namespace unbox::kernel
