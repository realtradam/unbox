#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config.hpp"

#include <string_view>

// Pure-core tests for the [wallpaper] config loader (src/config.cpp).
// No kernel, no wlroots — just toml text in, WallpaperConfig out. Mirrors
// ext-window-field's config test posture: defaults on absence, per-key
// validation, warnings on bad values, never throws.

namespace cfg = unbox::ext_wallpaper::config;
using cfg::FitMode;

// ---- empty / missing ---------------------------------------------------------

TEST_CASE("empty document yields defaults, no error") {
    const cfg::LoadResult r = cfg::load_from_string("");
    CHECK_FALSE(r.parse_error);
    CHECK(r.warnings.empty());
    CHECK(r.cfg.path.empty());
    CHECK(r.cfg.fit == FitMode::cover);
    CHECK(r.cfg.color == "#000000");
}

TEST_CASE("absent [wallpaper] table yields defaults") {
    const cfg::LoadResult r = cfg::load_from_string("[other]\nkey = 1\n");
    CHECK_FALSE(r.parse_error);
    CHECK(r.warnings.empty());
    CHECK(r.cfg.fit == FitMode::cover);
    CHECK(r.cfg.color == "#000000");
}

TEST_CASE("missing path key -> empty path (no image), no warning") {
    const cfg::LoadResult r = cfg::load_from_string("[wallpaper]\nfit = \"cover\"\n");
    CHECK(r.cfg.path.empty());
    CHECK(r.warnings.empty());
}

TEST_CASE("explicit empty path is valid (no image)") {
    const cfg::LoadResult r = cfg::load_from_string("[wallpaper]\npath = \"\"\n");
    CHECK_FALSE(r.parse_error);
    CHECK(r.cfg.path.empty());
    // empty path is allowed — glue treats it as "no image"
    CHECK(r.warnings.empty());
}

// ---- path --------------------------------------------------------------------

TEST_CASE("path parses as string") {
    const cfg::LoadResult r =
        cfg::load_from_string("[wallpaper]\npath = \"/home/me/wall.jpg\"\n");
    CHECK(r.cfg.path == "/home/me/wall.jpg");
    CHECK(r.warnings.empty());
}

TEST_CASE("non-string path warns and uses empty") {
    const cfg::LoadResult r = cfg::load_from_string("[wallpaper]\npath = 42\n");
    CHECK(r.cfg.path.empty());
    CHECK(r.warnings.size() == 1);
}

// ---- fit ---------------------------------------------------------------------

TEST_CASE("every fit value parses correctly") {
    CHECK(cfg::load_from_string("[wallpaper]\nfit = \"cover\"\n").cfg.fit   == FitMode::cover);
    CHECK(cfg::load_from_string("[wallpaper]\nfit = \"contain\"\n").cfg.fit == FitMode::contain);
    CHECK(cfg::load_from_string("[wallpaper]\nfit = \"stretch\"\n").cfg.fit == FitMode::stretch);
    CHECK(cfg::load_from_string("[wallpaper]\nfit = \"center\"\n").cfg.fit  == FitMode::center);
}

TEST_CASE("unknown fit falls back to cover with a warning") {
    const cfg::LoadResult r = cfg::load_from_string("[wallpaper]\nfit = \"tile\"\n");
    CHECK_FALSE(r.parse_error);
    CHECK(r.cfg.fit == FitMode::cover);
    CHECK(r.warnings.size() == 1);
}

TEST_CASE("non-string fit warns and keeps cover") {
    const cfg::LoadResult r = cfg::load_from_string("[wallpaper]\nfit = 3\n");
    CHECK(r.cfg.fit == FitMode::cover);
    CHECK(r.warnings.size() == 1);
}

// ---- color -------------------------------------------------------------------

TEST_CASE("valid 6-digit hex color parses") {
    const cfg::LoadResult r = cfg::load_from_string("[wallpaper]\ncolor = \"#1e1e2e\"\n");
    CHECK(r.cfg.color == "#1e1e2e");
    CHECK(r.warnings.empty());
}

