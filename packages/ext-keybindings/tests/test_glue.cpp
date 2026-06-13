#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unbox/ext-keybindings/ext_keybindings.hpp>
#include <unbox/ext-stage-dock/ext_stage_dock.hpp>
#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/server.hpp>

#include <cstdlib>
#include <memory>

// Glue tests — lenient, headless. The decision cores (combo parser, toml loader,
// matcher + tap SM, focus ring) are proven hard in test_policy.cpp; here we only
// verify the extension installs alongside ext-xdg-shell (its dependency),
// activates (fetching the Service — the only fatal path), drives the event loop,
// and shuts down cleanly with all RAII subscriptions releasing in order.

namespace {

auto make_headless_server() -> std::unique_ptr<unbox::kernel::Server> {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    return unbox::kernel::Server::create({});
}

} // namespace

TEST_CASE("ext-keybindings installs and activates atop ext-xdg-shell + ext-stage-dock") {
    auto server = make_headless_server();
    server->install(unbox::ext_xdg_shell::create());
    server->install(unbox::ext_stage_dock::create());
    server->install(unbox::ext_keybindings::create());
    // Topological activation runs xdg-shell first, then stage-dock, then
    // keybindings (depends_on both). A missing-Service throw would propagate.
    server->activate_extensions();
    CHECK(!server->socket_name().empty());
}

TEST_CASE("ext-keybindings dispatches and shuts down cleanly") {
    auto server = make_headless_server();
    server->install(unbox::ext_xdg_shell::create());
    server->install(unbox::ext_stage_dock::create());
    server->install(unbox::ext_keybindings::create());
    server->activate_extensions();
    for (int i = 0; i < 5; ++i) {
        CHECK(server->dispatch(10));
    }
    // Destruction tears down the key_filter link + the xdg-shell event
    // subscriptions + the stage-dock Service in reverse declaration order.
}

TEST_CASE("ext-keybindings degrades to defaults for a bad explicit config path") {
    auto server = make_headless_server();
    server->install(unbox::ext_xdg_shell::create());
    server->install(unbox::ext_stage_dock::create());
    // A non-existent --config path must NOT throw out of activate(); the
    // extension logs and uses compiled defaults.
    server->install(unbox::ext_keybindings::create(
        std::string("/nonexistent/path/to/unbox.toml")));
    server->activate_extensions();
    for (int i = 0; i < 3; ++i) {
        CHECK(server->dispatch(10));
    }
}
