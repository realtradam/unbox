#pragma once

#include <unbox/kernel/ui.hpp>
#include <unbox/kernel/wlr.hpp>

#include "ui_core.hpp"

#include <cstdint>
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

// Callback the substrate invokes to disable an extension whose data-event
// callback threw — injected by the kernel (Server::Impl). Mirrors the bus's
// detail::DisableSink but scoped to the substrate so ui.hpp carries no kernel
// internals.
using SubstrateDisableFn = std::function<void(ExtensionId)>;

class Substrate; // the concrete UiSubstrate, defined in ui_substrate.cpp

// One ui surface's private state (Rml context + GL target + scene node +
// bindings). Defined in ui_substrate.cpp; declared here so Substrate can own a
// list of them and the public SurfaceHandle can borrow one.
struct Surface;

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
    void on_touch_mode_changed(std::function<void(bool)> callback) override;
    void dirty(std::string_view name) override;
    void dirty() override;

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
    // create_surface yields nullptr. Never throws.
    static auto create(EGLDisplay egl_display, wlr_allocator* allocator,
                       wlr_renderer* renderer, SubstrateDisableFn disable)
        -> std::unique_ptr<Substrate>;

    ~Substrate();
    Substrate(const Substrate&) = delete;
    auto operator=(const Substrate&) -> Substrate& = delete;

    [[nodiscard]] auto available() const -> bool;

    // Create a surface owned by `who`, parented under `parent` scene tree.
    // Returns nullptr on any failure. Never throws.
    auto create_surface(ExtensionId who, wlr_scene_tree* parent, const UiSurfaceSpec& spec)
        -> std::unique_ptr<UiSurface>;

    // Render every dirty surface (called from the output frame handler).
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

    struct Impl;

private:
    explicit Substrate(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class SurfaceHandle;
};

} // namespace unbox::kernel
