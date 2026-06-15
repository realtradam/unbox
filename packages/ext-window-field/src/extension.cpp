#include <unbox/ext-window-field/ext_window_field.hpp>

#include "probe.hpp"

#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/ui.hpp>
#include <unbox/kernel/wlr.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
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
        dirty_wins();
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
    }

    // Re-read the bound list (count + every visible row field) and re-render the
    // field on the next frame. No-op when the surface is null (no-GL backend) —
    // the model is still tracked for the probe.
    void dirty_wins() {
        if (field_surface_ != nullptr) {
            field_surface_->dirty("wins");
        }
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

    // The currently focused window (a borrow valid until its unmapped). Drives
    // the per-row `focused` bool the RCSS highlights/raises. Cleared when its
    // window unmaps.
    Toplevel* focused_ = nullptr;

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

    // RAII subscriptions — declared LAST so they release FIRST at teardown,
    // before the field surface + model their callbacks touch (listener-lifetime).
    kernel::Subscription output_added_;
    kernel::Subscription mapped_;
    kernel::Subscription focused_sub_;
    kernel::Subscription unmapped_;
};

} // namespace

auto create() -> std::unique_ptr<kernel::Extension> {
    return std::make_unique<WindowFieldExtension>();
}

auto make_extension_with_probe() -> ExtensionWithProbe {
    auto ext = std::make_unique<WindowFieldExtension>();
    TestProbe* probe = ext.get();
    return ExtensionWithProbe{.extension = std::move(ext), .probe = probe};
}

} // namespace unbox::ext_window_field
