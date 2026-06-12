#pragma once

#include <unbox/kernel/wlr.hpp>

#include <memory>

// Slice-3 spike: the RMLUi -> wlr_scene bridge (prompts/kernel.md, plan §4).
// PRIVATE to the kernel; nothing here is a contract. Replaced wholesale by
// the real ui-substrate contract in slice 4+.
//
// A UiSpike owns a sibling GLES 3.2 EGL context (sharing the wlr renderer's
// EGLDisplay), an offscreen FBO into a wlr_buffer, an RMLUi context rendering
// a hello-world document, and a wlr_scene_buffer node showing it. It renders
// only when the RMLUi context is dirty, driven from an output frame handler.
//
// Everything runs on the single wl_event_loop thread.

namespace unbox::kernel {

class UiSpike {
public:
    // Which compositing plan the bridge landed on (plan §4 / brief A->B->C).
    enum class Plan {
        Disabled,  // could not start (no font / no GL); server runs as slice-2
        Dmabuf,    // Plan A: dmabuf-backed wlr_buffer imported as EGLImage FBO
        ShmCopy,   // Plan B: FBO + glReadPixels into a data-ptr wlr_buffer
    };

    // Builds the bridge and attaches a scene node under `parent`. `egl_display`
    // is the wlr renderer's EGLDisplay (wlr_egl_get_display); the sibling
    // context shares it. `allocator`/`renderer` are borrowed for the buffer
    // lifetime of the spike (owned by the server). Never throws: on any
    // failure it logs and yields a Disabled bridge (frame_count stays 0).
    static auto create(wlr_scene_tree* parent, EGLDisplay egl_display,
                       wlr_allocator* allocator, wlr_renderer* renderer)
        -> std::unique_ptr<UiSpike>;

    ~UiSpike();
    UiSpike(const UiSpike&) = delete;
    auto operator=(const UiSpike&) -> UiSpike& = delete;

    // Advance + render one frame if the RMLUi context is dirty (ticks the
    // bound frame counter, which dirties the document every call at spike
    // fidelity). Submits to the scene with damage. No-op when Disabled.
    void tick();

    // Crude input proof (NOT the slice-5 routing contract). Coords are
    // surface-local pixels within the spike node. Forwarded straight to the
    // RMLUi context; a hover/click makes the document's button react.
    void on_pointer_motion(double sx, double sy);
    void on_pointer_button(bool pressed);

    // The scene node's position/size, so the server can hit-test pointer
    // events against it. Layout coords; node sits at a fixed origin.
    [[nodiscard]] auto node() const -> wlr_scene_node*;

    [[nodiscard]] auto plan() const -> Plan;
    [[nodiscard]] auto frame_count() const -> int;

    // Orientation self-check on the submitted buffer (Plan B / shm path only,
    // where the CPU readback exists). The document carries distinctive solid
    // bands at its top and bottom edges; this samples the buffer and returns:
    //   +1  upright: top band is in the TOP rows, bottom band in the bottom
    //   -1  flipped: bands are swapped (the bug this fix prevents)
    //    0  indeterminate: not the shm path, or no frame rendered yet, or the
    //        bands were not found (e.g. spike disabled)
    // Lets a headless test assert orientation can never silently regress.
    [[nodiscard]] auto check_orientation() const -> int;

    struct Impl;

private:
    explicit UiSpike(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace unbox::kernel
