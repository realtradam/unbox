#pragma once

#include "policy.hpp"

#include <string>
#include <string_view>
#include <vector>

// Pure decision core: the toml loader. Parses an unbox.toml document (as TEXT —
// file discovery/reading is an effect kept in the glue) into a list of
// policy::Binding, skipping malformed entries and recording a human-readable
// warning for each skip. toml++ is the only dependency; no wlroots, no kernel.
// Doctest-covered in tests/test_policy.cpp.

namespace unbox::ext_keybindings::config {

// The outcome of loading. `bindings` holds every well-formed [[keybind]] (in
// document order). `warnings` holds one message per skipped/ill-formed entry or
// a single parse-error message. `parse_error` is true iff the document itself
// failed to parse (toml syntax error) — in that case `bindings` is empty and
// `warnings` has the parser's message.
//
// NOTE: an empty document, or one with zero valid bindings, is NOT a parse
// error — it returns empty `bindings` with parse_error=false. The glue decides
// to fall back to compiled defaults when `bindings` ends up empty (whatever the
// cause), and logs every warning.
struct LoadResult {
    std::vector<policy::Binding> bindings;
    std::vector<std::string> warnings;
    bool parse_error = false;
};

// Parse `toml_text` and extract the [[keybind]] array-of-tables per the
// USER-APPROVED schema:
//   keys    = "Mod+...+Key" | "Mod"   (required, string)
//   action  = "spawn" | "focus-next" | "focus-prev" | "close-active" | "quit"
//   command = "..."                    (required for action="spawn")
// Each entry is validated independently: a malformed combo, unknown action,
// missing/empty keys, missing command for spawn, or wrong value types skip that
// ONE entry (with a warning) and never abort the rest.
[[nodiscard]] auto load_from_string(std::string_view toml_text) -> LoadResult;

} // namespace unbox::ext_keybindings::config
