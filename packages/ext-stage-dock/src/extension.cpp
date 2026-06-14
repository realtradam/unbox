#include <unbox/ext-stage-dock/ext_stage_dock.hpp>

#include "dock_layout.hpp"
#include "gesture.hpp"
#include "probe.hpp"
#include "reveal.hpp"

#include "anim.hpp"

#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/frames.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/ui.hpp>
#include <unbox/kernel/wlr.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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

// The dock width (revealed) in px. The rail is kDockWidth wide x the full output
// height tall (dock_box). d1 animates the reveal via the body translateX in RCSS
// (not by resizing the surface). 288 = the original 240 widened ~20% so the
// horizontal gradient (dark left -> transparent right) extends farther right;
// the cards stay 224dp (RCSS) left-aligned, so the extra width is to their right.
constexpr int kDockWidth = 288;

// d1-fix C++-driven slide animation. The dock open/close is animated in C++
// (the SlideAnimator pure core), NOT by RmlUi: RmlUi only starts a CSS
// transition on a class/definition change, never on the inline
// data-style-transform we drive `slide` with, so the keyboard/minimize/restore
// paths used to snap. We read the duration + tween from the RCSS `transition` on
// #panel (UiSurface::transition_timing) so they stay hot-reloadable; these are
// the FALLBACKS used when the document has not rendered a frame yet (so computed
// values do not exist) or authors no transition. kFallbackDurS matches the
// authored 0.36s; the fallback ease is linear (RmlUi 6.2 has no `linear`
// keyword, so the RCSS carrier uses cubic-in-out — see dock.rcss).
constexpr double kFallbackDurS = 0.36; // seconds, matches dock.rcss #panel

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

// The dock document now lives in EXTERNAL ASSET FILES (loaded via
// UiSurfaceSpec::rml_path so design changes need no recompile + dev hot-reload):
//   assets/ext-stage-dock/dock.rml   — the RML STRUCTURE (data-model "ui",
//     data-for="row : slots", the rounded overflow:hidden div.slot, the
//     full-bleed div.thumb with data-style-decorator image(...), the display:none
//     title + its {{ row.title }} binding, data-event-click restore, the d1
//     transform/translateX reveal body). It links the styles via
//     <link type="text/rcss" href="dock.rcss"/> (RmlUi resolves href relative to
//     the document's own dir, which the kernel asset root sets up).
//   assets/ext-stage-dock/dock.rcss  — ALL the RCSS (body.dock, div.slot,
//     div.thumb, span.title, @keyframes slot-enter, …).
// The C++ binding setup (bind_list*/bind_string/bind_event in create_dock_surface)
// is UNCHANGED — the substrate re-applies the bindings across hot-reloads.
//
// The design rationale that was inlined here is preserved in the asset files'
// own comments + the report (transparent per-pixel-alpha strip; the card is a
// rounded overflow:hidden clip container with a full-bleed child carrying the
// image() decorator — an element's OWN decorator is not clipped to its OWN
// border-radius, so the preview rides on a child; image( <uri> cover center
// center ) fit/align verified against vendored DecoratorTiled.cpp:220-251; the
// -2dp thumb overscan clipped by the rounded overflow; d1 slot-enter animation;
// transform-origin 0% 0%).

