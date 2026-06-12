#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// The typed extension bus — the architecture itself (AGENTS.md: "Cross-
// extension coupling anchors to exported TYPED symbols"). Two hook kinds:
//
//   Event<Args...>  fire-and-forget, N listeners, error-isolated. The kernel
//                   (or an extension) emits; every subscriber is invoked in
//                   subscription order. A listener that throws is caught at
//                   this boundary and its OWNING extension is disabled (all
//                   its subscriptions dropped) — the emit still completes and
//                   the remaining listeners still fire. The session never dies.
//
//   Filter<T>       an ordered value-in -> value-out chain. apply(v) threads v
//                   through each link in subscription order; each link returns
//                   the (possibly modified) value for the next. A throwing link
//                   is skipped and its extension disabled; the chain continues
//                   with the value as it stood before that link.
//
// Every subscribe() returns a move-only RAII Subscription whose destruction
// unsubscribes (listener-lifetime.md). A callback MAY drop its own
// Subscription, and an extension may be disabled mid-dispatch: removal is
// deferred so the in-flight iteration stays valid (the compaction happens
// after the dispatch unwinds).
//
// PURE CORE: no wlroots, no GL, no RMLUi types appear here. The bus is fully
// exercisable with nothing running (tests/test_kernel.cpp). Everything runs on
// the single wl_event_loop thread; no internal locking.

namespace unbox::kernel {

// Opaque per-extension identity, assigned by the Server at install time and
// carried by each extension's Host. Hooks tag every subscription with the id
// of the subscribing extension so a throwing callback disables the RIGHT one.
// id 0 is reserved for "the kernel itself" (kernel-emitted, kernel-owned
// subscriptions are never auto-disabled).
enum class ExtensionId : std::uint32_t {};

inline constexpr ExtensionId kernel_extension_id{0};

namespace detail {

// Shared sink the hooks call when a callback throws: it disables the owning
// extension (dropping every subscription it holds across ALL hooks) and logs.
// Implemented by the Server; injected into each hook at construction so the
// bus core carries no kernel dependency. A null sink (default) means "no
// isolation registry" — a throw is swallowed and logged-by-caller; used only
// in pure-core tests that assert fan-out, where no extension registry exists.
class DisableSink {
public:
    virtual ~DisableSink() = default;
    // Called from inside a dispatch when `who`'s callback threw. MUST be
    // re-entrant-safe w.r.t. the hook currently dispatching (the hook defers
    // its own compaction); the sink marks the extension dead and requests
    // each registered hook to purge that id once its dispatch unwinds.
    virtual void disable(ExtensionId who) noexcept = 0;
};

} // namespace detail

class Subscription;

namespace detail {

// Non-template base every hook derives from, so a single registry can purge an
// extension's subscriptions across heterogeneous Event<...>/Filter<...>
// instances without knowing their payload types.
class HookBase {
public:
    virtual ~HookBase() = default;

    // Drop every subscription owned by `who`. Safe to call during this hook's
    // own dispatch: entries are tombstoned now and physically erased when the
    // outermost dispatch finishes.
    virtual void purge(ExtensionId who) noexcept = 0;

    // Bind this hook to the isolation registry's sink (so a throwing callback
    // here disables its owning extension everywhere). Called by Host::adopt()
    // when an extension exports a hook it default-constructed as a member.
    virtual void set_sink(DisableSink* sink) noexcept = 0;

protected:
    // Token identifying one subscription slot within a hook; handed to the
    // Subscription so it can ask the hook to remove exactly that slot.
    using Token = std::uint64_t;
    static constexpr Token invalid_token = 0;

    virtual void unsubscribe(Token token) noexcept = 0;

    friend class unbox::kernel::Subscription;
};

} // namespace detail

// Move-only RAII handle for one subscription. Destruction (or reset()/release-
// by-move) unsubscribes. Holding it as a member of the extension (or of one of
// the extension's RAII members) is the contract: when the extension is
// destroyed, the member dies and the subscription drops. Never store a raw
// hook reference or a bare callback across a unit boundary instead of this.
class Subscription {
public:
    Subscription() = default;
    Subscription(detail::HookBase* hook, std::uint64_t token) : hook_(hook), token_(token) {}

    Subscription(Subscription&& other) noexcept
        : hook_(other.hook_), token_(other.token_) {
        other.hook_ = nullptr;
        other.token_ = detail::HookBase::invalid_token;
    }
    auto operator=(Subscription&& other) noexcept -> Subscription& {
        if (this != &other) {
            reset();
            hook_ = other.hook_;
            token_ = other.token_;
            other.hook_ = nullptr;
            other.token_ = detail::HookBase::invalid_token;
        }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    auto operator=(const Subscription&) -> Subscription& = delete;

    ~Subscription() { reset(); }

    // Explicitly unsubscribe early. Idempotent. Safe to call from within the
    // subscribed callback (the hook defers physical removal).
    void reset() noexcept {
        if (hook_ != nullptr) {
            hook_->unsubscribe(token_);
            hook_ = nullptr;
            token_ = detail::HookBase::invalid_token;
        }
    }

    [[nodiscard]] auto active() const noexcept -> bool { return hook_ != nullptr; }

private:
    detail::HookBase* hook_ = nullptr;
    std::uint64_t token_ = detail::HookBase::invalid_token;
};

// ---- Event<Args...> : fire-and-forget, N listeners, error-isolated ----------

template <typename... Args>
class Event final : public detail::HookBase {
public:
    using Callback = std::function<void(Args...)>;

