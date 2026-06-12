#pragma once

#include <unbox/kernel/extension.hpp>

#include <memory>
#include <string>

// The compositor core. The kernel names NO concrete feature and boots
// featureless. It owns the generic plumbing (compositor, subcompositor,
// data-device, output/scene glue, cursor + seat) plus the extension host +
// typed bus + the ui substrate (the kernel's RMLUi subsystem, reached by
// extensions via Host::ui() — see <unbox/kernel/ui.hpp>). ALL shell policy
// (xdg-shell toplevels, focus, cycling, interactive move/resize, keybindings)
// lives in extensions installed via install() before run().
//
// Calling context: single wl_event_loop thread. run() blocks; terminate()
// is safe to call from event handlers (e.g. a keybinding extension).

namespace unbox::kernel {

class Server {
public:
    struct Options {
        // Command spawned via `/bin/sh -c` (in a child process, with
        // WAYLAND_DISPLAY pointing at this server) once the socket is
        // live. Dev convenience mirroring tinywl's -s. Empty = nothing.
        std::string startup_cmd{};

        // DEPRECATED no-op (slice 5). The slice-3 ui spike retired into the
        // real ui substrate (Host::ui()); this flag no longer does anything
        // and is kept only so host-bin's --ui-spike plumbing keeps compiling
        // until the orchestrator removes it (change-request in
        // reports/kernel.md). Setting it has no effect. Remove on next
        // host-bin edit.
        bool ui_spike = false;
    };

    // Creates the display, backend, renderer, allocator, scene, cursor, seat,
    // and the ui substrate, then starts the backend and opens the socket.
    // Throws std::runtime_error if any wlroots component fails.
    [[nodiscard]] static auto create(Options options) -> std::unique_ptr<Server>;

    ~Server();
    Server(const Server&) = delete;
    auto operator=(const Server&) -> Server& = delete;

    // The WAYLAND_DISPLAY name clients connect with (e.g. "wayland-1").
    [[nodiscard]] auto socket_name() const -> std::string;

    // Install an extension (ownership transfer). Call after create(), before
    // activate_extensions()/run(). Order of install() calls does NOT determine
    // activation order — that is computed topologically from each Manifest's
    // depends_on at activate_extensions() time. Installing two extensions with
    // the same Manifest id throws std::runtime_error here (duplicate id).
    void install(std::unique_ptr<Extension> extension);

    // Activate every installed extension exactly once, in topological order by
    // Manifest depends_on (ties broken by tier then install order). Throws
    // std::runtime_error on a missing dependency, a dependency cycle, or a
    // duplicate id; the offending ids are named in what(). An exception thrown
    // by an extension's own activate() propagates out (activation failure is
    // fatal — a core extension that cannot start is a broken session, not an
    // isolated one). Idempotent: a second call is a no-op. run() calls this
    // first if it was not called already.
    void activate_extensions();

    // Runs the event loop until terminate(). Calls activate_extensions() first
    // if not already done.
    void run();

    // One event-loop turn (≤ timeout_ms); for tests and embedders.
    // Returns false if the event loop failed to dispatch.
    auto dispatch(int timeout_ms) -> bool;

    // Stops run(). Safe from within event handlers.
    void terminate();

    // ---- ui-substrate test instrumentation (kernel suite only) ----
    // Narrow probes into the kernel-owned ui substrate, kept so the slice-3
    // regression value (frame-advance + upright-buffer guard) survives as
    // substrate tests and the production-sync decision is checkable. Not for
    // extensions (they drive the substrate via Host::ui()); single-thread only.

    // Total frames the substrate has rendered+submitted across all ui surfaces.
    [[nodiscard]] auto ui_frame_count() const -> int;
    // Orientation of a CPU-readback (shm-path) surface's submitted buffer:
    // +1 upright, -1 vertically flipped (the bug), 0 indeterminate (no shm
    // surface, no frame yet, or no GL path). The kernel suite asserts != -1.
    [[nodiscard]] auto ui_orientation() const -> int;
    // True when the EGL fence-sync submission path is active (Plan-A dmabuf +
    // EGL_KHR_fence_sync) — i.e. no glFinish on the hot path (notes/plan.md §7).
    [[nodiscard]] auto ui_fence_sync_active() const -> bool;

    // Pin the substrate's touch-mode for tests (none = automatic). Mirrors
    // UiSubstrate::TouchModeOverride; lets the suite drive the state machine and
    // its on_touch_mode_changed notification. Test instrumentation;
    // single-thread only.
    enum class UiTouchOverride { automatic, force_off, force_on };
    void ui_set_touch_override(UiTouchOverride ov);

    // Opaque to consumers; defined in src/ (kernel-private state).
    struct Impl;

private:
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace unbox::kernel
