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

#include "../vt_core.hpp" // VT-switch decision core, mirrored from input.cpp

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
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

#include <signal.h>
#include <unistd.h>

using unbox::kernel::Listener;
namespace spike = unbox::kernel::spike;

namespace {

// ---- Verbose, crash-survivable diagnostic log --------------------------------
//
// The field failure was a BLACK SCREEN that forced a hard reboot — and the log
// was on /tmp (tmpfs), so the reboot WIPED it. Now the log goes to a PERSISTENT
// path that survives a reboot: $UNBOX_SPIKE_LOG if set, else $HOME/rml-spike.log
// (NEVER /tmp). Every interesting step (backend/renderer pick, output modeset,
// each commit heartbeat, client spawn/exec, EACH client connect, EACH surface
// map/unmap, EACH live-texture import, scene insertion) is logged to BOTH stderr
// AND that file. We FLUSH **and fsync()** after every line so a hard reboot (or
// a freeze followed by a power-cycle) still preserves the log up to the freeze
// point. Single-threaded event loop ⇒ no locking.
FILE* g_log = nullptr;
std::string g_log_path;

void log_open() {
    if (const char* env = getenv("UNBOX_SPIKE_LOG"); env != nullptr && env[0] != '\0') {
        g_log_path = env;
    } else if (const char* home = getenv("HOME"); home != nullptr && home[0] != '\0') {
        g_log_path = std::string(home) + "/rml-spike.log";
    } else {
        // Last resort only (no HOME): the cwd, still NOT tmpfs by default.
        g_log_path = "rml-spike.log";
    }
    g_log = std::fopen(g_log_path.c_str(), "w"); // truncate on start
}
void log_close() {
    if (g_log != nullptr) {
        std::fflush(g_log);
        ::fsync(::fileno(g_log));
        std::fclose(g_log);
        g_log = nullptr;
    }
}
[[gnu::format(printf, 1, 2)]] void slog(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const double t = spike::now_sec();
    std::fprintf(stderr, "[%.3f] %s\n", t, buf);
    if (g_log != nullptr) {
        std::fprintf(g_log, "[%.3f] %s\n", t, buf);
        std::fflush(g_log);            // push out of stdio's buffer...
        ::fsync(::fileno(g_log));      // ...AND down to disk: a hard reboot keeps the tail
    }
}

struct Runner; // fwd

// One keyboard device. MIRRORS the shipped kernel's src/input.cpp: every
// keyboard from the seat gets its OWN key + modifiers + destroy listeners, held
// in a list (NOT a single shared pointer that the last device clobbers). On a
// real DRM seat there can be several keyboard devices; the escape hatch must
// fire on a key from ANY of them, so EACH needs its own live key listener.
struct Keyboard {
    Runner* runner = nullptr;
    wlr_keyboard* keyboard = nullptr;
    Listener key_l, mods_l, destroy_l;
};

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
    // Per-device keyboards (mirrors input.cpp's `keyboards` list). `focus_kb` is
    // the one currently driving wlr_seat focus (the last to send a key), used to
    // hand a newly-mapped toplevel keyboard focus. Never dereferenced for input
    // routing — each device's own listener carries its own wlr_keyboard.
    std::list<Keyboard> keyboards;
    wlr_keyboard* focus_kb = nullptr;
    // The toplevel wl_surface that currently holds keyboard focus on the seat
    // (set via wlr_seat_keyboard_notify_enter). Tracking it lets us (a) re-assert
    // focus idempotently if a key arrives before any enter landed (device/map
    // ordering on a real DRM seat is not guaranteed), and (b) focus a toplevel
    // that mapped BEFORE the first keyboard device appeared. Mirrors the shipped
    // ext-xdg-shell discipline of holding the focused surface and re-entering.
    wlr_surface* focused_surface = nullptr;
    // The last toplevel that mapped — the focus candidate. Kept so a keyboard
    // that is hot-plugged AFTER the toplevel mapped can still be handed focus.
    wlr_surface* last_toplevel = nullptr;
    // Per-key forward instrumentation: keys forwarded to the focused client so
    // the log proves typing is reaching foot even though the agent cannot see it.
    long keys_forwarded = 0;
    // Cursor liveness: true once we have shown a default xcursor image on the
    // wlr cursor plane. The cursor stays a wlr plane (NEVER drawn into RmlUi) per
    // the plan; we just make sure it HAS an image so it is visible.
    bool cursor_shown = false;
    // Pointer/touch routing instrumentation (counts, not per-event spam after the
    // first few): so the log shows events ARE landing on a client surface.
    long pointer_enters = 0;
    long pointer_motions = 0;
    long pointer_misses = 0;
    long touch_downs = 0;
    // LIVE-UPDATE LOOP instrumentation (the field "stuck on a single frame" fix).
    // The client lives as an imported texture (NOT a wlr_scene surface node), so
    // the spike must DRIVE the client update loop itself: (a) send frame-done to
    // every mapped client surface + its subsurfaces/popups each composited frame
    // (without it the client draws ONCE and waits forever -> stuck frame), and
    // (b) re-import the surface's CURRENT buffer on each commit (the buffer ptr
    // changes per frame). These counters prove the loop is alive in the log.
    long client_commits = 0;   // per-surface wl_surface.commit count (all surfaces)
    long live_reimports = 0;   // live-texture re-imports (a real new buffer adopted)
    long frame_done_sends = 0; // wlr_surface_send_frame_done calls (tree-walked)
    double last_loop_report = 0.0;
    // A fixed ~60Hz event-loop timer drives the composite/present clock
    // independently of output `frame` damage semantics (which stall a static
    // nested/DRM output and would freeze client progress). The dirty-gate still
    // decides render-vs-skip; this only keeps the clock alive for the GO/NO-GO.
    wl_event_source* tick = nullptr;

    // SAFETY (criterion A): the guaranteed backstops that make a real DRM seat
    // un-lockable. A self-timeout timer terminates the loop after N seconds; two
    // signal sources turn SIGINT/SIGTERM into a CLEAN wl_display_terminate (so
    // wlroots restores the VT to text mode — no hard reboot). VT switching +
    // quit keys are handled inline in the keyboard handler, BEFORE any forward.
    wl_event_source* safety_timer = nullptr;
    wl_event_source* sigint_src = nullptr;
    wl_event_source* sigterm_src = nullptr;
    // The dead-man interval (ms). Pressing `P` re-arms safety_timer to this full
    // interval — so holding the session open past the interval REQUIRES periodic
    // P presses. That doubles as a real-seat keyboard-input liveness test: if the
    // session survives past the interval, P is reaching the handler ⇒ input works.
    int deadman_ms = 15000;

