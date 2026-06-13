#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>

#include "policy.hpp"
#include "probe.hpp"

#include <unbox/kernel/host.hpp>
#include <unbox/kernel/listener.hpp>
#include <unbox/kernel/wlr.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

// The glue. Translates the kernel's input/output catalogue and the raw
// xdg-shell signals into window-management policy, calls the pure core in
// policy.hpp for decisions, and exports the typed Service/Events downstream
// slices consume. Mirrors the former kernel toplevel.cpp/input.cpp tinywl
// shape, re-expressed against the Host ABI.
//
// Everything runs on the single wl_event_loop thread. Every resource is a RAII
// member of XdgShellExtension; teardown is reverse-declaration destruction (no
// manual teardown lists — extension-agent.md).

namespace unbox::ext_xdg_shell {
namespace {

using kernel::Host;
using kernel::Listener;
using kernel::Subscription;

class XdgShellExtension;

// One managed application window. Implements the public Toplevel handle; the
// extension hands borrows of `Entry*` (upcast to Toplevel*) in event payloads.
struct ToplevelEntry final : Toplevel {
    XdgShellExtension* ext = nullptr;
    wlr_xdg_toplevel* xdg_toplevel = nullptr;
    wlr_scene_tree* scene = nullptr;
    bool mapped = false;

    // Typed surface->scene-tree association (replaces the old .data
    // convention); held here so it unregisters when the toplevel is destroyed.
    kernel::SurfaceRegistration surface_reg;

    Listener map;
    Listener unmap;
    Listener commit;
    Listener destroy;
    Listener request_move;
    Listener request_resize;
    Listener request_maximize;
    Listener request_fullscreen;

    // ---- public Toplevel contract ----
    [[nodiscard]] auto title() const -> std::string_view override {
        const char* t = xdg_toplevel != nullptr ? xdg_toplevel->title : nullptr;
        return t != nullptr ? std::string_view{t} : std::string_view{};
    }
    [[nodiscard]] auto app_id() const -> std::string_view override {
        const char* a = xdg_toplevel != nullptr ? xdg_toplevel->app_id : nullptr;
        return a != nullptr ? std::string_view{a} : std::string_view{};
    }
    void focus() override; // defined after XdgShellExtension
    void close() override {
        if (xdg_toplevel != nullptr) {
            wlr_xdg_toplevel_send_close(xdg_toplevel);
        }
    }

    // ---- minimize mechanism (slice 10 / b1) ----
    [[nodiscard]] auto geometry() const -> wlr_box override {
        // Layout origin = scene-node position offset by the window-geometry
        // origin (same decomposition begin_resize uses); size = current xdg
        // window geometry (wlroots falls this back to the surface extent when
        // the client set no explicit geometry).
        wlr_box box{};
        if (scene == nullptr || xdg_toplevel == nullptr) {
            return box;
        }
        const wlr_box& geo = xdg_toplevel->base->geometry;
        box.x = scene->node.x + geo.x;
        box.y = scene->node.y + geo.y;
        box.width = geo.width;
        box.height = geo.height;
        return box;
    }
    [[nodiscard]] auto scene_tree() -> wlr_scene_tree* override { return scene; }
    void hide() override {
        if (scene != nullptr) {
            wlr_scene_node_set_enabled(&scene->node, false);
        }
    }
    void show() override {
        if (scene != nullptr) {
            wlr_scene_node_set_enabled(&scene->node, true);
        }
    }
};

struct PopupEntry {
    wlr_xdg_popup* xdg_popup = nullptr;
    // Typed association for THIS popup's surface, so a nested popup parented to
    // it resolves via Host::scene_tree_for().
    kernel::SurfaceRegistration surface_reg;
    Listener commit;
    Listener destroy;
};

class XdgShellExtension final : public kernel::Extension, public ActivationProbe {
public:
    XdgShellExtension()
        : manifest_{"xdg-shell", kernel::Tier::core, {}} {}

    [[nodiscard]] auto manifest() const -> const kernel::Manifest& override { return manifest_; }

