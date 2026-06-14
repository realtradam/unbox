#pragma once

#include <unbox/kernel/ui.hpp>
#include <unbox/kernel/wlr.hpp>

#include "ui_core.hpp"

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

// The real ui substrate (slice 5) — the kernel's RMLUi subsystem behind the
// typed <unbox/kernel/ui.hpp> facade. Replaces the slice-3 ui spike. PRIVATE
// to the kernel; the only contract is ui.hpp.
//
// One sibling GLES 3.2 EGL context (shared wlr EGLDisplay), one Rml::Initialise
// and one font atlas are shared across ALL ui surfaces (the 3.7 GiB budget:
// one atlas). Each ui surface owns its own Rml::Context + offscreen FBO +
// wlr_buffer + wlr_scene_buffer node, so per-surface damage is independent. The
// proven slice-3 bridge mechanics (Plan A dmabuf-backed FBO, Plan B shm copy,
// the SetOutputFramebuffer flip for upright buffers) are reused per surface;
// the per-frame glFinish is replaced with an EGL fence (EGL_KHR_fence_sync).
//
// Everything runs on the single wl_event_loop thread. Surfaces created via the
// public facade carry the OWNING extension id so a throwing data-event callback
// disables that extension (the substrate calls back into the kernel's
// DisableSink). The kernel drives rendering from the output frame handler
// (tick_all) and routes input via the route_* methods (consume-or-pass).

// The adapted RMLUi GL3 render interface lives in the GLOBAL namespace
// (src/rmlui_renderer_gl3.h, mirroring upstream). Forward-declared here so the
// substrate can hold a unique_ptr to it without the header pulling RMLUi in;
// the full type is included only in ui_substrate.cpp.
class RenderInterface_GL3;

namespace unbox::kernel {

// The kernel's shared file watcher (src/file_watcher.hpp); the substrate borrows
// it for (UNBOX_DEV-gated) asset hot-reload. Forward-declared to keep the header
// free of inotify internals.
class FileWatcher;

// Callback the substrate invokes to disable an extension whose data-event
// callback threw — injected by the kernel (Server::Impl). Mirrors the bus's
// detail::DisableSink but scoped to the substrate so ui.hpp carries no kernel
// internals.
using SubstrateDisableFn = std::function<void(ExtensionId)>;

// Callback the substrate invokes to ask the kernel to schedule an output frame
// — the dirty-gate kick for live surface elements (a client commit, or the
// continuous frame-callback loop while >=1 element exists). Injected by the
// kernel (Server::Impl::schedule_driver_frame). No-op-safe before any output.
using SubstrateScheduleFn = std::function<void()>;

class Substrate; // the concrete UiSubstrate, defined in ui_substrate.cpp

// One ui surface's private state (Rml context + GL target + scene node +
// bindings). Defined in ui_substrate.cpp; declared here so Substrate can own a
// list of them and the public SurfaceHandle can borrow one.
struct Surface;

// One preview's private state (snapshot dmabuf + imported EGLImage/texture +
// RmlUi URI registration). Defined in ui_substrate.cpp; declared here so the
// public PreviewHandle can borrow one.
struct PreviewState;

// One LIVE surface element's private state (the client wl_surface's seq-gated
// zero-copy import + EGLImage/texture + RmlUi URI registration + the RAII commit
// listener that dirties on a client commit). Defined in ui_substrate.cpp;
// declared here so the public SurfaceElementHandle can borrow one.
struct SurfaceElementState;

// Concrete Preview handed to an extension. A thin, owning handle over a
// PreviewState that lives in the Substrate's list; destruction frees the GL
// texture + EGLImage + dmabuf and unregisters the URI.
class PreviewHandle final : public Preview {
public:
    PreviewHandle(Substrate* substrate, PreviewState* state)
        : substrate_(substrate), state_(state) {}
    ~PreviewHandle() override;
    PreviewHandle(const PreviewHandle&) = delete;
    auto operator=(const PreviewHandle&) -> PreviewHandle& = delete;

