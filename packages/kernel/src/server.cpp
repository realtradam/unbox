#include "server_impl.hpp"

#include <ctime>
#include <stdexcept>
#include <unistd.h>
#include <unordered_map>
#include <utility>

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

void Server::install(std::unique_ptr<Extension> extension) {
    impl_->install(std::move(extension));
}

void Server::activate_extensions() {
    impl_->activate_extensions();
}

void Server::run() {
    impl_->activate_extensions();
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

auto Server::ui_frame_count() const -> int {
    return impl_->substrate != nullptr ? impl_->substrate->frame_count() : 0;
}

auto Server::ui_orientation() const -> int {
    return impl_->substrate != nullptr ? impl_->substrate->orientation() : 0;
}

auto Server::ui_fence_sync_active() const -> bool {
    return impl_->substrate != nullptr && impl_->substrate->fence_sync_active();
}

void Server::ui_set_touch_override(UiTouchOverride ov) {
    if (impl_->substrate == nullptr) {
        return;
    }
    UiSubstrate::TouchModeOverride mapped = UiSubstrate::TouchModeOverride::automatic;
    if (ov == UiTouchOverride::force_off) {
        mapped = UiSubstrate::TouchModeOverride::force_off;
    } else if (ov == UiTouchOverride::force_on) {
        mapped = UiSubstrate::TouchModeOverride::force_on;
    }
    impl_->substrate->set_touch_mode_override(mapped);
}

// ---- PerExtensionUi (per-extension ui-substrate facade) --------------------

auto PerExtensionUi::create_surface(const UiSurfaceSpec& spec) -> std::unique_ptr<UiSurface> {
    if (server_->substrate == nullptr) {
        return nullptr;
    }
    wlr_scene_tree* parent = server_->scene_layers[static_cast<std::size_t>(spec.layer)];
    return server_->substrate->create_surface(id_, parent, spec);
}

auto PerExtensionUi::available() const -> bool {
    return server_->substrate != nullptr && server_->substrate->available();
}

auto PerExtensionUi::touch_mode() const -> bool {
    return server_->substrate != nullptr && server_->substrate->touch_mode();
}

void PerExtensionUi::set_touch_mode_override(TouchModeOverride ov) {
    if (server_->substrate != nullptr) {
        server_->substrate->set_touch_mode_override(ov);
    }
}

// ---- Impl lifecycle --------------------------------------------------------

void Server::Impl::register_hook(detail::HookBase& hook) {
    hook.set_sink(this);
    all_hooks.push_back(&hook);
}

void Server::Impl::disable(ExtensionId who) noexcept {
    // Error isolation: a callback owned by `who` threw. Mark the extension dead
    // and purge its subscriptions from every hook. Safe mid-dispatch — each
    // hook tombstones now and compacts when its dispatch unwinds.
    for (ExtensionSlot& slot : extensions) {
        if (slot.id == who && !slot.disabled) {
            slot.disabled = true;
            wlr_log(WLR_ERROR, "extension '%s' disabled: a hook callback threw",
                    slot.extension->manifest().id.c_str());
        }
    }
    for (detail::HookBase* hook : all_hooks) {
        hook->purge(who);
    }
}

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

    // Ordered z-bands. wlr_scene_tree_create appends as the top child of its
    // parent, so creating background -> overlay yields exactly that stacking
    // order (background lowest, overlay highest).
    for (auto& layer : scene_layers) {
        layer = require(wlr_scene_tree_create(&scene->tree), "wlr_scene_tree (layer)");
    }

    cursor = require(wlr_cursor_create(), "wlr_cursor");
    wlr_cursor_attach_output_layout(cursor, output_layout);
    cursor_mgr = require(wlr_xcursor_manager_create(nullptr, 24), "wlr_xcursor_manager");
    attach_cursor_handlers();

    new_input.connect(backend->events.new_input, [this](void* data) {
        handle_new_input(static_cast<wlr_input_device*>(data));
    });
    seat = require(wlr_seat_create(display, "seat0"), "wlr_seat");
    attach_seat_handlers();

    // Register every kernel-emitted hook with the isolation registry.
    for (detail::HookBase* hook : {
             static_cast<detail::HookBase*>(&ev_output_added),
             static_cast<detail::HookBase*>(&ev_output_removed),
             static_cast<detail::HookBase*>(&ev_pointer_motion),
             static_cast<detail::HookBase*>(&ev_pointer_button),
             static_cast<detail::HookBase*>(&ev_pointer_axis),
             static_cast<detail::HookBase*>(&ev_pointer_frame),
             static_cast<detail::HookBase*>(&ev_touch_down),
             static_cast<detail::HookBase*>(&ev_touch_motion),
             static_cast<detail::HookBase*>(&ev_touch_up),
             static_cast<detail::HookBase*>(&ev_touch_cancel),
             static_cast<detail::HookBase*>(&ev_touch_frame),
             static_cast<detail::HookBase*>(&key_filter),
         }) {
        all_hooks.push_back(hook); // sink already set via {this} constructor
    }

    const char* socket_cstr = wl_display_add_socket_auto(display);
    if (socket_cstr == nullptr) {
        throw std::runtime_error("failed to add a Wayland socket");
    }
    socket = socket_cstr;

    // Advertise OUR socket in the PROCESS environment. The process inherited
    // WAYLAND_DISPLAY from its parent (e.g. labwc's wayland-0), but our real
    // socket is whatever wl_display_add_socket_auto just picked. Without this,
    // children spawned by extensions (ext-keybindings forks fuzzel) inherit the
    // stale parent value and connect to the WRONG compositor -> "no monitors".
    // setenv here makes every child (the -s startup spawn AND extension spawns)
    // reach unbox by default. tinywl/sway do exactly this.
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1);

    if (!wlr_backend_start(backend)) {
        throw std::runtime_error("failed to start the wlr_backend");
    }

    // The ui substrate is always built; it reports available()==false on a
    // backend with no GL path (headless pixman) and create_surface yields
    // nullptr there, so extensions degrade gracefully. Never throws.
    start_substrate();

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

