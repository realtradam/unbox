#pragma once

#include <string>

// Slice-1 probe surface: proves the kernel compiles against wlroots from
// C++ and links wlroots, libwayland-server, and the vendored RMLUi. The
// real contracts (extension host, bus, scene/seat glue, ui substrate)
// replace this from slice 2 on.
//
// Calling context: everything in unbox runs on the single wl_event_loop
// thread unless a contract explicitly states otherwise.

namespace unbox::kernel {

/// The wlroots version this kernel was compiled against (the 0.20 pin).
[[nodiscard]] auto wlroots_version() -> std::string;

/// The RMLUi version linked from the vendored subproject.
[[nodiscard]] auto rmlui_version() -> std::string;

/// Creates and immediately destroys a wl_display: proves we can call into
/// libwayland-server/wlroots at runtime, not just link. Returns true on
/// success. No side effects beyond wlroots log initialization.
[[nodiscard]] auto link_probe() -> bool;

} // namespace unbox::kernel
