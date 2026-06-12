#pragma once

#include <unbox/kernel/wlr.hpp>

#include <cstdint>
#include <unordered_map>

// Typed surface -> scene-tree association (the kernel-owned replacement for the
// old untyped `wlr_surface.data` / `wlr_xdg_surface.data` cross-extension
// convention). That `void*` agreement had zero compile/link enforcement and
// violated "cross-extension coupling anchors to exported TYPED symbols"
// (AGENTS.md); this is the typed contract.
//
// Ownership model: an extension that HOSTS a surface in a scene tree (xdg
// toplevels in ext-xdg-shell, layer surfaces in ext-layer-shell, future
// xwayland) registers the association via Host::host_surface() and keeps the
// returned move-only RAII handle as a member. The MAP lives in the kernel; the
// kernel stores an opaque association and names no feature — it does not know
// what a toplevel or a layer surface is. Any other extension resolves a
// surface to its host tree via Host::scene_tree_for() — e.g. ext-xdg-shell
// resolving a popup's parent surface to the parent's scene tree.
//
// This does NOT forbid a unit using `.data` PRIVATELY within itself (e.g.
// stashing its own per-node back-pointer) — only the CROSS-UNIT agreement
// dies. Cross-unit, route through this typed contract.
//
// Single wl_event_loop thread throughout; no internal locking.

namespace unbox::kernel {

namespace detail {

// Pure pointer-keyed association with token-defended RAII semantics. No
// wlroots semantics — it stores pointer identities, which is exactly why it is
// unit-testable with no compositor running. The kernel embeds ONE instance and
// the typed SurfaceRegistration / Host methods are thin shims over it.
//
// Double-register of the same key REPLACES the value and bumps the key's token;
// the previous holder's handle therefore becomes a no-op on destruction (its
// token no longer matches), so it can never tear down the newer association.
class PointerAssoc {
public:
    using Token = std::uint64_t;
    static constexpr Token invalid_token = 0;

    struct Slot {
        void* value;
        Token token;
    };

    // Returns the token now owning `key`. Replaces any existing mapping.
    auto set(void* key, void* value) -> Token {
        const Token token = ++next_token_;
        map_[key] = Slot{value, token};
        return token;
    }

    // Erase `key` ONLY if `token` is still the owning token (defends against a
    // stale handle unregistering a newer registration of the same key).
    void clear(void* key, Token token) noexcept {
        auto it = map_.find(key);
        if (it != map_.end() && it->second.token == token) {
            map_.erase(it);
        }
    }

    [[nodiscard]] auto get(void* key) const -> void* {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : it->second.value;
    }

    [[nodiscard]] auto size() const -> std::size_t { return map_.size(); }

private:
    std::unordered_map<void*, Slot> map_;
    Token next_token_ = 0;
};

} // namespace detail

// Move-only RAII handle for one surface->tree association. Destruction (or
// reset()/move-out) unregisters — but only if this handle still owns the
// surface's current mapping (re-hosting the same surface elsewhere supersedes
// this handle, whose destruction then becomes a safe no-op). Hold it as a
// member of the hosting entity so the association's lifetime equals the node's.
class SurfaceRegistration {
public:
    SurfaceRegistration() = default;
    SurfaceRegistration(detail::PointerAssoc* store, void* key, detail::PointerAssoc::Token token)
        : store_(store), key_(key), token_(token) {}

    SurfaceRegistration(SurfaceRegistration&& other) noexcept
        : store_(other.store_), key_(other.key_), token_(other.token_) {
        other.store_ = nullptr;
        other.key_ = nullptr;
        other.token_ = detail::PointerAssoc::invalid_token;
    }
    auto operator=(SurfaceRegistration&& other) noexcept -> SurfaceRegistration& {
        if (this != &other) {
            reset();
            store_ = other.store_;
            key_ = other.key_;
            token_ = other.token_;
            other.store_ = nullptr;
            other.key_ = nullptr;
            other.token_ = detail::PointerAssoc::invalid_token;
        }
        return *this;
    }
    SurfaceRegistration(const SurfaceRegistration&) = delete;
    auto operator=(const SurfaceRegistration&) -> SurfaceRegistration& = delete;

    ~SurfaceRegistration() { reset(); }

    // Unregister early. Idempotent. No-op if this handle was superseded by a
    // later registration of the same surface.
    void reset() noexcept {
        if (store_ != nullptr) {
            store_->clear(key_, token_);
            store_ = nullptr;
            key_ = nullptr;
            token_ = detail::PointerAssoc::invalid_token;
        }
    }

    [[nodiscard]] auto active() const noexcept -> bool { return store_ != nullptr; }

private:
    detail::PointerAssoc* store_ = nullptr;
    void* key_ = nullptr;
    detail::PointerAssoc::Token token_ = detail::PointerAssoc::invalid_token;
};

} // namespace unbox::kernel