    void activate(Host& host) override {
        host_ = &host;
        service_.ext = this;

        // Export our hooks through the kernel's isolation registry so a
        // throwing subscriber disables the SUBSCRIBER, not us.
        host.adopt(on_mapped_);
        host.adopt(on_unmapped_);
        host.adopt(on_focused_);

        // The xdg-shell v3 global lives HERE now (extension-creates-the-global
        // split). wlr_xdg_shell is owned by the display; it dies with it.
        xdg_shell_ = wlr_xdg_shell_create(host.display(), 3);
        if (xdg_shell_ == nullptr) {
            throw std::runtime_error("ext-xdg-shell: wlr_xdg_shell_create failed");
        }
        new_toplevel_.connect(xdg_shell_->events.new_toplevel, [this](void* data) {
            handle_new_toplevel(static_cast<wlr_xdg_toplevel*>(data));
        });
        new_popup_.connect(xdg_shell_->events.new_popup, [this](void* data) {
            handle_new_popup(static_cast<wlr_xdg_popup*>(data));
        });

        // Pointer routing is ENTIRELY ours: the kernel moves the cursor and
        // emits, but forwards NOTHING to clients (host.hpp catalogue). We
        // hit-test the scene and call wlr_seat_pointer_notify_enter/motion/
        // button/axis/frame, plus focus + grab handling. Grabs suppress the
        // forward simply by not notifying while a move/resize is in flight.
        sub_motion_ = host.subscribe(host.on_pointer_motion(),
                                     [this](const kernel::PointerMotionEvent& e) {
                                         process_pointer_motion(e.lx, e.ly, e.time_msec);
                                     });
        sub_button_ = host.subscribe(host.on_pointer_button(),
                                     [this](const kernel::PointerButtonEvent& e) {
                                         process_pointer_button(e);
                                     });
        sub_axis_ = host.subscribe(host.on_pointer_axis(),
                                   [this](const kernel::PointerAxisEvent& e) {
                                       process_pointer_axis(e);
                                   });
        sub_frame_ = host.subscribe(host.on_pointer_frame(),
                                    [this] { wlr_seat_pointer_notify_frame(host_->seat()); });

        // Touch (slice-2 parity): tap-to-focus + down/up/motion routing.
        sub_touch_down_ = host.subscribe(host.on_touch_down(),
                                         [this](const kernel::TouchDownEvent& e) {
                                             process_touch_down(e);
                                         });
        sub_touch_motion_ = host.subscribe(host.on_touch_motion(),
                                           [this](const kernel::TouchMotionEvent& e) {
                                               process_touch_motion(e);
                                           });
        sub_touch_up_ = host.subscribe(host.on_touch_up(),
                                       [this](const kernel::TouchUpEvent& e) {
                                           process_touch_up(e);
                                       });
        sub_touch_cancel_ = host.subscribe(host.on_touch_cancel(),
                                           [this](const kernel::TouchCancelEvent& e) {
                                               process_touch_cancel(e);
                                           });
        sub_touch_frame_ = host.subscribe(host.on_touch_frame(),
                                           [this] { wlr_seat_touch_notify_frame(host_->seat()); });

        // Register the typed service so downstream slices link against us.
        host.provide_service<Service>(&service_);

        activated_ = true;
    }

    // Probe used by the headless integration test to confirm activation ran.
    [[nodiscard]] auto activated() const -> bool override { return activated_; }

    // ---- exported hooks (also reached through service_) ----
    [[nodiscard]] auto on_mapped() -> kernel::Event<const ToplevelEvent&>& { return on_mapped_; }
    [[nodiscard]] auto on_unmapped() -> kernel::Event<const ToplevelEvent&>& {
        return on_unmapped_;
    }
    [[nodiscard]] auto on_focused() -> kernel::Event<const ToplevelEvent&>& { return on_focused_; }

