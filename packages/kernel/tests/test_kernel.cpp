#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unbox/kernel/kernel.hpp>
#include <unbox/kernel/server.hpp>

#include <cstdlib>

TEST_CASE("kernel compiles against and links wlroots + libwayland-server") {
    CHECK(unbox::kernel::link_probe());
    CHECK(unbox::kernel::wlroots_version().substr(0, 4) == "0.20");
}

TEST_CASE("vendored RMLUi subproject compiled and linked") {
    CHECK(!unbox::kernel::rmlui_version().empty());
}

TEST_CASE("server boots and shuts down on the headless backend") {
    // Headless backend + pixman renderer: no GPU, no parent session needed.
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    auto server = unbox::kernel::Server::create({});
    CHECK(!server->socket_name().empty());
    for (int i = 0; i < 3; ++i) {
        CHECK(server->dispatch(10));
    }
    // Destruction runs the full tinywl shutdown sequence.
}
