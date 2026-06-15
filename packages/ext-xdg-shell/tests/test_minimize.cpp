#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/server.hpp>
#include <unbox/kernel/wlr.hpp>

#include <wayland-client.h>

// xdg-shell CLIENT bindings, generated from the canonical xdg-shell.xml the
// same way ext-layer-shell generates its protocol code (a single C++ TU
// #includes the header + the private-code-as-header exactly once). This lets an
// in-process real client map an xdg_toplevel so we can drive the slice-10 b1
// minimize mechanism (hide/show/geometry/scene_tree) end-to-end on the wlr
// headless backend.
#include "xdg-shell-client-protocol.h"
#include "xdg-shell-client-protocol-code.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <unistd.h>

// slice 10 / b1 — Toplevel minimize mechanism, integration-tested with a real
// client. We install ext-xdg-shell plus a tiny OBSERVER extension that depends
// on it, fetches its Service, and captures the live Toplevel* borrow + Host on
// on_toplevel_mapped (and notes any on_toplevel_unmapped). A real client maps a
// toplevel; we then exercise hide()/show()/geometry()/scene_tree() against the
// borrow and assert the scene node enable-bit flips, no unmap fired, geometry
// is non-empty, and scene_tree() resolves to the same tree as
// Host::scene_tree_for(the focused surface). Lenient ext-tier glue per AGENTS.md
// (the strict correctness lives in the pure-core test_policy.cpp).

namespace {

using unbox::ext_xdg_shell::Service;
using unbox::ext_xdg_shell::Toplevel;
using unbox::ext_xdg_shell::ToplevelEvent;

// ---- the observer extension (test-only, depends on xdg-shell) --------------
//
// Holds onto the kernel Host so the test can resolve scene_tree_for() the way a
// real consumer (ext-stage-dock) would. Captures the mapped Toplevel borrow;
// the borrow is valid until on_toplevel_unmapped, which is exactly when we stop
// touching it. No ownership of anything kernel-owned.
class Observer final : public unbox::kernel::Extension {
public:
    [[nodiscard]] auto manifest() const -> const unbox::kernel::Manifest& override {
        return manifest_;
    }

    void activate(unbox::kernel::Host& host) override {
        host_ = &host;
        // Typed cross-extension coupling: a missing xdg-shell is a nullptr here
        // (and a link error on the Service type), never a string lookup.
        Service* svc = host.service<Service>();
        REQUIRE(svc != nullptr);
        sub_mapped_ = host.subscribe(svc->on_toplevel_mapped(),
                                     [this](const ToplevelEvent& e) {
                                         mapped_ = e.toplevel;
                                         ++mapped_count_;
                                     });
        sub_unmapped_ = host.subscribe(svc->on_toplevel_unmapped(),
                                       [this](const ToplevelEvent& e) {
                                           if (e.toplevel == mapped_) {
                                               mapped_ = nullptr;
                                           }
                                           ++unmapped_count_;
                                       });
    }

    [[nodiscard]] auto host() -> unbox::kernel::Host* { return host_; }
    [[nodiscard]] auto mapped() -> Toplevel* { return mapped_; }
    [[nodiscard]] auto mapped_count() const -> int { return mapped_count_; }
    [[nodiscard]] auto unmapped_count() const -> int { return unmapped_count_; }

private:
    unbox::kernel::Manifest manifest_{"test-observer", unbox::kernel::Tier::standard,
                                      {"xdg-shell"}};
    unbox::kernel::Host* host_ = nullptr;
    Toplevel* mapped_ = nullptr;
    int mapped_count_ = 0;
    int unmapped_count_ = 0;
    unbox::kernel::Subscription sub_mapped_;
    unbox::kernel::Subscription sub_unmapped_;
};

// ---- minimal xdg-shell client ----------------------------------------------

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
    std::uint32_t last_configure_serial = 0;
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
    c->last_configure_serial = serial;
    xdg_surface_ack_configure(xs, serial);
    c->configured = true;
}
const xdg_surface_listener kXSurfaceListener{xsurface_configure};

