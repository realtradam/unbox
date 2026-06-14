#pragma once

#include <cstdint>

// A typed, RAII per-frame animation primitive. While at least one FrameRequest
// is alive the kernel SCHEDULES output frames continuously (so an animation
// advances even when the scene is otherwise idle) and runs each request's
// callback once per rendered output frame, BEFORE the ui substrate ticks and
// the scene commits — so a callback that updates state + UiSurface::dirty() is
// rendered THAT frame. When the last request dies the kernel stops requesting
// frames (no busy render at rest). Each FrameRequest is a move-only handle,
// mirroring FileWatch / Subscription / SurfaceRegistration; destroying (or
// reset()/move-out) the handle stops its callback.
//
// See Host::request_frames() for the registration entry point + the full
// per-frame / dt / error-isolation / driver-output semantics.
//
// Single wl_event_loop thread throughout; no internal locking.

namespace unbox::kernel {

namespace detail {

// The registry a FrameRequest unregisters from. The kernel's FrameDriver
// implements this; the handle holds a borrow + a token (like FileWatch /
// PointerAssoc's token defense) so a stale handle can never tear down a reused
// slot. Abstract so frames.hpp stays free of wlr/loop internals.
class FrameRegistry {
public:
    using Token = std::uint64_t;
    static constexpr Token invalid_token = 0;

    virtual ~FrameRegistry() = default;
    // Stop the per-frame callback identified by `token`. Idempotent / no-op if
    // already gone.
    virtual void remove_frame_request(Token token) noexcept = 0;

protected:
    FrameRegistry() = default;
};

} // namespace detail

// A live per-frame animation callback. Move-only RAII: destruction / reset() /
// move-out stops the callback (and, if it was the last one, the kernel stops
// requesting frames). Hold it as a member of the entity that owns the animation
// (e.g. an extension), so the callback's lifetime equals that entity's. A
// default-constructed handle is inactive.
class FrameRequest {
public:
    FrameRequest() = default;
    FrameRequest(detail::FrameRegistry* registry, detail::FrameRegistry::Token token)
        : registry_(registry), token_(token) {}

    FrameRequest(FrameRequest&& other) noexcept
        : registry_(other.registry_), token_(other.token_) {
        other.registry_ = nullptr;
        other.token_ = detail::FrameRegistry::invalid_token;
    }
    auto operator=(FrameRequest&& other) noexcept -> FrameRequest& {
        if (this != &other) {
            reset();
            registry_ = other.registry_;
            token_ = other.token_;
            other.registry_ = nullptr;
            other.token_ = detail::FrameRegistry::invalid_token;
        }
        return *this;
    }
    FrameRequest(const FrameRequest&) = delete;
    auto operator=(const FrameRequest&) -> FrameRequest& = delete;

    ~FrameRequest() { reset(); }

    // Stop the per-frame callback early. Idempotent.
    void reset() noexcept {
        if (registry_ != nullptr) {
            registry_->remove_frame_request(token_);
            registry_ = nullptr;
            token_ = detail::FrameRegistry::invalid_token;
        }
    }

    [[nodiscard]] auto active() const noexcept -> bool { return registry_ != nullptr; }

private:
    detail::FrameRegistry* registry_ = nullptr;
    detail::FrameRegistry::Token token_ = detail::FrameRegistry::invalid_token;
};

} // namespace unbox::kernel