class StageDockExtension final : public kernel::Extension, public TestProbe,
                                  public ext_stage_dock::Service {
public:
    auto manifest() const -> const kernel::Manifest& override { return manifest_; }

    // ---- TestProbe (src/probe.hpp; glue-test only) ----
    [[nodiscard]] auto activated() const -> bool override { return activated_; }
    void minimize_focused() override { do_minimize_focused(); }
    void restore(std::size_t i) override { do_restore(i); }
    [[nodiscard]] auto slot_count() const -> std::size_t override { return slots_.size(); }
    [[nodiscard]] auto has_focused() const -> bool override { return focused_ != nullptr; }

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
        // is ext-keybindings' tap-launcher). When NOTHING is focused there is
        // nothing to minimize, so we do NOT consume the chord (let the key pass
        // rather than silently eating it — minor UX, brief §Also).
        key_filter_ = host.subscribe(
            host.key_filter(), [this](kernel::KeyEvent ev) {
                if (ev.pressed && ev.keysym == kMinimizeKeysym &&
                    (ev.modifiers & kMinimizeMods) != 0 && focused_ != nullptr) {
                    do_minimize_focused();
                    ev.handled = true; // consume (only when we actually acted)
                }
                return ev;
            });

        // e1 OPEN path — the kernel touch bus. The dock is HIDDEN when a finger
        // lands at the left edge, so the implicit-grab contract (host.hpp:242-244)
        // routes the WHOLE down->motion->up/cancel stream to these subscriptions
        // even after we make the dock visible mid-drag. Feed each event to the
        // Controller and apply the side-effects it returns. The Controller gates
        // on edge-slop / already-open itself (empty Outcome = ignored).
        touch_down_ = host.subscribe(
            host.on_touch_down(), [this](const kernel::TouchDownEvent& e) {
                // Edge-swipe OPEN begins: a finger landed at the edge. This is a
                // finger-DRIVEN scrub (the finger is the clock), so stop any
                // running animation frame loop and scrub from here.
                apply_scrub(controller_.touch_down(e.touch_id, e.lx, e.ly, e.time_msec),
                            /*on_drag_start=*/true);
            });
        touch_motion_ = host.subscribe(
            host.on_touch_motion(), [this](const kernel::TouchMotionEvent& e) {
                apply_scrub(controller_.touch_motion(e.touch_id, e.lx, e.ly, e.time_msec),
                            /*on_drag_start=*/false);
            });
        touch_up_ = host.subscribe(
            host.on_touch_up(), [this](const kernel::TouchUpEvent& e) {
                // RELEASE = "resume": ease from the current finger value to the
                // snap target the controller decided (open or closed). The
                // animator's completion hides the surface on a CLOSE (on_frame).
                apply_release(controller_.touch_up(e.touch_id, e.time_msec));
            });
        touch_cancel_ = host.subscribe(
            host.on_touch_cancel(), [this](const kernel::TouchCancelEvent& e) {
                apply_release(controller_.touch_cancel(e.touch_id));
            });

        // Create the dock surface up front, kept hidden until the first slot. It
        // lives on the overlay layer at the left edge; geometry from dock_layout
        // + the first output's size. The substrate is null on a no-GL backend
        // (e.g. headless pixman) — create_surface returns nullptr; we degrade
        // gracefully (model still tracked, hide/show still works, no visual).
        create_dock_surface();

        // Register the Service so ext-keybindings (or any extension) can drive
        // dock policy — toggle_visible, and future minimize/restore — through
        // the typed cross-extension coupling (no strings, link-time safety).
        host.provide_service<ext_stage_dock::Service>(this);

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
            tl->focus();
            // Re-establish focused_ DIRECTLY rather than relying on focus()
            // re-emitting on_toplevel_focused. KEY: hide() never moved keyboard
            // focus at the seat, so when we minimize the only window the seat
            // STILL holds focus on the (now hidden) window; our local focused_
            // went nullptr only because first_non_minimized_other() found no
            // other window. On restore, tl->focus() is then focusing the
            // already-seat-focused window, which the kernel treats as a no-op and
            // does NOT re-emit on_toplevel_focused — leaving focused_ stale at
            // nullptr, so the next Super+M (guarded on focused_ != nullptr) was a
            // no-op until a new map set focused_. We KNOW tl is the focused window
            // now (we just restored + focused it), so set it here. If the event
            // does also fire it just re-sets the same value (idempotent).
            focused_ = tl;
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

    // ---- Service: toggle_visible -------------------------------------------
    void toggle_visible() override { do_toggle_visible(); }

    void do_toggle_visible() {
        if (dock_surface_ == nullptr) {
            return;
        }
        if (controller_.open()) {
            // Close: animate the slide-out; on_frame hides the surface once the
            // CLOSE animation completes (replaces the old dock_settled hide).
            apply_animated(controller_.close_now());
        } else {
            // Open: make the surface visible (open_now sets make_visible) then
            // animate the slide-in. apply_animated honours make_visible FIRST.
            apply_animated(controller_.open_now());
        }
    }

    // ---- d1-fix glue: C++-driven slide animation ---------------------------
    // The animation is owned in C++ (anim::SlideAnimator), driven once per frame
    // by a kernel FrameRequest, with the duration + tween READ FROM RCSS so they
    // stay hot-reloadable. There are three routing paths a controller Outcome can
    // take, distinguished by who is the "clock":
    //   * apply_animated — the keyboard/minimize/restore open/close paths: PLAY
    //     the animation from the current slide value to the controller's target.
    //   * apply_scrub    — the finger-driven drag-MOVE (and the OPEN drag start):
    //     the finger IS the clock, so set slide directly, no animation.
    //   * apply_release  — a drag/touch RELEASE: RESUME the animation from the
    //     current finger value to the snapped target the controller decided.
    // All three no-op the visual when the surface is null (no-GL backend); the
    // controller state still advances for the model/probe.

    // PLAY: animate the dock to the controller's freshly-set target slide value.
    // make_visible FIRST (a hidden surface is not composited, so it cannot be
    // seen sliding in), then start the run from the CURRENT slide_px_ (so an
    // interrupted animation continues smoothly) to controller_.slide_px().
    void apply_animated(const gesture::Outcome& o) {
        if (dock_surface_ == nullptr) {
            return;
        }
        if (o.make_visible) {
            dock_surface_->set_visible(true);
        }
        animate_to(controller_.slide_px(), /*release_scale=*/false);
    }

    // SCRUB: the finger drives slide directly (drag-move, and the edge-swipe OPEN
    // start). set_immediate() cancels any in-flight run; on a drag START we also
    // stop the frame loop (the brief: "reset any running FrameRequest on drag
    // start") so a prior keyboard animation does not fight the finger.
    void apply_scrub(const gesture::Outcome& o, bool on_drag_start) {
        if (dock_surface_ == nullptr) {
            return;
        }
        if (on_drag_start && frame_.active()) {
            frame_.reset(); // stop the animation loop; the finger takes over
        }
        if (o.make_visible) {
            dock_surface_->set_visible(true);
        }
        if (o.dirty_slide) {
            slide_px_ = controller_.slide_px();
            anim_.set_immediate(slide_px_);
            dock_surface_->dirty("slide");
        }
    }

    // RESUME: a release eases from the current finger value to the snapped target
    // the controller decided. Scaled duration (release_scale) so a near-complete
    // drag finishes quickly. make_visible honoured (an OPEN-commit release).
    void apply_release(const gesture::Outcome& o) {
        if (dock_surface_ == nullptr) {
            return;
        }
        if (o.make_visible) {
            dock_surface_->set_visible(true);
        }
        animate_to(controller_.slide_px(), /*release_scale=*/true);
    }

    // Start a run to `target` px. Reads the RCSS-authored duration + tween from
    // #panel's `transition: transform` (hot-reloadable) with a sane fallback
    // (kFallbackDurS, linear) when the document has not rendered yet / authors no
    // transition. `release_scale` shrinks the duration by the fraction of the
    // dock width still to travel, so a drag released near the snap target settles
    // fast (a full-distance move keeps the full duration). Arms the per-frame
    // FrameRequest if it is not already running.
    void animate_to(double target, bool release_scale) {
        const double from = slide_px_;

        double duration = kFallbackDurS;
        std::function<float(float)> ease; // null == linear (the fallback)
        if (auto t = dock_surface_->transition_timing("panel", "transform")) {
            duration = t->duration > 0.0 ? t->duration : kFallbackDurS;
            ease = std::move(t->ease);
        }

        if (release_scale) {
            // Scale by the remaining-distance fraction of the full dock width so
            // a near-finished drag completes quickly. dock_width is the full
            // travel; clamp to [0,1] (the recognizer can overshoot in theory).
            const double width = static_cast<double>(kDockWidth);
            if (width > 0.0) {
                const double frac = std::clamp(std::abs(target - from) / width, 0.0, 1.0);
                duration *= frac;
            }
        }

        anim_.start(from, target, duration, std::move(ease));
        // A zero/degenerate-duration run already landed on the target (no frames
        // needed); reflect it now and skip arming the loop.
        if (!anim_.active()) {
            slide_px_ = anim_.value();
            dock_surface_->dirty("slide");
            on_animation_finished();
            return;
        }
        if (!frame_.active()) {
            frame_ = host_->request_frames([this](double dt) { on_frame(dt); });
        }
    }

    // The per-frame tick (runs BEFORE the surface renders each frame while the
    // FrameRequest is held): advance the animator, push the value into the
    // `slide` binding, and when the run completes stop the frame loop (don't hold
    // frames at rest) + run the completion hook (the CLOSE-hide).
    void on_frame(double dt) {
        if (dock_surface_ == nullptr) {
            frame_.reset();
            return;
        }
        slide_px_ = anim_.tick(dt);
        dock_surface_->dirty("slide");
        if (!anim_.active()) {
            on_animation_finished();
            frame_.reset(); // idle: stop scheduling frames
        }
    }

    // Run when a play/resume animation reaches its target. If we animated to the
    // CLOSED state (dock not open), hide the surface NOW — this REPLACES the old
    // dock_settled/transitionend hide. Guarded on !controller_.open() so a reveal
    // that raced a conceal (reopened mid-animation) does not hide an open dock.
    void on_animation_finished() {
        if (dock_surface_ != nullptr && !controller_.open()) {
            dock_surface_->set_visible(false);
        }
    }

    // A monotonic millisecond clock for the CLOSE path: UiSurface::bind_drag
    // carries no time_msec, but the recognizer needs a time base for its fling
    // velocity. steady_clock keeps it consistent in shape with the bus path
    // (which supplies time_msec) — only relative deltas matter to the recognizer.
    [[nodiscard]] static auto now_ms() -> std::uint32_t {
        const auto t = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t).count());
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

    // Re-render the dock list and ANIMATE the dock reveal. The dock is revealed
    // iff there is at least one slot. The slide is value-driven: these non-gesture
    // call sites flow through the one Controller (which sets the OPEN/CLOSED target
    // px) and then the C++ SlideAnimator (apply_animated), which eases slide_px_
    // from its current value to the target over frames (the d1 fix — RmlUi never
    // animated the inline transform):
    //   empty -> non-empty: open_now() (make the surface visible FIRST — a hidden
    //     surface is not composited, so it can't be seen sliding in — then animate
    //     the slide-in via apply_animated).
    //   non-empty -> empty: close_now() animates the slide-out; on_frame hides the
    //     surface once the CLOSE animation completes (replaces the old transitionend
    //     dock_settled hide).
    // The surface is a fixed full-height rail; its size never changes with the
    // card count (the RCSS scrolls/centers the cards within it). Only visibility
    // toggles: shown when there is >= 1 slot, hidden (after the slide-out) when
    // empty so the rail is not always eating the left strip.
    //
    // No-op on the visual when the surface is null (no-GL backend); the model is
    // still tracked, and slot_count()/the c2 invariants are unchanged.
    void refresh_slots() {
        if (dock_surface_ == nullptr) {
            return;
        }
        dock_surface_->dirty("slots");

        const bool want_open = !slots_.empty();
        if (want_open == controller_.open()) {
            return; // reveal state unchanged (e.g. minimize a 2nd window)
        }
        if (want_open) {
            // Reveal: composite before animating, then animate the body in.
            apply_animated(controller_.open_now());
        } else {
            // Conceal: animate the body out; on_frame hides it on completion.
            apply_animated(controller_.close_now());
        }
    }

    // Create the dock UiSurface (overlay, left edge) and register all data
    // bindings BEFORE the first frame. The surface is a FULL-HEIGHT left RAIL:
    // kDockWidth (240) wide x the full OUTPUT HEIGHT tall, at the output's
    // top-left, REGARDLESS of card count (the RCSS owns the in-rail flow:
    // flex-column centering + overflow-y scroll). It is hidden (spec.visible =
    // false) until the first slot, so the rail only appears when there are
    // minimized windows (it does not always eat the left strip). set_size never
    // changes the height afterwards — only visibility toggles.
    //
    // ACCEPTED CAVEAT (deferred): while shown, the full-height surface captures
    // pointer/touch across the whole 240px left strip — windows under it there
    // are not clickable. Width is kept minimal (240). The real fix is the
    // deferred input-transparent UiSurfaceSpec flag (report change-req).
    //
    // Null surface (no-GL backend) is fine — we just skip it and the model is
    // still tracked.
    void create_dock_surface() {
        const layout::DockMetrics m = dock_metrics();
        const layout::Box frame = layout::dock_box(m, 1.0); // full-height rail

        kernel::UiSurfaceSpec spec;
        // External asset (RELATIVE to the asset root the orchestrator wires) so
        // the dock document is editable without recompiling + dev hot-reloads.
        spec.rml_path = "ext-stage-dock/dock.rml";
        spec.model = "ui";
        spec.x = frame.x;
        spec.y = frame.y;
        spec.width = frame.w;
        // Full output height. Guard >= 1: the substrate rejects non-positive
        // geometry, and on a backend with no output yet frame.h could be 0 (the
        // dock is hidden until a slot exists anyway).
        spec.height = std::max(1, frame.h);
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

        // Reveal binding (registered before the first frame, same rule as the
        // list bindings; captures only `this`, whose members outlive the surface).
        // `slide` drives the panel's data-style-transform translateX(px). It now
        // reads the GLUE-OWNED slide_px_ (driven by the C++ SlideAnimator, or set
        // directly during a finger scrub) rather than the controller's value: the
        // animator interpolates between the controller's open/closed TARGETS over
        // time. NOTE (d1 fix): the old `dragging` (data-class-dragging) +
        // `dock_settled` (transitionend) bindings are GONE — RmlUi never animated
        // the inline transform, so the class-toggle machinery was dead; the C++
        // animator's completion (on_frame) now does the close-hide instead.
        dock_surface_->bind_double("slide", [this]() -> double { return slide_px_; });

        // CLOSE/scrub path — UiSurface::bind_drag. The OPEN dock is a visible ui
        // surface, so the substrate captures its touches into our RMLUi document
        // (NOT the kernel bus); the body opts into dragging via RCSS `drag: drag;`
        // and authors data-event-dragstart/drag/dragend all naming "dock_drag".
        // x/y are surface-LOCAL document px — fed straight to the recognizer (no
        // layout-origin subtract). bind_drag carries no time, so we stamp a
        // monotonic ms clock. A tap still fires data-event-click -> restore(), so
        // tap-to-restore coexists. drag-START/-MOVE SCRUB slide_px_ directly (the
        // finger is the clock); drag-END RESUMES the animator from the current
        // finger value to the snapped target (apply_release).
        dock_surface_->bind_drag(
            "dock_drag", [this](kernel::UiSurface::DragPhase p, double x, double y) {
                switch (p) {
                case kernel::UiSurface::DragPhase::start:
                    // Finger takes over: stop any running animation loop + scrub.
                    apply_scrub(controller_.drag_start(x, y, now_ms()),
                                /*on_drag_start=*/true);
                    break;
                case kernel::UiSurface::DragPhase::move:
                    apply_scrub(controller_.drag_move(x, y, now_ms()),
                                /*on_drag_start=*/false);
                    break;
                case kernel::UiSurface::DragPhase::end:
                    // Resume: ease from the finger value to the snapped target.
                    apply_release(controller_.drag_end(now_ms()));
                    break;
                }
            });

        // Seed the output geometry into the Controller and set the CLOSED target
        // so the very first render shows the dock off-screen (translateX(
        // -dock_width)) — matching spec.visible=false. close_now() leaves open_
        // false and the closed offset as the target; seed slide_px_ to it directly
        // (no animation at create) so the first frame renders the dock off-screen.
        controller_.set_metrics(m);
        controller_.close_now();
        slide_px_ = controller_.slide_px();
        anim_.set_immediate(slide_px_);
        dock_surface_->dirty("slide");
    }

    // Dock metrics from the first output's size (queried via output_layout). On
    // a backend with no output yet, falls back to 0x0 (the dock is hidden until
    // a slot exists anyway). dock_box() turns these into the full-height rail
    // rect (kDockWidth wide x output height tall, at the output top-left).
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

    // e1 gesture state. The Controller (src/gesture.hpp) is the pure decision
    // core: it owns the recognizer + the event->state transition (open_, the snap
    // commit, and the open/closed TARGET slide_px) for BOTH input sources (the
    // kernel touch bus = OPEN, UiSurface::bind_drag = CLOSE). The glue feeds it
    // events and routes the resulting target through the SlideAnimator. Declared
    // BEFORE dock_surface_ (like slots_) so it stays alive while the surface tears
    // down. Constructed with the real dock width (kDockWidth) + default recognizer
    // tunables; set_metrics() seeds the output geometry once an output exists.
    gesture::Controller controller_{
        reveal::RevealConfig{.dock_width = kDockWidth},
        layout::DockMetrics{.dock_width = kDockWidth}};

    // d1-fix C++ slide animation state. slide_px_ is the LIVE translateX px the
    // `slide` binding reads — driven by anim_ (the SlideAnimator) for the
    // keyboard/minimize/restore/drag-release paths, or set directly during a
    // finger scrub. anim_ holds the current run; frame_ is the kernel per-frame
    // tick that advances it (armed only while a run is live; reset at rest).
    // slide_px_ + anim_ are declared BEFORE dock_surface_ so the `slide` getter
    // reading slide_px_ stays valid through the surface's teardown.
    double slide_px_ = 0.0;     // live translateX px (closed = -dock_width, open = 0)
    anim::SlideAnimator anim_;  // the interruptible easing run

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
    // e1 OPEN path: the kernel touch bus. Held as members so they unsubscribe on
    // teardown before the controller they feed is gone.
    kernel::Subscription touch_down_;
    kernel::Subscription touch_motion_;
    kernel::Subscription touch_up_;
    kernel::Subscription touch_cancel_;

    // The per-frame animation tick. Declared LAST (with the subscriptions) so it
    // is destroyed FIRST at teardown: its callback captures `this` and touches
    // dock_surface_ / anim_ / slide_px_ / controller_, so it must stop before any
    // of them are gone (same listener-lifetime rule as the Subscriptions).
    kernel::FrameRequest frame_;
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
