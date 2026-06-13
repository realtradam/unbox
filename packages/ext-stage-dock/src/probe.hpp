#pragma once

#include <unbox/kernel/extension.hpp>

#include <cstddef>
#include <memory>

// Test-only probe surface (PRIVATE — src/, never part of the contract). The
// headless glue test needs to drive the c2 minimize/restore PIPELINE and read
// the dock MODEL without a synthetic keyboard device (the Super+M keystroke
// decode is trivial; the pipeline it triggers is what matters). The public
// create() hides the concrete extension behind kernel::Extension; this factory
// hands back the same Extension plus a borrowed probe the test polls/drives.
//
// On a no-GL backend (headless pixman) the ui substrate is null, so the dock
// UiSurface is null and previews are null — the probe still exercises the model
// + hide()/show() (exactly what the brief says to test there). Glue/shell test
// convenience only; never a contract claim.

namespace unbox::ext_stage_dock {

// A non-owning view onto the live extension for tests. Valid as long as the
// returned unique_ptr (and thus the extension) is alive. All calls are on the
// single event-loop thread, like the extension itself.
class TestProbe {
public:
    virtual ~TestProbe() = default;

    // True once activate() completed (Service fetched, hooks wired, dock surface
    // creation attempted).
    [[nodiscard]] virtual auto activated() const -> bool = 0;

    // Drive the minimize pipeline on the currently focused window exactly as the
    // Super+M key path does (snapshot -> hide -> slot -> reveal dock + re-focus).
    // No-op if nothing is focused.
    virtual void minimize_focused() = 0;

    // Drive the restore pipeline for slot `i` (show + focus the window, drop the
    // slot). Guards the index.
    virtual void restore(std::size_t i) = 0;

    // The current number of dock slots (minimized windows).
    [[nodiscard]] virtual auto slot_count() const -> std::size_t = 0;

    // Whether the dock currently has a focused window (focused_ != nullptr) — the
    // exact guard do_minimize_focused()/the Super+M filter check before acting.
    // After a restore the dock MUST report a focused window (restore re-focuses
    // the shown window), or the next minimize would be a no-op. Glue-test only.
    [[nodiscard]] virtual auto has_focused() const -> bool = 0;
};

struct ExtensionWithProbe {
    std::unique_ptr<unbox::kernel::Extension> extension; // install() this
    TestProbe* probe = nullptr;                          // borrow into the above
};

// Same extension as create(), but also yields a probe borrow.
[[nodiscard]] auto make_extension_with_probe() -> ExtensionWithProbe;

} // namespace unbox::ext_stage_dock