    // ---- focus / hit-test (used by ToplevelEntry::focus and the glue) ----
    void focus_toplevel(ToplevelEntry* entry) {
        if (entry == nullptr || !entry->mapped) {
            return;
        }
        wlr_seat* seat = host_->seat();
        wlr_surface* surface = entry->xdg_toplevel->base->surface;
        wlr_surface* prev = seat->keyboard_state.focused_surface;
        if (prev == surface) {
            return;
        }
        if (prev != nullptr) {
            if (wlr_xdg_toplevel* p = wlr_xdg_toplevel_try_from_wlr_surface(prev)) {
                wlr_xdg_toplevel_set_activated(p, false);
            }
        }
        wlr_scene_node_raise_to_top(&entry->scene->node);

        wlr_xdg_toplevel_set_activated(entry->xdg_toplevel, true);
        if (wlr_keyboard* kb = wlr_seat_get_keyboard(seat)) {
            wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes, kb->num_keycodes,
                                           &kb->modifiers);
        }
        emit(on_focused_, entry);
    }

private:
    // ---- toplevel lifecycle ----
    void handle_new_toplevel(wlr_xdg_toplevel* xdg_toplevel) {
        auto owned = std::make_unique<ToplevelEntry>();
        ToplevelEntry* entry = owned.get();
        entry->ext = this;
        entry->xdg_toplevel = xdg_toplevel;
        entry->scene = wlr_scene_xdg_surface_create(
            host_->scene_layer(kernel::SceneLayer::normal), xdg_toplevel->base);
        // PRIVATE bookkeeping: our own hit-test recovers the entry from the
        // tree node's data (an intra-unit use of .data, which the registry
        // contract explicitly still permits). The CROSS-UNIT surface->tree
        // coupling goes through the typed registry below, never .data.
        entry->scene->node.data = entry;
        // Typed surface->scene-tree association so popups (ours or descendants)
        // resolve this toplevel's tree via Host::scene_tree_for().
        entry->surface_reg =
            host_->host_surface(xdg_toplevel->base->surface, entry->scene);
        toplevels_.emplace(xdg_toplevel, std::move(owned));

        entry->map.connect(xdg_toplevel->base->surface->events.map, [this, entry](void*) {
            entry->mapped = true;
            focus_toplevel(entry);
            emit(on_mapped_, entry);
        });
        entry->unmap.connect(xdg_toplevel->base->surface->events.unmap, [this, entry](void*) {
            emit(on_unmapped_, entry);
            if (entry == grabbed_) {
                grab_.on_grab_target_lost();
                end_grab();
            }
            entry->mapped = false;
        });
        entry->commit.connect(xdg_toplevel->base->surface->events.commit, [entry](void*) {
            if (entry->xdg_toplevel->base->initial_commit) {
                // 0x0 configure: the client picks its own dimensions (tinywl).
                wlr_xdg_toplevel_set_size(entry->xdg_toplevel, 0, 0);
            }
        });
        entry->destroy.connect(xdg_toplevel->events.destroy, [this, entry](void*) {
            // Last action: erases `entry` (and its Listeners with it).
            toplevels_.erase(entry->xdg_toplevel);
        });
        entry->request_move.connect(xdg_toplevel->events.request_move, [this, entry](void*) {
            begin_move(entry);
        });
        entry->request_resize.connect(xdg_toplevel->events.request_resize,
                                      [this, entry](void* data) {
                                          const auto* ev =
                                              static_cast<wlr_xdg_toplevel_resize_event*>(data);
                                          begin_resize(entry, ev->edges);
                                      });
        entry->request_maximize.connect(xdg_toplevel->events.request_maximize, [entry](void*) {
            // Unsupported, but xdg-shell demands a configure reply.
            if (entry->xdg_toplevel->base->initialized) {
                wlr_xdg_surface_schedule_configure(entry->xdg_toplevel->base);
            }
        });
        entry->request_fullscreen.connect(xdg_toplevel->events.request_fullscreen,
                                          [entry](void*) {
                                              if (entry->xdg_toplevel->base->initialized) {
                                                  wlr_xdg_surface_schedule_configure(
                                                      entry->xdg_toplevel->base);
                                              }
                                          });
    }

