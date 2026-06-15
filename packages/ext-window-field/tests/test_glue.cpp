#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "probe.hpp" // PRIVATE: a unit may read its own src/ (drives the model)

#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/server.hpp>
#include <unbox/kernel/wlr.hpp>

#include <wayland-client.h>

// CLIENT-side xdg-shell bindings, generated from the canonical xdg-shell.xml the
// same way ext-stage-dock's test_glue.cpp does (header + private-code-as-header,
// #included once by this single C++ TU). Lets in-process real clients map
// xdg_toplevels so we drive the window-field map/unmap/focus PIPELINE end to end
// on the wlr headless backend.
#include "xdg-shell-client-protocol.h"
#include "xdg-shell-client-protocol-code.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <unistd.h>

// ext-window-field GLUE test — lenient, headless (AGENTS.md: strict cores,
// lenient shell). This unit has NO pure decision core (layout is RCSS, not C++),
// so the whole verification is this integration pipeline:
//   install ext-xdg-shell + ext-window-field, activate (fetch the Service — the
//   only fatal path), map a REAL toplevel, and assert (via the private probe)
//   that it became a TRACKED window (count 1, hidden out of wlr_scene), mapping
//   a SECOND grows the list to 2, unmapping removes it, and on_toplevel_focused
//   flips the tracked focused index.
//
// On the headless pixman backend the ui substrate has no GL path, so the field
// UiSurface + the SurfaceElements are null — exactly the degrade the brief calls
// for: the MODEL/list + Toplevel::hide() still work; there is no live texture to
// assert (live_uri is empty, has_surface_element() false). A separate OBSERVER
// extension (mirroring ext-xdg-shell/tests/test_minimize.cpp) captures the mapped
// Toplevel borrows so the test can drive on_toplevel_focused via the Service.

namespace {

using unbox::ext_window_field::ExtensionWithProbe;
using unbox::ext_window_field::TestProbe;
using unbox::ext_xdg_shell::Service;
using unbox::ext_xdg_shell::Toplevel;
using unbox::ext_xdg_shell::ToplevelEvent;

// ---- observer extension: captures the live Toplevel borrows (test-only) -----
//
// Depends on xdg-shell (so its Service is registered first). Holds the Service
// (to call Toplevel::focus(), which produces on_toplevel_focused) and the most
// recently mapped Toplevel*, plus the FIRST mapped one — both borrows valid
// mapped..unmapped, so the test can re-focus the first window after the second
// maps and assert the field's focused index follows.
class Observer final : public unbox::kernel::Extension {
public:
    [[nodiscard]] auto manifest() const -> const unbox::kernel::Manifest& override {
        return manifest_;
    }

    void activate(unbox::kernel::Host& host) override {
        svc_ = host.service<Service>();
        REQUIRE(svc_ != nullptr);
        sub_mapped_ = host.subscribe(
            svc_->on_toplevel_mapped(), [this](const ToplevelEvent& e) {
                last_mapped_ = e.toplevel;
                if (first_mapped_ == nullptr) {
                    first_mapped_ = e.toplevel;
                }
            });
        sub_unmapped_ = host.subscribe(
            svc_->on_toplevel_unmapped(), [this](const ToplevelEvent& e) {
                if (e.toplevel == last_mapped_) {
                    last_mapped_ = nullptr;
                }
                if (e.toplevel == first_mapped_) {
                    first_mapped_ = nullptr;
                }
            });
    }

    [[nodiscard]] auto service() -> Service* { return svc_; }
    [[nodiscard]] auto last_mapped() -> Toplevel* { return last_mapped_; }
    [[nodiscard]] auto first_mapped() -> Toplevel* { return first_mapped_; }

private:
    unbox::kernel::Manifest manifest_{"test-observer", unbox::kernel::Tier::standard,
                                      {"xdg-shell"}};
    Service* svc_ = nullptr;
    Toplevel* last_mapped_ = nullptr;
    Toplevel* first_mapped_ = nullptr;
    unbox::kernel::Subscription sub_mapped_;
    unbox::kernel::Subscription sub_unmapped_;
};

// ---- minimal xdg-shell client (mirrors ext-stage-dock/tests/test_glue.cpp) ---

struct Client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
};

// One mapped toplevel's client objects (the field tracks multiple windows, so we
// map several from the same connection).
struct Window {
    wl_surface* surface = nullptr;
    xdg_surface* xsurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_buffer* buffer = nullptr;
    bool configured = false;
};

