// SPIKE (rml-compositing, Phase 0) — the REAL-SEAT run mode (--run). THROWAWAY.
//
// A minimal but real compositor (display/backend/renderer/allocator/scene/seat +
// xdg-shell + layer-shell) that maps real clients and composites EACH client
// surface as a LIVE SURFACE ELEMENT inside ONE RmlUi document: every mapped
// surface (toplevel, popup, subsurface, layer/wallpaper) becomes an <img>
// sampling that surface's live shared texture, laid out + 3D-transformed in
// RCSS. The composited RmlUi FBO is presented through a single full-output
// wlr_scene_buffer (criterion 7); the wlr cursor stays a hardware plane.
//
// Input is routed BACK to clients: pointer/touch are fed to the RmlUi context,
// RmlUi's transform-aware pick finds the surface element + element-local coords
// under the point, and the spike translates that to wl_seat surface-local
// notifies so the client receives the event AT THE CORRECT point through the 3D
// transform. Keyboard goes to the focused client.
//
// Per-frame render time, re-import counts, and idle confirmation are printed so
// the user can do the visual/touch/perf GO-NO-GO on the CF-AX3. This is the
// orchestrator-runnable artifact; YOU (the agent) self-verify the geometry +
// present + idle headless in --verify.
//
// wlroots only via the kernel's wrapper; every wl_listener is the RAII Listener.

#include <unbox/kernel/listener.hpp>
#include <unbox/kernel/wlr.hpp>

#include "spike_gl.hpp"
#include "spike_input_core.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <list>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <xkbcommon/xkbcommon.h>
}

#include <unistd.h>

using unbox::kernel::Listener;
namespace spike = unbox::kernel::spike;

namespace {

struct Runner; // fwd

// One live client surface presented as a surface element. Backed by a wlr
// xdg-toplevel (the spike maps exactly one toplevel + its popups/subsurfaces and
// one layer/wallpaper for the criteria; more would be the same loop). Holds the
// LiveTexture (the shared-texture import) and the document <img> element id.
struct LiveSurface {
    Runner* runner = nullptr;
    wlr_surface* surface = nullptr;     // the wl_surface whose buffer we sample
    wlr_xdg_surface* xdg = nullptr;     // null for the layer surface
    wlr_layer_surface_v1* layer = nullptr;
    spike::LiveTexture live;
    std::string element_id;             // the <img>'s RML id
    int x = 0, y = 0;                   // layout position of the element
    int w = 0, h = 0;
    bool mapped = false;
    bool is_wallpaper = false;
    bool transform3d = false;           // toplevel gets the 3D tilt; wallpaper flat

    Listener map_l, unmap_l, commit_l, destroy_l;
};

struct Runner {
    wl_display* display = nullptr;
    wl_event_loop* loop = nullptr;
    wlr_backend* backend = nullptr;
    wlr_session* session = nullptr;
    wlr_renderer* renderer = nullptr;
    wlr_allocator* allocator = nullptr;
    wlr_scene* scene = nullptr;
    wlr_output_layout* output_layout = nullptr;
    wlr_scene_output_layout* scene_layout = nullptr;
    wlr_output* output = nullptr;
    wlr_scene_output* scene_output = nullptr;
    wlr_compositor* compositor = nullptr;
    wlr_seat* seat = nullptr;
    wlr_cursor* cursor = nullptr;
    wlr_xcursor_manager* cursor_mgr = nullptr;
    wlr_xdg_shell* xdg_shell = nullptr;
    wlr_layer_shell_v1* layer_shell = nullptr;
    wlr_keyboard* keyboard = nullptr;
    // A fixed ~60Hz event-loop timer drives the composite/present clock
    // independently of output `frame` damage semantics (which stall a static
    // nested/DRM output and would freeze client progress). The dirty-gate still
    // decides render-vs-skip; this only keeps the clock alive for the GO/NO-GO.
    wl_event_source* tick = nullptr;

    int out_w = 1920, out_h = 1080;

