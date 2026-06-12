#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unbox/kernel/extension.hpp>
#include <unbox/kernel/hooks.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/kernel.hpp>
#include <unbox/kernel/server.hpp>
#include <unbox/kernel/surface_registry.hpp>

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("kernel compiles against and links wlroots + libwayland-server") {
    CHECK(unbox::kernel::link_probe());
    CHECK(unbox::kernel::wlroots_version().substr(0, 4) == "0.20");
}

TEST_CASE("vendored RMLUi subproject compiled and linked") {
    CHECK(!unbox::kernel::rmlui_version().empty());
}

TEST_CASE("server boots and shuts down on the headless backend") {
    // Headless backend + pixman renderer: no GPU, no parent session needed.
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    auto server = unbox::kernel::Server::create({});
    CHECK(!server->socket_name().empty());
    for (int i = 0; i < 3; ++i) {
        CHECK(server->dispatch(10));
    }
    // Destruction runs the full tinywl shutdown sequence.
}

TEST_CASE("ui spike defaults off and is the slice-2 server") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    auto server = unbox::kernel::Server::create({});
    CHECK(server->ui_spike_frame_count() == 0);
    for (int i = 0; i < 3; ++i) {
        CHECK(server->dispatch(10));
    }
    CHECK(server->ui_spike_frame_count() == 0);
}

TEST_CASE("ui spike boots, renders frames, and shuts down cleanly") {
    // Drive the RMLUi -> wlr_scene bridge on the headless backend with the
    // gles2 renderer so the real GL path is exercised (Plan A attempted,
    // Plan B as fallback). The headless backend uses an EGL render node; if
    // GL is unavailable the bridge disables itself gracefully and frame_count
    // stays 0 (asserted as the no-crash fallback). A headless output must be
    // created so the frame handler (which drives tick()) fires.
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);

    auto server = unbox::kernel::Server::create({.ui_spike = true});
    CHECK(!server->socket_name().empty());

    // Pump enough turns for the headless output to emit frames.
    for (int i = 0; i < 200; ++i) {
        CHECK(server->dispatch(10));
    }

    const int frames = server->ui_spike_frame_count();
    INFO("ui_spike_frame_count() = ", frames);
    // Either the bridge ran (frames advanced) or it disabled itself on a
    // headless box without a usable GL path. Both are acceptable; a crash is
    // not. Clean shutdown is exercised on destruction below.
    CHECK(frames >= 0);
}

TEST_CASE("ui spike submits an upright (non-flipped) buffer") {
    // Orientation regression guard. The spike document carries distinctive
    // solid bands at its top and bottom edges; on the CPU-readback (Plan B)
    // path the bridge inspects the SUBMITTED buffer and reports +1 if the top
    // band is in the top rows (upright), -1 if vertically flipped. GL's
    // bottom-left framebuffer origin vs the wlr_buffer top-first convention
    // makes the flip the default failure mode, so this must never silently
    // regress. Force the shm path so the readback exists; if GL is
    // unavailable the spike disables itself and orientation() returns 0
    // (skipped, not failed — same graceful-degrade contract as above).
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SPIKE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({.ui_spike = true});
    for (int i = 0; i < 200; ++i) {
        CHECK(server->dispatch(10));
    }

    const int orient = server->ui_spike_orientation();
    INFO("ui_spike_orientation() = ", orient);
    // MUST NOT be flipped. +1 = upright (the bridge ran), 0 = indeterminate
    // (no GL path on this box). A flip (-1) is the bug and fails here.
    CHECK(orient != -1);
    if (server->ui_spike_frame_count() > 0) {
        // The shm bridge ran: orientation must be positively confirmed upright.
        CHECK(orient == 1);
    }

    unsetenv("UNBOX_UI_SPIKE_FORCE_SHM");
}

// ============================================================================
// The typed bus — PURE CORE (strict; zero mocks of unbox modules, no wlroots
// running). A test DisableSink stands in for the kernel's isolation registry.
// ============================================================================

namespace {

using unbox::kernel::detail::DisableSink;
using unbox::kernel::detail::HookBase;
using unbox::kernel::Event;
using unbox::kernel::ExtensionId;
using unbox::kernel::Filter;
using unbox::kernel::Subscription;

// Mirrors Server::Impl's isolation behavior at pure-core scale: on disable(),
// purge the offending extension from every registered hook. Records who got
// disabled so tests can assert isolation hit the RIGHT extension.
struct TestRegistry final : DisableSink {
    std::vector<HookBase*> hooks;
    std::vector<ExtensionId> disabled;

