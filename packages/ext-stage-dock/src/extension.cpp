#include <unbox/ext-stage-dock/ext_stage_dock.hpp>

#include "dock_layout.hpp"
#include "probe.hpp"
#include "reveal.hpp"

#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/ui.hpp>
#include <unbox/kernel/wlr.hpp>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// ext-stage-dock glue (c2 STATIC INTEGRATION). The decision cores live in
// src/reveal.hpp (the reversible edge-swipe recognizer) and src/dock_layout.hpp
// (reveal -> on-screen geometry) — both wlroots/GL/RMLUi-free and doctest-hard;
// c2 uses dock_layout for the dock frame box only (no reveal animation yet, that
// is d1; no gesture yet, that is e1).
//
// This step wires the END-TO-END minimize pipeline with NO animation/gesture:
//   minimize key (Super+M, stopgap) -> snapshot the focused window into a
//   Preview, hide() its scene node, add a slot to the dock model, reveal the
//   dock; tap a slot -> show()+focus() that window, drop the slot (frees the
//   Preview texture), hide the dock when empty.
//
// Everything runs on the single wl_event_loop thread. Every resource is a RAII
// member of StageDockExtension; teardown is reverse-declaration destruction (no
// manual teardown lists — extension-agent.md). The Subscriptions release FIRST
// (declared last), then the dock UiSurface (its bindings capture `this` and read
// slots_, so it is destroyed BEFORE slots_), then the slots' Previews — all
// before the Host borrow goes away.

namespace unbox::ext_stage_dock {
namespace {

using kernel::Host;

// ext-xdg-shell's Toplevel is the window handle. KEY INSIGHT (brief): minimize
// is hide(), NOT unmap — a minimized window stays mapped, so its Toplevel*
// borrow stays valid until on_toplevel_unmapped fires. We therefore MAY store a
// minimized window's Toplevel* in a slot and deref it later to restore; we drop
// the slot the moment on_toplevel_unmapped fires for it (never deref after).
using Toplevel = ext_xdg_shell::Toplevel;

// The stopgap minimize chord: Super(LOGO)+M. A STOPGAP per the brief — the
// config-driven migration (a `minimize` action in ext-keybindings + a Service
// we export) is a post-d1 step (change-request in the report).
constexpr std::uint32_t kMinimizeKeysym = XKB_KEY_m;       // 0x06d
constexpr std::uint32_t kMinimizeMods = WLR_MODIFIER_LOGO;  // Super/LOGO

// The dock width (revealed) in px — matches the dock_layout default. For c2 the
// dock sits fully revealed (no reveal animation); d1 animates dock_box(f).
constexpr int kDockWidth = 240;

// A minimized window's dock entry: the live Toplevel* borrow (valid until its
// unmapped event), the frozen Preview (owns the imported texture; null when the
// substrate has no GL path), and a copied title (the Toplevel's title() view is
// call-only, so we copy at minimize time). app_id copied too for the deferred
// favicon lookup. // TODO favicon: app_id -> icon file via the XDG icon theme
// (needs an icon-lookup dependency the user must approve; deferred in c2).
struct Slot {
    Toplevel* tl = nullptr;                       // borrow, live until unmapped
    std::unique_ptr<kernel::Preview> preview;     // owns the snapshot texture
    std::string title;                            // copied at minimize time
    std::string app_id;                           // copied; for // TODO favicon
};

// Inline RML for the dock document. A vertical stack of preview cards, dark/
// rounded like the Stage-Manager reference. Animation-ready (a `dock` class on
// the body + a `slot` class per card) but STATIC now — d1 adds the RCSS
// transitions. data-model "ui" (the substrate default); iterates the "slots"
// list bound below. {{ row.preview }} is the Preview source_uri(); the
// data-event-click delivers the row index to restore().
constexpr const char* kDockRml = R"RML(<rml>
<head>
<style>
body.dock {
    background-color: #1c1c1ee6;
    padding: 8dp;
    font-family: sans-serif;
}
div.slot {
    display: block;
    margin-bottom: 8dp;
    padding: 6dp;
    background-color: #2e2e32ff;
    border-radius: 10dp;
}
div.slot img.preview {
    display: block;
    width: 100%;
    height: 84dp;
    border-radius: 6dp;
}
div.slot span.title {
    display: block;
    margin-top: 4dp;
    color: #e6e6e6ff;
    font-size: 13dp;
    text-align: center;
}
</style>
</head>
<body data-model="ui" class="dock">
<div data-for="row : slots" class="slot" data-event-click="restore(it_index)">
<img class="preview" src="{{ row.preview }}"/>
<span class="title">{{ row.title }}</span>
</div>
</body>
</rml>)RML";

class StageDockExtension final : public kernel::Extension, public TestProbe {
public:
    auto manifest() const -> const kernel::Manifest& override { return manifest_; }

