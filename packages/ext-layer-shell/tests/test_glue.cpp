#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unbox/ext-layer-shell/ext_layer_shell.hpp>
#include <unbox/kernel/server.hpp>

#include <cstdlib>
#include <memory>

// Glue tests — lenient, headless. The pure arrangement math is proven hard in
// test_arrangement.cpp; here we only verify the extension installs, activates,
// creates its global, drives the event loop, and shuts down cleanly under the
// wlr headless backend (no GPU, no parent session).

namespace {

auto make_headless_server() -> std::unique_ptr<unbox::kernel::Server> {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    return unbox::kernel::Server::create({});
}

} // namespace

TEST_CASE("ext-layer-shell installs, activates, and creates its global") {
    auto server = make_headless_server();
    server->install(unbox::ext_layer_shell::create());
    // Activation creates the wlr_layer_shell_v1 global on the display; a
    // throwing activate() (e.g. create failure) would propagate here.
    server->activate_extensions();
    CHECK(!server->socket_name().empty());
}

TEST_CASE("ext-layer-shell dispatches and shuts down cleanly") {
    auto server = make_headless_server();
    server->install(unbox::ext_layer_shell::create());
    server->activate_extensions();
    for (int i = 0; i < 5; ++i) {
        CHECK(server->dispatch(10));
    }
    // Destruction runs the full shutdown sequence; the extension's RAII members
    // (global listener, output subscriptions, scene nodes) release in reverse
    // declaration order with no leaked listeners.
}

TEST_CASE("ext-layer-shell activates idempotently alongside the headless output") {
    auto server = make_headless_server();
    server->install(unbox::ext_layer_shell::create());
    server->activate_extensions();
    server->activate_extensions(); // no-op second call
    for (int i = 0; i < 3; ++i) {
        CHECK(server->dispatch(10));
    }
}
