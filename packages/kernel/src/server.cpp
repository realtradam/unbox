#include "server_impl.hpp"

#include <xkbcommon/xkbcommon.h> // virtual-keyboard test seam keymap

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

auto Server::ui_preview_import_is_dmabuf() const -> bool {
    return impl_->substrate != nullptr && impl_->substrate->preview_import_is_dmabuf();
}

auto Server::ui_surface_element_reimport_count() const -> int {
    return impl_->substrate != nullptr ? impl_->substrate->surface_element_reimport_count() : 0;
}

auto Server::ui_surface_element_frame_done_count() const -> int {
    return impl_->substrate != nullptr ? impl_->substrate->surface_element_frame_done_count() : 0;
}

auto Server::ui_surface_element_import_is_dmabuf() const -> bool {
    return impl_->substrate != nullptr && impl_->substrate->surface_element_import_is_dmabuf();
}

auto Server::ui_create_surface_element_for_test() -> bool {
    if (impl_->test_surface_element != nullptr) {
        return true; // already built (idempotent — no id churn on repeated polls)
    }
    if (impl_->substrate == nullptr || impl_->test_last_client_surface == nullptr) {
        return false;
    }
    impl_->test_surface_element =
        impl_->substrate->create_surface_element(impl_->test_last_client_surface);
    return impl_->test_surface_element != nullptr;
}

auto Server::ui_surface_element_uri() const -> std::string {
    return impl_->test_surface_element != nullptr ? impl_->test_surface_element->source_uri()
                                                  : std::string{};
}

auto Server::ui_surface_element_width() const -> int {
    return impl_->test_surface_element != nullptr ? impl_->test_surface_element->width() : 0;
}

auto Server::ui_surface_element_height() const -> int {
    return impl_->test_surface_element != nullptr ? impl_->test_surface_element->height() : 0;
}

void Server::ui_drop_surface_element_for_test() { impl_->test_surface_element.reset(); }

auto Server::ui_pixel(int x, int y) const -> unsigned int {
    return impl_->substrate != nullptr ? impl_->substrate->surface_pixel(x, y) : 0U;
}

auto Server::ui_surface_has_opaque_region() const -> bool {
    return impl_->substrate != nullptr && impl_->substrate->surface_has_opaque_region();
}

auto Server::ui_resize_realloc_count() const -> int {
    return impl_->substrate != nullptr ? impl_->substrate->resize_realloc_count() : 0;
}

auto Server::ui_element_count(const char* tag) const -> int {
    return impl_->substrate != nullptr ? impl_->substrate->element_count(tag) : 0;
}

auto Server::ui_click_element(const char* tag, int index) -> bool {
    return impl_->substrate != nullptr && impl_->substrate->click_element(tag, index);
}

auto Server::ui_drag_element(const char* tag, int index, double dx, double dy) -> bool {
    return impl_->substrate != nullptr && impl_->substrate->drag_element(tag, index, dx, dy);
}

auto Server::ui_reload_surface() -> bool {
    return impl_->substrate != nullptr && impl_->substrate->reload_first_surface();
}

void Server::ui_route_pointer_motion_for_test(double lx, double ly, unsigned int time_msec) {
    if (impl_->substrate != nullptr) {
        impl_->substrate->route_pointer_motion(lx, ly, time_msec);
    }
}

void Server::ui_route_pointer_button_for_test(double lx, double ly, bool pressed,
                                              unsigned int time_msec) {
    if (impl_->substrate != nullptr) {
        // A button needs a hover first (the cursor is already at (lx,ly) on a real
        // seat); feed a motion so the substrate's pick is current, then the button.
        impl_->substrate->route_pointer_motion(lx, ly, time_msec);
        (void)impl_->substrate->route_pointer_button(lx, ly, pressed, time_msec);
    }
}

void Server::ui_route_touch_down_for_test(int id, double lx, double ly, unsigned int time_msec) {
    if (impl_->substrate != nullptr) {
        (void)impl_->substrate->route_touch_down(id, lx, ly, time_msec);
    }
}

void Server::ui_route_touch_up_for_test(int id, unsigned int time_msec) {
    if (impl_->substrate != nullptr) {
        (void)impl_->substrate->route_touch_up(id, time_msec);
    }
}

namespace {
// A no-op wlr_keyboard impl for the virtual test keyboard (it never produces LED
// updates; the seat only needs a keyboard object + keymap to ship an enter).
const wlr_keyboard_impl kTestKeyboardImpl = {
    .name = "unbox-test-keyboard",
    .led_update = nullptr,
};
} // namespace

