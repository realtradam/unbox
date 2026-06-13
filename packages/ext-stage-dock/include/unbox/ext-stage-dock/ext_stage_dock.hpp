#pragma once

#include <unbox/kernel/extension.hpp>

#include <memory>

// ext-stage-dock — the stage dock (slice 10): the left-edge ui surface holding
// minimized-window PREVIEWS, REVEALED by a left-edge SWIPE (GLOSSARY / notes/
// plan.md §2). Tier: standard. Manifest { id "stage-dock", depends_on
// {"xdg-shell"} } — it will consume ext-xdg-shell's Service (toplevel lifecycle
// + the future hide()/show() minimize mechanism) once wiring lands.
//
// THIS step (b4) ships only the skeleton + the two PURE DECISION CORES
// (src/reveal.hpp the reversible edge-swipe recognizer, src/dock_layout.hpp the
// reveal→on-screen-geometry math), doctest-hard with NO kernel/wlroots running.
// activate() is a deliberate no-op for now; real wiring is later steps
// (c2 = static integration, d1 = animation, e1 = gesture).
//
// This header is the unit's WHOLE cross-extension contract: a factory only
// (mirroring ext-keybindings / ext-layer-shell). It is a LEAF for now — it
// exports no hooks or services yet. Single wl_event_loop thread throughout.

namespace unbox::ext_stage_dock {

// ---- Exported Service (cross-extension typed coupling) -----------------------
//
// Other extensions (ext-keybindings) fetch this via Host::service<T>() to drive
// dock policy (hiding/showing the dock, minimizing windows, etc.). Registered
// by this extension in activate(). See <unbox/kernel/host.hpp> for the service
// registry contract — single-responder, type-keyed, no strings.
class Service {
public:
    virtual ~Service() = default;

    // Toggle the dock's visibility: if the dock is currently shown (slid into
    // the visible area), hide it with the slide-out transition; if hidden, show
    // it with the slide-in. Works regardless of whether the dock has slots
    // (minimized windows) — showing an empty dock is valid. The auto-reveal on
    // minimize (refresh_slots) is independent: minimizing a window also shows
    // the dock.
    virtual void toggle_visible() = 0;

protected:
    Service() = default;
};

// Construct the extension (ownership transfer to the caller; host-bin installs
// it via Server::install at c2). Construction is cheap and side-effect free per
// the Extension contract; ALL wiring — once it exists — happens in activate().
[[nodiscard]] auto create() -> std::unique_ptr<kernel::Extension>;

} // namespace unbox::ext_stage_dock