// ---- Extension host --------------------------------------------------------

void Server::Impl::install(std::unique_ptr<Extension> extension) {
    if (extensions_activated) {
        throw std::runtime_error("Server::install called after activate_extensions");
    }
    const std::string& id = extension->manifest().id;
    for (const ExtensionSlot& slot : extensions) {
        if (slot.extension->manifest().id == id) {
            throw std::runtime_error("duplicate extension id: " + id);
        }
    }
    ExtensionSlot slot;
    // id 0 is the kernel; extensions start at 1.
    slot.id = static_cast<ExtensionId>(extensions.size() + 1);
    slot.host = std::make_unique<HostImpl>(this, slot.id);
    slot.extension = std::move(extension);
    extensions.push_back(std::move(slot));
}

void Server::Impl::activate_extensions() {
    if (extensions_activated) {
        return;
    }
    extensions_activated = true;

    // Topological sort by Manifest depends_on. Index extensions by id; ties
    // (no dependency relation) are broken by tier (core before standard) then
    // install order — deterministic activation.
    const std::size_t n = extensions.size();
    std::unordered_map<std::string, std::size_t> by_id;
    for (std::size_t i = 0; i < n; ++i) {
        by_id.emplace(extensions[i].extension->manifest().id, i);
    }

    // Build adjacency (dep -> dependents) and indegree; validate deps exist.
    std::vector<std::vector<std::size_t>> dependents(n);
    std::vector<int> indegree(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const Manifest& m = extensions[i].extension->manifest();
        for (const std::string& dep : m.depends_on) {
            auto it = by_id.find(dep);
            if (it == by_id.end()) {
                throw std::runtime_error("extension '" + m.id +
                                         "' depends on missing extension '" + dep + "'");
            }
            dependents[it->second].push_back(i);
            ++indegree[i];
        }
    }

    // Kahn's algorithm with a deterministic tie-break: among ready nodes pick
    // the lowest (tier, install-index). A linear scan is fine for the handful
    // of extensions a session installs.
    auto rank = [&](std::size_t i) {
        return std::pair<int, std::size_t>(
            static_cast<int>(extensions[i].extension->manifest().tier), i);
    };
    std::vector<bool> done(n, false);
    std::vector<std::size_t> order;
    order.reserve(n);
    for (std::size_t step = 0; step < n; ++step) {
        std::size_t pick = n;
        for (std::size_t i = 0; i < n; ++i) {
            if (!done[i] && indegree[i] == 0) {
                if (pick == n || rank(i) < rank(pick)) {
                    pick = i;
                }
            }
        }
        if (pick == n) {
            throw std::runtime_error("extension dependency cycle detected");
        }
        done[pick] = true;
        order.push_back(pick);
        for (std::size_t d : dependents[pick]) {
            --indegree[d];
        }
    }

    // Activate in topological order. An activate() throw is FATAL (not
    // isolated): a core extension that cannot start is a broken session.
    for (std::size_t i : order) {
        ExtensionSlot& slot = extensions[i];
        slot.extension->activate(*slot.host);
        slot.activated = true;
    }
}