    [[nodiscard]] auto source_uri() const -> std::string override;
    [[nodiscard]] auto source_width() const -> int override;
    [[nodiscard]] auto source_height() const -> int override;
    void refresh() override;

private:
    Substrate* substrate_;
    PreviewState* state_;
};

// Concrete SurfaceElement handed to an extension. A thin, owning handle over a
// SurfaceElementState that lives in the Substrate's list; destruction drops the
// live import (texture + EGLImage + held buffer lock), the commit listener, and
// the URI registration, and ends the frame-callback duty for its surface.
class SurfaceElementHandle final : public SurfaceElement {
public:
    SurfaceElementHandle(Substrate* substrate, SurfaceElementState* state)
        : substrate_(substrate), state_(state) {}
    ~SurfaceElementHandle() override;
    SurfaceElementHandle(const SurfaceElementHandle&) = delete;
    auto operator=(const SurfaceElementHandle&) -> SurfaceElementHandle& = delete;

    [[nodiscard]] auto source_uri() const -> std::string override;
    [[nodiscard]] auto width() const -> int override;
    [[nodiscard]] auto height() const -> int override;

private:
    Substrate* substrate_;
    SurfaceElementState* state_;
};

// Concrete UiSurface handed to an extension. A thin, owning handle over a
// Surface that lives in the Substrate's list; destruction removes the Surface
// (document + scene node). Per-extension (carries no id itself — its Surface
// records the owning extension).
class SurfaceHandle final : public UiSurface {
public:
    SurfaceHandle(Substrate* substrate, Surface* surface)
        : substrate_(substrate), surface_(surface) {}
    ~SurfaceHandle() override;
    SurfaceHandle(const SurfaceHandle&) = delete;
    auto operator=(const SurfaceHandle&) -> SurfaceHandle& = delete;

    void set_position(int x, int y) override;
    void set_size(int width, int height) override;
    void set_visible(bool visible) override;
    [[nodiscard]] auto visible() const -> bool override;

    void bind_int(std::string_view name, std::function<int()> getter) override;
    void bind_double(std::string_view name, std::function<double()> getter) override;
    void bind_bool(std::string_view name, std::function<bool()> getter) override;
    void bind_string(std::string_view name, std::function<std::string()> getter) override;
    void bind_event(std::string_view name, std::function<void()> callback) override;
    void bind_drag(std::string_view name,
                   std::function<void(DragPhase, double, double)> callback) override;
    void bind_list(std::string_view name, std::function<std::size_t()> count) override;
    void bind_list_string(std::string_view list, std::string_view field,
                          std::function<std::string(std::size_t)> getter) override;
    void bind_list_int(std::string_view list, std::string_view field,
                       std::function<int(std::size_t)> getter) override;
    void bind_list_double(std::string_view list, std::string_view field,
                          std::function<double(std::size_t)> getter) override;
    void bind_list_bool(std::string_view list, std::string_view field,
                        std::function<bool(std::size_t)> getter) override;
    void bind_list_event(std::string_view list, std::string_view event,
                         std::function<void(std::size_t)> callback) override;
    void on_touch_mode_changed(std::function<void(bool)> callback) override;
    void dirty(std::string_view name) override;
    void dirty() override;
    [[nodiscard]] auto transition_timing(std::string_view element_id,
                                         std::string_view property) const
        -> std::optional<TransitionTiming> override;

private:
    Substrate* substrate_;
    Surface* surface_;
};

// The substrate. Kernel-owned (one per Server). UiSubstrate is the per-
// extension facade view; PerExtensionUi (in ui_substrate.cpp) injects the
// owning id. The Substrate owns the GL/RMLUi state and every Surface.
class Substrate {
public:
    // Build the substrate on the wlr renderer's EGLDisplay. `egl_display` may
    // be EGL_NO_DISPLAY (no gles2 renderer) — then available() is false and
    // create_surface yields nullptr. `watcher` is the kernel's ONE shared
    // FileWatcher, used (dev only, UNBOX_DEV-gated) for asset hot-reload; pass
    // nullptr to disable watching. Never throws.
    static auto create(EGLDisplay egl_display, wlr_allocator* allocator,
                       wlr_renderer* renderer, FileWatcher* watcher,
                       SubstrateDisableFn disable, SubstrateScheduleFn schedule)
        -> std::unique_ptr<Substrate>;