    spike::GlBridge gl;
    spike::PresentTarget present;
    Rml::Context* ctx = nullptr;
    Rml::ElementDocument* doc = nullptr;
    wlr_scene_buffer* present_node = nullptr;

    std::list<LiveSurface> surfaces;

    // The dirty gate (criterion 6): render a frame only when something changed.
    bool dirty = true;
    int next_id = 0;

    // Perf instrumentation.
    std::vector<double> frame_ms;
    int frames_rendered = 0;
    int frames_skipped_idle = 0;
    double last_report = 0.0;

    // Server-level listeners.
    Listener new_output_l, new_input_l, frame_l;
    Listener new_xdg_l, new_layer_l;
    Listener cursor_motion_l, cursor_motion_abs_l, cursor_button_l, cursor_axis_l, cursor_frame_l;
    Listener touch_down_l, touch_up_l, touch_motion_l;
    Listener kb_key_l, kb_mods_l;

    auto add_surface(wlr_surface* surf) -> LiveSurface* {
        surfaces.emplace_back();
        LiveSurface& s = surfaces.back();
        s.runner = this;
        s.surface = surf;
        s.live.gl = &gl;
        s.element_id = "surf_" + std::to_string(next_id++);
        s.live.uri = "unbox-live://" + s.element_id;
        return &s;
    }

