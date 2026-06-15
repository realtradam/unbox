#pragma once

#include <string>
#include <string_view>
#include <vector>

// Pure decision core: the window-field config loader. Parses an unbox.toml
// document (as TEXT — file discovery/reading is an effect kept in the glue) into
// a ResizePolicy: how the field feeds a window's RCSS-computed tile size back to
// the client as a render resolution (ext-xdg-shell::Toplevel::set_size). toml++
// is the only dependency; no wlroots, no kernel. Doctest-covered in
// tests/test_config.cpp.
//
// SCHEMA (the [window-field] table; every key optional — absent => default):
//   [window-field]
//   resize_mode        = "settle"   # off | settle | continuous | debounced
//   resize_debounce_ms = 100        # used only when resize_mode = "debounced"
//
// resize_mode semantics (the tile size animates over an RCSS transition on every
// focus/map/unmap, so the question is WHEN to push the size to the client — each
// push makes the client reallocate + redraw):
//   off        — never resize: the client keeps its own size, letterbox-fit into
//                the tile by the RCSS `contain` decorator (the pre-feature look).
//   settle     — wait for the tile box to stop changing (the transition done),
//                then send ONE configure to the final size (~1 client redraw per
//                focus change; honours the precious GPU frame budget).
//   continuous — send a configure every frame the box changes (exact real-time
//                tracking, ~1 redraw per animation frame; heavy on this GPU).
//   debounced  — coalesce: configure at most once per resize_debounce_ms during
//                the animation, plus a final one on settle.

namespace unbox::ext_window_field::config {

// How the field pushes a tile's RCSS-computed size to its client.
enum class ResizeMode { off, settle, continuous, debounced };

// The parsed window-field policy. Defaults are the compiled-in fallback used
// when no config / no [window-field] table / a bad value is present.
struct ResizePolicy {
    ResizeMode mode = ResizeMode::settle; // default: one configure on settle
    int debounce_ms = 100;                // debounced only; clamped to >= 0
};

// The outcome of loading. `policy` is always populated (defaults where a key is
// absent or invalid). `warnings` holds one message per bad value or a single
// parse-error message. `parse_error` is true iff the document failed to parse
// (toml syntax error) — then `policy` is the pure default. NEVER throws (the
// toml parse error is caught here). An empty document / absent [window-field] is
// NOT an error — it yields defaults with parse_error=false and no warnings.
struct LoadResult {
    ResizePolicy policy;
    std::vector<std::string> warnings;
    bool parse_error = false;
};

// Parse `toml_text` and extract the [window-field] policy per the schema above.
// Each key is validated independently: an unknown resize_mode string, a
// non-string resize_mode, a negative/non-integer resize_debounce_ms falls back
// to that field's default with a warning and never aborts the rest.
[[nodiscard]] auto load_from_string(std::string_view toml_text) -> LoadResult;

} // namespace unbox::ext_window_field::config