    // ---- popup lifecycle (typed surface->tree registry; .data convention dead)
    void handle_new_popup(wlr_xdg_popup* xdg_popup) {
        auto owned = std::make_unique<PopupEntry>();
        PopupEntry* popup = owned.get();
        popup->xdg_popup = xdg_popup;
        popups_.emplace(xdg_popup, std::move(owned));

        // Parent scene tree resolved through the typed kernel contract — works
        // uniformly for an xdg parent (our toplevel/ancestor popup) AND a
        // non-xdg parent (a layer surface registered by ext-layer-shell). No
        // wlr_surface.data / wlr_xdg_surface.data agreement anymore.
        wlr_scene_tree* parent_tree =
            xdg_popup->parent != nullptr ? host_->scene_tree_for(xdg_popup->parent) : nullptr;
        if (parent_tree != nullptr) {
            wlr_scene_tree* popup_tree =
                wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
            // Register this popup's surface so a nested popup parented to it can
            // resolve through the same typed contract.
            popup->surface_reg =
                host_->host_surface(xdg_popup->base->surface, popup_tree);
        }

        popup->commit.connect(xdg_popup->base->surface->events.commit, [popup](void*) {
            if (popup->xdg_popup->base->initial_commit) {
                wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
            }
        });
        popup->destroy.connect(xdg_popup->events.destroy, [this, popup](void*) {
            // Last action: erases `popup` (and its Listeners with it).
            popups_.erase(popup->xdg_popup);
        });
    }

    // ---- scene hit-test (mirrors the former kernel toplevel_at) ----
    auto toplevel_at(double lx, double ly, wlr_surface** surface, double* sx, double* sy)
        -> ToplevelEntry* {
        wlr_scene_node* node = wlr_scene_node_at(&host_->scene()->tree.node, lx, ly, sx, sy);
        if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
            return nullptr;
        }
        wlr_scene_buffer* scene_buffer = wlr_scene_buffer_from_node(node);
        wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
        if (scene_surface == nullptr) {
            return nullptr;
        }
        *surface = scene_surface->surface;
        // Walk up to the tree whose data we set: the ToplevelEntry root.
        wlr_scene_tree* tree = node->parent;
        while (tree != nullptr && tree->node.data == nullptr) {
            tree = tree->node.parent;
        }
        if (tree == nullptr) {
            return nullptr;
        }
        // Only our toplevel trees carry a ToplevelEntry* (popups carry a
        // wlr_scene_tree* in base->data, not node.data, so they read null and
        // are skipped here — same as the former kernel).
        return static_cast<ToplevelEntry*>(tree->node.data);
    }

    // ---- interactive grabs ----
    // Tear down the grab: clear the grab target and restore the default cursor.
    // Called when the pure GrabMachine reports the grab ended (button release)
    // or the grabbed toplevel vanished.
    void end_grab() {
        grabbed_ = nullptr;
        wlr_cursor_set_xcursor(host_->cursor(), host_->cursor_manager(), "default");
    }

    // The client requested an interactive move. The pure machine decides
    // whether a grab actually engages and PINS it to the driving input source
    // (pointer button or the originating touch point); a request with nothing
    // down is ignored (the "drags without clicking" fix). Geometry is captured
    // from the driver's CURRENT layout position only when the grab engages —
    // the pointer's cursor position, or the touch point's last layout coords.
    void begin_move(ToplevelEntry* entry) {
        if (!grab_.on_request_move()) {
            return; // nothing held: do not start an unclicked drag
        }
        double lx = 0;
        double ly = 0;
        grab_driver_layout(&lx, &ly);
        grabbed_ = entry;
        grab_x_ = lx - entry->scene->node.x;
        grab_y_ = ly - entry->scene->node.y;
    }

    void begin_resize(ToplevelEntry* entry, std::uint32_t edges) {
        if (!grab_.on_request_resize()) {
            return;
        }
        double lx = 0;
        double ly = 0;
        grab_driver_layout(&lx, &ly);
        grabbed_ = entry;
        wlr_box* geo = &entry->xdg_toplevel->base->geometry;
        const double border_x = (entry->scene->node.x + geo->x) +
                                ((edges & WLR_EDGE_RIGHT) != 0 ? geo->width : 0);
        const double border_y = (entry->scene->node.y + geo->y) +
                                ((edges & WLR_EDGE_BOTTOM) != 0 ? geo->height : 0);
        grab_x_ = lx - border_x;
        grab_y_ = ly - border_y;
        grab_geobox_ = *geo;
        grab_geobox_.x += entry->scene->node.x;
        grab_geobox_.y += entry->scene->node.y;
        resize_edges_ = edges;
    }