void toplevel_configure(void*, xdg_toplevel*, std::int32_t, std::int32_t, wl_array*) {}
void toplevel_close(void*, xdg_toplevel*) {}
// Trailing members (configure_bounds, wm_capabilities — xdg-shell v4/v5) are
// value-initialized to nullptr; we bind at most v3 so they are never invoked.
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

// A 256x256 ARGB shm buffer so the surface actually maps (a toplevel maps on
// its first buffer-bearing commit after the initial configure).
auto make_buffer(Client& c, int w, int h) -> wl_buffer* {
    const int stride = w * 4;
    const int size = stride * h;
    char name[] = "/tmp/unbox-xdg-test-XXXXXX";
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

TEST_CASE("slice 10/b1: hide/show/geometry/scene_tree on a real mapped toplevel") {
    auto server = make_headless_server();
    server->install(unbox::ext_xdg_shell::create());
    auto observer_owned = std::make_unique<Observer>();
    Observer* observer = observer_owned.get();
    server->install(std::move(observer_owned));
    server->activate_extensions();
    REQUIRE(!server->socket_name().empty());

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

    // Map a toplevel: create surface -> xdg_surface -> xdg_toplevel, the
    // mandatory empty initial commit, wait for the configure, then attach a
    // buffer and commit to actually map it.
    c.surface = wl_compositor_create_surface(c.compositor);
    c.xsurface = xdg_wm_base_get_xdg_surface(c.wm_base, c.surface);
    xdg_surface_add_listener(c.xsurface, &kXSurfaceListener, &c);
    c.toplevel = xdg_surface_get_toplevel(c.xsurface);
    xdg_toplevel_add_listener(c.toplevel, &kToplevelListener, &c);
    xdg_toplevel_set_title(c.toplevel, "unbox-test");
    wl_surface_commit(c.surface); // initial empty commit -> expect configure

    for (int i = 0; i < 200 && !c.configured; ++i) {
        pump(*server, c.display);
    }
    REQUIRE(c.configured);

    wl_buffer* buffer = make_buffer(c, 256, 256);
    REQUIRE(buffer != nullptr);
    wl_surface_attach(c.surface, buffer, 0, 0);
    wl_surface_damage(c.surface, 0, 0, 256, 256);
    wl_surface_commit(c.surface);

    // Pump until the server-side map fires and the observer captures the borrow.
    for (int i = 0; i < 200 && observer->mapped() == nullptr; ++i) {
        pump(*server, c.display);
    }

    Toplevel* tl = observer->mapped();
    REQUIRE(tl != nullptr);
    REQUIRE(observer->mapped_count() == 1);
    REQUIRE(observer->unmapped_count() == 0);

    // scene_tree(): non-null, a borrow, and equal to what the kernel registry
    // resolves the toplevel's surface to (the typed surface->tree contract the
    // dock relies on). We independently recover the toplevel's root surface by
    // walking the buffer nodes under the returned tree (the kernel registry keys
    // on it), then confirm the round-trip scene_tree_for(surface) ==
    // scene_tree().
    wlr_scene_tree* tree = tl->scene_tree();
    REQUIRE(tree != nullptr);
    struct SurfaceCatch {
        wlr_surface* surface = nullptr;
    } caught;
    wlr_scene_node_for_each_buffer(
        &tree->node,
        [](wlr_scene_buffer* buf, int, int, void* data) {
            auto* sc = static_cast<SurfaceCatch*>(data);
            if (sc->surface == nullptr) {
                if (wlr_scene_surface* ss = wlr_scene_surface_try_from_buffer(buf)) {
                    sc->surface = ss->surface;
                }
            }
        },
        &caught);
    REQUIRE(caught.surface != nullptr);
    CHECK(observer->host()->scene_tree_for(caught.surface) == tree);

    // wl_surface() (RML compositing Wave 2): the contract now exposes the
    // toplevel's ROOT wl_surface — the handle ext-window-field passes to
    // UiSubstrate::create_surface_element(). For a mapped toplevel it is
    // non-null and IS the root surface: it must equal both the surface the scene
    // tree hosts (walked above) and the surface the kernel registry resolves
    // this very tree from. (Additive — scene_tree()/hide()/show()/geometry()
    // are unchanged; the existing wlr_scene compositing path is untouched.)
    wlr_surface* root = tl->wl_surface();
    REQUIRE(root != nullptr);
    CHECK(root == caught.surface);
    CHECK(observer->host()->scene_tree_for(root) == tree);

    // geometry(): a non-empty box for a mapped toplevel.
    wlr_box box = tl->geometry();
    CHECK(box.width > 0);
    CHECK(box.height > 0);

    // hide(): disables the scene node (no compositing, frame callbacks cease),
    // WITHOUT unmapping — no on_toplevel_unmapped, client stays mapped.
    CHECK(tree->node.enabled == true);
    tl->hide();
    CHECK(tree->node.enabled == false);
    tl->hide(); // idempotent
    CHECK(tree->node.enabled == false);
    // Pump a few turns: the client must NOT be unmapped by the hide.
    for (int i = 0; i < 10; ++i) {
        pump(*server, c.display);
    }
    CHECK(observer->unmapped_count() == 0);
    CHECK(observer->mapped() == tl); // still the live borrow

    // show(): re-enables the node. Idempotent.
    tl->show();
    CHECK(tree->node.enabled == true);
    tl->show();
    CHECK(tree->node.enabled == true);

    // A hidden toplevel must still unmap normally when the client withdraws it:
    // hide it again, then UNMAP from the client side by committing a NULL buffer
    // (the xdg_toplevel withdraw — the surface unmaps without the role object or
    // wl_surface resource being destroyed); on_toplevel_unmapped MUST fire.
    //
    // Teardown-order note (listener-lifetime): we deliberately unmap via a
    // null-buffer commit rather than tearing the wl_surface resource down
    // mid-session. Destroying the wl_surface while the server is still running
    // would run wlroots' surface_handle_resource_destroy, which asserts
    // wl_list_empty(&surface->events.commit.listener_list). Any commit listener
    // still bound to that surface at that instant aborts the whole test process.
    // Our own ToplevelEntry::commit listener IS released in time (it dies with
    // the entry on the xdg_toplevel destroy), but the kernel's headless
    // surface-capture test seam keeps a commit listener on every client surface
    // for the Server's lifetime and only detaches it during Server teardown
    // (before it destroys the clients) — so a client-driven surface-resource
    // destroy mid-test trips the assertion. We therefore let the surface (and
    // the rest of the client's objects) be reaped by the Server destructor's
    // ordered teardown (seam detached first, clients destroyed after), which is
    // the same ordering a real session uses. See reports/ext-xdg-shell.md and
    // the kernel change-request for the seam's missing per-surface unsubscribe.
    tl->hide();
    wl_surface_attach(c.surface, nullptr, 0, 0);
    wl_surface_commit(c.surface);
    for (int i = 0; i < 200 && observer->unmapped_count() == 0; ++i) {
        pump(*server, c.display);
    }
    CHECK(observer->unmapped_count() == 1);

    // Client shutdown. We destroy every client proxy (so libwayland-client frees
    // them — keeps the asan/lsan suite clean, mirroring the kernel's client
    // test) and then disconnect, but we do NOT pump the server afterwards. The
    // destroy requests sit unflushed-to-dispatch on the server until the Server
    // destructor reaps the whole connection via wl_display_destroy_clients —
    // which the kernel runs AFTER detaching its surface-capture commit seam, so
    // the wl_surface resource is destroyed with no commit listener still bound
    // (no surface_handle_resource_destroy assertion). Pumping here instead would
    // dispatch the surface-destroy while the server (and seam) is alive and trip
    // that wlroots assertion — the teardown-order bug this test now avoids.
    xdg_toplevel_destroy(c.toplevel);
    xdg_surface_destroy(c.xsurface);
    wl_surface_destroy(c.surface);
    c.toplevel = nullptr;
    c.xsurface = nullptr;
    c.surface = nullptr;
    wl_buffer_destroy(buffer);
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
    wl_display_disconnect(c.display);
}