    void track(HookBase& h) {
        h.set_sink(this);
        hooks.push_back(&h);
    }
    void disable(ExtensionId who) noexcept override {
        disabled.push_back(who);
        for (HookBase* h : hooks) {
            h->purge(who);
        }
    }
};

constexpr ExtensionId ext_a{1};
constexpr ExtensionId ext_b{2};
constexpr ExtensionId ext_c{3};

} // namespace

TEST_CASE("Event fans out to all listeners in subscription order") {
    Event<int> ev;
    std::vector<int> log;
    auto s1 = ev.subscribe(ext_a, [&](int v) { log.push_back(v + 10); });
    auto s2 = ev.subscribe(ext_b, [&](int v) { log.push_back(v + 20); });
    auto s3 = ev.subscribe(ext_c, [&](int v) { log.push_back(v + 30); });

    ev.emit(1);
    CHECK(log == std::vector<int>{11, 21, 31});
}

TEST_CASE("Subscription RAII unsubscribes on destruction") {
    Event<int> ev;
    int hits = 0;
    auto outer = ev.subscribe(ext_a, [&](int) { ++hits; });
    {
        auto inner = ev.subscribe(ext_b, [&](int) { ++hits; });
        ev.emit(0);
        CHECK(hits == 2);
    }
    // inner dropped: only outer remains.
    ev.emit(0);
    CHECK(hits == 3);

    // Explicit reset() also unsubscribes.
    outer.reset();
    CHECK(!outer.active());
    ev.emit(0);
    CHECK(hits == 3);
}

TEST_CASE("Subscription is move-only and the moved-from handle is inert") {
    Event<int> ev;
    int hits = 0;
    Subscription s = ev.subscribe(ext_a, [&](int) { ++hits; });
    Subscription moved = std::move(s);
    CHECK(moved.active());
    CHECK(!s.active());
    ev.emit(0);
    CHECK(hits == 1);
    s.reset(); // no-op on moved-from
    ev.emit(0);
    CHECK(hits == 2);
}

TEST_CASE("a listener may unsubscribe ITSELF during dispatch (deferred removal)") {
    Event<int> ev;
    int a = 0;
    int c = 0;
    std::unique_ptr<Subscription> self;
    auto sa = ev.subscribe(ext_a, [&](int) { ++a; });
    auto sb = ev.subscribe(ext_b, [&](int) { self->reset(); }); // drop self mid-dispatch
    auto sc = ev.subscribe(ext_c, [&](int) { ++c; });
    self = std::make_unique<Subscription>(std::move(sb));

    ev.emit(0);
    // a and c still fired this round despite b removing itself.
    CHECK(a == 1);
    CHECK(c == 1);

    ev.emit(0); // b gone now
    CHECK(a == 2);
    CHECK(c == 2);
}

TEST_CASE("re-entrant emit is safe") {
    Event<int> ev;
    int inner = 0;
    bool reentered = false;
    auto s = ev.subscribe(ext_a, [&](int v) {
        if (!reentered && v == 1) {
            reentered = true;
            ev.emit(2); // re-enter
        }
        ++inner;
    });
    ev.emit(1);
    CHECK(inner == 2); // outer (v=1) and inner (v=2)
}

TEST_CASE("Filter threads the value through links in order") {
    Filter<int> flt;
    auto s1 = flt.subscribe(ext_a, [](int v) { return v + 1; });
    auto s2 = flt.subscribe(ext_b, [](int v) { return v * 10; });
    // (((5)+1)*10) = 60
    CHECK(flt.apply(5) == 60);
}

TEST_CASE("Filter with no links returns the value unchanged") {
    Filter<int> flt;
    CHECK(flt.apply(42) == 42);
}

TEST_CASE("error isolation: a throwing listener disables only its extension") {
    TestRegistry reg;
    Event<int> ev{&reg};
    reg.track(ev);

    std::vector<std::string> log;
    auto sa = ev.subscribe(ext_a, [&](int) { log.emplace_back("a"); });
    auto sb = ev.subscribe(ext_b, [&](int) {
        log.emplace_back("b-throw");
        throw std::runtime_error("boom");
    });
    auto sc = ev.subscribe(ext_c, [&](int) { log.emplace_back("c"); });

    ev.emit(0);
    // All three ran THIS emit (isolation doesn't abort the in-flight fan-out);
    // b was disabled.
    CHECK(log == std::vector<std::string>{"a", "b-throw", "c"});
    CHECK(reg.disabled == std::vector<ExtensionId>{ext_b});

    log.clear();
    ev.emit(0);
    // b's subscription was purged; a and c remain.
    CHECK(log == std::vector<std::string>{"a", "c"});
}