    // A guaranteed-visible non-black background + test marker, composited UNDER
    // the RmlUi present node as plain wlr_scene_rects. This makes "black screen"
    // (nothing presenting at all) visibly different from "presenting but the
    // RmlUi/client layer is empty" (dark blue + a marker square show through).
    wlr_scene_rect* bg_rect = nullptr;
    wlr_scene_rect* marker_rect = nullptr;

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

    // Commit/present heartbeat (criterion B): a count of output commits so the
    // log shows the present loop is actually ticking even on a static scene.
    long commits = 0;

    // Client diagnosis: count connects, and a ~5s watchdog that screams if NO
    // client surface ever maps (the "foot did not appear" field failure). Set
    // true the first time ANY surface maps; the watchdog reads it.
    int client_connects = 0;
    bool any_surface_mapped = false;
    wl_event_source* client_watchdog = nullptr;
    wl_listener client_created_l{}; // raw: wl_display client-created is not a wlr signal
    wl_global* compositor_global = nullptr;

    // Server-level listeners.
    Listener new_output_l, new_input_l, frame_l;
    Listener new_toplevel_l, new_popup_l, new_layer_l;
    Listener cursor_motion_l, cursor_motion_abs_l, cursor_button_l, cursor_axis_l, cursor_frame_l;
    Listener touch_down_l, touch_up_l, touch_motion_l;
    // Seat protocol glue (mirrors src/input.cpp::attach_seat_handlers): let a
    // client set its own cursor over its surface, and restore the default
    // xcursor when the pointer focus leaves all client surfaces.
    Listener seat_request_cursor_l, seat_pointer_focus_change_l;

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
            const int reimports_before = s.live.reimports;
            // Gate the re-import on the surface's COMMIT SEQUENCE, not the buffer
            // pointer: foot recycles a small buffer pool, so the SAME wlr_buffer
            // pointer is re-committed with NEW contents. wlr_surface_state.seq
            // advances on every commit regardless of pool reuse, so this re-imports
            // the current buffer each new frame (the frozen-frame fix) while a
            // static client (no commit => no seq change) still does zero work.
            s.live.adopt(buf, s.surface->current.seq);
            // Natural size from the surface's current state.
            s.w = s.surface->current.width;
            s.h = s.surface->current.height;
            if (s.live.reimports != reimports_before) {
                ++r.live_reimports;
                slog("live-texture import: '%s' %dx%d dmabuf=%d tex=%u (reimport #%d)",
                     s.element_id.c_str(), s.live.width, s.live.height, s.live.is_dmabuf,
                     s.live.tex, s.live.reimports);
            }
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

// ---- The LIVE-UPDATE loop: frame callbacks to every client surface ----------
//
// THE STUCK-FRAME FIX. The shipped kernel's output frame handler (server.cpp)
// ends each frame with wlr_scene_output_send_frame_done(scene_output, now),
// which walks every SCENE surface node and completes its frame callbacks so the
// client is told "now is a good time to draw your next frame". But in this spike
// the client surfaces are NOT scene nodes — they live as imported live textures
// inside RmlUi, and the scene holds only our background rects + the single
// composited present buffer. So wlr_scene_output_send_frame_done NEVER reaches
// foot: the client draws its first buffer, its frame callback is never completed,
// and it waits forever -> the window is stuck on one frame (no typing/output/
// cursor-blink). We must drive the callbacks ourselves.
//
// This walks EVERY mapped client surface tree (toplevel + its subsurfaces, each
// popup + its subsurfaces, the wallpaper) — exactly what wlr_scene_output_send_
// frame_done does for scene nodes — and calls wlr_surface_send_frame_done on
// every mapped surface in the tree. wlr_surface_for_each_surface visits the
// surface and all its subsurfaces (root -> leaves), so subsurface callbacks are
// covered; xdg popups are tracked as their OWN LiveSurface (added in
// new_popup), so their tree is walked here too. Mirrors the SHIPPED behaviour.
void send_frame_done_to_clients(Runner& r) {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct WalkData {
        Runner* r;
        timespec* now;
    } wd{&r, &now};
    for (LiveSurface& s : r.surfaces) {
        if (!s.mapped || s.surface == nullptr) {
            continue;
        }
        wlr_surface_for_each_surface(
            s.surface,
            [](wlr_surface* surf, int /*sx*/, int /*sy*/, void* data) {
                auto* w = static_cast<WalkData*>(data);
                wlr_surface_send_frame_done(surf, w->now);
                ++w->r->frame_done_sends;
            },
            &wd);
    }
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

// Make the wlr cursor VISIBLE by giving its plane a default xcursor image. The
// cursor stays a wlr plane (hardware/output cursor), NEVER drawn into RmlUi —
// exactly as the plan requires. Called once a pointer/touch device exists and
// re-asserted whenever the pointer is not over a client surface (a client may
// have set its own cursor surface; when it leaves we restore the default).
// MIRRORS the shipped kernel (input.cpp seat_pointer_focus_change -> "default").
void show_default_cursor(Runner& r) {
    if (r.cursor == nullptr || r.cursor_mgr == nullptr) {
        return;
    }
    wlr_cursor_set_xcursor(r.cursor, r.cursor_mgr, "default");
    if (!r.cursor_shown) {
        r.cursor_shown = true;
        slog("CURSOR shown: default xcursor set on the wlr cursor plane (visible, hardware plane)");
    }
}

void notify_pointer_motion(Runner& r, double sx, double sy, std::uint32_t time, Routed& rt) {
    if (rt.s == nullptr || rt.s->surface == nullptr) {
        // No client surface under the cursor (over the document body / wallpaper
        // gap / a tilt's empty corner): clear client pointer focus and make sure
        // OUR default cursor is showing (the client can't have set one here).
        wlr_seat_pointer_notify_clear_focus(r.seat);
        show_default_cursor(r);
        ++r.pointer_misses;
        if (r.pointer_misses <= 4 || (r.pointer_misses % 240) == 0) {
            slog("pointer motion: NO surface under point (%.0f,%.0f) — misses=%ld "
                 "(cursor over document/empty area; default cursor shown)",
                 sx, sy, r.pointer_misses);
        }
        return;
    }
    wlr_seat_pointer_notify_enter(r.seat, rt.s->surface, rt.sx, rt.sy);
    wlr_seat_pointer_notify_motion(r.seat, time, rt.sx, rt.sy);
    wlr_seat_pointer_notify_frame(r.seat);
    ++r.pointer_motions;
    if (r.pointer_motions <= 4 || (r.pointer_motions % 240) == 0) {
        slog("pointer -> client surface '%s' at surface-local (%.1f,%.1f) [screen (%.0f,%.0f) "
             "through the 3D transform] motions=%ld",
             rt.s->element_id.c_str(), rt.sx, rt.sy, sx, sy, r.pointer_motions);
    }
}

// ---- xdg-shell ---------------------------------------------------------------

void on_surface_commit(Runner& r, LiveSurface& s) {
    // A client buffer commit is THE dirty source (criterion 6): a new frame is
    // scheduled only here (plus input/animation). This is ALSO the second half of
    // the stuck-frame fix: the client's per-frame buffer POINTER changes on each
    // commit, and LiveTexture::adopt early-returns when the buffer is unchanged —
    // so a real new buffer must be RE-IMPORTED. We mark the scene dirty so the
    // dirty-gate actually renders the updated texture this frame (composite_frame
    // re-adopts s.surface->buffer for every mapped surface). Marking dirty here
    // is what stops the idle gate from suppressing a REAL client update: a static
    // client (no commits) => no extra renders; an updating client (a commit per
    // frame) => one render per committed frame. Covers the whole surface tree:
    // the toplevel commit AND each subsurface/popup commit (each its own
    // LiveSurface with its own commit listener) both land here.
    ++r.client_commits;
    r.dirty = true;
    if (r.output != nullptr) {
        wlr_output_schedule_frame(r.output);
    }
    (void)s; // re-import happens in composite_frame (re-adopts s.surface->buffer)
}

// Give `surface` keyboard focus on the seat: set the active keyboard (so the
// client receives the keymap) and send the enter with the keyboard's current
// pressed keys + modifiers. Idempotent — calling it again for the already-
// focused surface is harmless and just re-asserts. MIRRORS input.cpp's
// wlr_seat_set_keyboard discipline + the ext-xdg-shell notify_enter on focus.
//
// CRITICAL FIX (real-seat "cannot type into foot"): the previous code only
// entered focus at MAP and ONLY if a keyboard already existed (r.focus_kb !=
// nullptr). On a real DRM seat the keyboard device and the client map can land
// in EITHER order, and wlr_seat_set_keyboard had not necessarily run for the
// keyboard that ends up sending keys — so the client never got an `enter` and
// every wlr_seat_keyboard_notify_key fell on a surface with no keyboard focus.
// Routing it through this helper, called on map AND on keyboard-add AND lazily
// on the first key, guarantees the focused client actually receives keys.
void focus_toplevel(Runner& r, wlr_surface* surface) {
    if (surface == nullptr) {
        return;
    }
    // Need a keyboard set on the seat so the enter ships the keymap. Prefer the
    // last device that drove the seat; else any device we have; else bail (we
    // will retry from new_keyboard once a device exists).
    wlr_keyboard* kb = r.focus_kb;
    if (kb == nullptr && !r.keyboards.empty()) {
        kb = r.keyboards.back().keyboard;
    }
    if (kb == nullptr) {
        slog("focus deferred: toplevel mapped but NO keyboard device yet "
             "(will enter on keyboard-add)");
        return;
    }
    wlr_seat_set_keyboard(r.seat, kb);
    wlr_seat_keyboard_notify_enter(r.seat, surface, kb->keycodes, kb->num_keycodes,
                                   &kb->modifiers);
    r.focused_surface = surface;
    slog("KEYBOARD FOCUS ENTER -> client surface %p (kb='%s') — keys now route to this client",
         static_cast<void*>(surface), kb->base.name != nullptr ? kb->base.name : "?");
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
    // Give the toplevel keyboard focus (robust helper — handles the case where
    // no keyboard device exists yet by deferring to new_keyboard).
    r.last_toplevel = s.surface;
    focus_toplevel(r, s.surface);
    r.dirty = true;
    r.any_surface_mapped = true;
    slog("CLIENT SURFACE MAP: toplevel %dx%d at (%d,%d) -> added to scene as live surface "
         "element '%s' (it WILL be composited as a live texture this frame)",
         s.w, s.h, s.x, s.y, s.element_id.c_str());
}

// MIRRORS the shipped ext-xdg-shell: wire from the xdg_shell's `new_toplevel` /
// `new_popup` signals, NOT `new_surface`. CRITICAL: on `new_surface` the surface
// has NO role yet (the client has not called get_toplevel/get_popup), so the old
// `xdg->role == TOPLEVEL` test was ALWAYS false there and the spike wired NOTHING
// — no commit handler, so the initial-commit `configure` was never sent, so the
// client (foot) waited forever for a configure and NEVER mapped. That is exactly
// the "foot did not appear" field failure. These signals fire with the role
// assigned, so the handshake completes and the client maps.
void handle_new_toplevel(Runner& r, wlr_xdg_toplevel* toplevel) {
    wlr_xdg_surface* xdg = toplevel->base;
    LiveSurface* s = r.add_surface(xdg->surface);
    s->xdg = xdg;
    slog("xdg TOPLEVEL created (app_id='%s' title='%s') — awaiting initial commit -> configure",
         toplevel->app_id != nullptr ? toplevel->app_id : "?",
         toplevel->title != nullptr ? toplevel->title : "?");
    s->map_l.connect(xdg->surface->events.map, [&r, s](void*) { on_xdg_map(r, *s); });
    s->unmap_l.connect(xdg->surface->events.unmap, [&r, s](void*) {
        s->mapped = false;
        r.dirty = true;
        // Drop keyboard focus if this was the focused toplevel (mirrors ext-xdg-
        // shell: an unmapped surface must not keep the seat's keyboard focus).
        if (r.focused_surface == s->surface) {
            wlr_seat_keyboard_notify_clear_focus(r.seat);
            r.focused_surface = nullptr;
        }
        if (r.last_toplevel == s->surface) {
            r.last_toplevel = nullptr;
        }
        slog("client surface UNMAP: toplevel element '%s'", s->element_id.c_str());
    });
    s->commit_l.connect(xdg->surface->events.commit, [&r, s](void*) {
        // The initial commit REQUIRES a configure reply before the client may
        // attach a buffer + map. 0x0 size lets the client pick its own dims
        // (tinywl/ext-xdg-shell discipline); set_size schedules the configure.
        if (s->xdg != nullptr && s->xdg->initial_commit) {
            slog("toplevel '%s' initial commit -> sending 0x0 configure (client picks size)",
                 s->element_id.c_str());
            wlr_xdg_toplevel_set_size(s->xdg->toplevel, 0, 0);
        }
        on_surface_commit(r, *s);
    });
    s->destroy_l.connect(xdg->surface->events.destroy, [&r, s](void*) { r.remove_surface(s); });
}

void handle_new_popup(Runner& r, wlr_xdg_popup* popup) {
    // Popups are surface elements too — answering criterion 4: each
    // subsurface/popup is its OWN element sampling its OWN live texture,
    // positioned at the popup's offset under its parent.
    wlr_xdg_surface* xdg = popup->base;
    LiveSurface* s = r.add_surface(xdg->surface);
    s->xdg = xdg;
    s->map_l.connect(xdg->surface->events.map, [&r, s](void*) {
        s->mapped = true;
        const wlr_box geo = s->xdg->geometry;
        s->w = geo.width > 0 ? geo.width : 200;
        s->h = geo.height > 0 ? geo.height : 100;
        s->x = (r.out_w) / 2 + s->xdg->popup->scheduled.geometry.x;
        s->y = (r.out_h) / 2 + s->xdg->popup->scheduled.geometry.y;
        r.dirty = true;
        r.any_surface_mapped = true;
        slog("CLIENT SURFACE MAP: popup -> added to scene as surface element '%s'",
             s->element_id.c_str());
    });
    s->unmap_l.connect(xdg->surface->events.unmap, [&r, s](void*) {
        s->mapped = false;
        r.dirty = true;
        slog("client surface UNMAP: popup element '%s'", s->element_id.c_str());
    });
    s->commit_l.connect(xdg->surface->events.commit, [&r, s](void*) {
        // A popup also needs its initial configure before it can map.
        if (s->xdg != nullptr && s->xdg->initial_commit) {
            wlr_xdg_surface_schedule_configure(s->xdg);
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
        r.any_surface_mapped = true;
        slog("CLIENT SURFACE MAP: layer-shell wallpaper -> added to scene as surface element '%s'",
             s->element_id.c_str());
    });
    s->unmap_l.connect(layer->surface->events.unmap, [&r, s](void*) {
        s->mapped = false;
        r.dirty = true;
        slog("client surface UNMAP: layer-shell wallpaper element '%s'", s->element_id.c_str());
    });
    s->commit_l.connect(layer->surface->events.commit, [&r, s](void*) { on_surface_commit(r, *s); });
    s->destroy_l.connect(layer->surface->events.destroy, [&r, s](void*) { r.remove_surface(s); });
}

// ---- output frame ------------------------------------------------------------

void on_frame(Runner& r) {
    const double dt = composite_frame(r, /*force=*/false);
    ++r.commits;
    // Heartbeat (criterion B): prove the present/commit loop is alive even on a
    // static scene. First few commits are logged individually (catches an early
    // freeze); after that, once a second via the [perf] line below.
    if (r.commits <= 5) {
        slog("output commit heartbeat #%ld (rendered=%d skipped_idle=%d present_node=%p)",
             r.commits, r.frames_rendered, r.frames_skipped_idle,
             static_cast<void*>(r.present_node));
    }
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

    // THE STUCK-FRAME FIX: drive the client update loop. The client surfaces are
    // imported live textures, NOT scene nodes, so the wlr_scene_output_send_frame_
    // done above never reaches them. Walk every mapped client surface tree and
    // complete its frame callbacks ourselves — without this foot draws ONCE and
    // waits forever (the field "stuck on a single frame"). This tells every
    // mapped surface + subsurface + popup "now is a good time to draw the next
    // frame", so typing/output/cursor-blink advance. Mirrors server.cpp.
    send_frame_done_to_clients(r);

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
        slog("[perf] frames=%d skipped_idle=%d commits=%ld avg=%.2fms p95=%.2fms max=%.2fms "
             "(~%.0f fps budget)",
             r.frames_rendered, r.frames_skipped_idle, r.commits, avg, p95, v.back(),
             avg > 0 ? 1000.0 / avg : 0.0);
        r.frame_ms.clear();
        r.last_report = t;
        (void)dt;
    }

    // Periodic LIVE-UPDATE-LOOP heartbeat (~1s): proves the client update loop is
    // ALIVE — commits coming in, buffers re-imported, and frame-done sent back so
    // the client keeps producing frames. On the user's next run a CLIMBING
    // frame_done (with client_commits + reimports climbing as they type) means
    // foot is no longer stuck on one frame: typing/output/cursor-blink advance.
    // (Counts only, not per-event spam, per the brief.)
    if (t - r.last_loop_report > 1.0) {
        slog("[live-loop] client commits=%ld reimports=%ld frame_done=%ld (mapped surfaces "
             "are being told to draw their next frame -> live update)",
             r.client_commits, r.live_reimports, r.frame_done_sends);
        r.last_loop_report = t;
    }
}

// ---- input devices -----------------------------------------------------------

// Re-arm the dead-man self-timeout to its full interval. Called when it is first
// armed and EVERY time `P` is pressed; if the session outlives the interval, P
// reached the handler ⇒ real-seat keyboard input is alive (the liveness test).
void deadman_rearm(Runner& r) {
    if (r.safety_timer != nullptr && r.deadman_ms > 0) {
        wl_event_source_timer_update(r.safety_timer, r.deadman_ms);
    }
}

// The kernel-hardwired escape-hatch + dead-man check, run on EVERY key event
// from EVERY keyboard device, BEFORE anything else. Returns true if the key was
// CONSUMED here (so it must NOT be forwarded to a client). MIRRORS the shipped
// kernel's src/input.cpp keysym-resolution + VT-switch discipline.
auto handle_escape_keys(Runner& r, Keyboard& kb, wlr_keyboard_key_event* ev) -> bool {
    const std::uint32_t keycode = ev->keycode + 8; // libinput keycode -> xkb
    const xkb_keysym_t* syms = nullptr;
    const int nsyms = xkb_state_key_get_syms(kb.keyboard->xkb_state, keycode, &syms);
    const std::uint32_t mods = wlr_keyboard_get_modifiers(kb.keyboard);
    const bool pressed = ev->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    const bool ctrl_alt = (mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) ==
                          (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT);

    for (int i = 0; i < nsyms; ++i) {
        const xkb_keysym_t sym = syms[i];

        // `P` (with NO ctrl/alt) -> reset the dead-man timer AND prove input is
        // live. This is the keep-alive: holding the session open past the
        // dead-man interval REQUIRES pressing P periodically. We do NOT consume
        // P — let it pass through to the client too (it is a normal letter); the
        // keep-alive is a side-effect, not a grab.
        if (pressed && !ctrl_alt && (sym == XKB_KEY_p || sym == XKB_KEY_P)) {
            deadman_rearm(r);
            slog("P pressed -> dead-man reset to %ds (real-seat keyboard input is LIVE)",
                 r.deadman_ms / 1000);
            // fall through: do not consume.
        }

        // Esc OR Ctrl+Alt+Backspace -> terminate the session cleanly.
        if (sym == XKB_KEY_Escape || (ctrl_alt && sym == XKB_KEY_BackSpace) ||
            (ctrl_alt && sym == XKB_KEY_Terminate_Server)) {
            if (pressed) {
                slog("QUIT KEY pressed (Esc / Ctrl+Alt+Backspace) -> terminating");
                wl_display_terminate(r.display);
            }
            return true; // consume press AND release; never forward
        }

        // Ctrl+Alt+F1..F12 -> switch the Linux VT (escape to a console).
        // vt_for_keysym() is the SAME decision core input.cpp uses.
        if (const std::optional<unsigned> vt = unbox::kernel::vt_for_keysym(sym)) {
            if (pressed) {
                if (r.session != nullptr) {
                    slog("VT-SWITCH key -> wlr_session_change_vt(%u)", *vt);
                    wlr_session_change_vt(r.session, *vt);
                } else {
                    slog("VT-SWITCH key but no session (nested/headless) -> no-op");
                }
            }
            return true; // consume: no client forward (press or release)
        }
    }
    return false;
}

void update_seat_caps(Runner& r) {
    std::uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_TOUCH;
    if (!r.keyboards.empty()) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(r.seat, caps);
}

// MIRRORS src/input.cpp::new_keyboard — per-device key/modifiers/destroy
// listeners, default XKB keymap, repeat info, seat keyboard set. The previous
// spike kept ONE shared listener that the last device clobbered; on a real DRM
// seat with several keyboard devices that could leave keys arriving on a device
// with no live listener — which is exactly the "no key events reached the
// handler" field failure. Per-device listeners fix that by construction.
void new_keyboard(Runner& r, wlr_input_device* dev) {
    wlr_keyboard* wlr_kb = wlr_keyboard_from_input_device(dev);

    r.keyboards.emplace_back();
    Keyboard& kb = r.keyboards.back();
    kb.runner = &r;
    kb.keyboard = wlr_kb;

    xkb_context* xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* km = xkb_keymap_new_from_names(xkb, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_kb, km);
    xkb_keymap_unref(km);
    xkb_context_unref(xkb);
    wlr_keyboard_set_repeat_info(wlr_kb, 25, 600);

    kb.key_l.connect(wlr_kb->events.key, [&r, &kb](void* data) {
        auto* ev = static_cast<wlr_keyboard_key_event*>(data);
        // Escape hatch + dead-man FIRST, kernel-hardwired, before any forward.
        if (handle_escape_keys(r, kb, ev)) {
            return;
        }
        r.focus_kb = kb.keyboard;
        wlr_seat_set_keyboard(r.seat, kb.keyboard);
        // Lazily (re)assert keyboard focus on the mapped toplevel if the seat is
        // not already focused on a client surface — covers the real-seat case
        // where the FIRST key arrives before any enter landed (e.g. keyboard
        // hot-plugged after map, or map/enter raced). Without this the notify_key
        // below would fall on a surface with no keyboard focus and never reach
        // the client (the field "cannot type into foot" symptom).
        if (wlr_seat_get_keyboard(r.seat) == nullptr || r.focused_surface == nullptr) {
            if (r.last_toplevel != nullptr) {
                focus_toplevel(r, r.last_toplevel);
            }
        }
        wlr_seat_keyboard_notify_key(r.seat, ev->time_msec, ev->keycode, ev->state);
        ++r.keys_forwarded;
        if (r.keys_forwarded <= 8 || (r.keys_forwarded % 64) == 0) {
            slog("key FORWARDED to client (keycode=%u state=%u) — total forwarded=%ld%s",
                 ev->keycode, static_cast<unsigned>(ev->state), r.keys_forwarded,
                 r.focused_surface == nullptr ? " [WARN: no focused surface!]" : "");
        }
    });
    kb.mods_l.connect(wlr_kb->events.modifiers, [&r, &kb](void*) {
        r.focus_kb = kb.keyboard;
        wlr_seat_set_keyboard(r.seat, kb.keyboard);
        wlr_seat_keyboard_notify_modifiers(r.seat, &kb.keyboard->modifiers);
    });
    kb.destroy_l.connect(dev->events.destroy, [&r, &kb](void*) {
        slog("keyboard device REMOVED: '%s'", kb.keyboard->base.name ? kb.keyboard->base.name : "?");
        if (r.focus_kb == kb.keyboard) {
            r.focus_kb = nullptr;
        }
        Keyboard* self = &kb;
        r.keyboards.remove_if([self](const Keyboard& e) { return &e == self; });
        update_seat_caps(r);
    });

    r.focus_kb = wlr_kb;
    wlr_seat_set_keyboard(r.seat, wlr_kb);
    slog("keyboard device ADDED: '%s' (escape-hatch + P-keepalive listener attached)",
         dev->name != nullptr ? dev->name : "?");
    // If a toplevel mapped BEFORE this keyboard appeared, its focus enter was
    // deferred (no keyboard then) — hand it focus now so keys reach the client.
    if (r.focused_surface == nullptr && r.last_toplevel != nullptr) {
        focus_toplevel(r, r.last_toplevel);
    }
}

void handle_new_input(Runner& r, wlr_input_device* dev) {
    switch (dev->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        new_keyboard(r, dev);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        slog("pointer device ADDED: '%s'", dev->name != nullptr ? dev->name : "?");
        wlr_cursor_attach_input_device(r.cursor, dev);
        // Make the cursor visible immediately (a default xcursor image on the
        // wlr plane) so it shows even before the first motion event.
        show_default_cursor(r);
        break;
    case WLR_INPUT_DEVICE_TOUCH:
        slog("touch device ADDED: '%s'", dev->name != nullptr ? dev->name : "?");
        wlr_cursor_attach_input_device(r.cursor, dev);
        show_default_cursor(r);
        break;
    default:
        slog("input device ADDED (other type=%d): '%s'", static_cast<int>(dev->type),
             dev->name != nullptr ? dev->name : "?");
        break;
    }
    update_seat_caps(r);
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
            ++r.pointer_enters;
            slog("pointer BUTTON %u state=%u -> client surface '%s' at surface-local (%.1f,%.1f) "
                 "[screen (%.0f,%.0f) through the 3D transform]",
                 ev->button, static_cast<unsigned>(ev->state), rt.s->element_id.c_str(), rt.sx,
                 rt.sy, r.cursor->x, r.cursor->y);
        } else {
            slog("pointer BUTTON %u state=%u: NO surface under cursor (%.0f,%.0f) — not forwarded",
                 ev->button, static_cast<unsigned>(ev->state), r.cursor->x, r.cursor->y);
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
            ++r.touch_downs;
            slog("TOUCH DOWN id=%d -> client surface '%s' at surface-local (%.1f,%.1f) [screen "
                 "(%.0f,%.0f) through the 3D transform] downs=%ld",
                 ev->touch_id, rt.s->element_id.c_str(), rt.sx, rt.sy, lx, ly, r.touch_downs);
        } else {
            slog("TOUCH DOWN id=%d: NO surface under point (%.0f,%.0f) — not forwarded",
                 ev->touch_id, lx, ly);
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
        slog("TOUCH UP id=%d -> client", ev->touch_id);
    });