void Server::Impl::start_substrate() {
    // The substrate needs the wlr renderer's EGLDisplay for its sibling GLES
    // 3.2 context. Only the gles2 renderer exposes one; under pixman (headless
    // CI) there is no GL path, so the substrate builds but reports unavailable.
    EGLDisplay display_egl = EGL_NO_DISPLAY;
    if (wlr_renderer_is_gles2(renderer)) {
        if (wlr_egl* egl = wlr_gles2_renderer_get_egl(renderer)) {
            display_egl = wlr_egl_get_display(egl);
        }
    } else {
        wlr_log(WLR_INFO, "ui-substrate: renderer is not gles2; substrate unavailable");
    }
    // A data-event/getter throw disables the owning extension via the same
    // isolation path the bus uses (Server::Impl is the DisableSink).
    substrate = Substrate::create(display_egl, allocator, renderer,
                                  [this](ExtensionId who) { disable(who); });
}

void Server::Impl::shutdown() {
    // Destroy extensions FIRST, in reverse activation order: their RAII members
    // (Subscriptions, Listeners, scene nodes) release while the wlr objects
    // they borrow are still alive. Reverse of `extensions` install order is a
    // safe superset of reverse-topological (a dependent installed later than
    // its dependency dies first; if installed earlier, it still only borrows).
    for (auto it = extensions.rbegin(); it != extensions.rend(); ++it) {
        it->extension.reset();
        it->host.reset();
    }
    extensions.clear();

    // The ui substrate owns scene nodes + GL objects on a sibling context and
    // borrows scene/renderer/allocator: tear it down before they die.
    substrate.reset();

    if (display != nullptr) {
        wl_display_destroy_clients(display);
    }

    // Server-level listeners detach BEFORE the wlr objects owning their signals
    // die; a wl_listener outliving its signal is a use-after-free.
    new_output.disconnect();
    new_input.disconnect();
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
        wlr_backend_destroy(backend);
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

    // Enable + set a mode, then commit. The committed mode is what gives the
    // output a non-zero width/height — and wlr_output_layout (below) advertises
    // the client-facing wl_output global ONLY for an output whose width/height
    // are > 0 (see output_update_global in wlroots' wlr_output_layout.c). So a
    // FAILED modeset leaves size 0 and the output silently global-less: clients
    // that need an output (layer-shell: fuzzel et al.) see "no monitors".
    //
    // Headless commits trivially succeed; the DRM modeset can fail for the
    // preferred mode. tinywl ignores the commit result (single-mode demo); we
    // must not. Try the preferred mode, then any other reported mode, then a
    // mode-less enable, so every backend ends up with a committed, advertisable
    // output where the hardware allows one at all.
    auto try_commit = [&](wlr_output_mode* mode) -> bool {
        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);
        if (mode != nullptr) {
            wlr_output_state_set_mode(&state, mode);
        }
        const bool ok = wlr_output_commit_state(wlr_output, &state);
        wlr_output_state_finish(&state);
        return ok;
    };

    bool committed = try_commit(wlr_output_preferred_mode(wlr_output));
    if (!committed) {
        wlr_output_mode* mode = nullptr;
        wl_list_for_each(mode, &wlr_output->modes, link) {
            if (try_commit(mode)) {
                committed = true;
                break;
            }
        }
    }
    if (!committed) {
        // Mode-less enable (modeless backends, or hardware that rejected every
        // mode). On a modeful backend this generally won't yield a usable size,
        // but it is the last resort and keeps the output enabled.
        committed = try_commit(nullptr);
    }
    if (!committed) {
        wlr_log(WLR_ERROR, "output %s: every commit failed; no wl_output global",
                wlr_output->name);
    }

    auto owned = std::make_unique<Output>();
    Output* output = owned.get();
    output->server = this;
    output->output = wlr_output;
    outputs.push_back(std::move(owned));

    output->frame.connect(wlr_output->events.frame, [this, output](void*) {
        if (substrate != nullptr) {
            substrate->tick_all();
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
        const OutputEvent ev{output->output};
        ev_output_removed.emit(ev);
        // Last action: destroys `output` (and these listeners with it).
        outputs.remove_if([output](const auto& owned) { return owned.get() == output; });
    });

    // Adding to the layout auto-advertises the wl_output global for an output
    // with a committed size (output_update_global in wlr_output_layout.c).
    wlr_output_layout_output* layout_output = wlr_output_layout_add_auto(output_layout, wlr_output);
    wlr_scene_output* scene_output = wlr_scene_output_create(scene, wlr_output);
    wlr_scene_output_layout_add_output(scene_layout, layout_output, scene_output);

    const OutputEvent ev{wlr_output};
    ev_output_added.emit(ev);
}

} // namespace unbox::kernel
