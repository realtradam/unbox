#pragma once

#include <unbox/kernel/extension.hpp>

#include <memory>
#include <string>

// The compositor core. Slice-4 shape: the kernel names NO concrete feature and
// boots featureless. It owns the generic plumbing (compositor, subcompositor,
// data-device, output/scene glue, cursor + seat, the kernel-internal ui
// spike) and the extension host + typed bus. ALL shell policy (xdg-shell
// toplevels, focus, cycling, interactive move/resize, keybindings) lives in
// extensions installed via install() before run().
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

        // Slice-3 spike surface (TEMPORARY — replaced by the real ui
        // substrate contract in slice 4+). When true, the kernel composites
        // a hello-world RML document as a wlr_scene_buffer node, proving the
        // RMLUi -> wlr_scene bridge. When false (default), behaviour is
        // exactly slice-2. If the spike cannot start (e.g. no font, no GL),
        // it disables itself gracefully and the server runs as if false.
        bool ui_spike = false;
    };

    // Creates the display, backend, renderer, allocator, scene, xdg-shell,
    // cursor, and seat, then starts the backend and opens the socket.
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

    // Frames the slice-3 spike bridge has submitted to the scene so far.
    // A probe for tests, removed with the spike surface. Returns 0 when
    // ui_spike is false or the spike disabled itself. Single-thread only.
    [[nodiscard]] auto ui_spike_frame_count() const -> int;

    // Orientation self-check of the spike's submitted buffer (slice-3 probe,
    // removed with the spike surface). The document carries distinctive top
    // and bottom bands; returns +1 if the buffer is upright (top band in the
    // top rows), -1 if vertically flipped, 0 if indeterminate (disabled, no
    // frame yet, or not the CPU-readback path). Single-thread only.
    [[nodiscard]] auto ui_spike_orientation() const -> int;

    // Opaque to consumers; defined in src/ (kernel-private state).
    struct Impl;

private:
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace unbox::kernel