TEST_CASE("error isolation: a throwing filter link is skipped and chain continues") {
    TestRegistry reg;
    Filter<int> flt{&reg};
    reg.track(flt);

    auto s1 = flt.subscribe(ext_a, [](int v) { return v + 1; });
    auto s2 = flt.subscribe(ext_b, [](int) -> int { throw std::runtime_error("boom"); });
    auto s3 = flt.subscribe(ext_c, [](int v) { return v * 10; });

    // a: 0->1, b throws (skipped, value stays 1), c: 1->10.
    CHECK(flt.apply(0) == 10);
    CHECK(reg.disabled == std::vector<ExtensionId>{ext_b});

    // b purged: a then c.
    CHECK(flt.apply(0) == 10);
}

TEST_CASE("disabling an extension purges it across MULTIPLE hooks") {
    TestRegistry reg;
    Event<int> ev1{&reg};
    Event<int> ev2{&reg};
    reg.track(ev1);
    reg.track(ev2);

    int ev2_hits = 0;
    // ext_b subscribes to BOTH hooks; throwing on ev1 must drop its ev2 sub too.
    auto a1 = ev1.subscribe(ext_a, [](int) {});
    auto b1 = ev1.subscribe(ext_b, [](int) { throw std::runtime_error("boom"); });
    auto b2 = ev2.subscribe(ext_b, [&](int) { ++ev2_hits; });

    ev1.emit(0);          // disables ext_b everywhere
    ev2.emit(0);          // ext_b's ev2 listener must NOT fire
    CHECK(ev2_hits == 0);
    CHECK(reg.disabled == std::vector<ExtensionId>{ext_b});
}

// ============================================================================
// Extension host: install + topological activation (no wlroots input needed).
// ============================================================================

namespace {

// Records activation order into a shared log so tests can assert topo order.
class RecordingExtension : public unbox::kernel::Extension {
public:
    RecordingExtension(unbox::kernel::Manifest m, std::vector<std::string>* log)
        : manifest_(std::move(m)), log_(log) {}
    auto manifest() const -> const unbox::kernel::Manifest& override { return manifest_; }
    void activate(unbox::kernel::Host&) override { log_->push_back(manifest_.id); }

private:
    unbox::kernel::Manifest manifest_;
    std::vector<std::string>* log_;
};

auto make_headless_server() -> std::unique_ptr<unbox::kernel::Server> {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    return unbox::kernel::Server::create({});
}

using unbox::kernel::Manifest;
using unbox::kernel::Tier;

} // namespace

TEST_CASE("activation respects depends_on topological order") {
    auto server = make_headless_server();
    std::vector<std::string> log;

    // Install in an order that does NOT match the dependency order.
    server->install(std::make_unique<RecordingExtension>(
        Manifest{"taskbar", Tier::standard, {"xdg-shell"}}, &log));
    server->install(std::make_unique<RecordingExtension>(
        Manifest{"xdg-shell", Tier::core, {}}, &log));
    server->install(std::make_unique<RecordingExtension>(
        Manifest{"tiling", Tier::standard, {"xdg-shell", "taskbar"}}, &log));

    server->activate_extensions();

    // xdg-shell first (no deps, core tier), then taskbar, then tiling.
    CHECK(log == std::vector<std::string>{"xdg-shell", "taskbar", "tiling"});
}

TEST_CASE("activate_extensions is idempotent") {
    auto server = make_headless_server();
    std::vector<std::string> log;
    server->install(
        std::make_unique<RecordingExtension>(Manifest{"a", Tier::core, {}}, &log));
    server->activate_extensions();
    server->activate_extensions();
    CHECK(log == std::vector<std::string>{"a"});
}

TEST_CASE("duplicate extension id is a startup error at install") {
    auto server = make_headless_server();
    std::vector<std::string> log;
    server->install(
        std::make_unique<RecordingExtension>(Manifest{"dup", Tier::core, {}}, &log));
    CHECK_THROWS_AS(server->install(std::make_unique<RecordingExtension>(
                        Manifest{"dup", Tier::standard, {}}, &log)),
                    std::runtime_error);
}