    // ---- TestProbe (src/probe.hpp; glue-test only) ----
    [[nodiscard]] auto activated() const -> bool override { return activated_; }
    void minimize_focused() override { do_minimize_focused(); }
    void restore(std::size_t i) override { do_restore(i); }
    [[nodiscard]] auto slot_count() const -> std::size_t override { return slots_.size(); }

    void activate(Host& host) override {
        host_ = &host;

        // Fatal: a missing ext-xdg-shell Service. The dock minimizes/restores
        // its toplevels and snapshots their scene trees — meaningless without
        // it. depends_on "xdg-shell" guarantees it activated first, so absence
        // is a broken core session (extension.hpp: activation failure is fatal).
        shell_ = host.service<ext_xdg_shell::Service>();
        if (shell_ == nullptr) {
            throw std::runtime_error(
                "ext-stage-dock: ext-xdg-shell Service unavailable (depends_on "
                "\"xdg-shell\" not satisfied)");
        }

        // Track mapped toplevels + the currently focused one. The Toplevel*
        // borrow is valid from its mapped event until its unmapped event, so
        // storing it across that window (here and in slots_) is the supported
        // pattern; we never deref it after unmapped.
        mapped_ = host.subscribe(
            shell_->on_toplevel_mapped(), [this](const ext_xdg_shell::ToplevelEvent& e) {
                mapped_toplevels_.push_back(e.toplevel);
                focused_ = e.toplevel; // map-focus: a freshly mapped window is focused
            });
        focused_sub_ = host.subscribe(
            shell_->on_toplevel_focused(), [this](const ext_xdg_shell::ToplevelEvent& e) {
                focused_ = e.toplevel;
            });
        unmapped_ = host.subscribe(
            shell_->on_toplevel_unmapped(), [this](const ext_xdg_shell::ToplevelEvent& e) {
                on_unmapped(e.toplevel);
            });

        // Minimize trigger (STOPGAP keybinding — Super+M). Consume the chord and
        // minimize the focused window. TODO: migrate to a config-driven
        // `minimize` action in ext-keybindings + a Service we export (post-d1;
        // change-request in the report). We do NOT trigger on Super alone (that
        // is ext-keybindings' tap-launcher).
        key_filter_ = host.subscribe(
            host.key_filter(), [this](kernel::KeyEvent ev) {
                if (ev.pressed && ev.keysym == kMinimizeKeysym &&
                    (ev.modifiers & kMinimizeMods) != 0) {
                    do_minimize_focused();
                    ev.handled = true; // consume
                }
                return ev;
            });

        // Create the dock surface up front, kept hidden until the first slot. It
        // lives on the overlay layer at the left edge; geometry from dock_layout
        // + the first output's size. The substrate is null on a no-GL backend
        // (e.g. headless pixman) — create_surface returns nullptr; we degrade
        // gracefully (model still tracked, hide/show still works, no visual).
        create_dock_surface();

        activated_ = true;
    }

private:
    // ---- minimize / restore (the c2 pipeline) -------------------------------