    ~Substrate();
    Substrate(const Substrate&) = delete;
    auto operator=(const Substrate&) -> Substrate& = delete;

    [[nodiscard]] auto available() const -> bool;

    // Create a surface owned by `who`, parented under `parent` scene tree.
    // Returns nullptr on any failure. Never throws.
    auto create_surface(ExtensionId who, wlr_scene_tree* parent, const UiSurfaceSpec& spec)
        -> std::unique_ptr<UiSurface>;

    // Snapshot the pixels under `source` into a dmabuf imported as a sampled GL
    // texture in the RMLUi context, registered under an "unbox-preview://N"
    // URI. Returns nullptr if unavailable or the snapshot/import failed. Never
    // throws. (See ui.hpp UiSubstrate::create_preview for the public contract.)
    auto create_preview(wlr_scene_tree* source) -> std::unique_ptr<Preview>;

    // Create a LIVE surface element backed by `client`'s current buffer (the
    // live sibling of create_preview). Returns nullptr if unavailable or the
    // initial import failed. `client` is a borrow the caller must outlive (see
    // ui.hpp UiSubstrate::create_surface_element). Never throws.
    auto create_surface_element(wlr_surface* client) -> std::unique_ptr<SurfaceElement>;

    // True while >=1 surface element exists: the kernel keeps a frame scheduled
    // so the frame-callback duty (send_frame_done_to_surface_elements) keeps the
    // client drawing even when nothing else is dirty.
    [[nodiscard]] auto has_surface_elements() const -> bool;

    // Frame-callback duty (the stuck-frame fix): send wl_surface frame-done to
    // EVERY live-element-backing surface. Called once per composited output frame
    // by the kernel (unconditionally, like wlr_scene_output_send_frame_done), so
    // the client keeps producing buffers regardless of whether we re-rendered.
    void send_frame_done_to_surface_elements(const timespec& now);

    // Render every dirty surface (called from the output frame handler). Also
    // re-imports every surface element's current buffer first (seq-gated).
    void tick_all();

    // ---- Input routing (kernel calls these BEFORE emitting on the bus) ----
    // Pointer motion is always observed (never consumes). Returns nothing.
    void route_pointer_motion(double lx, double ly, std::uint32_t time_msec);
    // Button / axis / touch: return true if a visible ui surface consumed the
    // event (kernel must then NOT emit it on the bus). Coords are layout-space.
    [[nodiscard]] auto route_pointer_button(double lx, double ly, bool pressed,
                                            std::uint32_t time_msec) -> bool;
    [[nodiscard]] auto route_pointer_axis(double lx, double ly, double delta,
                                          std::uint32_t time_msec) -> bool;
    [[nodiscard]] auto route_touch_down(std::int32_t id, double lx, double ly,
                                        std::uint32_t time_msec) -> bool;
    [[nodiscard]] auto route_touch_motion(std::int32_t id, double lx, double ly,
                                          std::uint32_t time_msec) -> bool;
    [[nodiscard]] auto route_touch_up(std::int32_t id, std::uint32_t time_msec) -> bool;

    // ---- touch-mode ----
    [[nodiscard]] auto touch_mode() const -> bool;
    void set_touch_mode_override(UiSubstrate::TouchModeOverride ov);

    // ---- test/inspection probes (kept from the spike's regression value) ----
    // Total frames rendered+submitted across all surfaces.
    [[nodiscard]] auto frame_count() const -> int;
    // Orientation self-check of the first shm-path surface's submitted buffer:
    // +1 upright, -1 flipped, 0 indeterminate. The orientation regression guard
    // survives here (was Server::ui_spike_orientation).
    [[nodiscard]] auto orientation() const -> int;
    // True if the EGL fence-sync path is the active Plan-A submission sync
    // (no glFinish on the hot path) — lets the suite assert the production sync.
    [[nodiscard]] auto fence_sync_active() const -> bool;

