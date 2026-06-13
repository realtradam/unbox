#include <unbox/ext-stage-dock/ext_stage_dock.hpp>

#include "dock_layout.hpp"
#include "reveal.hpp"

#include <unbox/kernel/host.hpp>
#include <unbox/kernel/wlr.hpp>

#include <memory>

// ext-stage-dock glue (b4 SKELETON). The decision cores live in src/reveal.hpp
// (the reversible edge-swipe recognizer) and src/dock_layout.hpp (reveal ->
// on-screen geometry) — both wlroots/GL/RMLUi-free and doctest-hard. THIS file
// is the thin effectful edge: for now it is a deliberate no-op that just logs at
// activate(), so the unit compiles and installs cleanly while the real wiring
// (c2 static integration, d1 animation, e1 gesture) lands in later steps.
//
// Everything runs on the single wl_event_loop thread. Every future resource will
// be a RAII member of StageDockExtension; teardown is reverse-declaration
// destruction (no manual teardown lists — extension-agent.md).

namespace unbox::ext_stage_dock {
namespace {

using kernel::Host;

class StageDockExtension final : public kernel::Extension {
public:
    auto manifest() const -> const kernel::Manifest& override { return manifest_; }

    void activate(Host& host) override {
        // No-op this step: no hooks, no service, no RML document yet. The pure
        // cores are exercised by the doctest suite, not at runtime. Real wiring
        // arrives at c2 (consume ext-xdg-shell's Service) / d1 / e1.
        (void)host;
        wlr_log(WLR_INFO, "ext-stage-dock: activate (skeleton; no wiring yet)");
    }

private:
    const kernel::Manifest manifest_{
        .id = "stage-dock",
        .tier = kernel::Tier::standard,
        .depends_on = {"xdg-shell"},
    };
};

} // namespace

auto create() -> std::unique_ptr<kernel::Extension> {
    return std::make_unique<StageDockExtension>();
}

} // namespace unbox::ext_stage_dock