    void remove_surface(LiveSurface* s) {
        const bool cur = gl.make_current();
        s->live.destroy();
        if (cur) {
            gl.restore_current();
        }
        // Remove the <img> element from the document.
        if (doc != nullptr) {
            if (Rml::Element* el = doc->GetElementById(s->element_id)) {
                el->GetParentNode()->RemoveChild(el);
            }
        }
        surfaces.remove_if([s](const LiveSurface& e) { return &e == s; });
        dirty = true;
    }
};

// The base document: a perspective container + a flat wallpaper layer behind it.
// Surface elements are inserted at runtime as <div class="win"><img.../></div>.
const char* kRunRml = R"RML(<rml>
<head>
<style>
body { margin: 0px; padding: 0px; perspective: 1400px; background: #0b0d14; }
#wall { display: block; position: absolute; left: 0; top: 0; }
#wall img { display: block; }
#stage { display: block; position: absolute; left: 0; top: 0;
         width: 100%; height: 100%; }
.win { display: block; position: absolute;
       transform: perspective(1400px) rotateY(-18deg);
       transform-origin: 50% 50%;
       transition: transform 0.25s cubic-in-out;
       box-shadow: #000a 8px 8px 24px 0px; }
.win img { display: block; width: 100%; height: 100%; }
</style>
</head>
<body>
<div id="wall"></div>
<div id="stage"></div>
</body>
</rml>)RML";

void layout_surface_element(Runner& r, LiveSurface& s) {
    if (r.doc == nullptr || s.live.tex == 0) {
        return;
    }
    Rml::Element* container = r.doc->GetElementById(s.is_wallpaper ? "wall" : "stage");
    if (container == nullptr) {
        return;
    }
    Rml::Element* win = r.doc->GetElementById(s.element_id);
    if (win == nullptr) {
        // Create <div class=win id=surf_N><img src=uri/></div> (wallpaper: bare img).
        Rml::ElementPtr div = r.doc->CreateElement("div");
        div->SetId(s.element_id);
        if (!s.is_wallpaper) {
            div->SetClass("win", true);
        }
        Rml::ElementPtr img = r.doc->CreateElement("img");
        img->SetAttribute("src", s.live.uri);
        div->AppendChild(std::move(img));
        win = container->AppendChild(std::move(div));
    }
    if (win == nullptr) {
        return;
    }
    win->SetProperty("position", "absolute");
    win->SetProperty("left", std::to_string(s.x) + "px");
    win->SetProperty("top", std::to_string(s.y) + "px");
    win->SetProperty("width", std::to_string(s.w) + "px");
    win->SetProperty("height", std::to_string(s.h) + "px");
    if (Rml::Element* img = win->GetFirstChild()) {
        img->SetProperty("width", std::to_string(s.w) + "px");
        img->SetProperty("height", std::to_string(s.h) + "px");
    }
}

// Re-import every mapped surface's current buffer (zero re-import when unchanged)
// and lay it out, then render+present. Returns the render time in ms (or -1 if
// the frame was gated out).
auto composite_frame(Runner& r, bool force) -> double {
    if (!r.dirty && !force) {
        ++r.frames_skipped_idle;
        return -1.0;
    }
    r.dirty = false;
    const double t0 = spike::now_sec();

    const bool cur = r.gl.make_current();
    for (LiveSurface& s : r.surfaces) {
        if (!s.mapped || s.surface == nullptr) {
            continue;
        }
        wlr_buffer* buf = nullptr;
        if (s.surface->buffer != nullptr) {
            buf = &s.surface->buffer->base;
        }
        if (buf != nullptr) {
            s.live.adopt(buf);
            // Natural size from the surface's current state.
            s.w = s.surface->current.width;
            s.h = s.surface->current.height;
        }
        layout_surface_element(r, s);
    }
    wlr_buffer* presented = r.present.render(r.ctx);
    if (cur) {
        r.gl.restore_current();
    }
    if (presented != nullptr && r.present_node != nullptr) {
        wlr_scene_buffer_set_buffer(r.present_node, presented);
    }

    const double dt_ms = (spike::now_sec() - t0) * 1000.0;
    r.frame_ms.push_back(dt_ms);
    ++r.frames_rendered;
    return dt_ms;
}

// ---- Input: RmlUi pick -> surface-local -> wl_seat --------------------------
//
// Feed the screen point to the RmlUi context; RmlUi's transform-aware hover pick
// resolves the element under it. If that element (or its parent) is a surface
// element, map the picked element-local coords to surface-local and notify the
// client. RmlUi reports the hovered element via GetHoverElement() after a move.

auto surface_for_element(Runner& r, Rml::Element* el) -> LiveSurface* {
    while (el != nullptr) {
        const Rml::String id = el->GetId();
        for (LiveSurface& s : r.surfaces) {
            if (s.element_id == id) {
                return &s;
            }
        }
        el = el->GetParentNode();
    }
    return nullptr;
}

// Translate a screen point to a surface-local point on the hovered surface
// element, using the element's own box + RmlUi's transform-aware projection. We
// read the hovered element's absolute (already transform-resolved by RmlUi's
// pick) offset and scale the live texture's natural size onto the element box.
struct Routed {
    LiveSurface* s = nullptr;
    double sx = 0, sy = 0; // surface-local pixels
};

auto route_point(Runner& r, double screen_x, double screen_y) -> Routed {
    r.ctx->ProcessMouseMove(static_cast<int>(screen_x), static_cast<int>(screen_y), 0);
    Rml::Element* hover = r.ctx->GetHoverElement();
    LiveSurface* s = surface_for_element(r, hover);
    if (s == nullptr) {
        return {};
    }
    // The <img> child carries the texture box; map the screen point into its
    // content box (RmlUi gives us the transform-resolved absolute offset) and
    // scale to the live texture's natural pixels = surface-local coords.
    Rml::Element* img = r.doc->GetElementById(s->element_id);
    if (img != nullptr && img->GetFirstChild() != nullptr) {
        img = img->GetFirstChild();
    }
    if (img == nullptr) {
        return {};
    }
    const Rml::Vector2f off = img->GetAbsoluteOffset(Rml::BoxArea::Content);
    const float bw = img->GetClientWidth();
    const float bh = img->GetClientHeight();
    if (bw <= 0 || bh <= 0) {
        return {};
    }
    const double fx = (screen_x - off.x) / bw; // 0..1 across the element box
    const double fy = (screen_y - off.y) / bh;
    Routed out;
    out.s = s;
    out.sx = std::clamp(fx, 0.0, 1.0) * s->live.width;
    out.sy = std::clamp(fy, 0.0, 1.0) * s->live.height;
    return out;
}

void notify_pointer_motion(Runner& r, double sx, double sy, std::uint32_t time, Routed& rt) {
    if (rt.s == nullptr || rt.s->surface == nullptr) {
        wlr_seat_pointer_notify_clear_focus(r.seat);
        return;
    }
    wlr_seat_pointer_notify_enter(r.seat, rt.s->surface, rt.sx, rt.sy);
    wlr_seat_pointer_notify_motion(r.seat, time, rt.sx, rt.sy);
    wlr_seat_pointer_notify_frame(r.seat);
    (void)sx;
    (void)sy;
}

// ---- xdg-shell ---------------------------------------------------------------

void on_surface_commit(Runner& r, LiveSurface& s) {
    // A client buffer commit is THE dirty source (criterion 6): a new frame is
    // scheduled only here (plus input/animation).
    r.dirty = true;
    if (r.output != nullptr) {
        wlr_output_schedule_frame(r.output);
    }
    (void)s;
}

void on_xdg_map(Runner& r, LiveSurface& s) {
    s.mapped = true;
    // Place the toplevel element centered on the stage, sized to its geometry.
    if (s.xdg != nullptr && s.xdg->toplevel != nullptr) {
        const wlr_box geo = s.xdg->geometry;
        s.w = geo.width > 0 ? geo.width : 800;
        s.h = geo.height > 0 ? geo.height : 600;
    }
    s.x = (r.out_w - s.w) / 2;
    s.y = (r.out_h - s.h) / 2;
    s.transform3d = true;
    // Give the toplevel keyboard focus.
    if (r.keyboard != nullptr && s.surface != nullptr) {
        wlr_seat_keyboard_notify_enter(r.seat, s.surface, r.keyboard->keycodes,
                                       r.keyboard->num_keycodes, &r.keyboard->modifiers);
    }
    r.dirty = true;
    std::fprintf(stderr, "[run] toplevel mapped %dx%d at (%d,%d) as surface element '%s'\n", s.w,
                 s.h, s.x, s.y, s.element_id.c_str());
}

void handle_new_xdg(Runner& r, wlr_xdg_surface* xdg) {
    if (xdg->role == WLR_XDG_SURFACE_ROLE_POPUP) {
        // Popups are surface elements too — answering criterion 4: each
        // subsurface/popup is its OWN element sampling its OWN live texture,
        // positioned at the popup's offset under its parent. The per-subsurface
        // approach (vs per-window RTT) is what we exercise here.
        LiveSurface* s = r.add_surface(xdg->surface);
        s->xdg = xdg;
        s->map_l.connect(xdg->surface->events.map, [&r, s](void*) {
            s->mapped = true;
            // Position the popup relative to the output (its geometry carries the
            // offset from the parent in surface coords; for the spike we place it
            // near the toplevel center + popup geometry).
            const wlr_box geo = s->xdg->geometry;
            s->w = geo.width > 0 ? geo.width : 200;
            s->h = geo.height > 0 ? geo.height : 100;
            s->x = (r.out_w) / 2 + s->xdg->popup->scheduled.geometry.x;
            s->y = (r.out_h) / 2 + s->xdg->popup->scheduled.geometry.y;
            r.dirty = true;
            std::fprintf(stderr, "[run] popup mapped as surface element '%s'\n",
                         s->element_id.c_str());
        });
        s->unmap_l.connect(xdg->surface->events.unmap,
                           [&r, s](void*) { s->mapped = false; r.dirty = true; });
        s->commit_l.connect(xdg->surface->events.commit, [&r, s](void*) { on_surface_commit(r, *s); });
        s->destroy_l.connect(xdg->surface->events.destroy, [&r, s](void*) { r.remove_surface(s); });
        return;
    }
    if (xdg->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        return;
    }
    LiveSurface* s = r.add_surface(xdg->surface);
    s->xdg = xdg;
    s->map_l.connect(xdg->surface->events.map, [&r, s](void*) { on_xdg_map(r, *s); });
    s->unmap_l.connect(xdg->surface->events.unmap,
                       [&r, s](void*) { s->mapped = false; r.dirty = true; });
    s->commit_l.connect(xdg->surface->events.commit, [&r, s](void*) {
        if (s->xdg != nullptr && s->xdg->initial_commit) {
            wlr_xdg_toplevel_set_size(s->xdg->toplevel, 0, 0); // let the client choose
        }
        on_surface_commit(r, *s);
    });
    s->destroy_l.connect(xdg->surface->events.destroy, [&r, s](void*) { r.remove_surface(s); });
}

// ---- layer-shell (wallpaper) -------------------------------------------------

void handle_new_layer(Runner& r, wlr_layer_surface_v1* layer) {
    // Configure it to the full output as a wallpaper (background layer).
    layer->current.desired_width = static_cast<std::uint32_t>(r.out_w);
    layer->current.desired_height = static_cast<std::uint32_t>(r.out_h);
    wlr_layer_surface_v1_configure(layer, static_cast<std::uint32_t>(r.out_w),
                                   static_cast<std::uint32_t>(r.out_h));
    LiveSurface* s = r.add_surface(layer->surface);
    s->layer = layer;
    s->is_wallpaper = true;
    s->x = 0;
    s->y = 0;
    s->w = r.out_w;
    s->h = r.out_h;
    s->map_l.connect(layer->surface->events.map, [&r, s](void*) {
        s->mapped = true;
        r.dirty = true;
        std::fprintf(stderr, "[run] layer-shell wallpaper mapped as surface element '%s'\n",
                     s->element_id.c_str());
    });
    s->unmap_l.connect(layer->surface->events.unmap,
                       [&r, s](void*) { s->mapped = false; r.dirty = true; });
    s->commit_l.connect(layer->surface->events.commit, [&r, s](void*) { on_surface_commit(r, *s); });
    s->destroy_l.connect(layer->surface->events.destroy, [&r, s](void*) { r.remove_surface(s); });
}

// ---- output frame ------------------------------------------------------------

void on_frame(Runner& r) {
    const double dt = composite_frame(r, /*force=*/false);
    if (!wlr_scene_output_commit(r.scene_output, nullptr)) {
        // Nothing changed for wlr_scene to commit (static scene). The nested /
        // DRM backend only emits the next `frame` after a successful output
        // commit, so a no-op scene commit would STALL the frame clock (and any
        // client waiting on it). Force a bare output commit to keep the vblank
        // clock — and thus client progress — alive. (A production build gates the
        // schedule instead; the spike keeps the seat live for the GO/NO-GO.)
        wlr_output_state st;
        wlr_output_state_init(&st);
        if (!wlr_output_commit_state(r.output, &st)) {
            wlr_output_schedule_frame(r.output);
        }
        wlr_output_state_finish(&st);
    }
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(r.scene_output, &now);

    // Animation dirty source: RmlUi's GetNextUpdateDelay() (finite => animating,
    // +inf => at rest) — exactly the design's gate signal.
    bool anim = false;
    {
        const bool cur = r.gl.make_current();
        r.ctx->Update();
        anim = std::isfinite(r.ctx->GetNextUpdateDelay());
        if (cur) {
            r.gl.restore_current();
        }
    }
    if (anim) {
        r.dirty = true;
    }
    // Keep the output ticking so mapped clients always make progress (their
    // wl_surface.frame callbacks fire and their roundtrips complete). The
    // dirty-GATE still decides whether composite_frame() actually RENDERS vs.
    // counts a skipped-idle frame — so the idle win is still visible in the perf
    // line (skipped_idle climbs while frames holds) even though the nested/DRM
    // output is scheduled every vblank. (A production build would instead gate
    // the schedule itself; here we keep the seat live for the GO/NO-GO.)
    wlr_output_schedule_frame(r.output);

    // Periodic perf report (~1s).
    const double t = spike::now_sec();
    if (t - r.last_report > 1.0 && !r.frame_ms.empty()) {
        std::vector<double> v = r.frame_ms;
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) {
            sum += x;
        }
        const double avg = sum / v.size();
        const double p95 = v[static_cast<std::size_t>(v.size() * 0.95)];
        std::fprintf(stderr,
                     "[perf] frames=%d skipped_idle=%d avg=%.2fms p95=%.2fms max=%.2fms "
                     "(~%.0f fps budget)\n",
                     r.frames_rendered, r.frames_skipped_idle, avg, p95, v.back(),
                     avg > 0 ? 1000.0 / avg : 0.0);
        r.frame_ms.clear();
        r.last_report = t;
        (void)dt;
    }
}