    // Current layout position of whatever input drives the grab: the touch
    // origin point if touch-driven, else the pointer cursor.
    void grab_driver_layout(double* lx, double* ly) {
        if (grab_.touch_driven()) {
            *lx = grab_touch_lx_;
            *ly = grab_touch_ly_;
        } else {
            wlr_cursor* cursor = host_->cursor();
            *lx = cursor->x;
            *ly = cursor->y;
        }
    }

    void process_cursor_move(double lx, double ly) {
        wlr_scene_node_set_position(&grabbed_->scene->node,
                                    static_cast<int>(lx - grab_x_),
                                    static_cast<int>(ly - grab_y_));
    }

    void process_cursor_resize(double lx, double ly) {
        const double border_x = lx - grab_x_;
        const double border_y = ly - grab_y_;
        int new_left = grab_geobox_.x;
        int new_right = grab_geobox_.x + grab_geobox_.width;
        int new_top = grab_geobox_.y;
        int new_bottom = grab_geobox_.y + grab_geobox_.height;

        if ((resize_edges_ & WLR_EDGE_TOP) != 0) {
            new_top = static_cast<int>(border_y);
            if (new_top >= new_bottom) {
                new_top = new_bottom - 1;
            }
        } else if ((resize_edges_ & WLR_EDGE_BOTTOM) != 0) {
            new_bottom = static_cast<int>(border_y);
            if (new_bottom <= new_top) {
                new_bottom = new_top + 1;
            }
        }
        if ((resize_edges_ & WLR_EDGE_LEFT) != 0) {
            new_left = static_cast<int>(border_x);
            if (new_left >= new_right) {
                new_left = new_right - 1;
            }
        } else if ((resize_edges_ & WLR_EDGE_RIGHT) != 0) {
            new_right = static_cast<int>(border_x);
            if (new_right <= new_left) {
                new_right = new_left + 1;
            }
        }
        wlr_box* geo = &grabbed_->xdg_toplevel->base->geometry;
        wlr_scene_node_set_position(&grabbed_->scene->node, new_left - geo->x,
                                    new_top - geo->y);
        wlr_xdg_toplevel_set_size(grabbed_->xdg_toplevel, new_right - new_left,
                                  new_bottom - new_top);
    }

    // ---- pointer routing ----
    void process_pointer_motion(double lx, double ly, std::uint32_t time_msec) {
        switch (grab_.on_motion()) {
        case policy::GrabAction::move_toplevel:
            process_cursor_move(lx, ly);
            return; // suppress client passthrough during a grab
        case policy::GrabAction::resize_toplevel:
            process_cursor_resize(lx, ly);
            return;
        case policy::GrabAction::end_grab:
        case policy::GrabAction::none:
            break; // passthrough below
        }
        double sx = 0;
        double sy = 0;
        wlr_surface* surface = nullptr;
        ToplevelEntry* entry = toplevel_at(lx, ly, &surface, &sx, &sy);
        if (entry == nullptr) {
            // Over nothing: draw the default cursor ourselves.
            wlr_cursor_set_xcursor(host_->cursor(), host_->cursor_manager(), "default");
        }
        wlr_seat* seat = host_->seat();
        if (surface != nullptr) {
            wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(seat, time_msec, sx, sy);
        } else {
            wlr_seat_pointer_clear_focus(seat);
        }
    }

