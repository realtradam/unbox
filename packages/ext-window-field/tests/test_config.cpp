#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config.hpp"

#include <string_view>

// Pure-core tests for the [window-field] resize-policy loader (src/config.cpp).
// No kernel, no wlroots — just toml text in, ResizePolicy out. Mirrors
// ext-keybindings' config test posture: defaults on absence, per-key validation,
// warnings on bad values, never throws.

namespace cfg = unbox::ext_window_field::config;
using cfg::ResizeMode;

TEST_CASE("empty document yields defaults, no error") {
    const cfg::LoadResult r = cfg::load_from_string("");
    CHECK_FALSE(r.parse_error);
    CHECK(r.warnings.empty());
    CHECK(r.policy.mode == ResizeMode::settle);
    CHECK(r.policy.debounce_ms == 100);
}

TEST_CASE("absent [window-field] table yields defaults") {
    const cfg::LoadResult r = cfg::load_from_string("[other]\nkey = 1\n");
    CHECK_FALSE(r.parse_error);
    CHECK(r.warnings.empty());
    CHECK(r.policy.mode == ResizeMode::settle);
}

TEST_CASE("every resize_mode value parses") {
    CHECK(cfg::load_from_string("[window-field]\nresize_mode = \"off\"\n").policy.mode ==
          ResizeMode::off);
    CHECK(cfg::load_from_string("[window-field]\nresize_mode = \"settle\"\n").policy.mode ==
          ResizeMode::settle);
    CHECK(cfg::load_from_string("[window-field]\nresize_mode = \"continuous\"\n").policy.mode ==
          ResizeMode::continuous);
    CHECK(cfg::load_from_string("[window-field]\nresize_mode = \"debounced\"\n").policy.mode ==
          ResizeMode::debounced);
}

TEST_CASE("unknown resize_mode falls back to settle with a warning") {
    const cfg::LoadResult r =
        cfg::load_from_string("[window-field]\nresize_mode = \"wobble\"\n");
    CHECK_FALSE(r.parse_error);
    CHECK(r.policy.mode == ResizeMode::settle);
    CHECK(r.warnings.size() == 1);
}

TEST_CASE("non-string resize_mode warns and keeps default") {
    const cfg::LoadResult r = cfg::load_from_string("[window-field]\nresize_mode = 3\n");
    CHECK(r.policy.mode == ResizeMode::settle);
    CHECK(r.warnings.size() == 1);
}

TEST_CASE("resize_debounce_ms parses and is independent of mode") {
    const cfg::LoadResult r = cfg::load_from_string(
        "[window-field]\nresize_mode = \"debounced\"\nresize_debounce_ms = 250\n");
    CHECK(r.policy.mode == ResizeMode::debounced);
    CHECK(r.policy.debounce_ms == 250);
    CHECK(r.warnings.empty());
}

TEST_CASE("zero debounce is accepted") {
    const cfg::LoadResult r =
        cfg::load_from_string("[window-field]\nresize_debounce_ms = 0\n");
    CHECK(r.policy.debounce_ms == 0);
    CHECK(r.warnings.empty());
}

TEST_CASE("negative debounce warns and keeps default") {
    const cfg::LoadResult r =
        cfg::load_from_string("[window-field]\nresize_debounce_ms = -5\n");
    CHECK(r.policy.debounce_ms == 100);
    CHECK(r.warnings.size() == 1);
}

TEST_CASE("non-integer debounce warns and keeps default") {
    const cfg::LoadResult r =
        cfg::load_from_string("[window-field]\nresize_debounce_ms = \"soon\"\n");
    CHECK(r.policy.debounce_ms == 100);
    CHECK(r.warnings.size() == 1);
}

TEST_CASE("toml syntax error sets parse_error and keeps defaults") {
    const cfg::LoadResult r = cfg::load_from_string("[window-field\nresize_mode = ");
    CHECK(r.parse_error);
    CHECK(r.policy.mode == ResizeMode::settle);
    CHECK(r.warnings.size() == 1);
}

TEST_CASE("[window-field] not a table warns") {
    const cfg::LoadResult r = cfg::load_from_string("window-field = 5\n");
    CHECK_FALSE(r.parse_error);
    CHECK(r.warnings.size() == 1);
    CHECK(r.policy.mode == ResizeMode::settle);
}
