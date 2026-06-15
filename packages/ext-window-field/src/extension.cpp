#include <unbox/ext-window-field/ext_window_field.hpp>

#include "config.hpp"
#include "geometry.hpp"
#include "probe.hpp"

#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/ui.hpp>
#include <unbox/kernel/wlr.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ext-window-field glue (RML compositing Phase 2, Wave 3 — GLOSSARY: "window
// field"). This is the WHOLE unit: there is no pure decision core to doctest —
// the "what windows exist + their per-row data" decision is trivial bookkeeping
// over a vector, and the LAYOUT (the only real policy) lives entirely in RCSS
// (assets/ext-window-field/field.{rml,rcss}) per the user's contract decision.
// So this glue only DRIVES the document: it pushes the live window list + the
// focused flag and lets RCSS lay them out and animate.
//
// The pipeline:
//   on_toplevel_mapped   -> create_surface_element(wl_surface) (owns the live
//                           import), Toplevel::hide() (take it OUT of wlr_scene
//                           — the surface element is now the only compositor of
//                           those pixels), append a Window row, dirty("wins").
//   on_toplevel_unmapped -> drop the Window (frees its SurfaceElement), dirty.
//   on_toplevel_focused  -> mark that window focused, dirty (RCSS raises/
//                           highlights the focused row).
//
// Keyboard focus itself is ext-xdg-shell::Toplevel::focus()'s job (the seat
// keyboard enter); pointer/touch input-back to the client is automatic in the
// kernel (a pick on a surface element routes to its client). So this extension
// only tracks the focused flag for the RCSS highlight — it does NOT wire seat
// calls. Click-to-focus a BACKGROUND window is a known gap (the kernel does not
// yet notify the wm that a surface element was pressed); see the report's
// change-request. For Wave 3, focus follows map + on_toplevel_focused (Alt+Tab
// via ext-keybindings produces the latter).
//
// Everything runs on the single wl_event_loop thread. Every resource is a RAII
// member of WindowField; teardown is reverse-declaration destruction (no manual
// teardown lists — extension-agent.md). The Subscriptions release FIRST
// (declared last), then the field UiSurface (its bindings capture `this` and
// read windows_, so it is destroyed BEFORE windows_), then the windows_ vector
// (each Window's SurfaceElement frees its import) — all before the Host borrow
// goes away.

