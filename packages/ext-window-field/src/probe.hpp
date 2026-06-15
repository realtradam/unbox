#pragma once

#include <unbox/kernel/extension.hpp>

#include <cstddef>
#include <memory>
#include <string>

// Test-only probe surface (PRIVATE — src/, never part of the contract). The
// headless glue test needs to read the window-field MODEL (the bound window
// list) without a GL substrate: on the headless pixman backend the ui surface +
// the SurfaceElements are null (no GL path), so there is nothing visual to
// assert — only the list/model and that Toplevel::hide() was driven. This probe
// exposes exactly that. The public create() hides the concrete extension behind
// kernel::Extension; make_extension_with_probe() hands back the same Extension
// plus a borrowed probe the test polls.
//
// Glue/shell test convenience only; never a contract claim. All calls are on
// the single event-loop thread, like the extension itself.

namespace unbox::ext_window_field {

// A non-owning view onto the live extension for tests. Valid as long as the
// returned unique_ptr (and thus the extension) is alive.
class TestProbe {
public:
    virtual ~TestProbe() = default;

    // True once activate() completed (Service fetched, hooks wired, the window
    // field ui surface creation attempted).
    [[nodiscard]] virtual auto activated() const -> bool = 0;

    // The number of windows currently tracked in the window field (the bound
    // "wins" list size).
    [[nodiscard]] virtual auto window_count() const -> std::size_t = 0;

    // The live_uri the list binds for window `i` (SurfaceElement::source_uri(),
    // or "" when the surface element is null — no GL path on the headless
    // pixman backend). Empty string for an out-of-range index.
    [[nodiscard]] virtual auto live_uri(std::size_t i) const -> std::string = 0;

    // Whether window `i` carries a non-null SurfaceElement (true only when the
    // substrate has a GL path). False on the headless pixman backend.
    [[nodiscard]] virtual auto has_surface_element(std::size_t i) const -> bool = 0;

    // The index of the currently focused window in the list, or -1 if none /
    // the focused window is not tracked. The RCSS keys its highlight/raise off
    // the per-row `focused` bool this drives.
    [[nodiscard]] virtual auto focused_index() const -> std::ptrdiff_t = 0;

    // How many tracked windows have been hidden out of wlr_scene (had
    // Toplevel::hide() driven on map). One per mapped window the field owns.
    [[nodiscard]] virtual auto hidden_count() const -> std::size_t = 0;
};

struct ExtensionWithProbe {
    std::unique_ptr<unbox::kernel::Extension> extension; // install() this
    TestProbe* probe = nullptr;                          // borrow into the above
};

// Same extension as create(), but also yields a probe borrow.
[[nodiscard]] auto make_extension_with_probe() -> ExtensionWithProbe;

} // namespace unbox::ext_window_field
