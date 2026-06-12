#include <unbox/ext-layer-shell/ext_layer_shell.hpp>

#include <unbox/ext-layer-shell/arrangement.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/listener.hpp>
#include <unbox/kernel/wlr.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

// ext-layer-shell glue. The decision core (which edge a surface reserves, how
// the usable area shrinks) lives in arrangement.hpp and is exercised without
// wlroots; THIS file is the thin effectful edge that binds wlroots signals and
// drives wlr_scene_layer_surface_v1_configure. The usable-area model in
// arrangement.hpp mirrors what that helper mutates — we keep our own per-output
// copy for bookkeeping and as the basis tiling (slice 7) will consume.

namespace unbox::ext_layer_shell {
namespace {

using kernel::Host;
using kernel::Listener;

// Map a protocol layer to its kernel SceneLayer band. background/bottom/top/
// overlay map 1:1; `normal` is toplevels-only and never a layer-shell band.
auto band_for_layer(enum zwlr_layer_shell_v1_layer layer) -> kernel::SceneLayer {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        return kernel::SceneLayer::background;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        return kernel::SceneLayer::bottom;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        return kernel::SceneLayer::top;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        return kernel::SceneLayer::overlay;
    }
    return kernel::SceneLayer::top; // unreachable: protocol validates the enum
}

// Give `surface` the seat keyboard focus, forwarding the currently-pressed
// keycodes/modifiers if a keyboard is present (wlroots defers to any active
// grab). Used for both `exclusive` (focus on map) and `on_demand` (focus on
// click/tap) interactivity.
void focus_keyboard(Host& host, wlr_surface* surface) {
    wlr_seat* seat = host.seat();
    wlr_keyboard* kbd = wlr_seat_get_keyboard(seat);
    if (kbd != nullptr) {
        wlr_seat_keyboard_notify_enter(seat, surface, kbd->keycodes,
                                       kbd->num_keycodes, &kbd->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(seat, surface, nullptr, 0, nullptr);
    }
}

class LayerShellExt;

// One live layer surface: its scene node and the wlroots signal bindings.
// Owned (unique_ptr) by LayerShellExt; destroyed when the wlroots layer surface
// is destroyed (its own destroy handler erases it from the owner) or at
// shutdown (reverse-declaration teardown of the owning extension).
class LayerSurface {
public:
    LayerSurface(LayerShellExt& owner, wlr_layer_surface_v1* surface,
                 wlr_scene_layer_surface_v1* scene);

    [[nodiscard]] auto wlr() const -> wlr_layer_surface_v1* { return surface_; }
    [[nodiscard]] auto scene() const -> wlr_scene_layer_surface_v1* { return scene_; }
    [[nodiscard]] auto output() const -> wlr_output* { return surface_->output; }

private:
    void update_keyboard_focus();

    LayerShellExt& owner_;
    wlr_layer_surface_v1* surface_;     // borrow; kernel/wlroots-owned
    wlr_scene_layer_surface_v1* scene_; // owned node, destroyed via destroy_
    // Typed surface->scene-tree association (replaces the dead wlr_surface.data
    // convention): lets ext-xdg-shell resolve popup parents via
    // Host::scene_tree_for(). Declared AFTER scene_ so it tears down (releasing
    // the map entry) before the node reference goes — reverse-declaration order.
    kernel::SurfaceRegistration host_reg_;
    Listener commit_;
    Listener destroy_;
    Listener new_popup_;
};

class LayerShellExt final : public kernel::Extension {
public:
    LayerShellExt() = default;

    auto manifest() const -> const kernel::Manifest& override { return manifest_; }

    void activate(Host& host) override {
        host_ = &host;

        // The global: version 5 (the vendored protocol XML / wlroots 0.20 cap).
        shell_ = wlr_layer_shell_v1_create(host.display(), 5);
        if (shell_ == nullptr) {
            throw std::runtime_error(
                "ext-layer-shell: wlr_layer_shell_v1_create failed");
        }

        new_surface_.connect(shell_->events.new_surface, [this](void* data) {
            on_new_surface(static_cast<wlr_layer_surface_v1*>(data));
        });

        // Track outputs: assign one to outputless surfaces and re-arrange when
        // the output set changes.
        output_added_ = host.subscribe(
            host.on_output_added(),
            [this](const kernel::OutputEvent& e) { on_output_added(e.output); });
        output_removed_ = host.subscribe(
            host.on_output_removed(),
            [this](const kernel::OutputEvent& e) { on_output_removed(e.output); });

        // Seed outputs that ALREADY EXIST at activation. Server::create() starts
        // the backend, so a nested/headless output is added during create() —
        // BEFORE extensions activate at run(). on_output_added fires only for
        // outputs added AFTER we subscribe above, so events-only tracking would
        // miss the pre-existing one forever: an output-less layer surface (e.g.
        // fuzzel, which passes nil output) would then get no output assigned, no
        // wlr_scene_layer_surface_v1_configure, no configure event, and we'd
        // close it. wlr_output_layout retains already-added outputs, so we
        // enumerate them here. (Contract gap noted in the report: late
        // subscribers miss kernel state; the kernel could replay or expose an
        // outputs() borrow. Fixed within the unit with what exists today.)
        wlr_output_layout_output* lo = nullptr;
        wl_list_for_each(lo, &host.output_layout()->outputs, link) {
            track_output(lo->output);
        }

        // on_demand keyboard interactivity (zwlr v4): focus a layer surface when
        // the user clicks OR taps it. We take focus only on interaction with OUR
        // surface and never steal it back — clicking/tapping elsewhere lets focus
        // move away normally (some other extension's hit handler, or a focus
        // clear, owns that). These are fire-and-forget Events with N subscribers,
        // so coexisting with ext-xdg-shell's toplevel-focusing handler on the
        // same hit stream is fine — the hits are disjoint (its toplevels vs our
        // layer surfaces). The kernel has already consumed any hit over its own
        // UI surfaces before we see the event; layer surfaces are client surfaces
        // and unaffected.
        pointer_button_ = host.subscribe(
            host.on_pointer_button(), [this](const kernel::PointerButtonEvent& e) {
                if (e.pressed) {
                    focus_on_demand_at(e.lx, e.ly);
                }
            });
        touch_down_ = host.subscribe(
            host.on_touch_down(), [this](const kernel::TouchDownEvent& e) {
                focus_on_demand_at(e.lx, e.ly);
            });
    }

    [[nodiscard]] auto host() -> Host& { return *host_; }

    // Recompute `output`'s usable area from every live surface on it and push a
    // configure to each. Called on commit and on output add/remove/change.
    void arrange(wlr_output* output) {
        if (output == nullptr || host_ == nullptr) {
            return;
        }
        wlr_box full{};
        wlr_output_layout_get_box(host_->output_layout(), output, &full);
        if (wlr_box_empty(&full)) {
            full = {.x = 0, .y = 0, .width = output->width, .height = output->height};
        }
        wlr_box usable = full;
        for (const auto& ls : surfaces_) {
            if (ls->output() == output) {
                wlr_scene_layer_surface_v1_configure(ls->scene(), &full, &usable);
            }
        }
        // Mirror the remaining usable area into our pure-core box (the basis a
        // usable-area service would publish; deliberately not exported as a
        // hook yet — see the report).
        usable_area(output) = Box{usable.x, usable.y, usable.width, usable.height};
    }

    // Drop a surface from the owned set (called from its destroy handler as the
    // LAST action — this deletes the LayerSurface).
    void erase(LayerSurface* which) {
        std::erase_if(surfaces_,
                      [which](const std::unique_ptr<LayerSurface>& p) {
                          return p.get() == which;
                      });
    }

private:
    void on_new_surface(wlr_layer_surface_v1* surface) {
        // Assign an output if the client did not request one (fuzzel passes
        // nil): fall back to the first tracked output. If NO output exists yet,
        // defer placement instead of closing the surface — wlroots requires an
        // output set before the first configure, and the protocol guarantees
        // the compositor will EVENTUALLY configure an unmapped surface. The
        // surface is parked in pending_ and placed when an output appears.
        if (surface->output == nullptr && outputs_.empty()) {
            park_pending(surface);
            return;
        }
        if (surface->output == nullptr) {
            surface->output = outputs_.front();
        }
        place(surface);
    }

    // Create the scene node and the live LayerSurface for `surface` (which now
    // has an output assigned). Its commit listener drives the first configure.
    void place(wlr_layer_surface_v1* surface) {
        wlr_scene_tree* parent =
            host_->scene_layer(band_for_layer(surface->current.layer));
        wlr_scene_layer_surface_v1* scene =
            wlr_scene_layer_surface_v1_create(parent, surface);
        if (scene == nullptr) {
            wlr_layer_surface_v1_destroy(surface);
            return;
        }

        // The LayerSurface registers surface->tree via Host::host_surface() in
        // its constructor (the typed surface->scene-tree contract), so xdg
        // popups parented to this layer surface resolve through
        // Host::scene_tree_for() — no wlr_surface.data write here.
        surfaces_.push_back(std::make_unique<LayerSurface>(*this, surface, scene));
    }

    // Park an output-less surface that arrived before any output exists. We
    // hold only a destroy listener so a client that gives up before an output
    // appears is dropped cleanly; placement happens in adopt_pending_surfaces.
    void park_pending(wlr_layer_surface_v1* surface) {
        auto pending = std::make_unique<Pending>();
        pending->surface = surface;
        Pending* raw = pending.get();
        pending->destroy.connect(surface->events.destroy, [this, raw](void*) {
            std::erase_if(pending_, [raw](const std::unique_ptr<Pending>& p) {
                return p.get() == raw; // LAST action: deletes the Pending
            });
        });
        pending_.push_back(std::move(pending));
    }

    // An output appeared: assign it to every parked surface and place them.
    void adopt_pending_surfaces(wlr_output* output) {
        if (pending_.empty()) {
            return;
        }
        // Move the parked surfaces out first: place() binds a fresh destroy
        // listener via LayerSurface, so the Pending's destroy listener must be
        // gone before placement to avoid a double binding.
        std::vector<wlr_layer_surface_v1*> ready;
        for (auto& p : pending_) {
            p->surface->output = output;
            ready.push_back(p->surface);
        }
        pending_.clear(); // drops the parking destroy listeners
        for (wlr_layer_surface_v1* s : ready) {
            place(s);
        }
    }

    // Start tracking `output` (idempotent — seeding at activate and a later
    // on_output_added for the same output must not double-insert). Re-arranges
    // the output so any surface already assigned to it (or pending placement)
    // gets configured once it exists.
    void track_output(wlr_output* output) {
        if (output == nullptr) {
            return;
        }
        if (std::find(outputs_.begin(), outputs_.end(), output) != outputs_.end()) {
            return;
        }
        outputs_.push_back(output);
        adopt_pending_surfaces(output);
        arrange(output);
    }

    void on_output_added(wlr_output* output) { track_output(output); }

    void on_output_removed(wlr_output* output) {
        std::erase(outputs_, output);
        usable_areas_.erase_output(output);
        // Surfaces on a vanished output: close them. Snapshot first since each
        // destroy handler erases from surfaces_.
        std::vector<wlr_layer_surface_v1*> doomed;
        for (const auto& ls : surfaces_) {
            if (ls->output() == output) {
                doomed.push_back(ls->wlr());
            }
        }
        for (wlr_layer_surface_v1* s : doomed) {
            wlr_layer_surface_v1_destroy(s);
        }
    }

    // Resolve the topmost scene surface at layout point (lx,ly); if it is one of
    // OUR tracked, mapped layer surfaces requesting on_demand keyboard
    // interactivity, give it keyboard focus. Called on pointer-button-press and
    // touch-down. We never clear focus here: taking focus on a hit to our
    // surface is the whole on_demand contract; moving focus AWAY is someone
    // else's hit (or a focus clear), never our job.
    void focus_on_demand_at(double lx, double ly) {
        double nx = 0;
        double ny = 0;
        wlr_scene_node* node =
            wlr_scene_node_at(&host_->scene()->tree.node, lx, ly, &nx, &ny);
        if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
            return;
        }
        wlr_scene_buffer* buffer = wlr_scene_buffer_from_node(node);
        wlr_scene_surface* scene_surface =
            wlr_scene_surface_try_from_buffer(buffer);
        if (scene_surface == nullptr) {
            return;
        }
        // Map the hit wlr_surface back to its layer surface, then confirm it is
        // OURS (tracked) and on_demand. The scene hit may land on a sub-surface
        // or popup of the layer surface; try_from_wlr_surface only resolves the
        // role surface itself, so also accept a hit whose layer surface we own.
        wlr_layer_surface_v1* layer =
            wlr_layer_surface_v1_try_from_wlr_surface(scene_surface->surface);
        if (layer == nullptr || !owns(layer)) {
            return;
        }
        if (!layer->surface->mapped) {
            return;
        }
        if (layer->current.keyboard_interactive !=
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) {
            return;
        }
        focus_keyboard(*host_, layer->surface);
    }

    [[nodiscard]] auto owns(wlr_layer_surface_v1* layer) const -> bool {
        for (const auto& ls : surfaces_) {
            if (ls->wlr() == layer) {
                return true;
            }
        }
        return false;
    }

    // Per-output usable area (our pure-core mirror). N == #outputs (tiny); a
    // flat pointer-keyed vector keeps the public header wlroots-free.
    struct UsableEntry {
        wlr_output* output;
        Box area;
    };
    auto usable_area(wlr_output* o) -> Box& {
        for (auto& e : usable_areas_.entries) {
            if (e.output == o) {
                return e.area;
            }
        }
        usable_areas_.entries.push_back({o, Box{}});
        return usable_areas_.entries.back().area;
    }
    struct UsableAreas {
        std::vector<UsableEntry> entries;
        void erase_output(wlr_output* o) {
            std::erase_if(entries,
                          [o](const UsableEntry& e) { return e.output == o; });
        }
    };

    const kernel::Manifest manifest_{
        .id = "layer-shell", .tier = kernel::Tier::core, .depends_on = {}};

    Host* host_ = nullptr;
    wlr_layer_shell_v1* shell_ = nullptr;
    Listener new_surface_;
    kernel::Subscription output_added_;
    kernel::Subscription output_removed_;
    kernel::Subscription pointer_button_; // on_demand focus on click
    kernel::Subscription touch_down_;      // on_demand focus on tap

    // A layer surface that arrived before any output existed. Held with only a
    // destroy listener until an output appears (adopt_pending_surfaces), then
    // placed. Owned; teardown drops the listener.
    struct Pending {
        wlr_layer_surface_v1* surface = nullptr; // borrow
        Listener destroy;
    };

    std::vector<wlr_output*> outputs_;                    // borrows; tracked set
    UsableAreas usable_areas_;                            // per-output mirror
    std::vector<std::unique_ptr<Pending>> pending_;       // owned; pre-output
    std::vector<std::unique_ptr<LayerSurface>> surfaces_; // owned
};

// ---- LayerSurface ----------------------------------------------------------

LayerSurface::LayerSurface(LayerShellExt& owner, wlr_layer_surface_v1* surface,
                           wlr_scene_layer_surface_v1* scene)
    : owner_(owner), surface_(surface), scene_(scene) {
    // Publish the typed surface->scene-tree association so xdg popups parented
    // to this layer surface resolve to our tree via Host::scene_tree_for(). The
    // RAII handle is a member; it unregisters when this LayerSurface dies.
    host_reg_ = owner_.host().host_surface(surface_->surface, scene_->tree);

    // Re-arrange this surface's output on every commit (covers the mandatory
    // initial-commit configure and any later anchor/zone/size change), then
    // re-evaluate keyboard focus.
    commit_.connect(surface_->surface->events.commit, [this](void*) {
        owner_.arrange(surface_->output);
        update_keyboard_focus();
    });

    // The wlroots layer surface is about to be freed. Do NOT destroy the scene
    // node here: wlr_scene_layer_surface_v1 installs its OWN layer_surface
    // destroy listener that tears the scene tree down for us (confirmed by ASan
    // — touching scene_->tree here is a use-after-free, since signal-emit order
    // between our listener and wlroots' is unspecified). We only reclaim the
    // area the surface reserved and erase ourselves. Copy what we need into
    // locals FIRST: erase(this) deletes *this, after which no member (including
    // owner_) may be touched, so we re-arrange through a captured owner ref.
    destroy_.connect(surface_->events.destroy, [this](void*) {
        LayerShellExt& owner = owner_;
        wlr_output* out = surface_->output;
        owner.erase(this); // deletes *this — LAST use of any member
        owner.arrange(out);
    });

    // Popups parented to this layer surface resolve to our scene tree via the
    // typed Host surface registration (host_reg_ above); ext-xdg-shell looks it
    // up with Host::scene_tree_for() and wlroots' scene helper wires the popup
    // nodes. Bound for completeness; no extra work here.
    new_popup_.connect(surface_->events.new_popup, [](void*) {});
}

// Keyboard interactivity on commit: focus an `exclusive` surface once mapped.
// `none` and `on_demand` are left alone here — `on_demand` takes focus only on
// a pointer/touch hit (LayerShellExt::focus_on_demand_at), never on commit.
void LayerSurface::update_keyboard_focus() {
    if (!surface_->surface->mapped) {
        return;
    }
    if (surface_->current.keyboard_interactive !=
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
        return;
    }
    focus_keyboard(owner_.host(), surface_->surface);
}

} // namespace

auto create() -> std::unique_ptr<kernel::Extension> {
    return std::make_unique<LayerShellExt>();
}

} // namespace unbox::ext_layer_shell