namespace unbox::ext_window_field {
namespace {

using kernel::Host;
using Toplevel = ext_xdg_shell::Toplevel;

// ---- config load (effect): discover + read the file, parse, fall back -------
//
// Mirrors ext-keybindings: file discovery/reading is the effect here; the pure
// parse lives in config.cpp. No readable file / parse error / bad values ->
// compiled defaults (resize_mode "settle"). Never throws.
auto read_file(const std::string& path, std::string& out) -> bool {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

auto discover_config_path(const std::optional<std::string>& explicit_path)
    -> std::optional<std::string> {
    if (explicit_path) {
        return explicit_path; // host-bin --config: use it verbatim
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::string(xdg) + "/unbox/unbox.toml";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::string(home) + "/.config/unbox/unbox.toml";
    }
    return std::nullopt;
}

// Load the window-field policy from the effective path (or compiled defaults).
// Logs every warning. Used both at activate() and on hot-reload.
auto load_policy(const std::optional<std::string>& effective_path) -> config::ResizePolicy {
    if (!effective_path) {
        wlr_log(WLR_INFO, "ext-window-field: no config path; using defaults (resize_mode settle)");
        return config::ResizePolicy{};
    }
    std::string text;
    if (!read_file(*effective_path, text)) {
        wlr_log(WLR_INFO, "ext-window-field: no config at '%s'; using defaults",
                effective_path->c_str());
        return config::ResizePolicy{};
    }
    config::LoadResult loaded = config::load_from_string(text);
    for (const std::string& w : loaded.warnings) {
        wlr_log(WLR_ERROR, "ext-window-field: %s", w.c_str());
    }
    return loaded.policy;
}

// One window in the field. The Toplevel* is a BORROW valid map..unmapped (per
// the ext-xdg-shell contract), so we may key our own tracking on it and deref
// it within that window; we drop the Window the instant on_toplevel_unmapped
// fires for it (never deref after). `element` owns the live import + the
// frame-callback duty; it is null on a no-GL backend (headless pixman) — the
// model is still tracked there (lenient-glue degrade). title/app_id are copied
// at map time (the Toplevel's title()/app_id() views are call-only).
struct Window {
    Toplevel* tl = nullptr;                              // borrow, live until unmapped
    std::unique_ptr<kernel::SurfaceElement> element;     // owns the live import (null = no GL)
    std::string title;                                   // copied at map time
    std::string app_id;                                  // copied at map time

    // FLOATING geometry STATE (field px), the bound source of truth RCSS applies
    // as data-style transform/size. Updated by the drag gesture cores; never read
    // back from the client. `z` is the stack order (higher = on top), bumped on
    // focus/raise so the active window rises.
    geom::Box box;
    int z = 0;

    // Resize-to-tile bookkeeping (the feedback loop, see apply_resize). `sent_*`
    // is the size we last configured the client to (so we re-send only on a real
    // change); `seen_*` is the element's resolved tile box observed on the
    // PREVIOUS frame (so "settle" = box unchanged since last frame). Both 0 until
    // the first laid-out frame.
    int sent_w = 0, sent_h = 0;
    int seen_w = 0, seen_h = 0;
};

// The single in-flight drag (one pointer => one move/resize at a time). Keyed on
// the Toplevel IDENTITY (not the row index) so a list reorder/erase mid-drag
// cannot misattribute it. `start_box` + `start_*` are captured on the start
// phase; every move/end recomputes from the CUMULATIVE pointer delta (the pure
// core is stateless — apply_drag(start_box, delta)).
struct DragSession {
    Toplevel* tl = nullptr;
    geom::Handle handle = geom::Handle::move;
    geom::Box start_box;
    double start_x = 0.0;
    double start_y = 0.0;
    bool active = false;
};

// The window field document lives in EXTERNAL ASSET FILES (loaded via
// UiSurfaceSpec::rml_path so layout changes need no recompile + dev hot-reload):
//   assets/ext-window-field/field.rml  — the RML STRUCTURE: data-model "ui",
//     data-for="w : wins" emitting one .win per window with the live <img
//     src="{{ w.live_uri }}">, data-class-focused="w.focused" for the RCSS
//     highlight/raise. It links field.rcss via <link type="text/rcss" ...>.
//   assets/ext-window-field/field.rcss — ALL the layout: the focused window
//     fills the field, the rest are a row of smaller tiles along the bottom; a
//     `transition` animates focus/layout changes.
// The C++ binding setup (bind_list*/bind_list_string/bool in create_field_surface)
// is re-applied by the substrate across hot-reloads.

class WindowFieldExtension final : public kernel::Extension, public TestProbe {
public:
    explicit WindowFieldExtension(std::optional<std::string> config_path)
        : config_path_(std::move(config_path)),
          effective_path_(discover_config_path(config_path_)),
          policy_(load_policy(effective_path_)) {}

    [[nodiscard]] auto manifest() const -> const kernel::Manifest& override { return manifest_; }

    // ---- TestProbe (src/probe.hpp; glue-test only) ----
    [[nodiscard]] auto activated() const -> bool override { return activated_; }
    [[nodiscard]] auto window_count() const -> std::size_t override { return windows_.size(); }
    [[nodiscard]] auto live_uri(std::size_t i) const -> std::string override {
        if (i >= windows_.size() || windows_[i].element == nullptr) {
            return std::string{};
        }
        return windows_[i].element->source_uri();
    }
    [[nodiscard]] auto has_surface_element(std::size_t i) const -> bool override {
        return i < windows_.size() && windows_[i].element != nullptr;
    }
    [[nodiscard]] auto focused_index() const -> std::ptrdiff_t override {
        return index_of(focused_);
    }
    [[nodiscard]] auto hidden_count() const -> std::size_t override { return hidden_count_; }

    void activate(Host& host) override {
        host_ = &host;

        // Fatal: a missing ext-xdg-shell Service. The field composites that
        // extension's toplevels as surface elements — meaningless without it.
        // depends_on "xdg-shell" guarantees it activated first, so absence is a
        // broken core session (extension.hpp: activation failure is fatal).
        shell_ = host.service<ext_xdg_shell::Service>();
        if (shell_ == nullptr) {
            throw std::runtime_error(
                "ext-window-field: ext-xdg-shell Service unavailable (depends_on "
                "\"xdg-shell\" not satisfied)");
        }

        // Create the window field surface up front (sized to the primary output
        // if one exists yet). Null on a no-GL backend (headless pixman) — we
        // degrade gracefully: the model is still tracked, hide() still runs,
        // there is just no composited surface.
        create_field_surface();

        // Re-size the field to a freshly-added output (Wave 3 tracks the primary
        // output only — multi-output is a follow-up; see the report). If the
        // field was created before any output existed (size 0), this gives it
        // its first real size.
        output_added_ = host.subscribe(
            host.on_output_added(), [this](const kernel::OutputEvent&) { size_to_primary_output(); });

        // Window lifecycle via the ext-xdg-shell Service. The Toplevel* borrow
        // is valid mapped..unmapped, so we key windows_ on it and only deref a
        // live one; we drop the Window on unmapped.
        mapped_ = host.subscribe(
            shell_->on_toplevel_mapped(),
            [this](const ext_xdg_shell::ToplevelEvent& e) { on_mapped(e.toplevel); });
        focused_sub_ = host.subscribe(
            shell_->on_toplevel_focused(),
            [this](const ext_xdg_shell::ToplevelEvent& e) { on_focused(e.toplevel); });
        unmapped_ = host.subscribe(
            shell_->on_toplevel_unmapped(),
            [this](const ext_xdg_shell::ToplevelEvent& e) { on_unmapped(e.toplevel); });

        // Config hot-reload: watch the EFFECTIVE config path so editing
        // unbox.toml re-applies the resize policy live, with no restart (the
        // kernel fires on_change on create too, so a later-written file is picked
        // up). A throwing callback is error-isolated by the kernel. No path -> no
        // watch (defaults stand). config_watch_ is a member: the watch lives
        // exactly as long as this extension.
        if (effective_path_) {
            config_watch_ = host.watch_file(*effective_path_, [this] { reload_config(); });
        }

        activated_ = true;
    }

private:
    // ---- window lifecycle ---------------------------------------------------

    // A toplevel mapped: composite it as a surface element + take it out of
    // wlr_scene. ext-xdg-shell focuses a freshly-mapped window, so it becomes
    // the focused window of the field (on_toplevel_focused may also fire — both
    // converge on the same value).
    void on_mapped(Toplevel* tl) {
        if (tl == nullptr) {
            return;
        }
        Window w;
        w.tl = tl;
        w.title = std::string(tl->title());   // copy: title() is call-only
        w.app_id = std::string(tl->app_id());  // copy: app_id() is call-only
        w.box = initial_placement(tl);         // floating: cascade + client's size
        w.z = ++z_counter_;                    // a freshly mapped window is on top

        // Turn the toplevel's ROOT wl_surface into a live surface element. The
        // wl_surface is a borrow valid until unmapped (we drop the element then).
        // create_surface_element returns null on a no-GL backend / failed import
        // (graceful degrade); the model is still tracked. wl_surface() returns
        // null only for an already-destroyed toplevel (never for a live mapped
        // one) — guard anyway.
        if (host_->ui().available()) {
            if (wlr_surface* surface = tl->wl_surface(); surface != nullptr) {
                w.element = host_->ui().create_surface_element(surface);
            }
        }

        // Click/tap-to-focus: a press/down routed to this element's tree focuses
        // its window. The handler captures `tl` (this window's identity, valid
        // map..unmapped) but is robust — it only acts if that window is STILL
        // tracked (index_of >= 0), so a stale fire is a harmless no-op. It is
        // tiny + non-throwing (Toplevel::focus() is no-op if unmapped) and
        // error-isolated by the substrate. focus() gives keyboard focus + fires
        // on_toplevel_focused, so focused_ updates and RCSS raises/highlights it.
        // The stored std::function dies with the element (dropped on unmap).
        if (w.element != nullptr) {
            w.element->on_pressed([this, tl] {
                if (index_of(tl) >= 0) {
                    tl->focus();
                }
            });
        }

        // Take the toplevel OUT of wlr_scene: the surface element is now the ONLY
        // compositor of those pixels (the substrate drives the client's frame
        // callbacks). hide() is NOT unmap — the client stays mapped, its Toplevel*
        // borrow stays valid, and no on_toplevel_unmapped fires.
        tl->hide();
        ++hidden_count_;

        windows_.push_back(std::move(w));
        focused_ = tl; // map-focus: a freshly mapped window is focused
        ++map_count_;
        dirty_wins();
    }

    // Initial floating box for a freshly mapped toplevel: the client's own
    // committed size (its xdg geometry — what it asked to be) clamped to the min,
    // cascaded down-right so stacked windows do not perfectly overlap, then
    // clamped into the field. A 0-size geometry (no buffer yet) falls back to a
    // sane default. Pure-ish (reads tl->geometry() + the output box).
    [[nodiscard]] auto initial_placement(Toplevel* tl) -> geom::Box {
        const geom::Limits lim;
        const wlr_box g = tl->geometry();
        geom::Box b;
        b.w = std::max(lim.min_w, g.width > 0 ? g.width : 800);
        b.h = std::max(lim.min_h, g.height > 0 ? g.height : 540);
        const int step = 36;
        const int n = static_cast<int>(map_count_ % 6);
        b.x = 60 + n * step;
        b.y = 60 + n * step;
        const wlr_box f = primary_output_box();
        return geom::clamp_to_field(b, f.width, f.height);
    }

    // A toplevel unmapping: drop its Window (frees the SurfaceElement — we must
    // NOT sample a surface element after its wl_surface is gone; UB per ui.hpp)
    // and clear focus tracking if it was the focused one. The Toplevel* borrow
    // is dead after this call — never deref it again.
    void on_unmapped(Toplevel* tl) {
        std::erase_if(windows_, [tl](const Window& w) { return w.tl == tl; });
        if (focused_ == tl) {
            focused_ = nullptr;
        }
        dirty_wins();
    }

    // Keyboard focus moved to `tl` (map-focus, click/tap-to-focus on a surface
    // element the kernel routed, or programmatic Toplevel::focus() — e.g.
    // ext-keybindings' Alt+Tab). We only update the focused flag the RCSS keys
    // its highlight/raise off; the seat keyboard enter itself is ext-xdg-shell's
    // job, already done by the time this fires.
    void on_focused(Toplevel* tl) {
        focused_ = tl;
        dirty_wins();
    }

    // ---- field surface ------------------------------------------------------

    // Create the window field UiSurface at SceneLayer::normal (the toplevel
    // band) sized to the primary output, and register all data bindings BEFORE
    // the first frame. Null surface (no-GL backend) is fine — we skip the
    // bindings and the model is still tracked.
    void create_field_surface() {
        wlr_box box = primary_output_box();

        kernel::UiSurfaceSpec spec;
        // External asset (RELATIVE to the asset root the orchestrator wires) so
        // the field document is editable without recompiling + dev hot-reloads.
        spec.rml_path = "ext-window-field/field.rml";
        spec.model = "ui";
        spec.x = box.x;
        spec.y = box.y;
        // The substrate rejects non-positive geometry; on a backend with no
        // output yet box is 0x0, so clamp to >= 1 (size_to_primary_output()
        // resizes to the real output on on_output_added).
        spec.width = std::max(1, box.width);
        spec.height = std::max(1, box.height);
        spec.layer = kernel::SceneLayer::normal; // the application toplevel band
        spec.visible = true;

        field_surface_ = host_->ui().create_surface(spec);
        if (field_surface_ == nullptr) {
            return; // no GL path: degrade gracefully (model only)
        }

        // List bindings. All registered BEFORE the first frame, capturing only
        // `this` (whose members outlive the surface, which is destroyed before
        // them in reverse declaration order). The list is named "wins"; the RML
        // iterates data-for="w : wins".
        field_surface_->bind_list(
            "wins", [this]() -> std::size_t { return windows_.size(); });
        // live_uri: the SurfaceElement's <img src> (its live texture URI). Empty
        // when the element is null (no-GL) — the RML still renders the row (RCSS
        // shows the placeholder background); the substrate ignores an empty src.
        field_surface_->bind_list_string(
            "wins", "live_uri", [this](std::size_t i) -> std::string {
                if (i >= windows_.size() || windows_[i].element == nullptr) {
                    return std::string{};
                }
                return windows_[i].element->source_uri();
            });
        // focused: the RCSS highlight/raise key (data-class-focused="w.focused").
        field_surface_->bind_list_bool(
            "wins", "focused", [this](std::size_t i) -> bool {
                return i < windows_.size() && windows_[i].tl == focused_;
            });
        // title / app_id: optional per-row strings (a future label/RCSS hook).
        field_surface_->bind_list_string(
            "wins", "title", [this](std::size_t i) -> std::string {
                return i < windows_.size() ? windows_[i].title : std::string{};
            });
        field_surface_->bind_list_string(
            "wins", "app_id", [this](std::size_t i) -> std::string {
                return i < windows_.size() ? windows_[i].app_id : std::string{};
            });

        // FLOATING geometry (field px) — the bound STATE the RML applies as
        // data-style transform/size + z-index. x/y drive a translate, w/h the
        // element box, z the stack order.
        field_surface_->bind_list_int("wins", "x", [this](std::size_t i) -> int {
            return i < windows_.size() ? windows_[i].box.x : 0;
        });
        field_surface_->bind_list_int("wins", "y", [this](std::size_t i) -> int {
            return i < windows_.size() ? windows_[i].box.y : 0;
        });
        field_surface_->bind_list_int("wins", "w", [this](std::size_t i) -> int {
            return i < windows_.size() ? windows_[i].box.w : 0;
        });
        field_surface_->bind_list_int("wins", "h", [this](std::size_t i) -> int {
            return i < windows_.size() ? windows_[i].box.h : 0;
        });
        field_surface_->bind_list_int("wins", "z", [this](std::size_t i) -> int {
            return i < windows_.size() ? windows_[i].z : 0;
        });

        // Close button (per-row click): ask the client to close. close() is a
        // request — the window stays valid until its own unmap fires.
        field_surface_->bind_list_event("wins", "close", [this](std::size_t i) {
            if (i < windows_.size()) {
                windows_[i].tl->close();
            }
        });
        // Raise (titlebar tap with no drag): focus + lift, same as a body click.
        field_surface_->bind_list_event("wins", "raise", [this](std::size_t i) {
            if (i < windows_.size()) {
                focus_and_raise(windows_[i].tl);
            }
        });

        // Drag interactions: one binding per chrome control, each baking in its
        // Handle. The row index identifies the window on the start phase; the
        // session then tracks it by identity (see on_window_drag).
        field_surface_->bind_list_drag(
            "wins", "dmove", [this](std::size_t i, kernel::UiSurface::DragPhase ph, double x, double y) {
                on_window_drag(i, geom::Handle::move, ph, x, y);
            });
        field_surface_->bind_list_drag(
            "wins", "dbl", [this](std::size_t i, kernel::UiSurface::DragPhase ph, double x, double y) {
                on_window_drag(i, geom::Handle::resize_bl, ph, x, y);
            });
        field_surface_->bind_list_drag(
            "wins", "dbr", [this](std::size_t i, kernel::UiSurface::DragPhase ph, double x, double y) {
                on_window_drag(i, geom::Handle::resize_br, ph, x, y);
            });
    }

    // ---- floating interaction (move / resize / focus) -----------------------

    // Focus + raise a window: keyboard focus (fires on_toplevel_focused -> RCSS
    // highlight) and bump its z so it sits on top. No-op for an untracked/null tl.
    void focus_and_raise(Toplevel* tl) {
        const std::ptrdiff_t idx = index_of(tl);
        if (idx < 0) {
            return;
        }
        windows_[static_cast<std::size_t>(idx)].z = ++z_counter_;
        tl->focus(); // keyboard focus + on_toplevel_focused (updates focused_)
        dirty_wins();
    }

    // A drag on a window's chrome (titlebar=move, grips=resize). Phase start
    // captures the box + pointer and resolves the row to a STABLE identity
    // (Toplevel*), focusing/raising the window; move/end recompute the box from
    // the cumulative pointer delta via the pure core and clamp it into the field.
    // Robust to a list reorder/erase mid-drag (it tracks the Toplevel, not the
    // row index). A throw is impossible here (pure math) but the substrate
    // isolates it regardless.
    void on_window_drag(std::size_t row, geom::Handle handle, kernel::UiSurface::DragPhase phase,
                        double x, double y) {
        if (phase == kernel::UiSurface::DragPhase::start) {
            if (row >= windows_.size()) {
                return;
            }
            Window& w = windows_[row];
            drag_ = DragSession{.tl = w.tl,
                                .handle = handle,
                                .start_box = w.box,
                                .start_x = x,
                                .start_y = y,
                                .active = true};
            focus_and_raise(w.tl);
            return;
        }
        // move / end: apply the cumulative delta to the start box, by identity.
        if (!drag_.active || drag_.handle != handle) {
            return; // not our session (e.g. a stale fire)
        }
        const std::ptrdiff_t idx = index_of(drag_.tl);
        if (idx < 0) {
            drag_.active = false; // the window went away mid-drag
            return;
        }
        const int dx = static_cast<int>(x - drag_.start_x);
        const int dy = static_cast<int>(y - drag_.start_y);
        geom::Box nb = geom::apply_drag(drag_.start_box, handle, dx, dy, geom::Limits{});
        const wlr_box f = primary_output_box();
        nb = geom::clamp_to_field(nb, f.width, f.height);
        windows_[static_cast<std::size_t>(idx)].box = nb;
        if (phase == kernel::UiSurface::DragPhase::end) {
            drag_.active = false;
        }
        dirty_wins(); // re-render at the new box + kick the client-resize loop
    }

    // Re-read the bound list (count + every visible row field) and re-render the
    // field on the next frame. No-op when the surface is null (no-GL backend) —
    // the model is still tracked for the probe. Also kicks the resize loop: a
    // map/unmap/focus/move/resize changes a window's box, so the client must be
    // re-sized to match (apply_resize reads the resolved <img> box and configures
    // the client).
    void dirty_wins() {
        if (field_surface_ != nullptr) {
            field_surface_->dirty("wins");
        }
        kick_resize();
    }

    // ---- resize-to-tile feedback loop ---------------------------------------

    // Start the per-frame resize pump if the policy is active and there is
    // something to do. Cheap + idempotent: a no-op when mode==off, no surface
    // (no-GL), or already pumping. Called whenever the layout may change
    // (dirty_wins, size_to_primary_output, reload). The pump stops itself once
    // every window's tile box has settled (apply_resize releases the handle).
    void kick_resize() {
        if (policy_.mode == config::ResizeMode::off || field_surface_ == nullptr ||
            host_ == nullptr || resize_frames_.active()) {
            return;
        }
        debounce_accum_ = 0.0;
        resize_frames_ = host_->request_frames([this](double dt) { apply_resize(dt); });
    }

    // Per-frame: read each window's RCSS-resolved tile box (rendered_width/height
    // — the kernel reading back the rectangle RCSS laid the <img> out to) and,
    // per the policy, configure its client to that size so the live texture fills
    // the tile 1:1 instead of being scaled into it. Runs only while the pump is
    // active; releases the pump (stops the frame clock) once all windows have
    // settled, so there is no busy render at rest.
    void apply_resize(double dt_seconds) {
        if (policy_.mode == config::ResizeMode::off) {
            resize_frames_.reset();
            return;
        }
        debounce_accum_ += dt_seconds;
        const double debounce_s = static_cast<double>(policy_.debounce_ms) / 1000.0;

        bool all_settled = true;
        bool sent_this_frame = false;
        for (Window& w : windows_) {
            if (w.element == nullptr) {
                continue; // no-GL: nothing to size
            }
            const int cw = w.element->rendered_width();
            const int ch = w.element->rendered_height();
            if (cw <= 0 || ch <= 0) {
                all_settled = false; // not laid out yet; keep pumping
                continue;
            }
            const bool stable = (cw == w.seen_w && ch == w.seen_h); // unchanged since last frame
            const bool needs = (cw != w.sent_w || ch != w.sent_h);  // differs from last configured

            bool do_send = false;
            switch (policy_.mode) {
                case config::ResizeMode::continuous:
                    do_send = needs;
                    break;
                case config::ResizeMode::settle:
                    do_send = needs && stable;
                    break;
                case config::ResizeMode::debounced:
                    do_send = needs && (stable || debounce_accum_ >= debounce_s);
                    break;
                case config::ResizeMode::off:
                    break;
            }
            if (do_send) {
                w.tl->set_size(cw, ch);
                w.sent_w = cw;
                w.sent_h = ch;
                sent_this_frame = true;
            }
            // Settled iff the box is stable AND matches what we last sent (nothing
            // left to do for this window). The tile box is pure RCSS layout — it
            // does NOT depend on the client's buffer size — so once we have sent
            // the stable size there is no feedback that could re-dirty it.
            if (!stable || cw != w.sent_w || ch != w.sent_h) {
                all_settled = false;
            }
            w.seen_w = cw;
            w.seen_h = ch;
        }
        if (sent_this_frame) {
            debounce_accum_ = 0.0;
        }
        if (all_settled) {
            resize_frames_.reset(); // stop the frame clock until the next layout change
        }
    }

    // Re-read the effective config file and swap the live policy. Mirrors
    // ext-keybindings' reload: error-isolated (load_policy never throws), keeps
    // the loop coherent. A switch to "off" lets apply_resize release the pump on
    // its next tick; any other mode re-kicks the pump so the new policy takes
    // hold immediately. Last-sent sizes are retained (no spurious reconfigure).
    void reload_config() {
        if (!effective_path_) {
            return;
        }
        policy_ = load_policy(effective_path_);
        wlr_log(WLR_INFO, "ext-window-field: config reloaded (resize_mode=%d) from '%s'",
                static_cast<int>(policy_.mode), effective_path_->c_str());
        kick_resize();
    }

    // Resize/reposition the field to the primary output's box. Called on
    // on_output_added. No-op when the surface is null or the box is empty.
    void size_to_primary_output() {
        if (field_surface_ == nullptr) {
            return;
        }
        wlr_box box = primary_output_box();
        if (box.width <= 0 || box.height <= 0) {
            return;
        }
        field_surface_->set_position(box.x, box.y);
        field_surface_->set_size(box.width, box.height);
        // The field resized -> every %-based tile box changed -> re-size clients.
        kick_resize();
    }

    // The primary (first) output's box in layout coords, or an empty box (0x0)
    // when no output exists yet. Wave 3 sizes the single field to the primary
    // output; multi-output (one field per output) is a follow-up.
    [[nodiscard]] auto primary_output_box() const -> wlr_box {
        wlr_box box{};
        wlr_output_layout* ol = host_->output_layout();
        if (ol == nullptr) {
            return box;
        }
        wlr_output_layout_output* lo = nullptr;
        wl_list_for_each(lo, &ol->outputs, link) {
            wlr_box b{};
            wlr_output_layout_get_box(ol, lo->output, &b);
            if (!wlr_box_empty(&b)) {
                box = b;
            }
            break; // primary output only (Wave 3)
        }
        return box;
    }

    // ---- helpers ------------------------------------------------------------

    // The list index of toplevel `tl`, or -1 if it is not tracked / is null.
    [[nodiscard]] auto index_of(Toplevel* tl) const -> std::ptrdiff_t {
        if (tl == nullptr) {
            return -1;
        }
        for (std::size_t i = 0; i < windows_.size(); ++i) {
            if (windows_[i].tl == tl) {
                return static_cast<std::ptrdiff_t>(i);
            }
        }
        return -1;
    }

    const kernel::Manifest manifest_{
        .id = "window-field",
        .tier = kernel::Tier::core,
        .depends_on = {"xdg-shell"},
    };

    Host* host_ = nullptr;
    ext_xdg_shell::Service* shell_ = nullptr; // borrow; fetched in activate()
    bool activated_ = false;                  // TestProbe; set at end of activate()
    std::size_t hidden_count_ = 0;            // # windows taken out of wlr_scene

    // Config (resize-to-tile policy). config_path_ is the explicit --config (or
    // none); effective_path_ is the resolved file actually loaded + watched.
    // policy_ is the live policy, swapped on hot-reload; it is read by
    // apply_resize, so it must outlive resize_frames_ (declared early => destroyed
    // late). debounce_accum_ accumulates frame dt for the "debounced" mode.
    std::optional<std::string> config_path_;
    std::optional<std::string> effective_path_;
    config::ResizePolicy policy_;
    double debounce_accum_ = 0.0;

    // The currently focused window (a borrow valid until its unmapped). Drives
    // the per-row `focused` bool the RCSS highlights/raises. Cleared when its
    // window unmaps.
    Toplevel* focused_ = nullptr;

    // Floating-window bookkeeping. z_counter_ is the monotonic stack-order source
    // (each focus/raise/map assigns ++z_counter_ so the active window is on top).
    // map_count_ drives the cascade placement offset. drag_ is the single in-
    // flight move/resize session (keyed by Toplevel identity, not row index).
    int z_counter_ = 0;
    std::size_t map_count_ = 0;
    DragSession drag_;

    // The window model. Declared BEFORE field_surface_ so the surface (whose
    // bindings read windows_) is destroyed FIRST — windows_ (and its
    // SurfaceElements) stay valid through the surface's teardown, then drop, all
    // before host_'s borrow ends. Each Window owns a SurfaceElement (frees its
    // import on erase/destruction).
    std::vector<Window> windows_;

    // The window field ui surface. Destroyed before windows_ (declared after it)
    // so any binding getter invoked during its teardown still sees a live
    // windows_; destroyed before host_'s borrow ends (it is a member). Null on a
    // no-GL backend.
    std::unique_ptr<kernel::UiSurface> field_surface_;

    // RAII handles — declared LAST so they release FIRST at teardown, before the
    // field surface + model their callbacks touch (listener-lifetime). The frame
    // pump (apply_resize) reads windows_/field_surface_/policy_, and the config
    // watch (reload_config) re-kicks it, so both must stop before those die.
    kernel::FrameRequest resize_frames_;
    kernel::FileWatch config_watch_;
    kernel::Subscription output_added_;
    kernel::Subscription mapped_;
    kernel::Subscription focused_sub_;
    kernel::Subscription unmapped_;
};

} // namespace

auto create(std::optional<std::string> config_path) -> std::unique_ptr<kernel::Extension> {
    return std::make_unique<WindowFieldExtension>(std::move(config_path));
}

auto make_extension_with_probe() -> ExtensionWithProbe {
    // Tests do not exercise config: no path -> compiled defaults (resize_mode
    // settle), no watch. The headless backend has no GL substrate, so the resize
    // loop has no elements to size anyway (apply_resize is a no-op there).
    auto ext = std::make_unique<WindowFieldExtension>(std::nullopt);
    TestProbe* probe = ext.get();
    return ExtensionWithProbe{.extension = std::move(ext), .probe = probe};
}

} // namespace unbox::ext_window_field