    void process_pointer_button(const kernel::PointerButtonEvent& e) {
        // Forwarding the button to the focused client is OURS (host.hpp: the
        // kernel forwards nothing). Feed the press/release to the pure grab
        // machine FIRST: it tracks button-down (so a later request_move only
        // grabs while held) and reports when a release ends a grab.
        const bool was_grabbing = grab_.grabbing();
        const policy::GrabAction action = grab_.on_button(e.pressed);
        const auto state =
            e.pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;

        if (action == policy::GrabAction::end_grab) {
            // The release that ends a pointer grab. We forwarded the PRESS that
            // begat the grab (before request_move arrived), which started the
            // wlr_seat IMPLICIT pointer grab; we MUST forward the matching
            // RELEASE now to close it. Skipping it (the old bug) left the seat's
            // implicit grab stuck forever, which then swallowed every later
            // touch-down — so after one mouse drag, touch grabs never engaged
            // again. The client that requested the move ignores this stray
            // release. THEN tear down our own grab.
            wlr_seat_pointer_notify_button(host_->seat(), e.time_msec, e.button, state);
            end_grab();
            return;
        }
        if (was_grabbing) {
            return; // still grabbing (shouldn't happen on a button, but be safe)
        }

        wlr_seat_pointer_notify_button(host_->seat(), e.time_msec, e.button, state);
        if (e.pressed) {
            // Click-to-focus on press.
            double sx = 0;
            double sy = 0;
            wlr_surface* surface = nullptr;
            focus_toplevel(toplevel_at(e.lx, e.ly, &surface, &sx, &sy));
        }
    }