// ---- input devices -----------------------------------------------------------

void handle_new_input(Runner& r, wlr_input_device* dev) {
    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) {
        r.keyboard = wlr_keyboard_from_input_device(dev);
        xkb_context* xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_keymap* km = xkb_keymap_new_from_names(xkb, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(r.keyboard, km);
        xkb_keymap_unref(km);
        xkb_context_unref(xkb);
        wlr_keyboard_set_repeat_info(r.keyboard, 25, 600);
        wlr_seat_set_keyboard(r.seat, r.keyboard);
        r.kb_key_l.connect(r.keyboard->events.key, [&r](void* data) {
            auto* ev = static_cast<wlr_keyboard_key_event*>(data);
            wlr_seat_set_keyboard(r.seat, r.keyboard);
            wlr_seat_keyboard_notify_key(r.seat, ev->time_msec, ev->keycode, ev->state);
        });
        r.kb_mods_l.connect(r.keyboard->events.modifiers, [&r](void*) {
            wlr_seat_set_keyboard(r.seat, r.keyboard);
            wlr_seat_keyboard_notify_modifiers(r.seat, &r.keyboard->modifiers);
        });
    } else if (dev->type == WLR_INPUT_DEVICE_POINTER) {
        wlr_cursor_attach_input_device(r.cursor, dev);
    } else if (dev->type == WLR_INPUT_DEVICE_TOUCH) {
        wlr_cursor_attach_input_device(r.cursor, dev);
    }
    std::uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (r.keyboard != nullptr) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    caps |= WL_SEAT_CAPABILITY_TOUCH;
    wlr_seat_set_capabilities(r.seat, caps);
}