void Server::ui_add_test_keyboard() {
    if (impl_->test_keyboard != nullptr || impl_->seat == nullptr) {
        return;
    }
    auto* kb = new wlr_keyboard();
    wlr_keyboard_init(kb, &kTestKeyboardImpl, "unbox-test-keyboard");
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(kb, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
    impl_->test_keyboard = kb;
    // Advertise the keyboard capability + set it on the seat so an enter ships
    // the keymap (mirrors input.cpp::new_keyboard's seat wiring).
    wlr_seat_set_capabilities(impl_->seat, WL_SEAT_CAPABILITY_POINTER |
                                               WL_SEAT_CAPABILITY_TOUCH |
                                               WL_SEAT_CAPABILITY_KEYBOARD);
    wlr_seat_set_keyboard(impl_->seat, kb);
}

void Server::ui_send_key_for_test(unsigned int keycode, bool pressed) {
    if (impl_->seat == nullptr) {
        return;
    }
    // The post-filter equivalent of input.cpp's key path: the seat forwards the
    // key to whatever surface holds keyboard focus (set by focus_keyboard()).
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const std::uint32_t t = static_cast<std::uint32_t>(now.tv_sec) * 1000U +
                            static_cast<std::uint32_t>(now.tv_nsec) / 1000000U;
    wlr_seat_keyboard_notify_key(impl_->seat, t, keycode,
                                 pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                         : WL_KEYBOARD_KEY_STATE_RELEASED);
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

auto PerExtensionUi::create_preview(wlr_scene_tree* source) -> std::unique_ptr<Preview> {
    if (server_->substrate == nullptr) {
        return nullptr;
    }
    return server_->substrate->create_preview(source);
}

auto PerExtensionUi::create_surface_element(wlr_surface* client)
    -> std::unique_ptr<SurfaceElement> {
    if (server_->substrate == nullptr) {
        return nullptr;
    }
    return server_->substrate->create_surface_element(client);
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
    // Capture the session out-param: on the real DRM seat it is the libseat
    // session the VT-switch escape hatch (input.cpp) drives; NULL under
    // headless/nested (no real seat), where VT switching no-ops cleanly.
    backend = require(wlr_backend_autocreate(wl_display_get_event_loop(display), &session),
                      "wlr_backend");
    renderer = require(wlr_renderer_autocreate(backend), "wlr_renderer");
    wlr_renderer_init_wl_display(renderer, display);
    allocator = require(wlr_allocator_autocreate(backend, renderer), "wlr_allocator");

    compositor = wlr_compositor_create(display, 5, renderer);
    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);

    // Surface-element test seam (kernel suite only): track each new client
    // wl_surface and record the latest one that has committed a buffer, so a
    // headless test can build a real SurfaceElement from a real client surface.
    // No production behaviour depends on this; it just observes new_surface.
    if (compositor != nullptr) {
        test_new_surface.connect(compositor->events.new_surface, [this](void* data) {
            auto* surface = static_cast<wlr_surface*>(data);
            test_surface_captures.emplace_back();
            Impl::TestSurfaceCapture& cap = test_surface_captures.back();
            cap.surface = surface;
            cap.commit.connect(surface->events.commit, [this, surface](void*) {
                if (surface->buffer != nullptr) {
                    test_last_client_surface = surface;
                }
            });
            // RAII to the surface's lifetime: when the client destroys this
            // wl_surface, drop its capture record — unsubscribing the commit
            // listener BEFORE wlroots destroys the surface resource (which asserts
            // commit.listener_list is empty). Clearing the record is the handler's
            // LAST action; nothing touches it afterwards (listener.hpp's
            // destroy-event pattern). Also forget it if it was the captured one, so
            // no probe builds a SurfaceElement from a dead surface.
            cap.destroy.connect(surface->events.destroy, [this, surface](void*) {
                if (test_last_client_surface == surface) {
                    test_last_client_surface = nullptr;
                }
                test_surface_captures.remove_if(
                    [surface](const Impl::TestSurfaceCapture& c) { return c.surface == surface; });
            });
        });
    }

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
    // isolation path the bus uses (Server::Impl is the DisableSink). The
    // substrate uses the kernel's ONE shared FileWatcher for (UNBOX_DEV-gated)
    // asset hot-reload — the same watcher Host::watch_file uses for config.
    substrate = Substrate::create(display_egl, allocator, renderer, seat, file_watcher(),
                                  [this](ExtensionId who) { disable(who); },
                                  [this] { schedule_driver_frame(); });
}

auto Server::Impl::file_watcher() -> FileWatcher* {
    // Lazily create the ONE shared inotify watcher on first use (config watch or
    // asset hot-reload), carrying the kernel's disable sink for error isolation.
    if (watcher == nullptr) {
        if (display == nullptr) {
            return nullptr;
        }
        watcher = std::make_unique<FileWatcher>(wl_display_get_event_loop(display),
                                                [this](ExtensionId who) { disable(who); });
    }
    return watcher.get();
}

auto Server::Impl::frame_driver() -> FrameDriver* {
    // Lazily create the per-frame animation driver on first use, carrying the
    // kernel's disable sink for error isolation. No loop/wlr resource of its own
    // (the frame handler drives it), so it is always creatable.
    if (frames == nullptr) {
        frames = std::make_unique<FrameDriver>([this](ExtensionId who) { disable(who); });
    }
    return frames.get();
}

void Server::Impl::schedule_driver_frame() {
    // Pick / keep the primary driving output: the first one still present.
    if (frame_driver_output == nullptr && !outputs.empty()) {
        frame_driver_output = outputs.front()->output;
    }
    if (frame_driver_output != nullptr) {
        wlr_output_schedule_frame(frame_driver_output);
    }
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

    // Surface-element test seam: drop the test element (releases its import +
    // commit hook) and the capture listeners BEFORE the substrate/compositor go.
    test_surface_element.reset();
    test_new_surface.disconnect();
    test_surface_captures.clear(); // each record's commit+destroy listeners unsubscribe
    test_last_client_surface = nullptr;
    // The virtual test keyboard (if added) is finished + freed before the seat /
    // display die (a wlr_keyboard outliving the seat it was set on is UB).
    if (test_keyboard != nullptr) {
        wlr_keyboard_finish(test_keyboard);
        delete test_keyboard;
        test_keyboard = nullptr;
    }

    // The ui substrate owns scene nodes + GL objects on a sibling context and
    // borrows scene/renderer/allocator: tear it down before they die. (Its asset
    // FileWatch handles release here, removing those watches from the watcher.)
    substrate.reset();

    // The shared file watcher removes its wl_event_loop source + closes the
    // inotify fd here — AFTER every FileWatch holder (extensions, substrate) is
    // gone, and while the display/loop is still alive (the source must be
    // removed before wl_display_destroy).
    watcher.reset();

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
        // Per-frame animation callbacks (Host::request_frames) run on the PRIMARY
        // output's frame only — the first output added is the frame driver, so a
        // multi-output session gets ONE shared dt and the callbacks fire once per
        // displayed frame rather than once per output. They run BEFORE
        // substrate->tick_all()/commit so a callback that updates state +
        // UiSurface::dirty() is composited THIS frame.
        if (frame_driver_output == nullptr) {
            frame_driver_output = output->output; // promote a survivor / first output
        }
        if (output->output == frame_driver_output && frames != nullptr &&
            frames->has_requests()) {
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const double t = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
            const double dt = (last_frame_time < 0.0) ? 0.0 : (t - last_frame_time);
            last_frame_time = t;
            frames->drain(dt); // error-isolated; may add/remove requests (incl. its own)
            // Keep the frames coming while any request is still alive after the
            // drain (a callback may have removed the last one — then we stop,
            // returning to idle: no busy render at rest).
            if (frames->has_requests()) {
                wlr_output_schedule_frame(frame_driver_output);
            } else {
                last_frame_time = -1.0; // reset the dt base for the next animation
            }
        }

        if (substrate != nullptr) {
            substrate->tick_all();
        }
        wlr_scene_output* scene_output = wlr_scene_get_scene_output(scene, output->output);
        wlr_scene_output_commit(scene_output, nullptr);

        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_scene_output_send_frame_done(scene_output, &now);

        // Live surface elements (RML compositing) are NOT wlr_scene surface
        // nodes — they live as imported textures inside the substrate's RmlUi
        // documents — so wlr_scene_output_send_frame_done above never reaches
        // their backing wl_surfaces. Drive their frame callbacks ourselves so the
        // client keeps producing buffers (the stuck-frame fix). Done on the
        // PRIMARY output only (one frame-done per composited frame, like the
        // request_frames drain) and unconditionally (a client needs callbacks to
        // progress regardless of whether we re-rendered). While >=1 element exists
        // keep a frame scheduled so the loop self-sustains even when otherwise
        // idle (the continuous frame-callback duty).
        if (substrate != nullptr && output->output == frame_driver_output &&
            substrate->has_surface_elements()) {
            substrate->send_frame_done_to_surface_elements(now);
            wlr_output_schedule_frame(frame_driver_output);
        }
    });
    output->request_state.connect(wlr_output->events.request_state, [output](void* data) {
        const auto* event = static_cast<wlr_output_event_request_state*>(data);
        wlr_output_commit_state(output->output, event->state);
    });
    output->destroy.connect(wlr_output->events.destroy, [this, output](void*) {
        const OutputEvent ev{output->output};
        ev_output_removed.emit(ev);
        // If the frame DRIVER output is going away, re-point it (and reset the dt
        // base) so live animations keep advancing on a surviving output. This
        // MUST all happen BEFORE `outputs.remove_if` below: that call destroys
        // `output` together with THIS very listener's std::function storage, so
        // it has to be the LAST action — nothing may touch the lambda or its
        // captures afterwards.
        if (frame_driver_output == output->output) {
            frame_driver_output = nullptr;
            last_frame_time = -1.0;
            // Promote a SURVIVING output (skip the one being destroyed) and, if
            // any frame request is alive, schedule its next frame.
            for (const auto& owned : outputs) {
                if (owned.get() != output) {
                    frame_driver_output = owned->output;
                    break;
                }
            }
            if (frame_driver_output != nullptr && frames != nullptr && frames->has_requests()) {
                wlr_output_schedule_frame(frame_driver_output);
            }
        }
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