    void process_pointer_axis(const kernel::PointerAxisEvent& e) {
        // Scroll forwarding is OURS (host.hpp: kernel forwards nothing).
        // Suppress during a grab, consistent with button/motion.
        if (grab_.grabbing()) {
            return;
        }
        wlr_seat_pointer_notify_axis(host_->seat(), e.time_msec, e.orientation, e.delta,
                                     e.delta_discrete, e.source,
                                     WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    }

    // ---- touch routing (slice-2 parity + touch-initiated grabs) ----
    void process_touch_down(const kernel::TouchDownEvent& e) {
        // Tell the grab machine a touch point is down BEFORE the client's
        // request_move can arrive (round-trip), so a touch CSD drag engages.
        grab_.on_touch_down(e.touch_id);
        // Track its layout position to drive a touch-driven grab.
        grab_touch_lx_ = e.lx;
        grab_touch_ly_ = e.ly;

        double sx = 0;
        double sy = 0;
        wlr_surface* surface = nullptr;
        ToplevelEntry* entry = toplevel_at(e.lx, e.ly, &surface, &sx, &sy);
        if (entry != nullptr) {
            focus_toplevel(entry); // tap raises + focuses
        }
        if (surface != nullptr) {
            // Record the surface's layout origin (lx - sx) so motion can derive
            // surface-local coords. Known layout-origin-during-grab skew is
            // accepted until slice 5 (see package doc).
            touch_points_.insert_or_assign(e.touch_id,
                                           TouchPoint{surface, e.lx - sx, e.ly - sy});
            wlr_seat_touch_notify_down(host_->seat(), surface, e.time_msec, e.touch_id, sx, sy);
        }
    }

    void process_touch_motion(const kernel::TouchMotionEvent& e) {
        // Keep the originating point's layout position fresh for the grab.
        if (grab_.touch_driven()) {
            grab_touch_lx_ = e.lx;
            grab_touch_ly_ = e.ly;
        }
        switch (grab_.on_touch_motion(e.touch_id)) {
        case policy::GrabAction::move_toplevel:
            process_cursor_move(e.lx, e.ly);
            return; // grab consumes the drag: no client notify (avoids the
                    // moving-surface-origin skew fighting the grab)
        case policy::GrabAction::resize_toplevel:
            process_cursor_resize(e.lx, e.ly);
            return;
        case policy::GrabAction::end_grab:
        case policy::GrabAction::none:
            break; // passthrough below
        }
        auto it = touch_points_.find(e.touch_id);
        if (it == touch_points_.end()) {
            return; // down landed on no surface; nothing to route
        }
        wlr_seat_touch_notify_motion(host_->seat(), e.time_msec, e.touch_id,
                                     e.lx - it->second.origin_x, e.ly - it->second.origin_y);
    }

    void process_touch_up(const kernel::TouchUpEvent& e) {
        // The originating point lifting ends a touch-driven grab.
        if (grab_.on_touch_up(e.touch_id) == policy::GrabAction::end_grab) {
            end_grab();
        }
        touch_points_.erase(e.touch_id);
        wlr_seat_touch_notify_up(host_->seat(), e.time_msec, e.touch_id);
    }

    void process_touch_cancel(const kernel::TouchCancelEvent& e) {
        if (grab_.on_touch_cancel(e.touch_id) == policy::GrabAction::end_grab) {
            end_grab();
        }
        touch_points_.erase(e.touch_id);
        if (wlr_touch_point* point = wlr_seat_touch_get_point(host_->seat(), e.touch_id)) {
            wlr_seat_touch_notify_cancel(host_->seat(), point->client);
        }
    }

    void emit(kernel::Event<const ToplevelEvent&>& ev, ToplevelEntry* entry) {
        const ToplevelEvent payload{entry};
        ev.emit(payload);
    }

    // ---- the Service the public header exposes ----
    struct ServiceImpl final : Service {
        XdgShellExtension* ext = nullptr;
        [[nodiscard]] auto on_toplevel_mapped()
            -> kernel::Event<const ToplevelEvent&>& override {
            return ext->on_mapped();
        }
        [[nodiscard]] auto on_toplevel_unmapped()
            -> kernel::Event<const ToplevelEvent&>& override {
            return ext->on_unmapped();
        }
        [[nodiscard]] auto on_toplevel_focused()
            -> kernel::Event<const ToplevelEvent&>& override {
            return ext->on_focused();
        }
    };

    struct TouchPoint {
        wlr_surface* surface = nullptr;
        double origin_x = 0.0;
        double origin_y = 0.0;
    };

    kernel::Manifest manifest_;
    Host* host_ = nullptr;
    wlr_xdg_shell* xdg_shell_ = nullptr;
    bool activated_ = false;

    // Exported hooks (adopt()ed; stable members — never moved).
    kernel::Event<const ToplevelEvent&> on_mapped_;
    kernel::Event<const ToplevelEvent&> on_unmapped_;
    kernel::Event<const ToplevelEvent&> on_focused_;
    ServiceImpl service_{};

    // Window/popup ownership.
    std::unordered_map<wlr_xdg_toplevel*, std::unique_ptr<ToplevelEntry>> toplevels_;
    std::unordered_map<wlr_xdg_popup*, std::unique_ptr<PopupEntry>> popups_;

    // Grab state (one at a time). The pure machine owns the move/resize/none
    // mode + button-down tracking; these hold the geometry for the active grab.
    policy::GrabMachine grab_;
    ToplevelEntry* grabbed_ = nullptr;
    double grab_x_ = 0.0;
    double grab_y_ = 0.0;
    wlr_box grab_geobox_{};
    std::uint32_t resize_edges_ = 0;
    // Last layout position of the originating touch point (drives a
    // touch-driven grab; the pointer path reads wlr_cursor instead).
    double grab_touch_lx_ = 0.0;
    double grab_touch_ly_ = 0.0;

    // Touch implicit grabs (touch_id -> origin surface + layout origin).
    std::unordered_map<std::int32_t, TouchPoint> touch_points_;

    // Raw xdg-shell signal listeners (RAII).
    Listener new_toplevel_;
    Listener new_popup_;

    // Kernel-catalogue subscriptions (RAII; dropped on destruction).
    Subscription sub_motion_;
    Subscription sub_button_;
    Subscription sub_axis_;
    Subscription sub_frame_;
    Subscription sub_touch_down_;
    Subscription sub_touch_motion_;
    Subscription sub_touch_up_;
    Subscription sub_touch_cancel_;
    Subscription sub_touch_frame_;

    friend struct ServiceImpl;
};

void ToplevelEntry::focus() { ext->focus_toplevel(this); }

} // namespace

auto create() -> std::unique_ptr<kernel::Extension> {
    return std::make_unique<XdgShellExtension>();
}

auto make_extension_with_probe() -> ExtensionWithProbe {
    auto ext = std::make_unique<XdgShellExtension>();
    ActivationProbe* probe = ext.get();
    return {std::move(ext), probe};
}

} // namespace unbox::ext_xdg_shell
