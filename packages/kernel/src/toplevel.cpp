#include "server_impl.hpp"

#include <algorithm>

namespace unbox::kernel {

// ---- Focus ------------------------------------------------------------------

void Server::Impl::focus_toplevel(Toplevel* toplevel) {
    // Keyboard focus only (pointer focus follows the cursor).
    if (toplevel == nullptr) {
        return;
    }
    wlr_surface* surface = toplevel->xdg_toplevel->base->surface;
    wlr_surface* prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface == surface) {
        return;
    }
    if (prev_surface != nullptr) {
        // Deactivate the previously focused toplevel (client stops drawing
        // its focused decoration state, e.g. hides the caret).
        wlr_xdg_toplevel* prev = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev != nullptr) {
            wlr_xdg_toplevel_set_activated(prev, false);
        }
    }

    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    auto it = std::find(mapped_toplevels.begin(), mapped_toplevels.end(), toplevel);
    if (it != mapped_toplevels.end()) {
        mapped_toplevels.splice(mapped_toplevels.begin(), mapped_toplevels, it);
    }
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    // The seat tracks the focused surface and routes key events to it.
    if (wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat)) {
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
                                       keyboard->num_keycodes, &keyboard->modifiers);
    }
}

auto Server::Impl::toplevel_at(double lx, double ly, wlr_surface** surface, double* sx, double* sy)
    -> Toplevel* {
    // Topmost scene node at the given layout coords; we only care about
    // buffer nodes belonging to a surface tree rooted at a Toplevel.
    wlr_scene_node* node = wlr_scene_node_at(&scene->tree.node, lx, ly, sx, sy);
    if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
        return nullptr;
    }
    wlr_scene_buffer* scene_buffer = wlr_scene_buffer_from_node(node);
    wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (scene_surface == nullptr) {
        return nullptr;
    }
    *surface = scene_surface->surface;

    // Walk up to the tree whose data field we set: the Toplevel root.
    wlr_scene_tree* tree = node->parent;
    while (tree != nullptr && tree->node.data == nullptr) {
        tree = tree->node.parent;
    }
    if (tree == nullptr) {
        return nullptr;
    }
    return static_cast<Toplevel*>(tree->node.data);
}

// ---- Interactive move/resize grabs -------------------------------------------

void Server::Impl::reset_cursor_mode() {
    cursor_mode = CursorMode::Passthrough;
    grabbed_toplevel = nullptr;
}

void Server::Impl::begin_interactive(Toplevel* toplevel, CursorMode mode, std::uint32_t edges) {
    // The compositor consumes pointer events itself during a grab instead
    // of forwarding them. (tinywl note kept: a fuller compositor would
    // verify this against a recent button-press serial.)
    grabbed_toplevel = toplevel;
    cursor_mode = mode;

    if (mode == CursorMode::Move) {
        grab_x = cursor->x - toplevel->scene_tree->node.x;
        grab_y = cursor->y - toplevel->scene_tree->node.y;
    } else {
        wlr_box* geo_box = &toplevel->xdg_toplevel->base->geometry;
        const double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
                                ((edges & WLR_EDGE_RIGHT) != 0 ? geo_box->width : 0);
        const double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
                                ((edges & WLR_EDGE_BOTTOM) != 0 ? geo_box->height : 0);
        grab_x = cursor->x - border_x;
        grab_y = cursor->y - border_y;

        grab_geobox = *geo_box;
        grab_geobox.x += toplevel->scene_tree->node.x;
        grab_geobox.y += toplevel->scene_tree->node.y;
        resize_edges = edges;
    }
}

// ---- xdg-shell toplevels ------------------------------------------------------

void Server::Impl::handle_new_toplevel(wlr_xdg_toplevel* xdg_toplevel) {
    auto owned = std::make_unique<Toplevel>();
    Toplevel* toplevel = owned.get();
    toplevel->server = this;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->scene_tree = wlr_scene_xdg_surface_create(&scene->tree, xdg_toplevel->base);
    toplevel->scene_tree->node.data = toplevel;
    // Popups look this up to find their parent's scene tree.
    xdg_toplevel->base->data = toplevel->scene_tree;
    toplevels.emplace(xdg_toplevel, std::move(owned));

    toplevel->map.connect(xdg_toplevel->base->surface->events.map, [this, toplevel](void*) {
        mapped_toplevels.push_front(toplevel);
        focus_toplevel(toplevel);
        wlr_log(WLR_INFO, "toplevel mapped: %s",
                toplevel->xdg_toplevel->title != nullptr ? toplevel->xdg_toplevel->title : "?");
    });
    toplevel->unmap.connect(xdg_toplevel->base->surface->events.unmap, [this, toplevel](void*) {
        if (toplevel == grabbed_toplevel) {
            reset_cursor_mode();
        }
        mapped_toplevels.remove(toplevel);
    });
    toplevel->commit.connect(xdg_toplevel->base->surface->events.commit, [toplevel](void*) {
        if (toplevel->xdg_toplevel->base->initial_commit) {
            // Reply to the initial commit with a 0x0 configure: the client
            // picks its own dimensions.
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        }
    });
    toplevel->destroy.connect(xdg_toplevel->events.destroy, [this, toplevel](void*) {
        // Last action: destroys `toplevel` (and these listeners with it).
        toplevels.erase(toplevel->xdg_toplevel);
    });

    toplevel->request_move.connect(xdg_toplevel->events.request_move, [this, toplevel](void*) {
        begin_interactive(toplevel, CursorMode::Move, 0);
    });
    toplevel->request_resize.connect(
        xdg_toplevel->events.request_resize, [this, toplevel](void* data) {
            const auto* event = static_cast<wlr_xdg_toplevel_resize_event*>(data);
            begin_interactive(toplevel, CursorMode::Resize, event->edges);
        });
    toplevel->request_maximize.connect(
        xdg_toplevel->events.request_maximize, [toplevel](void*) {
            // Unsupported, but xdg-shell demands a configure reply.
            if (toplevel->xdg_toplevel->base->initialized) {
                wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
            }
        });
    toplevel->request_fullscreen.connect(
        xdg_toplevel->events.request_fullscreen, [toplevel](void*) {
            if (toplevel->xdg_toplevel->base->initialized) {
                wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
            }
        });
}

// ---- xdg-shell popups ----------------------------------------------------------

void Server::Impl::handle_new_popup(wlr_xdg_popup* xdg_popup) {
    auto owned = std::make_unique<Popup>();
    Popup* popup = owned.get();
    popup->xdg_popup = xdg_popup;
    popups.emplace(xdg_popup, std::move(owned));

    // Parent's scene tree was stashed in base->data when it was created
    // (toplevel or ancestor popup).
    wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    if (parent != nullptr && parent->data != nullptr) {
        auto* parent_tree = static_cast<wlr_scene_tree*>(parent->data);
        xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    }

    popup->commit.connect(xdg_popup->base->surface->events.commit, [popup](void*) {
        if (popup->xdg_popup->base->initial_commit) {
            // A fuller compositor would also unconstrain the popup to keep
            // it on-screen here.
            wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
        }
    });
    popup->destroy.connect(xdg_popup->events.destroy, [this, popup](void*) {
        // Last action: destroys `popup` (and these listeners with it).
        popups.erase(popup->xdg_popup);
    });
}

} // namespace unbox::kernel
