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

// Construct the extension (ownership transfer to the caller; host-bin installs
// it via Server::install at c2). Construction is cheap and side-effect free per
// the Extension contract; ALL wiring — once it exists — happens in activate().
[[nodiscard]] auto create() -> std::unique_ptr<kernel::Extension>;

} // namespace unbox::ext_stage_dock
