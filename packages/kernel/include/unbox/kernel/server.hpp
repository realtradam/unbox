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

    // Opaque to consumers; defined in src/ (kernel-private state).
    struct Impl;

private:
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace unbox::kernel