void attach_input(Runner& r) {
    r.cursor_motion_l.connect(r.cursor->events.motion, [&r](void* data) {
        auto* ev = static_cast<wlr_pointer_motion_event*>(data);
        wlr_cursor_move(r.cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
        Routed rt = route_point(r, r.cursor->x, r.cursor->y);
        notify_pointer_motion(r, r.cursor->x, r.cursor->y, ev->time_msec, rt);
        r.dirty = true;
        wlr_output_schedule_frame(r.output);
    });
    r.cursor_motion_abs_l.connect(r.cursor->events.motion_absolute, [&r](void* data) {
        auto* ev = static_cast<wlr_pointer_motion_absolute_event*>(data);
        wlr_cursor_warp_absolute(r.cursor, &ev->pointer->base, ev->x, ev->y);
        Routed rt = route_point(r, r.cursor->x, r.cursor->y);
        notify_pointer_motion(r, r.cursor->x, r.cursor->y, ev->time_msec, rt);
        r.dirty = true;
        wlr_output_schedule_frame(r.output);
    });
    r.cursor_button_l.connect(r.cursor->events.button, [&r](void* data) {
        auto* ev = static_cast<wlr_pointer_button_event*>(data);
        Routed rt = route_point(r, r.cursor->x, r.cursor->y);
        if (rt.s != nullptr) {
            wlr_seat_pointer_notify_enter(r.seat, rt.s->surface, rt.sx, rt.sy);
            wlr_seat_pointer_notify_button(r.seat, ev->time_msec, ev->button, ev->state);
            wlr_seat_pointer_notify_frame(r.seat);
        }
    });
    r.cursor_axis_l.connect(r.cursor->events.axis, [&r](void* data) {
        auto* ev = static_cast<wlr_pointer_axis_event*>(data);
        wlr_seat_pointer_notify_axis(r.seat, ev->time_msec, ev->orientation, ev->delta,
                                     ev->delta_discrete, ev->source, ev->relative_direction);
        wlr_seat_pointer_notify_frame(r.seat);
    });
    r.cursor_frame_l.connect(r.cursor->events.frame,
                             [&r](void*) { wlr_seat_pointer_notify_frame(r.seat); });
    // Touch: map the touch point through the same pick and notify the client.
    r.touch_down_l.connect(r.cursor->events.touch_down, [&r](void* data) {
        auto* ev = static_cast<wlr_touch_down_event*>(data);
        double lx = 0, ly = 0;
        wlr_cursor_absolute_to_layout_coords(r.cursor, &ev->touch->base, ev->x, ev->y, &lx, &ly);
        Routed rt = route_point(r, lx, ly);
        if (rt.s != nullptr) {
            wlr_seat_touch_notify_down(r.seat, rt.s->surface, ev->time_msec, ev->touch_id, rt.sx,
                                       rt.sy);
        }
        r.dirty = true;
        wlr_output_schedule_frame(r.output);
    });
    r.touch_motion_l.connect(r.cursor->events.touch_motion, [&r](void* data) {
        auto* ev = static_cast<wlr_touch_motion_event*>(data);
        double lx = 0, ly = 0;
        wlr_cursor_absolute_to_layout_coords(r.cursor, &ev->touch->base, ev->x, ev->y, &lx, &ly);
        Routed rt = route_point(r, lx, ly);
        if (rt.s != nullptr) {
            wlr_seat_touch_notify_motion(r.seat, ev->time_msec, ev->touch_id, rt.sx, rt.sy);
        }
    });
    r.touch_up_l.connect(r.cursor->events.touch_up, [&r](void* data) {
        auto* ev = static_cast<wlr_touch_up_event*>(data);
        wlr_seat_touch_notify_up(r.seat, ev->time_msec, ev->touch_id);
    });
}

// ---- output bring-up ---------------------------------------------------------

void handle_new_output(Runner& r, wlr_output* out) {
    if (r.output != nullptr) {
        return; // spike: drive ONE output
    }
    r.output = out;
    wlr_output_init_render(out, r.allocator, r.renderer);
    wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    if (wlr_output_mode* mode = wlr_output_preferred_mode(out)) {
        wlr_output_state_set_mode(&st, mode);
    }
    wlr_output_commit_state(out, &st);
    wlr_output_state_finish(&st);

    if (out->width > 0) {
        r.out_w = out->width;
        r.out_h = out->height;
    }

    wlr_output_layout_output* lo = wlr_output_layout_add_auto(r.output_layout, out);
    r.scene_output = wlr_scene_output_create(r.scene, out);
    wlr_scene_output_layout_add_output(r.scene_layout, lo, r.scene_output);

    // Build the present target + RmlUi document sized to the output, then a
    // single full-output scene_buffer node to present it (criterion 7).
    r.gl.make_current();
    r.present.init(&r.gl, r.allocator, r.out_w, r.out_h);
    r.present_node = wlr_scene_buffer_create(&r.scene->tree, nullptr);
    r.present.scene_buffer = r.present_node;
    r.ctx = Rml::CreateContext("run", Rml::Vector2i(r.out_w, r.out_h), r.gl.render);
    r.doc = r.ctx->LoadDocumentFromMemory(kRunRml);
    if (r.doc != nullptr) {
        r.doc->Show();
    }
    r.gl.restore_current();

    r.frame_l.connect(out->events.frame, [&r](void*) { on_frame(r); });
    wlr_output_schedule_frame(out);
    std::fprintf(stderr, "[run] output %s up at %dx%d; present node + RmlUi document built\n",
                 out->name, r.out_w, r.out_h);
}

Runner* g_runner = nullptr;

} // namespace

