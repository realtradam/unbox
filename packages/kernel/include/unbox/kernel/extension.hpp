#pragma once

#include <string>
#include <vector>

// What an extension IS, to the kernel. An extension is an in-process unit that
// contributes capabilities (hooks, services, ui surfaces, protocol glue)
// through the Host API it receives in activate(). The kernel names no concrete
// extension; host-bin (the composition root) names them all.
//
// Lifetime / deactivation: there is NO teardown method by design. An
// extension's lifetime IS the session's; deactivation = destruction. Hold
// every resource (Subscriptions, service registrations, scene nodes, wlroots
// Listeners) as a member; they release in REVERSE declaration order when the
// extension object is destroyed. So declare members in dependency order
// (things that depend on the Host's borrows last). The kernel destroys
// extensions in reverse activation (topological) order at shutdown.
//
// Everything runs on the single wl_event_loop thread.

namespace unbox::kernel {

class Host;

// The activation tier. Lower tiers activate first and may be depended upon by
// higher tiers; an extension may also depend on same-tier extensions by id
// (resolved topologically within the install set). See GLOSSARY "core"/
// "standard". The kernel itself is below all tiers and is always present.
enum class Tier {
    core,     // minimum usable session: xdg-shell, layer-shell, keybindings…
    standard, // on-by-default features: taskbar, launcher, tiling, OSK…
};

// An extension's self-declaration. `id` is for ACTIVATION ORDERING and
// DIAGNOSTICS ONLY — capabilities are NEVER looked up by string (AGENTS.md:
// string-keyed lookups are forbidden; cross-extension coupling goes through
// exported typed hook/service symbols). Two installed extensions sharing an id
// is a startup error; a depends_on naming an id not in the install set is a
// startup error; a dependency cycle is a startup error.
struct Manifest {
    std::string id;                    // unique, stable, e.g. "ext-xdg-shell"
    Tier tier = Tier::standard;        // activation-order tier
    std::vector<std::string> depends_on; // ids that must activate before this
};

// The base every extension implements. Construction is cheap and side-effect
// free; ALL wiring happens in activate(). activate() is called once, in
// topological order, before Server::run(). The Host& borrow is valid for the
// whole session (until this extension is destroyed at shutdown) — but it is a
// per-extension facade and must NOT be handed to another extension.
class Extension {
public:
    virtual ~Extension() = default;

    // Static identity; must return the same value every call (the Server reads
    // it once at install time). No side effects.
    [[nodiscard]] virtual auto manifest() const -> const Manifest& = 0;

    // Wire up: subscribe to hooks, register services, create ui surfaces and
    // scene nodes, attach wlroots Listeners — storing every returned RAII
    // handle as a member of `this`. May throw to signal a fatal activation
    // failure; the Server aborts startup with that error (activation failures
    // are not isolated — a core extension that can't start is a broken
    // session, surfaced to host-bin, not silently disabled). Runtime callback
    // throws ARE isolated (hooks.hpp).
    virtual void activate(Host& host) = 0;
};

} // namespace unbox::kernel
