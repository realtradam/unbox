#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "probe.hpp" // PRIVATE: a unit may read its own src/ (drives the c2 pipeline)

#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/server.hpp>
#include <unbox/kernel/wlr.hpp>

#include <wayland-client.h>

// CLIENT-side xdg-shell bindings, generated from the canonical xdg-shell.xml the
// same way ext-xdg-shell's test_minimize.cpp does (header + private-code-as-
// header, #included once by this single C++ TU). Lets an in-process real client
// map an xdg_toplevel so we drive the c2 minimize/restore PIPELINE end to end on
// the wlr headless backend.
#include "xdg-shell-client-protocol.h"
#include "xdg-shell-client-protocol-code.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <unistd.h>

// ext-stage-dock GLUE test — lenient, headless (AGENTS.md: strict cores, lenient
// shell). The decision cores (reveal recognizer + dock layout) are proven hard
// in test_policy.cpp; here we verify the c2 STATIC INTEGRATION pipeline:
//   install ext-xdg-shell + ext-stage-dock, activate (fetch the Service — the
//   only fatal path), map a real toplevel, drive minimize via the private probe
//   and assert the window's scene node is DISABLED + a slot exists, then drive
//   restore and assert the node is ENABLED again + the slot is gone.
// On the headless pixman backend the ui substrate has no GL path, so the dock
// UiSurface + Previews are null — exactly the degrade the brief calls for: the
// MODEL + hide()/show() still work; there is no visual to assert. A separate
// OBSERVER extension (mirroring ext-xdg-shell/tests/test_minimize.cpp) captures
// the mapped Toplevel borrow so the test can read its scene node's enable-bit —
// the exact bit Toplevel::hide()/show() flips.

namespace {

using unbox::ext_stage_dock::ExtensionWithProbe;
using unbox::ext_stage_dock::TestProbe;
using unbox::ext_xdg_shell::Service;
using unbox::ext_xdg_shell::Toplevel;
using unbox::ext_xdg_shell::ToplevelEvent;

// ---- observer extension: captures the live Toplevel borrow (test-only) ------
//
// Depends on xdg-shell (so its Service is registered first). Holds the mapped
// Toplevel* — a borrow valid mapped..unmapped, which is exactly the window we
// touch. Lets the test read scene_tree()->node.enabled to confirm hide()/show().
class Observer final : public unbox::kernel::Extension {
public:
    [[nodiscard]] auto manifest() const -> const unbox::kernel::Manifest& override {
        return manifest_;
    }

    void activate(unbox::kernel::Host& host) override {
        Service* svc = host.service<Service>();
        REQUIRE(svc != nullptr);
        sub_mapped_ = host.subscribe(
            svc->on_toplevel_mapped(),
            [this](const ToplevelEvent& e) { mapped_ = e.toplevel; });
        sub_unmapped_ = host.subscribe(
            svc->on_toplevel_unmapped(), [this](const ToplevelEvent& e) {
                if (e.toplevel == mapped_) {
                    mapped_ = nullptr;
                }
            });
    }

    [[nodiscard]] auto mapped() -> Toplevel* { return mapped_; }

private:
    unbox::kernel::Manifest manifest_{"test-observer", unbox::kernel::Tier::standard,
                                      {"xdg-shell"}};
    Toplevel* mapped_ = nullptr;
    unbox::kernel::Subscription sub_mapped_;
    unbox::kernel::Subscription sub_unmapped_;
};

// ---- minimal xdg-shell client (mirrors ext-xdg-shell/tests/test_minimize.cpp)

struct Client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;

