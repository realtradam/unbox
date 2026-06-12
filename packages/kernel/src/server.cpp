#include "server_impl.hpp"

#include <ctime>
#include <stdexcept>
#include <unistd.h>

namespace unbox::kernel {

namespace {

template <typename T>
auto require(T* ptr, const char* what) -> T* {
    if (ptr == nullptr) {
        throw std::runtime_error(std::string("failed to create ") + what);
    }
    return ptr;
}

} // namespace

// ---- Server (public surface) ----------------------------------------------

auto Server::create(Options options) -> std::unique_ptr<Server> {
    auto impl = std::make_unique<Impl>();
    impl->options = std::move(options);
    try {
        impl->init();
    } catch (...) {
        impl->shutdown();
        throw;
    }
    return std::unique_ptr<Server>(new Server(std::move(impl)));
}

Server::Server(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Server::~Server() {
    impl_->shutdown();
}

auto Server::socket_name() const -> std::string {
    return impl_->socket;
}

void Server::run() {
    wlr_log(WLR_INFO, "unbox running on WAYLAND_DISPLAY=%s", impl_->socket.c_str());
    wl_display_run(impl_->display);
}

auto Server::dispatch(int timeout_ms) -> bool {
    wl_event_loop* loop = wl_display_get_event_loop(impl_->display);
    const int rc = wl_event_loop_dispatch(loop, timeout_ms);
    wl_display_flush_clients(impl_->display);
    return rc >= 0;
}

void Server::terminate() {
    wl_display_terminate(impl_->display);
}

auto Server::ui_spike_frame_count() const -> int {
    return impl_->ui_spike != nullptr ? impl_->ui_spike->frame_count() : 0;
}

auto Server::ui_spike_orientation() const -> int {
    return impl_->ui_spike != nullptr ? impl_->ui_spike->check_orientation() : 0;
}

// ---- Impl lifecycle --------------------------------------------------------

void Server::Impl::init() {
    wlr_log_init(WLR_INFO, nullptr);

    display = require(wl_display_create(), "wl_display");
    backend = require(wlr_backend_autocreate(wl_display_get_event_loop(display), nullptr),
                      "wlr_backend");
    renderer = require(wlr_renderer_autocreate(backend), "wlr_renderer");
    wlr_renderer_init_wl_display(renderer, display);
    allocator = require(wlr_allocator_autocreate(backend, renderer), "wlr_allocator");

    wlr_compositor_create(display, 5, renderer);
    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);

    output_layout = require(wlr_output_layout_create(display), "wlr_output_layout");
    new_output.connect(backend->events.new_output, [this](void* data) {
        handle_new_output(static_cast<wlr_output*>(data));
    });

    scene = require(wlr_scene_create(), "wlr_scene");
    scene_layout = require(wlr_scene_attach_output_layout(scene, output_layout),
                           "wlr_scene_output_layout");

    xdg_shell = require(wlr_xdg_shell_create(display, 3), "wlr_xdg_shell");
    new_xdg_toplevel.connect(xdg_shell->events.new_toplevel, [this](void* data) {
        handle_new_toplevel(static_cast<wlr_xdg_toplevel*>(data));
    });
    new_xdg_popup.connect(xdg_shell->events.new_popup, [this](void* data) {
        handle_new_popup(static_cast<wlr_xdg_popup*>(data));
    });

    cursor = require(wlr_cursor_create(), "wlr_cursor");
    wlr_cursor_attach_output_layout(cursor, output_layout);
    cursor_mgr = require(wlr_xcursor_manager_create(nullptr, 24), "wlr_xcursor_manager");
    attach_cursor_handlers();

    new_input.connect(backend->events.new_input, [this](void* data) {
        handle_new_input(static_cast<wlr_input_device*>(data));
    });
    seat = require(wlr_seat_create(display, "seat0"), "wlr_seat");
    attach_seat_handlers();

    const char* socket_cstr = wl_display_add_socket_auto(display);
    if (socket_cstr == nullptr) {
        throw std::runtime_error("failed to add a Wayland socket");
    }
    socket = socket_cstr;

    if (!wlr_backend_start(backend)) {
        throw std::runtime_error("failed to start the wlr_backend");
    }

    if (options.ui_spike) {
        start_ui_spike();
    }

    if (!options.startup_cmd.empty()) {
        if (fork() == 0) {
            // Child only: don't pollute our own environment.
            setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
            execl("/bin/sh", "/bin/sh", "-c", options.startup_cmd.c_str(),
                  static_cast<char*>(nullptr));
            _exit(127);
        }
    }
}

void Server::Impl::start_ui_spike() {
    // The bridge needs the wlr renderer's EGLDisplay to build its sibling
    // GLES 3.2 context. Only the gles2 renderer exposes one; under the
    // pixman renderer (e.g. headless CI) there is no GL path, so the spike
    // stays disabled — slice-2 behaviour is preserved.
    if (!wlr_renderer_is_gles2(renderer)) {
        wlr_log(WLR_INFO, "ui-spike: renderer is not gles2; spike disabled");
        return;
    }
    wlr_egl* egl = wlr_gles2_renderer_get_egl(renderer);
    if (egl == nullptr) {
        wlr_log(WLR_ERROR, "ui-spike: gles2 renderer has no wlr_egl");
        return;
    }
    EGLDisplay display_egl = wlr_egl_get_display(egl);
    ui_spike = UiSpike::create(&scene->tree, display_egl, allocator, renderer);
}

void Server::Impl::shutdown() {
    // Slice-3 spike: tear down before scene/renderer/allocator die (it owns
    // a scene node, GL objects on a sibling context, and borrows the others).
    ui_spike.reset();

    if (display != nullptr) {
        wl_display_destroy_clients(display); // fires toplevel/popup destroy events
    }

    // Server-level listeners must detach BEFORE the wlr objects owning their
    // signals die; a wl_listener outliving its signal is a use-after-free.
    new_output.disconnect();
    new_input.disconnect();
    new_xdg_toplevel.disconnect();
    new_xdg_popup.disconnect();
    cursor_motion.disconnect();
    cursor_motion_absolute.disconnect();
    cursor_button.disconnect();
    cursor_axis.disconnect();
    cursor_frame.disconnect();
    cursor_touch_down.disconnect();
    cursor_touch_up.disconnect();
    cursor_touch_motion.disconnect();
    cursor_touch_cancel.disconnect();
    cursor_touch_frame.disconnect();
    seat_request_cursor.disconnect();
    seat_pointer_focus_change.disconnect();
    seat_request_set_selection.disconnect();

    if (scene != nullptr) {
        wlr_scene_node_destroy(&scene->tree.node);
        scene = nullptr;
    }
    if (cursor_mgr != nullptr) {
        wlr_xcursor_manager_destroy(cursor_mgr);
        cursor_mgr = nullptr;
    }
    if (cursor != nullptr) {
        wlr_cursor_destroy(cursor);
        cursor = nullptr;
    }
    if (allocator != nullptr) {
        wlr_allocator_destroy(allocator);
        allocator = nullptr;
    }
    if (renderer != nullptr) {
        wlr_renderer_destroy(renderer);
        renderer = nullptr;
    }
    if (backend != nullptr) {
        wlr_backend_destroy(backend); // fires output + input-device destroy events
        backend = nullptr;
    }
    if (display != nullptr) {
        wl_display_destroy(display);
        display = nullptr;
    }
}

// ---- Outputs ----------------------------------------------------------------

void Server::Impl::handle_new_output(wlr_output* wlr_output) {
    wlr_output_init_render(wlr_output, allocator, renderer);

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (wlr_output_mode* mode = wlr_output_preferred_mode(wlr_output)) {
        wlr_output_state_set_mode(&state, mode);
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    auto owned = std::make_unique<Output>();
    Output* output = owned.get();
    output->server = this;
    output->output = wlr_output;
    outputs.push_back(std::move(owned));

    output->frame.connect(wlr_output->events.frame, [this, output](void*) {
        // Slice-3 spike: render the RMLUi document if dirty, before commit so
        // its damage is picked up this frame. Cheap no-op when disabled.
        if (ui_spike != nullptr) {
            ui_spike->tick();
        }

        wlr_scene_output* scene_output = wlr_scene_get_scene_output(scene, output->output);
        wlr_scene_output_commit(scene_output, nullptr);

        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_scene_output_send_frame_done(scene_output, &now);
    });
    output->request_state.connect(wlr_output->events.request_state, [output](void* data) {
        const auto* event = static_cast<wlr_output_event_request_state*>(data);
        wlr_output_commit_state(output->output, event->state);
    });
    output->destroy.connect(wlr_output->events.destroy, [this, output](void*) {
        // Last action: destroys `output` (and these listeners with it).
        outputs.remove_if([output](const auto& owned) { return owned.get() == output; });
    });

    wlr_output_layout_output* layout_output = wlr_output_layout_add_auto(output_layout, wlr_output);
    wlr_scene_output* scene_output = wlr_scene_output_create(scene, wlr_output);
    wlr_scene_output_layout_add_output(scene_layout, layout_output, scene_output);

    wlr_log(WLR_INFO, "new output %s", wlr_output->name);
}

} // namespace unbox::kernel
