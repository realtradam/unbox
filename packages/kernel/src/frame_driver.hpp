#pragma once

#include <unbox/kernel/frames.hpp>
#include <unbox/kernel/hooks.hpp> // ExtensionId

#include <functional>
#include <unordered_map>
#include <vector>

// The kernel's per-frame animation driver. Holds the set of live per-frame
// callbacks registered via Host::request_frames; the kernel's output frame
// handler drains them (in order, reentrancy-safe) BEFORE the ui substrate ticks
// and the scene commits, and — while the set is non-empty — keeps requesting
// output frames so animations advance even when the scene is idle.
//
// Error-isolated: a throwing callback is caught at the boundary and the owning
// extension is disabled via the injected sink (same contract as hooks / file
// watches), never the session.
//
// Single wl_event_loop thread throughout; no internal locking.

namespace unbox::kernel {

class FrameDriver final : public detail::FrameRegistry {
public:
    using Token = detail::FrameRegistry::Token;

    // `disable` disables the owning extension when its callback throws (injected
    // by the kernel, same as the bus/substrate/file-watch isolation sink).
    explicit FrameDriver(std::function<void(ExtensionId)> disable);
    ~FrameDriver() override = default;
    FrameDriver(const FrameDriver&) = delete;
    auto operator=(const FrameDriver&) -> FrameDriver& = delete;

    // Register `on_frame` (fired each frame with dt seconds, error-isolated to
    // `who`). Returns a FrameRequest RAII handle; destroying it removes the
    // callback. Always active (the driver itself has no loop dependency — the
    // caller decides whether a handle is inert when there is no output).
    [[nodiscard]] auto add(std::function<void(double)> on_frame, ExtensionId who) -> FrameRequest;

    // True if at least one callback is live (the kernel keeps scheduling frames
    // while this holds).
    [[nodiscard]] auto has_requests() const noexcept -> bool { return !entries_.empty(); }

    // Fire every live callback once with `dt_seconds`. Reentrancy-safe: a
    // callback may add or remove requests (including its own) mid-drain — a
    // request added during the drain first fires NEXT frame, one removed during
    // the drain does not fire again this drain.
    void drain(double dt_seconds);

    // detail::FrameRegistry — stop the callback with this token (handle dtor).
    void remove_frame_request(Token token) noexcept override;

private:
    struct Entry {
        std::function<void(double)> on_frame;
        ExtensionId who{};
    };

    std::function<void(ExtensionId)> disable_;
    Token next_token_ = 0;
    std::unordered_map<Token, Entry> entries_;
};

} // namespace unbox::kernel
