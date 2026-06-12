#pragma once

#include <unbox/kernel/wlr.hpp>

#include <functional>
#include <utility>

// RAII wrapper over a wl_listener. An extension that does its own wlroots glue
// (ext-xdg-shell binds xdg_shell signals; ext-layer-shell binds layer-shell
// signals) MUST NOT hold a bare wl_listener across the boundary
// (listener-lifetime.md, AGENTS.md): a wl_listener still linked into a signal
// after its owner dies corrupts the signal's list on the next emit. This type
// makes the link's lifetime equal to the wrapper's: connect() subscribes,
// destruction (or disconnect()) unsubscribes. Hold it as a member; it dies
// with you.
//
// Borrows received in a handler (the void* data, any wlroots pointer reached
// through it) are valid ONLY for that call — never store them.
//
// A handler MAY destroy its own Listener — the destroy-event pattern: a
// handler erases the entity that owns the Listener (and the Listener with it).
// This is safe because the thunk touches NOTHING after handler_() returns. But
// the handler itself must not touch its captures after triggering its own
// destruction: make the erase/delete the handler's LAST action. (The bus's
// Subscription formalizes this for hook callbacks; this type is for raw
// wlroots signals an extension must bind directly.)
//
// Single wl_event_loop thread; no internal synchronization.

namespace unbox::kernel {

class Listener {
public:
    Listener() {
        node_.self = this;
        node_.listener.notify = &Listener::thunk;
        wl_list_init(&node_.listener.link);
    }
    ~Listener() { disconnect(); }
    Listener(const Listener&) = delete;
    auto operator=(const Listener&) -> Listener& = delete;

    // Subscribe to `signal`; `handler` receives the signal's data pointer
    // (cast it to the documented event type). Re-connecting first disconnects.
    void connect(wl_signal& signal, std::function<void(void*)> handler) {
        disconnect();
        handler_ = std::move(handler);
        wl_signal_add(&signal, &node_.listener);
    }

    // Unsubscribe. Idempotent; called automatically on destruction.
    void disconnect() {
        wl_list_remove(&node_.listener.link);
        wl_list_init(&node_.listener.link);
    }

private:
    struct Node {
        wl_listener listener; // MUST stay first: thunk casts wl_listener* -> Node*
        Listener* self;
    };

    static void thunk(wl_listener* listener, void* data) {
        auto* node = reinterpret_cast<Node*>(listener);
        node->self->handler_(data);
    }

    Node node_{};
    std::function<void(void*)> handler_;
};

} // namespace unbox::kernel
