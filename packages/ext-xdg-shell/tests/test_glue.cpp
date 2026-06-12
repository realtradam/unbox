#include <doctest/doctest.h>

#include "probe.hpp"

#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/server.hpp>

#include <cstdlib>
#include <memory>

// Glue smoke test on the wlr headless backend: create a Server, install the
// extension, activate, dispatch a few turns, assert activation actually ran
// (probe), and exercise clean shutdown on destruction. Lenient by design
// (AGENTS.md testing policy) — the heavy correctness lives in test_policy.cpp.

namespace {

auto make_headless_server() -> std::unique_ptr<unbox::kernel::Server> {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    return unbox::kernel::Server::create({});
}

} // namespace

TEST_CASE("ext-xdg-shell activates on a headless server and runs clean") {
    auto server = make_headless_server();

    auto with_probe = unbox::ext_xdg_shell::make_extension_with_probe();
    auto* probe = with_probe.probe;
    REQUIRE(probe != nullptr);
    CHECK_FALSE(probe->activated()); // construction is side-effect free

    server->install(std::move(with_probe.extension));
    server->activate_extensions();

    // Activation must have created the xdg-shell global, wired the hooks, and
    // registered the service.
    CHECK(probe->activated());

    for (int i = 0; i < 5; ++i) {
        CHECK(server->dispatch(10));
    }
    // Destruction runs the extension teardown (RAII) then the server shutdown.
}

TEST_CASE("ext-xdg-shell is a core extension named xdg-shell") {
    auto ext = unbox::ext_xdg_shell::create();
    const auto& m = ext->manifest();
    CHECK(m.id == "xdg-shell");
    CHECK(m.tier == unbox::kernel::Tier::core);
    CHECK(m.depends_on.empty());
}