void wm_base_ping(void*, xdg_wm_base* wm, std::uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
const xdg_wm_base_listener kWmBaseListener{wm_base_ping};

void registry_global(void* data, wl_registry* reg, std::uint32_t name,
                     const char* iface, std::uint32_t version) {
    auto* c = static_cast<Client*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        c->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        c->shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        c->wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(
            reg, name, &xdg_wm_base_interface, version < 3 ? version : 3));
        xdg_wm_base_add_listener(c->wm_base, &kWmBaseListener, c);
    }
}
void registry_global_remove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistryListener{registry_global, registry_global_remove};

void xsurface_configure(void* data, xdg_surface* xs, std::uint32_t serial) {
    auto* w = static_cast<Window*>(data);
    xdg_surface_ack_configure(xs, serial);
    w->configured = true;
}
const xdg_surface_listener kXSurfaceListener{xsurface_configure};

void toplevel_configure(void*, xdg_toplevel*, std::int32_t, std::int32_t, wl_array*) {}
void toplevel_close(void*, xdg_toplevel*) {}
const xdg_toplevel_listener kToplevelListener{toplevel_configure, toplevel_close, {}, {}};

// Cooperative single-thread pump (the standard libwayland guarded-read dance).
void pump(unbox::kernel::Server& server, wl_display* client) {
    wl_display_flush(client);
    server.dispatch(5);
    while (wl_display_prepare_read(client) != 0) {
        wl_display_dispatch_pending(client);
    }
    wl_display_flush(client);
    pollfd pfd{wl_display_get_fd(client), POLLIN, 0};
    if (poll(&pfd, 1, 5) > 0 && (pfd.revents & POLLIN)) {
        wl_display_read_events(client);
    } else {
        wl_display_cancel_read(client);
    }
    wl_display_dispatch_pending(client);
}

auto make_headless_server() -> std::unique_ptr<unbox::kernel::Server> {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    return unbox::kernel::Server::create({});
}

// A w*h ARGB shm buffer so the surface actually maps (a toplevel maps on its
// first buffer-bearing commit after the initial configure).
auto make_buffer(Client& c, int w, int h) -> wl_buffer* {
    const int stride = w * 4;
    const int size = stride * h;
    char name[] = "/tmp/unbox-window-field-test-XXXXXX";
    int fd = mkstemp(name);
    REQUIRE(fd >= 0);
    unlink(name);
    REQUIRE(ftruncate(fd, size) == 0);
    REQUIRE(c.shm != nullptr);
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, size);
    wl_buffer* buffer =
        wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

// Map one toplevel from connection `c` and pump until it is mapped server-side.
// `gate` is polled until non-null (the observer's most-recently-mapped borrow),
// so the caller can confirm the new window mapped.
void map_window(unbox::kernel::Server& server, Client& c, Window& w, const char* title) {
    w.surface = wl_compositor_create_surface(c.compositor);
    w.xsurface = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
    xdg_surface_add_listener(w.xsurface, &kXSurfaceListener, &w);
    w.toplevel = xdg_surface_get_toplevel(w.xsurface);
    xdg_toplevel_add_listener(w.toplevel, &kToplevelListener, &w);
    xdg_toplevel_set_title(w.toplevel, title);
    wl_surface_commit(w.surface); // mandatory empty initial commit

    for (int i = 0; i < 200 && !w.configured; ++i) {
        pump(server, c.display);
    }
    REQUIRE(w.configured);

    w.buffer = make_buffer(c, 256, 256);
    REQUIRE(w.buffer != nullptr);
    wl_surface_attach(w.surface, w.buffer, 0, 0);
    wl_surface_damage(w.surface, 0, 0, 256, 256);
    wl_surface_commit(w.surface);
}

void destroy_window(Window& w) {
    if (w.toplevel != nullptr) {
        xdg_toplevel_destroy(w.toplevel);
    }
    if (w.xsurface != nullptr) {
        xdg_surface_destroy(w.xsurface);
    }
    if (w.surface != nullptr) {
        wl_surface_destroy(w.surface);
    }
    if (w.buffer != nullptr) {
        wl_buffer_destroy(w.buffer);
    }
    w = Window{};
}

} // namespace

TEST_CASE("ext-window-field installs and activates atop ext-xdg-shell") {
    auto server = make_headless_server();
    server->install(unbox::ext_xdg_shell::create());
    // Topological activation runs xdg-shell first (window-field depends_on it),
    // so activate() finds the Service. A missing-Service throw would propagate.
    ExtensionWithProbe ewp = unbox::ext_window_field::make_extension_with_probe();
    TestProbe* probe = ewp.probe;
    server->install(std::move(ewp.extension));
    server->activate_extensions();
    CHECK(!server->socket_name().empty());
    CHECK(probe->activated());
    CHECK(probe->window_count() == 0);
    CHECK(probe->focused_index() == -1);
    CHECK(probe->hidden_count() == 0);
}