TEST_CASE("valid 3-digit hex color parses") {
    const cfg::LoadResult r = cfg::load_from_string("[wallpaper]\ncolor = \"#fff\"\n");
    CHECK(r.cfg.color == "#fff");
    CHECK(r.warnings.empty());
}

TEST_CASE("bad color (missing hash) warns and keeps default") {
    const cfg::LoadResult r = cfg::load_from_string("[wallpaper]\ncolor = \"1e1e2e\"\n");
    CHECK(r.cfg.color == "#000000");
    CHECK(r.warnings.size() == 1);
}

TEST_CASE("bad color (wrong length) warns and keeps default") {
    const cfg::LoadResult r = cfg::load_from_string("[wallpaper]\ncolor = \"#12\"\n");
    CHECK(r.cfg.color == "#000000");
    CHECK(r.warnings.size() == 1);
}

TEST_CASE("non-string color warns and keeps default") {
    const cfg::LoadResult r = cfg::load_from_string("[wallpaper]\ncolor = 0\n");
    CHECK(r.cfg.color == "#000000");
    CHECK(r.warnings.size() == 1);
}

// ---- parse error -------------------------------------------------------------

TEST_CASE("toml syntax error sets parse_error and keeps defaults") {
    const cfg::LoadResult r = cfg::load_from_string("[wallpaper\nfit = ");
    CHECK(r.parse_error);
    CHECK(r.cfg.fit == FitMode::cover);
    CHECK(r.cfg.color == "#000000");
    CHECK(r.warnings.size() == 1);
}

// ---- [wallpaper] not a table -------------------------------------------------

TEST_CASE("[wallpaper] not a table warns") {
    const cfg::LoadResult r = cfg::load_from_string("wallpaper = 5\n");
    CHECK_FALSE(r.parse_error);
    CHECK(r.warnings.size() == 1);
    CHECK(r.cfg.fit == FitMode::cover);
}

// ---- rmlui_fit_keyword mapping -----------------------------------------------

TEST_CASE("rmlui_fit_keyword maps all FitMode values") {
    CHECK(cfg::rmlui_fit_keyword(FitMode::cover)   == "cover");
    CHECK(cfg::rmlui_fit_keyword(FitMode::contain) == "contain");
    CHECK(cfg::rmlui_fit_keyword(FitMode::stretch) == "fill");
    CHECK(cfg::rmlui_fit_keyword(FitMode::center)  == "scale-none");
}

// ---- independent per-key validation ------------------------------------------

TEST_CASE("bad fit and bad color both warn independently") {
    const cfg::LoadResult r = cfg::load_from_string(
        "[wallpaper]\nfit = \"tile\"\ncolor = \"notacolor\"\n");
    CHECK_FALSE(r.parse_error);
    CHECK(r.cfg.fit == FitMode::cover);
    CHECK(r.cfg.color == "#000000");
    CHECK(r.warnings.size() == 2);
}

// ---- default_image_path (pure path-join helper) ------------------------------

TEST_CASE("default_image_path joins asset root and relative segment") {
    CHECK(cfg::default_image_path("/usr/share/unbox") ==
          "/usr/share/unbox/ext-wallpaper/default.jpg");
}

TEST_CASE("default_image_path strips a trailing slash from the root") {
    CHECK(cfg::default_image_path("/usr/share/unbox/") ==
          "/usr/share/unbox/ext-wallpaper/default.jpg");
}

TEST_CASE("default_image_path strips multiple trailing slashes") {
    CHECK(cfg::default_image_path("/opt/unbox///") ==
          "/opt/unbox/ext-wallpaper/default.jpg");
}

TEST_CASE("default_image_path with bare dot (process cwd fallback)") {
    CHECK(cfg::default_image_path(".") == "./ext-wallpaper/default.jpg");
}

TEST_CASE("default_image_path with empty root gives a usable relative path") {
    // An empty asset_root should not crash; it yields the relative segment.
    const std::string p = cfg::default_image_path("");
    CHECK(p == "/ext-wallpaper/default.jpg");
}