    void do_minimize_focused() {
        if (focused_ == nullptr) {
            return;
        }
        Toplevel* tl = focused_;

        Slot slot;
        slot.tl = tl;
        slot.title = std::string(tl->title());   // copy: title() is call-only
        slot.app_id = std::string(tl->app_id());  // copy; for // TODO favicon
        // Snapshot the window into a Preview if the substrate has a GL path.
        // create_preview borrows scene_tree() only for the call; it NEVER
        // throws and returns null on a no-GL backend (degrade: empty preview).
        if (host_->ui().available()) {
            slot.preview = host_->ui().create_preview(tl->scene_tree());
        }
        slots_.push_back(std::move(slot));

        // Hide the live window (disable its scene node — NOT unmap; it stays
        // mapped, its Toplevel* borrow stays valid for restore).
        tl->hide();

        // Move keyboard focus to another mapped, non-minimized window if any.
        Toplevel* next = first_non_minimized_other(tl);
        if (next != nullptr) {
            next->focus(); // produces on_toplevel_focused -> updates focused_
        } else {
            focused_ = nullptr; // nothing else to focus
        }

        refresh_slots();
    }

    void do_restore(std::size_t i) {
        // Guard the index — RmlUi delivers it from the document (it_index).
        if (i >= slots_.size()) {
            return;
        }
        Toplevel* tl = slots_[i].tl;
        // Drop the slot FIRST (frees its Preview texture). Then show()+focus()
        // the live toplevel (its borrow is still valid — it never unmapped).
        slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(i));
        if (tl != nullptr) {
            tl->show();
            tl->focus(); // produces on_toplevel_focused -> updates focused_
        }
        refresh_slots();
    }

    // on_toplevel_unmapped for `tl`: if it has a slot, drop it (and its Preview)
    // — NEVER refresh()/show() an unmapped toplevel (its scene tree is gone; UB
    // per ui.hpp). Also forget it from the mapped set + focus tracking.
    void on_unmapped(Toplevel* tl) {
        std::erase_if(slots_, [tl](const Slot& s) { return s.tl == tl; });
        std::erase(mapped_toplevels_, tl);
        if (focused_ == tl) {
            focused_ = nullptr;
        }
        refresh_slots();
    }

    // ---- helpers ------------------------------------------------------------

    // The first mapped window that is neither `except` nor currently minimized
    // (in a slot), for re-focusing after a minimize. Null if none.
    [[nodiscard]] auto first_non_minimized_other(Toplevel* except) -> Toplevel* {
        for (Toplevel* tl : mapped_toplevels_) {
            if (tl == except || is_minimized(tl)) {
                continue;
            }
            return tl;
        }
        return nullptr;
    }

    [[nodiscard]] auto is_minimized(Toplevel* tl) const -> bool {
        for (const Slot& s : slots_) {
            if (s.tl == tl) {
                return true;
            }
        }
        return false;
    }

    // Re-render the dock list and toggle dock visibility (it shows iff there is
    // at least one slot). No-op on the visual when the surface is null (no-GL).
    void refresh_slots() {
        if (dock_surface_ != nullptr) {
            dock_surface_->dirty("slots");
            dock_surface_->set_visible(!slots_.empty());
        }
    }