    // Seat protocol glue (mirrors src/input.cpp::attach_seat_handlers). A client
    // (foot) may request its OWN cursor surface (e.g. the text I-beam) while the
    // pointer is over it; honor that only for the currently pointer-focused
    // client. When the pointer focus leaves all client surfaces, restore OUR
    // default xcursor so the cursor never goes invisible over the document.
    r.seat_request_cursor_l.connect(r.seat->events.request_set_cursor, [&r](void* data) {
        auto* ev = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
        if (r.seat->pointer_state.focused_client == ev->seat_client) {
            wlr_cursor_set_surface(r.cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
            slog("client set its own cursor surface (e.g. text I-beam over foot)");
        }
    });
    r.seat_pointer_focus_change_l.connect(
        r.seat->pointer_state.events.focus_change, [&r](void* data) {
            auto* ev = static_cast<wlr_seat_pointer_focus_change_event*>(data);
            if (ev->new_surface == nullptr) {
                show_default_cursor(r);
            }
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
    wlr_output_mode* mode = wlr_output_preferred_mode(out);
    if (mode != nullptr) {
        wlr_output_state_set_mode(&st, mode);
    }
    const bool modeset_ok = wlr_output_commit_state(out, &st);
    wlr_output_state_finish(&st);

    if (out->width > 0) {
        r.out_w = out->width;
        r.out_h = out->height;
    }
    slog("output ADDED + MODESET: name='%s' %dx%d refresh=%dmHz preferred_mode=%d commit=%s",
         out->name, r.out_w, r.out_h, mode != nullptr ? mode->refresh : 0, mode != nullptr,
         modeset_ok ? "OK" : "FAILED");
    if (!modeset_ok) {
        slog("WARNING: modeset commit FAILED — the panel will likely stay black. "
             "Check WLR_RENDERER=gles2 and DRM permissions.");
    }

    wlr_output_layout_output* lo = wlr_output_layout_add_auto(r.output_layout, out);
    r.scene_output = wlr_scene_output_create(r.scene, out);
    wlr_scene_output_layout_add_output(r.scene_layout, lo, r.scene_output);

    // Load the xcursor theme for this output's scale BEFORE we ever set an
    // xcursor image (wlr_cursor_set_xcursor needs the theme loaded at the right
    // scale to produce a buffer for the plane). Without this the cursor plane
    // has no image => the "no mouse cursor visible" field symptom. Mirrors the
    // shipped kernel, which loads the theme on output bring-up.
    if (r.cursor_mgr != nullptr) {
        wlr_xcursor_manager_load(r.cursor_mgr, out->scale);
        slog("xcursor theme loaded for output scale %.2f (cursor can now show an image)",
             out->scale);
    }
    // If a pointer/touch device already exists, show the default cursor now that
    // the theme is loaded (device-add may have run before the output came up).
    show_default_cursor(r);

    // Guaranteed-visible NON-BLACK background + a test marker (criterion C),
    // created in the scene tree FIRST so they sit UNDER the RmlUi present node.
    // If the RmlUi/dmabuf present path works, the opaque composite covers these
    // (you see the tilted window on the document's own dark-blue body). If the
    // present path is BROKEN (no buffer reaches present_node), wlr_scene still
    // paints these — so "totally black" (nothing presents / modeset failed) is
    // visibly distinct from "dark blue + marker" (presenting, but the RmlUi
    // layer is empty). Dark blue: an unmistakable "the spike is alive" signal.
    const float kBlue[4] = {0.05f, 0.08f, 0.20f, 1.0f};
    const float kAmber[4] = {1.0f, 0.65f, 0.0f, 1.0f};
    r.bg_rect = wlr_scene_rect_create(&r.scene->tree, r.out_w, r.out_h, kBlue);
    r.marker_rect = wlr_scene_rect_create(&r.scene->tree, 64, 64, kAmber);
    wlr_scene_node_set_position(&r.marker_rect->node, 24, 24);

    // Build the present target + RmlUi document sized to the output, then a
    // single full-output scene_buffer node to present it (criterion 7). Created
    // AFTER the background rects so it renders ON TOP of them.
    r.gl.make_current();
    const bool present_ok = r.present.init(&r.gl, r.allocator, r.out_w, r.out_h);
    r.present_node = wlr_scene_buffer_create(&r.scene->tree, nullptr);
    r.present.scene_buffer = r.present_node;
    r.ctx = Rml::CreateContext("run", Rml::Vector2i(r.out_w, r.out_h), r.gl.render);
    r.doc = r.ctx->LoadDocumentFromMemory(kRunRml);
    if (r.doc != nullptr) {
        r.doc->Show();
    }
    r.gl.restore_current();
    slog("present target init=%d dmabuf=%d; RmlUi document=%s; background+marker rects placed",
         present_ok, r.present.dmabuf, r.doc != nullptr ? "loaded" : "FAILED");

    r.frame_l.connect(out->events.frame, [&r](void*) { on_frame(r); });
    wlr_output_schedule_frame(out);
    slog("output '%s' up at %dx%d; present node + RmlUi document built", out->name, r.out_w,
         r.out_h);
}

Runner* g_runner = nullptr;

// EACH client connect: wl_display's client-created signal is a raw wl_listener
// (not a wlr signal, so no RAII Listener wraps it). For a single-TU throwaway
// spike this is in-bounds; we never let it outlive the display (removed in
// teardown). Loud per-connect logging answers "did foot even connect?".
void on_client_created(wl_listener* l, void* data) {
    auto* client = static_cast<wl_client*>(data);
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    ++g_runner->client_connects;
    slog("CLIENT CONNECT #%d: a wl_client connected (pid=%d uid=%d) — now waiting for it to "
         "create + MAP a surface",
         g_runner->client_connects, static_cast<int>(pid), static_cast<int>(uid));
    (void)l;
}

} // namespace

auto run_real_seat(const char* startup_cmd) -> int {
    log_open();
    wlr_log_init(WLR_INFO, nullptr);
    slog("=== rml-compositing-spike --run START (persistent log: %s) ===", g_log_path.c_str());
    slog("env: WLR_BACKENDS=%s WLR_RENDERER=%s", getenv("WLR_BACKENDS") ? getenv("WLR_BACKENDS") : "(auto)",
         getenv("WLR_RENDERER") ? getenv("WLR_RENDERER") : "(auto)");
    Runner r;
    g_runner = &r;

    r.display = wl_display_create();
    r.loop = wl_display_get_event_loop(r.display);
    // Loudly log EACH client connect (diagnose "did foot connect?"). Raw
    // wl_listener — removed in teardown before the display dies.
    r.client_created_l.notify = on_client_created;
    wl_display_add_client_created_listener(r.display, &r.client_created_l);
    r.backend = wlr_backend_autocreate(r.loop, &r.session);
    if (r.backend == nullptr) {
        slog("FATAL: failed to create backend");
        log_close();
        return 1;
    }
    slog("backend created: session=%s (NULL session => nested/headless, no VT switching)",
         r.session != nullptr ? "present (real seat)" : "NULL");
    r.renderer = wlr_renderer_autocreate(r.backend);
    wlr_renderer_init_wl_display(r.renderer, r.display);
    r.allocator = wlr_allocator_autocreate(r.backend, r.renderer);
    slog("renderer selected: gles2=%d (RML compositing requires gles2)",
         wlr_renderer_is_gles2(r.renderer));

    if (!wlr_renderer_is_gles2(r.renderer)) {
        slog("FATAL: renderer is not gles2 — RML compositing needs the GL path. "
             "Set WLR_RENDERER=gles2.");
        log_close();
        return 1;
    }

    // SAFETY (criterion A) — signal handlers FIRST, so even an early hang during
    // bring-up can be killed cleanly. wl_event_loop_add_signal turns the signal
    // into a normal event-loop dispatch on the single thread: SIGINT/SIGTERM ->
    // wl_display_terminate -> wl_display_run returns -> clean wlroots/session
    // teardown restores the VT to text mode. This is what lets `kill`/`timeout`/
    // an SSH `pkill` exit WITHOUT a hard reboot.
    r.sigint_src = wl_event_loop_add_signal(r.loop, SIGINT, [](int, void* data) {
        slog("SIGINT received -> wl_display_terminate (clean exit)");
        wl_display_terminate(static_cast<wl_display*>(data));
        return 0;
    }, r.display);
    r.sigterm_src = wl_event_loop_add_signal(r.loop, SIGTERM, [](int, void* data) {
        slog("SIGTERM received -> wl_display_terminate (clean exit)");
        wl_display_terminate(static_cast<wl_display*>(data));
        return 0;
    }, r.display);

    // SAFETY — the GUARANTEED backstop: a DEAD-MAN self-timeout that terminates
    // the session no matter what, so the machine can NEVER be locked again.
    // DEFAULT 15s (was 120). Override with UNBOX_SPIKE_TIMEOUT seconds (0 =
    // disabled, for a deliberate long real-seat session once you trust the key
    // escapes). Pressing `P` re-arms it to the FULL interval (see handle_escape_
    // keys) — so keeping the session alive past 15s REQUIRES periodic P presses,
    // which doubles as the real-seat keyboard-input liveness test.
    int timeout_s = 15;
    if (const char* env = getenv("UNBOX_SPIKE_TIMEOUT")) {
        timeout_s = std::atoi(env);
    }
    r.deadman_ms = timeout_s * 1000;
    if (timeout_s > 0) {
        // The timer fires against the Runner so it can log + re-arm. It is a
        // ONE-SHOT (we never re-arm it ourselves on expiry): when it fires, the
        // session dies — UNLESS a `P` press re-armed it first.
        r.safety_timer = wl_event_loop_add_timer(r.loop, [](void* data) {
            auto* rr = static_cast<Runner*>(data);
            slog("DEAD-MAN FIRED (no `P` press within %ds) -> wl_display_terminate. "
                 "If you expected the session to stay open, real-seat keyboard input is DEAD "
                 "(P never reached the handler).",
                 rr->deadman_ms / 1000);
            wl_display_terminate(rr->display);
            return 0;
        }, &r);
        deadman_rearm(r);
        slog("DEAD-MAN self-timeout armed: %ds (press P to reset; UNBOX_SPIKE_TIMEOUT=0 to "
             "disable). Survival past %ds == keyboard input WORKS.",
             timeout_s, timeout_s);
    } else {
        slog("DEAD-MAN self-timeout DISABLED (UNBOX_SPIKE_TIMEOUT=0) — rely on Esc / "
             "Ctrl+Alt+Backspace / Ctrl+Alt+F-key / signals to exit");
    }
    slog("ESCAPE HATCH: Esc or Ctrl+Alt+Backspace = quit; Ctrl+Alt+F1..F12 = switch VT; "
         "SIGINT/SIGTERM = clean quit; P = reset dead-man (keyboard liveness test)");

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
    // Wire from new_toplevel/new_popup (role assigned) — NOT new_surface (no role
    // yet). This is what makes the configure handshake complete so clients map.
    r.new_toplevel_l.connect(r.xdg_shell->events.new_toplevel, [&r](void* data) {
        handle_new_toplevel(r, static_cast<wlr_xdg_toplevel*>(data));
    });
    r.new_popup_l.connect(r.xdg_shell->events.new_popup, [&r](void* data) {
        handle_new_popup(r, static_cast<wlr_xdg_popup*>(data));
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
        slog("FATAL: GL bridge init failed — NO-GO on this hardware");
        log_close();
        return 1;
    }

    const char* socket = wl_display_add_socket_auto(r.display);
    if (socket == nullptr) {
        slog("FATAL: failed to add wayland socket");
        log_close();
        return 1;
    }
    // Export WAYLAND_DISPLAY in OUR environment so EVERY child (the spawn below
    // AND anything it forks) inherits our socket, not the stale parent value.
    // Mirrors the shipped kernel (server.cpp): without this the client connects
    // to the WRONG compositor and nothing shows — a black-screen cause.
    setenv("WAYLAND_DISPLAY", socket, 1);
    slog("WAYLAND_DISPLAY=%s (exported into process env; children inherit it)", socket);

    if (!wlr_backend_start(r.backend)) {
        slog("FATAL: failed to start backend");
        log_close();
        return 1;
    }
    slog("backend started; up on WAYLAND_DISPLAY=%s", socket);

    if (startup_cmd != nullptr && startup_cmd[0] != '\0') {
        const pid_t pid = fork();
        if (pid == 0) {
            setenv("WAYLAND_DISPLAY", socket, 1);
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, static_cast<char*>(nullptr));
            // Only reached if exec failed.
            std::fprintf(stderr, "[run] exec of client failed: %s\n", startup_cmd);
            _exit(127);
        }
        if (pid > 0) {
            slog("client SPAWN: pid=%d cmd='%s' (watch for a 'client surface MAP' line; if it "
                 "never comes, the client could not connect/render)",
                 static_cast<int>(pid), startup_cmd);
        } else {
            slog("WARNING: fork() failed; no client spawned");
        }
    } else {
        slog("no startup command — connect your own client to WAYLAND_DISPLAY=%s", socket);
    }

    // NO-CLIENT watchdog (~5s): if nothing maps a surface by then, scream loudly
    // in the log so the "background+marker but no foot" case is unambiguous.
    r.client_watchdog = wl_event_loop_add_timer(r.loop, [](void* data) {
        auto* rr = static_cast<Runner*>(data);
        if (!rr->any_surface_mapped) {
            slog("*** NO CLIENT MAPPED — foot did not connect/render within ~5s. ***");
            slog("    connects-so-far=%d. If 0: the client could NOT connect (wrong "
                 "WAYLAND_DISPLAY, client crash, or missing binary). If >0: it connected but "
                 "produced no buffer (missing fonts, GL/shm failure). Only background+marker "
                 "will show.",
                 rr->client_connects);
        } else {
            slog("client-mapped check OK: at least one surface mapped within ~5s.");
        }
        return 0; // one-shot
    }, &r);
    wl_event_source_timer_update(r.client_watchdog, 5000);

    slog("entering event loop (wl_display_run)");
    wl_display_run(r.display);
    slog("event loop exited — tearing down cleanly (wlroots restores the VT to text mode)");

    // Teardown. Disconnect every RAII Listener bound to a wlr signal BEFORE the
    // wlr objects (cursor/backend/seat) are destroyed — a still-linked listener
    // trips wlr_cursor_destroy's `wl_list_empty(listener_list)` assertion (the
    // Runner's Listener members would otherwise unsubscribe only at Runner's
    // destruction, AFTER these destroys). Also drop per-surface listeners.
    for (LiveSurface& s : r.surfaces) {
        s.map_l.disconnect();
        s.unmap_l.disconnect();
        s.commit_l.disconnect();
        s.destroy_l.disconnect();
    }
    r.new_output_l.disconnect();
    r.new_input_l.disconnect();
    r.frame_l.disconnect();
    r.new_toplevel_l.disconnect();
    r.new_popup_l.disconnect();
    r.new_layer_l.disconnect();
    r.cursor_motion_l.disconnect();
    r.cursor_motion_abs_l.disconnect();
    r.cursor_button_l.disconnect();
    r.cursor_axis_l.disconnect();
    r.cursor_frame_l.disconnect();
    r.touch_down_l.disconnect();
    r.touch_up_l.disconnect();
    r.touch_motion_l.disconnect();
    r.seat_request_cursor_l.disconnect();
    r.seat_pointer_focus_change_l.disconnect();
    for (Keyboard& kb : r.keyboards) {
        kb.key_l.disconnect();
        kb.mods_l.disconnect();
        kb.destroy_l.disconnect();
    }
    // The raw client-created wl_listener must not outlive the display.
    wl_list_remove(&r.client_created_l.link);

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
    if (r.safety_timer != nullptr) {
        wl_event_source_remove(r.safety_timer);
    }
    if (r.client_watchdog != nullptr) {
        wl_event_source_remove(r.client_watchdog);
    }
    if (r.sigint_src != nullptr) {
        wl_event_source_remove(r.sigint_src);
    }
    if (r.sigterm_src != nullptr) {
        wl_event_source_remove(r.sigterm_src);
    }
    wl_display_destroy(r.display);
    slog("=== rml-compositing-spike --run EXIT 0 (VT restored) ===");
    log_close();
    return 0;
}