    // ---- preview test instrumentation (kernel suite only) ----
    // Whether the last create_preview imported the snapshot via the dmabuf ->
    // EGLImage -> sampled texture path (Plan A) rather than failing. Lets the
    // suite assert the spike's GO path engaged on hardware that supports it.
    [[nodiscard]] auto preview_import_is_dmabuf() const -> bool;

    // ---- surface-element test instrumentation (kernel suite only) ----
    // Total live re-imports across all surface elements: bumps once per REAL
    // re-import (a seq advance). A static client (no commit => no seq change)
    // never bumps it; an updating client bumps once per committed frame. Lets
    // the suite assert the seq-gate (a new buffer/seq => exactly one re-import;
    // re-adopting the same seq => zero) mirroring spike --verify criterion 1.
    [[nodiscard]] auto surface_element_reimport_count() const -> int;
    // Total wl_surface frame-done sends across all surface elements: bumps once
    // per element per composited frame (the frame-callback duty). Lets the suite
    // assert frame-done is driven per frame (spike --verify criterion 6 / §0c).
    [[nodiscard]] auto surface_element_frame_done_count() const -> int;
    // Whether the most recent surface-element import took the dmabuf ->
    // EGLImage -> texture path (vs the shm-upload fallback). Lets the suite see
    // which path engaged on the test backend.
    [[nodiscard]] auto surface_element_import_is_dmabuf() const -> bool;
    // Packed 0xRRGGBBAA of the first shm-path surface's submitted buffer at
    // layout pixel (x,y) (readback row 0 = top). 0 if no shm surface / no frame
    // / out of bounds. A position-aware probe (like orientation()) so the suite
    // can assert a preview's known source color landed at the expected spot.
    [[nodiscard]] auto surface_pixel(int x, int y) const -> std::uint32_t;
    // Whether the first surface's scene_buffer node carries a non-empty opaque
    // region. The per-pixel-alpha contract requires this to be FALSE: a forced
    // opaque region would tell wlr_scene to skip alpha-blending the ARGB8888
    // buffer (occluding the scene below). The substrate never sets one — this
    // probe lets the suite assert that invariant. False if no surface exists.
    [[nodiscard]] auto surface_has_opaque_region() const -> bool;
    // Number of set_size GL-target reallocations performed so far (across all
    // surfaces). A same-size set_size must NOT bump it (only-on-change guard);
    // a grow/shrink does. Lets the suite prove the no-op-same-size guard.
    [[nodiscard]] auto resize_realloc_count() const -> int;
    // Count elements with `tag` in the first surface's loaded document (0 if no
    // surface / not loaded yet). Proves a data-for list rendered N rows.
    [[nodiscard]] auto element_count(const char* tag) const -> int;
    // Click the index-th `tag` element in the first surface's document (fires
    // its data-event-click). False if no such element. Drives a row event.
    auto click_element(const char* tag, int index) -> bool;
    // Test seam: synthesize a real RmlUi drag on the index-th `tag` element of
    // the first surface (press at the element's content centre, move PAST RmlUi's
    // drag threshold by (dx,dy), then release), so a bind_drag callback receives
    // start/move/end with surface-LOCAL coords — the same path a real captured
    // pointer/touch drag takes, without an input device. False if no such
    // element / no document. GL-path only (no-op skip when unavailable). Test
    // instrumentation; single-thread only.
    auto drag_element(const char* tag, int index, double dx, double dy) -> bool;
    // Test seam: synchronously reload the first surface's document from its file
    // (the same reload the dev inotify watcher drives), so tests trigger reload
    // deterministically without racing real filesystem events. Returns true if a
    // NEW document was installed (false if no file-backed surface / parse failed
    // — old doc kept). Test instrumentation only.
    auto reload_first_surface() -> bool;

    struct Impl;

private:
    explicit Substrate(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class SurfaceHandle;
    friend class PreviewHandle;
    friend class SurfaceElementHandle;
};

} // namespace unbox::kernel