    wl_surface* surface = nullptr;
    xdg_surface* xsurface = nullptr;
    xdg_toplevel* toplevel = nullptr;

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
    auto* c = static_cast<Client*>(data);
    xdg_surface_ack_configure(xs, serial);
    c->configured = true;
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

// A 256x256 ARGB shm buffer so the surface actually maps (a toplevel maps on its
// first buffer-bearing commit after the initial configure).
auto make_buffer(Client& c, int w, int h) -> wl_buffer* {
    const int stride = w * 4;
    const int size = stride * h;
    char name[] = "/tmp/unbox-stage-dock-test-XXXXXX";
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

} // namespace

TEST_CASE("ext-stage-dock installs and activates atop ext-xdg-shell") {
    auto server = make_headless_server();
    server->install(unbox::ext_xdg_shell::create());
    // Topological activation runs xdg-shell first (stage-dock depends_on it), so
    // activate() finds the Service. A missing-Service throw would propagate out.
    ExtensionWithProbe ewp = unbox::ext_stage_dock::make_extension_with_probe();
    TestProbe* probe = ewp.probe;
    server->install(std::move(ewp.extension));
    server->activate_extensions();
    CHECK(!server->socket_name().empty());
    CHECK(probe->activated());
    CHECK(probe->slot_count() == 0);
    // minimize_focused with nothing focused is a guarded no-op.
    probe->minimize_focused();
    CHECK(probe->slot_count() == 0);
}

TEST_CASE("ext-stage-dock c2: minimize hides the focused window + adds a slot; restore shows it + drops the slot") {
    auto server = make_headless_server();
    server->install(unbox::ext_xdg_shell::create());

    auto observer_owned = std::make_unique<Observer>();
    Observer* observer = observer_owned.get();
    server->install(std::move(observer_owned));

    ExtensionWithProbe ewp = unbox::ext_stage_dock::make_extension_with_probe();
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

    // Map a toplevel: surface -> xdg_surface -> xdg_toplevel, the mandatory empty
    // initial commit, wait for the configure, then attach a buffer + commit to map.
    c.surface = wl_compositor_create_surface(c.compositor);
    c.xsurface = xdg_wm_base_get_xdg_surface(c.wm_base, c.surface);
    xdg_surface_add_listener(c.xsurface, &kXSurfaceListener, &c);
    c.toplevel = xdg_surface_get_toplevel(c.xsurface);
    xdg_toplevel_add_listener(c.toplevel, &kToplevelListener, &c);
    xdg_toplevel_set_title(c.toplevel, "unbox-dock-test");
    wl_surface_commit(c.surface);

    for (int i = 0; i < 200 && !c.configured; ++i) {
        pump(*server, c.display);
    }
    REQUIRE(c.configured);

    wl_buffer* buffer = make_buffer(c, 256, 256);
    REQUIRE(buffer != nullptr);
    wl_surface_attach(c.surface, buffer, 0, 0);
    wl_surface_damage(c.surface, 0, 0, 256, 256);
    wl_surface_commit(c.surface);

    // Pump until the server-side map fires: ext-xdg-shell focuses the freshly
    // mapped toplevel, the dock's on_toplevel_mapped records it as focused_, and
    // the observer captures the live borrow.
    for (int i = 0; i < 200 && observer->mapped() == nullptr; ++i) {
        pump(*server, c.display);
    }
    Toplevel* tl = observer->mapped();
    REQUIRE(tl != nullptr);
    wlr_scene_tree* tree = tl->scene_tree();
    REQUIRE(tree != nullptr);
    CHECK(tree->node.enabled == true); // mapped + visible before minimize
    REQUIRE(probe->slot_count() == 0);

    // --- minimize the focused window (the Super+M path, driven via the probe) --
    probe->minimize_focused();
    CHECK(probe->slot_count() == 1);          // a slot was added (model works w/o GL)
    CHECK(tree->node.enabled == false);       // hide() disabled the scene node
    CHECK(observer->mapped() == tl);          // NOT unmapped — minimize != unmap

    // Pump a few turns: a hide must not unmap the client (no on_toplevel_unmapped).
    for (int i = 0; i < 10; ++i) {
        pump(*server, c.display);
    }
    CHECK(probe->slot_count() == 1);
    CHECK(observer->mapped() == tl);

    // --- restore slot 0 ---
    probe->restore(0);
    CHECK(probe->slot_count() == 0);          // slot dropped (Preview, if any, freed)
    CHECK(tree->node.enabled == true);        // show() re-enabled the scene node
    // INVARIANT: restore must re-establish focused_ on the shown window, else the
    // next minimize (guarded on focused_ != nullptr) is a no-op (the real-seat
    // bug). With ONE window, minimize had set focused_=nullptr, so this is the
    // re-establishment the restore path is responsible for.
    CHECK(probe->has_focused() == true);

    // A guarded out-of-range restore is a no-op.
    probe->restore(5);
    CHECK(probe->slot_count() == 0);

    for (int i = 0; i < 10; ++i) {
        pump(*server, c.display);
    }

    // --- RE-MINIMIZE the same window after the dock fully emptied (1->0->1) ----
    // Real-seat regression: with ONE window, minimize sets focused_=nullptr (no
    // OTHER window to focus), and restore must RE-ESTABLISH focused_ to the shown
    // window. hide() never moves seat keyboard focus, so the restoring focus() is
    // a no-op at the seat and on_toplevel_focused does NOT re-fire — so the glue
    // must set focused_ itself on restore. If it does not, this second
    // minimize_focused() (guarded on focused_ != nullptr) is a no-op and the slot
    // count stays 0. The fix makes the SAME window minimizable again with NO new
    // window mapped. This is the SAME tl (never unmapped); its borrow is still
    // live (observer still holds it).
    CHECK(observer->mapped() == tl);          // still the only mapped window
    CHECK(tree->node.enabled == true);        // restored + visible before re-minimize
    CHECK(probe->has_focused() == true);      // dock still tracks a focused window
    probe->minimize_focused();
    CHECK(probe->slot_count() == 1);          // re-minimized into the dock (the bug fix)
    CHECK(tree->node.enabled == false);       // hide() disabled it again
    CHECK(observer->mapped() == tl);          // still NOT unmapped

    for (int i = 0; i < 10; ++i) {
        pump(*server, c.display);
    }
    // Restore once more so we tear down from a clean (empty, shown) state.
    probe->restore(0);
    CHECK(probe->slot_count() == 0);
    CHECK(tree->node.enabled == true);

    for (int i = 0; i < 10; ++i) {
        pump(*server, c.display);
    }

    // Teardown: destroy the client toplevel; on_toplevel_unmapped fires and the
    // dock forgets the window. Dropping the server destroys extensions in reverse
    // activation order; within the dock, Subscriptions release first, then the
    // dock UiSurface (null here), then the slots' Previews — all before the Host
    // borrow ends (asan watches this).
    wl_buffer_destroy(buffer);
    xdg_toplevel_destroy(c.toplevel);
    xdg_surface_destroy(c.xsurface);
    wl_surface_destroy(c.surface);
    c.toplevel = nullptr;
    c.xsurface = nullptr;
    c.surface = nullptr;
    for (int i = 0; i < 50; ++i) {
        pump(*server, c.display);
    }

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