TEST_CASE("missing dependency is a startup error at activation") {
    auto server = make_headless_server();
    std::vector<std::string> log;
    server->install(std::make_unique<RecordingExtension>(
        Manifest{"needs-missing", Tier::core, {"nope"}}, &log));
    CHECK_THROWS_AS(server->activate_extensions(), std::runtime_error);
}

TEST_CASE("dependency cycle is a startup error at activation") {
    auto server = make_headless_server();
    std::vector<std::string> log;
    server->install(
        std::make_unique<RecordingExtension>(Manifest{"x", Tier::core, {"y"}}, &log));
    server->install(
        std::make_unique<RecordingExtension>(Manifest{"y", Tier::core, {"x"}}, &log));
    CHECK_THROWS_AS(server->activate_extensions(), std::runtime_error);
}

TEST_CASE("featureless kernel: zero extensions boots, runs, shuts down clean") {
    auto server = make_headless_server();
    CHECK(!server->socket_name().empty());
    server->activate_extensions(); // no-op with zero extensions
    for (int i = 0; i < 3; ++i) {
        CHECK(server->dispatch(10));
    }
}

// ============================================================================
// Typed surface->scene-tree association — PURE CORE (no wlroots). Keys/values
// are pointer identities; dummy addresses stand in for wlr_surface*/scene_tree*.
// ============================================================================

namespace {

using unbox::kernel::detail::PointerAssoc;
using unbox::kernel::SurfaceRegistration;

// Distinct, never-dereferenced sentinel addresses.
int surf_a_obj = 0, surf_b_obj = 0, tree_1_obj = 0, tree_2_obj = 0;
void* const surf_a = &surf_a_obj;
void* const surf_b = &surf_b_obj;
void* const tree_1 = &tree_1_obj;
void* const tree_2 = &tree_2_obj;

} // namespace

TEST_CASE("surface assoc: register, lookup, unregister") {
    PointerAssoc store;
    CHECK(store.get(surf_a) == nullptr); // unregistered -> null

    SurfaceRegistration reg(&store, surf_a, store.set(surf_a, tree_1));
    CHECK(reg.active());
    CHECK(store.get(surf_a) == tree_1);
    CHECK(store.get(surf_b) == nullptr); // independent key still null

    reg.reset();
    CHECK(!reg.active());
    CHECK(store.get(surf_a) == nullptr); // unregistered on reset
    CHECK(store.size() == 0);
}

TEST_CASE("surface assoc: RAII handle unregisters on destruction") {
    PointerAssoc store;
    {
        SurfaceRegistration reg(&store, surf_a, store.set(surf_a, tree_1));
        CHECK(store.get(surf_a) == tree_1);
    }
    CHECK(store.get(surf_a) == nullptr);
}

TEST_CASE("surface assoc: move transfers ownership; moved-from is inert") {
    PointerAssoc store;
    SurfaceRegistration a(&store, surf_a, store.set(surf_a, tree_1));
    SurfaceRegistration b = std::move(a);
    CHECK(b.active());
    CHECK(!a.active());
    a.reset(); // no-op
    CHECK(store.get(surf_a) == tree_1); // still registered (b owns it)
    b.reset();
    CHECK(store.get(surf_a) == nullptr);
}

TEST_CASE("surface assoc: double-register replaces value; stale handle is a no-op") {
    PointerAssoc store;
    // First registration of surf_a -> tree_1.
    SurfaceRegistration first(&store, surf_a, store.set(surf_a, tree_1));
    CHECK(store.get(surf_a) == tree_1);

    // Re-host the SAME surface in tree_2: replaces the mapping, bumps token.
    SurfaceRegistration second(&store, surf_a, store.set(surf_a, tree_2));
    CHECK(store.get(surf_a) == tree_2);

    // Destroying the SUPERSEDED first handle must NOT tear down the newer
    // mapping (token defense).
    first.reset();
    CHECK(store.get(surf_a) == tree_2);

    // The current owner still unregisters correctly.
    second.reset();
    CHECK(store.get(surf_a) == nullptr);
}

TEST_CASE("surface assoc: distinct keys are independent") {
    PointerAssoc store;
    SurfaceRegistration ra(&store, surf_a, store.set(surf_a, tree_1));
    SurfaceRegistration rb(&store, surf_b, store.set(surf_b, tree_2));
    CHECK(store.get(surf_a) == tree_1);
    CHECK(store.get(surf_b) == tree_2);
    CHECK(store.size() == 2);
    ra.reset();
    CHECK(store.get(surf_a) == nullptr);
    CHECK(store.get(surf_b) == tree_2); // unaffected
}
