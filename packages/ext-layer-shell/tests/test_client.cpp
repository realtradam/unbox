#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unbox/ext-layer-shell/ext_layer_shell.hpp>
#include <unbox/kernel/server.hpp>

#include <wayland-client.h>

// The generated client header (and code) name a request argument `namespace` —
// a C identifier but a C++ keyword. Same scoped-macro fix wlr.hpp documents for
// the server side: rename it across exactly these two generated includes. The
// only call that touches it (get_layer_surface's last arg) is a string literal
// we pass positionally, so the rename never leaks into our own code.
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
// xdg-shell marshalling code: supplies xdg_popup_interface, referenced by the
// layer-shell tables (get_popup). Code-form only; we use no xdg-shell requests.
#include "xdg-shell-client-protocol-code.h"
// The protocol marshalling tables/stubs (wayland-scanner private-code), emitted
// as a header so it can be compiled by the C++ TU exactly once (see meson.build).
#include "wlr-layer-shell-unstable-v1-client-protocol-code.h"
#undef namespace

#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>

// REGRESSION: a real wayland CLIENT (the fuzzel reproduction, in-process) binds
// zwlr_layer_shell_v1, creates a layer surface with a NIL output, and MUST
// receive a configure. Before the fix, ext-layer-shell's output tracking was
// events-only: the headless output created during Server::create() (BEFORE
// activation) was never tracked, so an output-less surface got no output, no
// wlr_scene_layer_surface_v1_configure, no configure event — the exact symptom
// in /tmp/opencode/fuzzel-trace.log (get_layer_surface(... nil ...) then a bare
// closed() with no configure). This test would have caught it.
//
// Server and client share this single thread; we pump both loops cooperatively
// (the standard libwayland prepare_read / read_events dance, non-blocking).

namespace {

struct Client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_output* output = nullptr;
    zwlr_layer_shell_v1* layer_shell = nullptr;

    wl_surface* surface = nullptr;
    zwlr_layer_surface_v1* layer_surface = nullptr;

    bool configured = false;
    bool closed = false;
    std::uint32_t configure_serial = 0;
};

void registry_global(void* data, wl_registry* reg, std::uint32_t name,
                     const char* iface, std::uint32_t version) {
    auto* c = static_cast<Client*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        c->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, wl_output_interface.name) == 0 &&
               c->output == nullptr) {
        c->output = static_cast<wl_output*>(
            wl_registry_bind(reg, name, &wl_output_interface, version));
    } else if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        c->layer_shell = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(
            reg, name, &zwlr_layer_shell_v1_interface,
            version < 4 ? version : 4));
    }
}
void registry_global_remove(void*, wl_registry*, std::uint32_t) {}

const wl_registry_listener kRegistryListener{registry_global,
                                             registry_global_remove};

void ls_configure(void* data, zwlr_layer_surface_v1* ls, std::uint32_t serial,
                  std::uint32_t, std::uint32_t) {
    auto* c = static_cast<Client*>(data);
    c->configured = true;
    c->configure_serial = serial;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
}
void ls_closed(void* data, zwlr_layer_surface_v1*) {
    static_cast<Client*>(data)->closed = true;
}
const zwlr_layer_surface_v1_listener kLayerSurfaceListener{ls_configure,
                                                           ls_closed};

// Pump server and client once, without blocking the client read.
void pump(unbox::kernel::Server& server, wl_display* client) {
    wl_display_flush(client);
    server.dispatch(5);

    // Drain anything already queued, then do a guarded non-blocking read.
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
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1); // output exists BEFORE activation
    return unbox::kernel::Server::create({});
}

} // namespace

TEST_CASE("a real client's nil-output layer surface receives a configure") {
    auto server = make_headless_server();
    server->install(unbox::ext_layer_shell::create());
    server->activate_extensions();
    REQUIRE(!server->socket_name().empty());

    Client c;
    c.display = wl_display_connect(server->socket_name().c_str());
    REQUIRE(c.display != nullptr);
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &kRegistryListener, &c);

    // Round 1: let the registry advertise globals and our binds reach the
    // server (compositor, output, layer_shell).
    for (int i = 0; i < 50 && (c.compositor == nullptr || c.layer_shell == nullptr);
         ++i) {
        pump(*server, c.display);
    }
    REQUIRE(c.compositor != nullptr);
    REQUIRE(c.layer_shell != nullptr);

    // Create a layer surface with a NIL output (exactly fuzzel's get_layer_surface
    // call: output=nil, layer=overlay, namespace="launcher").
    c.surface = wl_compositor_create_surface(c.compositor);
    c.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        c.layer_shell, c.surface, /*output=*/nullptr,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "launcher");
    REQUIRE(c.layer_surface != nullptr);
    zwlr_layer_surface_v1_add_listener(c.layer_surface, &kLayerSurfaceListener, &c);
    zwlr_layer_surface_v1_set_size(c.layer_surface, 382, 386);
    zwlr_layer_surface_v1_set_anchor(c.layer_surface, 0);
    // The mandatory initial empty commit (no buffer): the compositor MUST reply
    // with a configure.
    wl_surface_commit(c.surface);

    // Round 2: pump until the configure arrives (or we give up -> the bug).
    for (int i = 0; i < 200 && !c.configured && !c.closed; ++i) {
        pump(*server, c.display);
    }

    CHECK(c.configured);          // the fix: a configure was sent
    CHECK_FALSE(c.closed);        // the bug: surface closed with no configure
    CHECK(c.configure_serial != 0);

    if (c.layer_surface != nullptr) {
        zwlr_layer_surface_v1_destroy(c.layer_surface);
    }
    if (c.surface != nullptr) {
        wl_surface_destroy(c.surface);
    }
    wl_display_flush(c.display);
    pump(*server, c.display);
    wl_display_disconnect(c.display);
}