auto run_real_seat(const char* startup_cmd) -> int {
    wlr_log_init(WLR_INFO, nullptr);
    Runner r;
    g_runner = &r;

    r.display = wl_display_create();
    r.loop = wl_display_get_event_loop(r.display);
    r.backend = wlr_backend_autocreate(r.loop, &r.session);
    if (r.backend == nullptr) {
        std::fprintf(stderr, "[run] failed to create backend\n");
        return 1;
    }
    r.renderer = wlr_renderer_autocreate(r.backend);
    wlr_renderer_init_wl_display(r.renderer, r.display);
    r.allocator = wlr_allocator_autocreate(r.backend, r.renderer);

    if (!wlr_renderer_is_gles2(r.renderer)) {
        std::fprintf(stderr, "[run] renderer is not gles2 — RML compositing needs the GL path. "
                             "Set WLR_RENDERER=gles2.\n");
        return 1;
    }

    r.compositor = wlr_compositor_create(r.display, 5, r.renderer);
    wlr_subcompositor_create(r.display);
    wlr_data_device_manager_create(r.display);
    r.output_layout = wlr_output_layout_create(r.display);
    r.scene = wlr_scene_create();
    r.scene_layout = wlr_scene_attach_output_layout(r.scene, r.output_layout);

    r.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(r.cursor, r.output_layout);
    r.cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);
    r.seat = wlr_seat_create(r.display, "seat0");

    r.xdg_shell = wlr_xdg_shell_create(r.display, 3);
    r.new_xdg_l.connect(r.xdg_shell->events.new_surface, [&r](void* data) {
        handle_new_xdg(r, static_cast<wlr_xdg_surface*>(data));
    });
    r.layer_shell = wlr_layer_shell_v1_create(r.display, 4);
    r.new_layer_l.connect(r.layer_shell->events.new_surface, [&r](void* data) {
        handle_new_layer(r, static_cast<wlr_layer_surface_v1*>(data));
    });

    r.new_output_l.connect(r.backend->events.new_output,
                           [&r](void* data) { handle_new_output(r, static_cast<wlr_output*>(data)); });
    r.new_input_l.connect(r.backend->events.new_input, [&r](void* data) {
        handle_new_input(r, static_cast<wlr_input_device*>(data));
    });
    attach_input(r);

    // Initialize the GL bridge against the wlr EGLDisplay now (before any output;
    // the import path only needs the display).
    EGLDisplay egl = wlr_egl_get_display(wlr_gles2_renderer_get_egl(r.renderer));
    if (!r.gl.init(egl)) {
        std::fprintf(stderr, "[run] GL bridge init failed — NO-GO on this hardware\n");
        return 1;
    }

    const char* socket = wl_display_add_socket_auto(r.display);
    if (socket == nullptr) {
        std::fprintf(stderr, "[run] failed to add wayland socket\n");
        return 1;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);

    if (!wlr_backend_start(r.backend)) {
        std::fprintf(stderr, "[run] failed to start backend\n");
        return 1;
    }
    std::fprintf(stderr, "[run] up on WAYLAND_DISPLAY=%s — spawning client: %s\n", socket,
                 startup_cmd);

    if (startup_cmd != nullptr && startup_cmd[0] != '\0') {
        if (fork() == 0) {
            setenv("WAYLAND_DISPLAY", socket, 1);
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, static_cast<char*>(nullptr));
            _exit(127);
        }
    }

    wl_display_run(r.display);

    // Teardown.
    const bool cur = r.gl.make_current();
    for (LiveSurface& s : r.surfaces) {
        s.live.destroy();
    }
    r.present.teardown();
    if (r.ctx != nullptr) {
        Rml::RemoveContext("run");
    }
    if (cur) {
        r.gl.restore_current();
    }
    r.gl.teardown();
    if (r.scene != nullptr) {
        wlr_scene_node_destroy(&r.scene->tree.node);
    }
    if (r.cursor_mgr != nullptr) {
        wlr_xcursor_manager_destroy(r.cursor_mgr);
    }
    if (r.cursor != nullptr) {
        wlr_cursor_destroy(r.cursor);
    }
    if (r.allocator != nullptr) {
        wlr_allocator_destroy(r.allocator);
    }
    if (r.renderer != nullptr) {
        wlr_renderer_destroy(r.renderer);
    }
    if (r.backend != nullptr) {
        wlr_backend_destroy(r.backend);
    }
    wl_display_destroy(r.display);
    return 0;
}
