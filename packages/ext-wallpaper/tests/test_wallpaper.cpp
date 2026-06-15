#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unbox/ext-wallpaper/ext_wallpaper.hpp>
#include <unbox/kernel/extension.hpp>

// Smoke test: the factory yields an extension with the correct manifest.
// No kernel running; this is cheap and ABI-surface only.

TEST_CASE("ext-wallpaper: factory yields the wallpaper extension") {
    auto ext = unbox::ext_wallpaper::create();
    REQUIRE(ext != nullptr);
    CHECK(ext->manifest().id == "wallpaper");
    CHECK(ext->manifest().tier == unbox::kernel::Tier::standard);
    CHECK(ext->manifest().depends_on.empty());
}

TEST_CASE("ext-wallpaper: factory accepts explicit config path") {
    auto ext = unbox::ext_wallpaper::create("/tmp/nonexistent.toml");
    REQUIRE(ext != nullptr);
    CHECK(ext->manifest().id == "wallpaper");
}