    Event() = default;
    explicit Event(detail::DisableSink* sink) : sink_(sink) {}
    Event(const Event&) = delete;
    auto operator=(const Event&) -> Event& = delete;

    // Subscribe `cb`, owned by extension `who`. Returns the RAII handle; let it
    // die to unsubscribe. Listeners fire in subscription order on emit().
    [[nodiscard]] auto subscribe(ExtensionId who, Callback cb) -> Subscription {
        const Token token = ++next_token_;
        entries_.push_back(Entry{token, who, std::move(cb), false});
        return Subscription(this, token);
    }

    // Fire the event: invoke every live listener in subscription order with a
    // copy of the args (Args are passed by value semantics of std::function;
    // pass borrows as raw pointers/refs in Args to avoid copies — see the
    // kernel catalogue in host.hpp). A listener that throws is caught here and
    // its extension disabled; the emit still completes. Re-entrant emit() and
    // unsubscribe-during-emit are safe.
    void emit(Args... args) {
        ++depth_;
        const std::size_t n = entries_.size(); // new subscriptions during emit don't fire
        for (std::size_t i = 0; i < n; ++i) {
            Entry& e = entries_[i];
            if (e.dead) {
                continue;
            }
            try {
                e.cb(args...);
            } catch (...) {
                disable_owner(e.who);
            }
        }
        --depth_;
        compact_if_idle();
    }

    void purge(ExtensionId who) noexcept override {
        for (Entry& e : entries_) {
            if (e.who == who) {
                e.dead = true;
            }
        }
        compact_if_idle();
    }

    void set_sink(detail::DisableSink* sink) noexcept override { sink_ = sink; }

private:
    struct Entry {
        Token token;
        ExtensionId who;
        Callback cb;
        bool dead;
    };

    void unsubscribe(Token token) noexcept override {
        for (Entry& e : entries_) {
            if (e.token == token) {
                e.dead = true;
                break;
            }
        }
        compact_if_idle();
    }

    void disable_owner(ExtensionId who) noexcept {
        if (sink_ != nullptr && who != kernel_extension_id) {
            sink_->disable(who); // routes back through purge() on every hook
        } else {
            purge(who);
        }
    }

    void compact_if_idle() noexcept {
        if (depth_ != 0) {
            return; // a dispatch is in flight; erasing now would invalidate it
        }
        std::erase_if(entries_, [](const Entry& e) { return e.dead; });
    }

    std::vector<Entry> entries_;
    detail::DisableSink* sink_ = nullptr;
    Token next_token_ = 0;
    int depth_ = 0;
};

// ---- Filter<T> : ordered value-in -> value-out chain -------------------------

template <typename T>
class Filter final : public detail::HookBase {
public:
    // Each link receives the current value and returns the value for the next
    // link. Take T by value and return T (the chain threads by value).
    using Link = std::function<T(T)>;

    Filter() = default;
    explicit Filter(detail::DisableSink* sink) : sink_(sink) {}
    Filter(const Filter&) = delete;
    auto operator=(const Filter&) -> Filter& = delete;

    // Append `link`, owned by extension `who`. Links run in subscription order.
    [[nodiscard]] auto subscribe(ExtensionId who, Link link) -> Subscription {
        const Token token = ++next_token_;
        entries_.push_back(Entry{token, who, std::move(link), false});
        return Subscription(this, token);
    }

    // Thread `value` through the chain and return the result. A link that
    // throws is skipped (and its extension disabled); the chain continues with
    // the value as it stood BEFORE that link. With no links, returns `value`.
    [[nodiscard]] auto apply(T value) -> T {
        ++depth_;
        const std::size_t n = entries_.size();
        for (std::size_t i = 0; i < n; ++i) {
            Entry& e = entries_[i];
            if (e.dead) {
                continue;
            }
            try {
                value = e.link(value);
            } catch (...) {
                disable_owner(e.who);
            }
        }
        --depth_;
        compact_if_idle();
        return value;
    }

    void purge(ExtensionId who) noexcept override {
        for (Entry& e : entries_) {
            if (e.who == who) {
                e.dead = true;
            }
        }
        compact_if_idle();
    }

    void set_sink(detail::DisableSink* sink) noexcept override { sink_ = sink; }

private:
    struct Entry {
        Token token;
        ExtensionId who;
        Link link;
        bool dead;
    };

    void unsubscribe(Token token) noexcept override {
        for (Entry& e : entries_) {
            if (e.token == token) {
                e.dead = true;
                break;
            }
        }
        compact_if_idle();
    }

    void disable_owner(ExtensionId who) noexcept {
        if (sink_ != nullptr && who != kernel_extension_id) {
            sink_->disable(who);
        } else {
            purge(who);
        }
    }

    void compact_if_idle() noexcept {
        if (depth_ != 0) {
            return;
        }
        std::erase_if(entries_, [](const Entry& e) { return e.dead; });
    }

    std::vector<Entry> entries_;
    detail::DisableSink* sink_ = nullptr;
    Token next_token_ = 0;
    int depth_ = 0;
};

} // namespace unbox::kernel
