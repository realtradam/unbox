#pragma once

#include <cstdint>

// A typed, RAII file-watch primitive. The kernel owns ONE inotify instance on
// the wl_event_loop (created lazily on the first watch); each FileWatch is a
// move-only handle to one watched path, mirroring Subscription /
// SurfaceRegistration. Destroying (or reset()/move-out) the handle stops the
// watch. The watched file's on_change callback fires on the event-loop thread,
// coalesced (one save = one callback), and is error-isolated to the extension
// that registered it (a throw disables THAT extension, never the session).
//
// See Host::watch_file() for the registration entry point and the full
// editor-save / not-yet-existing-file semantics.
//
// Single wl_event_loop thread throughout; no internal locking.

namespace unbox::kernel {

namespace detail {

// The registry a FileWatch unregisters from. The kernel's FileWatcher
// implements this; the handle holds a borrow + a token (like PointerAssoc's
// token defense) so a stale handle can never tear down a reused slot. Abstract
// so watch.hpp stays free of inotify/wl internals.
class WatchRegistry {
public:
    using Token = std::uint64_t;
    static constexpr Token invalid_token = 0;

    virtual ~WatchRegistry() = default;
    // Stop the watch identified by `token`. Idempotent / no-op if already gone.
    virtual void remove_watch(Token token) noexcept = 0;

protected:
    WatchRegistry() = default;
};

} // namespace detail

// A live file watch. Move-only RAII: destruction / reset() / move-out stops
// watching. Hold it as a member of the entity that owns the watch (e.g. an
// extension), so the watch's lifetime equals that entity's. A default-
// constructed handle is inactive.
class FileWatch {
public:
    FileWatch() = default;
    FileWatch(detail::WatchRegistry* registry, detail::WatchRegistry::Token token)
        : registry_(registry), token_(token) {}

    FileWatch(FileWatch&& other) noexcept : registry_(other.registry_), token_(other.token_) {
        other.registry_ = nullptr;
        other.token_ = detail::WatchRegistry::invalid_token;
    }
    auto operator=(FileWatch&& other) noexcept -> FileWatch& {
        if (this != &other) {
            reset();
            registry_ = other.registry_;
            token_ = other.token_;
            other.registry_ = nullptr;
            other.token_ = detail::WatchRegistry::invalid_token;
        }
        return *this;
    }
    FileWatch(const FileWatch&) = delete;
    auto operator=(const FileWatch&) -> FileWatch& = delete;

    ~FileWatch() { reset(); }

    // Stop watching early. Idempotent.
    void reset() noexcept {
        if (registry_ != nullptr) {
            registry_->remove_watch(token_);
            registry_ = nullptr;
            token_ = detail::WatchRegistry::invalid_token;
        }
    }

    [[nodiscard]] auto active() const noexcept -> bool { return registry_ != nullptr; }

private:
    detail::WatchRegistry* registry_ = nullptr;
    detail::WatchRegistry::Token token_ = detail::WatchRegistry::invalid_token;
};

} // namespace unbox::kernel
