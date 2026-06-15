#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unbox/ext-window-field/ext_window_field.hpp>
#include <unbox/kernel/extension.hpp>

// SCAFFOLD (Wave 3). Smoke test that the factory + manifest are wired. The
// owner-agent replaces/extends this with the real headless glue test (install
// ext-xdg-shell + ext-window-field on the wlr headless backend, map a real
// in-process client toplevel, and assert it becomes a listed surface element +
// that focus routes), mirroring ext-stage-dock's test_glue.cpp client-codegen.

TEST_CASE("ext-window-field: factory yields the window-field extension") {
    auto ext = unbox::ext_window_field::create();
    REQUIRE(ext != nullptr);
    CHECK(ext->manifest().id == "window-field");
    CHECK(ext->manifest().tier == unbox::kernel::Tier::core);
    REQUIRE(ext->manifest().depends_on.size() == 1);
    CHECK(ext->manifest().depends_on[0] == "xdg-shell");
}