    // Create the dock UiSurface (overlay, left edge, full output height) and
    // register all data bindings BEFORE the first frame. Null surface (no-GL
    // backend) is fine — we just skip it and the model is still tracked.
    void create_dock_surface() {
        const layout::DockMetrics m = dock_metrics();
        const layout::Box frame = layout::dock_box(m, 1.0); // fully revealed (c2)

        kernel::UiSurfaceSpec spec;
        spec.rml_inline = kDockRml;
        spec.model = "ui";
        spec.x = frame.x;
        spec.y = frame.y;
        spec.width = frame.w;
        spec.height = frame.h;
        spec.layer = kernel::SceneLayer::overlay;
        spec.visible = false; // shown when slot count > 0

        dock_surface_ = host_->ui().create_surface(spec);
        if (dock_surface_ == nullptr) {
            return; // no GL path: degrade gracefully (model only)
        }

        // List bindings (b2 list-binding family). All registered BEFORE the
        // first frame, capturing only `this` (whose members outlive the
        // surface, which is destroyed before them in reverse declaration order).
        dock_surface_->bind_list(
            "slots", [this]() -> std::size_t { return slots_.size(); });
        dock_surface_->bind_list_string(
            "slots", "preview", [this](std::size_t i) -> std::string {
                if (i >= slots_.size()) {
                    return std::string{};
                }
                return slots_[i].preview ? slots_[i].preview->source_uri()
                                         : std::string{};
            });
        dock_surface_->bind_list_string(
            "slots", "title", [this](std::size_t i) -> std::string {
                return i < slots_.size() ? slots_[i].title : std::string{};
            });
        dock_surface_->bind_list_event(
            "slots", "restore", [this](std::size_t i) { do_restore(i); });
    }

    // Dock metrics from the first output's size (queried via output_layout). On
    // a backend with no output yet, falls back to 0x0 (the dock is hidden until
    // a slot exists anyway). The dock keeps its fixed width and full height.
    [[nodiscard]] auto dock_metrics() const -> layout::DockMetrics {
        int ow = 0;
        int oh = 0;
        wlr_output_layout* ol = host_->output_layout();
        if (ol != nullptr) {
            wlr_output_layout_output* lo = nullptr;
            wl_list_for_each(lo, &ol->outputs, link) {
                wlr_box box{};
                wlr_output_layout_get_box(ol, lo->output, &box);
                if (!wlr_box_empty(&box)) {
                    ow = box.width;
                    oh = box.height;
                }
                break; // first output (c2)
            }
        }
        layout::DockMetrics m;
        m.output_w = ow;
        m.output_h = oh;
        m.dock_width = kDockWidth;
        return m;
    }

    const kernel::Manifest manifest_{
        .id = "stage-dock",
        .tier = kernel::Tier::standard,
        .depends_on = {"xdg-shell"},
    };

    Host* host_ = nullptr;
    ext_xdg_shell::Service* shell_ = nullptr; // borrow; fetched in activate()
    bool activated_ = false;                  // TestProbe; set at end of activate()

    // Window tracking. Toplevel* are borrows valid mapped..unmapped; we add on
    // mapped, drop on unmapped, and only deref live ones.
    std::vector<Toplevel*> mapped_toplevels_;
    Toplevel* focused_ = nullptr;

    // The dock model. Declared BEFORE dock_surface_ so the surface (whose
    // bindings read slots_) is destroyed FIRST — slots_ (and its Previews) stay
    // valid through the surface's teardown, then drop, all before host_'s borrow
    // ends. Each Slot owns a Preview (frees its texture on erase/destruction).
    std::vector<Slot> slots_;

    // The dock ui surface. Destroyed before slots_ (declared after it) so any
    // getter invoked during its teardown still sees a live slots_; destroyed
    // before host_'s borrow ends (it is a member). Null on a no-GL backend.
    std::unique_ptr<kernel::UiSurface> dock_surface_;

    // RAII subscriptions — declared LAST so they release FIRST at teardown,
    // before the dock surface + model their callbacks touch (listener-lifetime).
    kernel::Subscription mapped_;
    kernel::Subscription focused_sub_;
    kernel::Subscription unmapped_;
    kernel::Subscription key_filter_;
};

} // namespace

auto create() -> std::unique_ptr<kernel::Extension> {
    return std::make_unique<StageDockExtension>();
}

auto make_extension_with_probe() -> ExtensionWithProbe {
    auto ext = std::make_unique<StageDockExtension>();
    TestProbe* probe = ext.get();
    return ExtensionWithProbe{.extension = std::move(ext), .probe = probe};
}

} // namespace unbox::ext_stage_dock
