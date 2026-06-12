#pragma once

#include <memory>
#include <string>

// The compositor core. Slice-2 shape: a faithful tinywl port (plus touch)
// living wholly inside the kernel; slice 4 splits shell policy out into
// extensions behind typed contracts.
//
// Calling context: single wl_event_loop thread. run() blocks; terminate()
// is safe to call from event handlers (e.g. a keybinding).

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

    // Runs the event loop until terminate() (default binding: Alt+Escape).
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