TEST_CASE("ext-window-field: map tracks a window + hides it; a 2nd grows the list; "
          "focus flips the flag; unmap removes it") {
    auto server = make_headless_server();
    server->install(unbox::ext_xdg_shell::create());

    auto observer_owned = std::make_unique<Observer>();
    Observer* observer = observer_owned.get();
    server->install(std::move(observer_owned));

    ExtensionWithProbe ewp = unbox::ext_window_field::make_extension_with_probe();
    TestProbe* probe = ewp.probe;
    server->install(std::move(ewp.extension));

    server->activate_extensions();
    REQUIRE(!server->socket_name().empty());
    REQUIRE(probe->activated());

    Client c;
    c.display = wl_display_connect(server->socket_name().c_str());
    REQUIRE(c.display != nullptr);
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &kRegistryListener, &c);

    for (int i = 0; i < 50 &&
                    (c.compositor == nullptr || c.wm_base == nullptr || c.shm == nullptr);
         ++i) {
        pump(*server, c.display);
    }
    REQUIRE(c.compositor != nullptr);
    REQUIRE(c.wm_base != nullptr);
    REQUIRE(c.shm != nullptr);

    // --- map the FIRST toplevel -> becomes a tracked window, hidden from scene
    Window w1;
    map_window(*server, c, w1, "unbox-field-1");
    for (int i = 0; i < 200 && observer->last_mapped() == nullptr; ++i) {
        pump(*server, c.display);
    }
    Toplevel* tl1 = observer->last_mapped();
    REQUIRE(tl1 != nullptr);
    CHECK(probe->window_count() == 1);
    // hide() took it OUT of wlr_scene (the surface element is now the compositor).
    CHECK(probe->hidden_count() == 1);
    // ext-xdg-shell focuses a freshly-mapped window, so the field tracks it as
    // focused (map-focus; on_toplevel_focused may also have fired — same value).
    CHECK(probe->focused_index() == 0);
    // No GL path on headless pixman: the surface element is null, so live_uri is
    // empty + has_surface_element() is false (lenient-glue degrade per the brief).
    CHECK(probe->has_surface_element(0) == false);
    CHECK(probe->live_uri(0).empty());

    // --- map a SECOND toplevel -> the list grows to 2, the 2nd is now focused
    Window w2;
    map_window(*server, c, w2, "unbox-field-2");
    for (int i = 0; i < 200 && observer->last_mapped() == tl1; ++i) {
        pump(*server, c.display);
    }
    Toplevel* tl2 = observer->last_mapped();
    REQUIRE(tl2 != nullptr);
    CHECK(tl2 != tl1);
    CHECK(probe->window_count() == 2);
    CHECK(probe->hidden_count() == 2);
    CHECK(probe->focused_index() == 1); // map-focus moved to the 2nd window

    // --- on_toplevel_focused flips the focused flag: re-focus the FIRST window
    // via Toplevel::focus() (exactly what ext-keybindings' Alt+Tab does). The
    // field's focused index must follow back to window 0.
    REQUIRE(observer->first_mapped() == tl1);
    tl1->focus();
    for (int i = 0; i < 50 && probe->focused_index() != 0; ++i) {
        pump(*server, c.display);
    }
    CHECK(probe->focused_index() == 0);
    CHECK(probe->window_count() == 2); // focus changes membership not at all

    // --- unmap the SECOND window -> the list shrinks back to 1
    destroy_window(w2);
    for (int i = 0; i < 200 && probe->window_count() != 1; ++i) {
        pump(*server, c.display);
    }
    CHECK(probe->window_count() == 1);
    // The first window is still tracked + still the focused one.
    CHECK(probe->focused_index() == 0);

    // --- unmap the FIRST too -> empty + no focused window
    destroy_window(w1);
    for (int i = 0; i < 200 && probe->window_count() != 0; ++i) {
        pump(*server, c.display);
    }
    CHECK(probe->window_count() == 0);
    CHECK(probe->focused_index() == -1);

    // Teardown: dropping the server destroys extensions in reverse activation
    // order; within the field, Subscriptions release first, then the field
    // UiSurface (null here), then the windows_ vector (empty now) — all before
    // the Host borrow ends (asan watches this).
    if (c.wm_base != nullptr) {
        xdg_wm_base_destroy(c.wm_base);
    }
    if (c.shm != nullptr) {
        wl_shm_destroy(c.shm);
    }
    if (c.compositor != nullptr) {
        wl_compositor_destroy(c.compositor);
    }
    if (c.registry != nullptr) {
        wl_registry_destroy(c.registry);
    }
    wl_display_flush(c.display);
    pump(*server, c.display);
    wl_display_disconnect(c.display);
}
