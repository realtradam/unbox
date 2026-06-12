#pragma once

#include <unbox/kernel/wlr.hpp>

#include <functional>
#include <utility>

namespace unbox::kernel {

// RAII wl_listener: connect() subscribes, destruction/disconnect()
// unsubscribes. PRIVATE slice-2 helper — the public typed subscription
// handle arrives with the bus in slice 4 (.unbox/rules/listener-lifetime.md).
//
// A handler MAY destroy its own Listener (the destroy-event pattern: a
// handler erases its owning entity from a container). This is safe because
// thunk() touches nothing after handler_() returns — but the handler itself
// must not touch captures after triggering its own destruction; make the
// erase/delete its LAST action.
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

    void connect(wl_signal& signal, std::function<void(void*)> handler) {
        disconnect();
        handler_ = std::move(handler);
        wl_signal_add(&signal, &node_.listener);
    }

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
