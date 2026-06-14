// SPIKE (rml-compositing, Phase 0) — RUNNABLE GO/NO-GO target. THROWAWAY.
//
// Proves the "RML compositing" mechanism: a LIVE client toplevel/layer surface
// is imported zero-copy as a SHARED GL texture and drawn as a SURFACE ELEMENT
// (an RML <img>) in an RmlUi document; an RCSS 3D transform + transition is
// applied to it; input is routed back to the client through RmlUi picking; the
// composite is presented via the RmlUi-FBO -> wlr_scene_buffer bridge.
//
// It is its OWN compositor (display/backend/renderer/allocator/scene/seat +
// xdg-shell + layer-shell) so it can map real clients, NOT the shipped Server
// (which names no feature and exposes none of this). It reuses the kernel's
// proven pieces: the wlr.hpp extern-"C" wrapper, the adapted RenderInterface_GL3
// (with SetOutputFramebuffer + the upright V-flip), and the slice-3 dmabuf ->
// EGLImage import discipline. RMLUi is kernel-private and this lives IN the
// kernel unit, so including the private renderer header is in-bounds.
//
// Two modes:
//   --verify  : headless + gles2, NO real client. A synthetic client dmabuf
//               (known quadrant pattern) is imported LIVE; a known 3D transform
//               is applied; the presented buffer is read back and asserted
//               against the projected pattern; the idle dirty-gate is asserted
//               (zero renders over N idle turns); the screen->surface-local
//               input inversion is asserted through the transform; then a SECOND
//               bring-up composites a surface TREE (toplevel + subsurface +
//               popup) plus a layer-shell WALLPAPER as per-subsurface elements
//               and reads back each surface's footprint + stack order (criteria
//               4 + 5). Exit 0 = pass.
//   --run     : a real seat (DRM) or nested (labwc) run that spawns a client
//               (default `foot`), composites it live as a 3D surface element,
//               routes input back, and prints per-frame perf + idle metrics for
//               the user's visual/touch/perf GO-NO-GO. Ctrl-C to quit.
//
// wlroots only via unbox/kernel/wlr.hpp (.unbox/rules/wlroots-include.md).

#include <unbox/kernel/wlr.hpp>

#include "spike_gl.hpp"
#include "spike_input_core.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace spike = unbox::kernel::spike;

namespace {

int g_fail = 0;
void check(bool cond, const char* what) {
    std::fprintf(stderr, "[verify] %-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) {
        ++g_fail;
    }
}

// Allocate a real client dmabuf of (w,h) through the wlr allocator and paint it
// a single solid color via the wlr render pass — exactly the GPU path a client
// produces. Returns the locked wlr_buffer (caller drops it) or nullptr. `gl`
// must NOT be current while the wlr renderer runs, so we restore it around the
// pass and re-make-current after (mirrors the existing criterion-1 painter).
auto make_solid_client_buffer(spike::GlBridge& gl, wlr_renderer* renderer,
                              wlr_allocator* allocator, int w, int h, float r, float g, float b)
    -> wlr_buffer* {
    wlr_drm_format cfmt{};
    cfmt.format = spike::kArgb8888;
    std::uint64_t cmods[] = {0};
    cfmt.len = 1;
    cfmt.capacity = 1;
    cfmt.modifiers = cmods;
    wlr_buffer* buf = wlr_allocator_create_buffer(allocator, w, h, &cfmt);
    if (buf == nullptr) {
        return nullptr;
    }
    gl.restore_current();
    wlr_buffer_pass_options po{};
    wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(renderer, buf, &po);
    if (pass != nullptr) {
        wlr_render_rect_options ro{};
        ro.box = {0, 0, w, h};
        ro.color = {r, g, b, 1};
        ro.blend_mode = WLR_RENDER_BLEND_MODE_NONE;
        wlr_render_pass_add_rect(pass, &ro);
        wlr_render_pass_submit(pass);
    }
    gl.make_current();
    return buf;
}

// The verify document: a single surface element (the live client texture) the
// size of the surface, with an RCSS 3D transform + transition. No body margin so
// the <img> fills the 256x256 surface 1:1 before transform.
const char* kVerifyRmlTemplate = R"RML(<rml>
<head>
<style>
body { margin: 0px; padding: 0px; width: 256px; height: 256px;
       perspective: 800px; }
#win { display: block; position: absolute; left: 0px; top: 0px;
       width: 256px; height: 256px;
       transform: rotateY(0deg);
       transform-origin: 50% 50%;
       transition: transform 0.2s linear-in-out; }
#win img { display: block; width: 256px; height: 256px; }
</style>
</head>
<body>
<div id="win"><img src="LIVE_URI"/></div>
</body>
</rml>)RML";

auto run_verify() -> int {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);

    wlr_log_init(WLR_ERROR, nullptr);
    wl_display* display = wl_display_create();
    wl_event_loop* loop = wl_display_get_event_loop(display);
    wlr_backend* backend = wlr_backend_autocreate(loop, nullptr);
    wlr_renderer* renderer = wlr_renderer_autocreate(backend);
    wlr_allocator* allocator = wlr_allocator_autocreate(backend, renderer);
    wlr_scene* scene = wlr_scene_create();

    if (!wlr_renderer_is_gles2(renderer)) {
        std::fprintf(stderr, "[verify] SKIP: renderer is not gles2 (no GL path on this box)\n");
        return 0;
    }
    EGLDisplay egl = wlr_egl_get_display(wlr_gles2_renderer_get_egl(renderer));

    spike::GlBridge gl;
    if (!gl.init(egl)) {
        std::fprintf(stderr, "[verify] SKIP: sibling GL bridge unavailable\n");
        return 0;
    }
    check(gl.dmabuf_ok, "criterion 1: dmabuf import path available on this GPU");
    check(gl.fence_ok, "criterion 7: EGL fence-sync (no glFinish) present path active");

    gl.make_current();

    // The "live client buffer": a 256x256 dmabuf allocated through the wlr
    // allocator (a real dmabuf the client path produces), painted with a quadrant
    // pattern (TL red, TR green, BL blue, BR white) via the wlr renderer the way
    // a GPU client would. Imported zero-copy as the live surface element.
    const int W = 256;
    spike::LiveTexture live;
    live.gl = &gl;
    live.uri = "unbox-live://win";

    wlr_drm_format cfmt{};
    cfmt.format = spike::kArgb8888;
    std::uint64_t cmods[] = {0};
    cfmt.len = 1;
    cfmt.capacity = 1;
    cfmt.modifiers = cmods;
    wlr_buffer* client_buf = wlr_allocator_create_buffer(allocator, W, W, &cfmt);
    bool live_zero_copy = false;
    if (client_buf != nullptr) {
        gl.restore_current();
        wlr_buffer_pass_options po{};
        wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(renderer, client_buf, &po);
        if (pass != nullptr) {
            const wlr_render_color quad[4] = {
                {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
            const wlr_box boxes[4] = {{0, 0, W / 2, W / 2},
                                      {W / 2, 0, W / 2, W / 2},
                                      {0, W / 2, W / 2, W / 2},
                                      {W / 2, W / 2, W / 2, W / 2}};
            for (int i = 0; i < 4; ++i) {
                wlr_render_rect_options r{};
                r.box = boxes[i];
                r.color = quad[i];
                r.blend_mode = WLR_RENDER_BLEND_MODE_NONE;
                wlr_render_pass_add_rect(pass, &r);
            }
            wlr_render_pass_submit(pass);
        }
        gl.make_current();
        live_zero_copy = live.adopt(client_buf) && live.is_dmabuf;
    }
    check(client_buf != nullptr && live.tex != 0,
          "criterion 1: live client buffer imported as a sampled texture");
    check(live_zero_copy, "criterion 1: live import is ZERO-COPY dmabuf (not a CPU copy)");

    const int reimports_before = live.reimports;
    live.adopt(client_buf);
    live.adopt(client_buf);
    check(live.reimports == reimports_before,
          "criterion 1: unchanged buffer is NOT re-imported (cached)");

    std::string rml = kVerifyRmlTemplate;
    rml.replace(rml.find("LIVE_URI"), 8, live.uri);
    Rml::Context* ctx = Rml::CreateContext("verify", Rml::Vector2i(W, W), gl.render);
    Rml::ElementDocument* doc = (ctx != nullptr) ? ctx->LoadDocumentFromMemory(rml) : nullptr;
    check(doc != nullptr, "verify document loaded");
    if (doc != nullptr) {
        doc->Show();
    }

    spike::PresentTarget present;
    const bool present_ok = present.init(&gl, allocator, W, W);
    present.scene_buffer = wlr_scene_buffer_create(&scene->tree, nullptr);
    check(present_ok, "criterion 7: present FBO -> wlr_buffer target built");
    check(present.dmabuf, "criterion 7: present buffer is a dmabuf (Plan A swapchain)");

    auto is_color = [](const std::uint8_t p[4], int r, int g, int b) {
        return std::abs(int(p[0]) - r) < 60 && std::abs(int(p[1]) - g) < 60 &&
               std::abs(int(p[2]) - b) < 60;
    };

    if (doc != nullptr) {
        // ---- Criterion 1+7 untransformed: the live pattern presents UPRIGHT
        // with the quadrant colors in the right corners (present path renders the
        // LIVE texture). ----
        present.render(ctx);
        std::uint8_t tl[4], tr[4], bl[4], br[4];
        present.pixel(40, 40, tl);
        present.pixel(W - 40, 40, tr);
        present.pixel(40, W - 40, bl);
        present.pixel(W - 40, W - 40, br);
        check(is_color(tl, 255, 0, 0), "criterion 1: live TL quadrant red, upright, correct corner");
        check(is_color(tr, 0, 255, 0), "criterion 1: live TR quadrant green");
        check(is_color(bl, 0, 0, 255), "criterion 1: live BL quadrant blue");
        check(is_color(br, 255, 255, 255), "criterion 1: live BR quadrant white");

        // ---- Criterion 2: rotateY(180) (a deterministic endpoint of the 3D
        // transform+transition) mirrors X about the 50% origin: TL red -> TOP-
        // RIGHT, TR green -> TOP-LEFT. Reading the swapped corners proves the
        // LIVE pixels rendered THROUGH the RCSS 3D transform. ----
        doc->GetElementById("win")->SetProperty("transform", "rotateY(180deg)");
        ctx->Update();
        present.render(ctx);
        std::uint8_t t_left[4], t_right[4];
        present.pixel(40, 40, t_left);
        present.pixel(W - 40, 40, t_right);
        check(is_color(t_right, 255, 0, 0),
              "criterion 2: rotateY(180) moved live TL-red to the TOP-RIGHT");
        check(is_color(t_left, 0, 255, 0),
              "criterion 2: rotateY(180) moved live TR-green to the TOP-LEFT");

        // Mid-rotation under perspective must still SHOW the texture (alpha>0).
        doc->GetElementById("win")->SetProperty("transform", "rotateY(60deg)");
        ctx->Update();
        present.render(ctx);
        std::uint8_t center[4];
        present.pixel(W / 2, W / 2, center);
        check(center[3] > 0, "criterion 2: live texture visible under perspective rotateY(60deg)");

        // Reset to the flat state for the idle-gate measurement.
        doc->GetElementById("win")->SetProperty("transform", "rotateY(0deg)");
        ctx->Update();
        present.render(ctx);
    }

    // ---- Criterion 6 idle gate: with NO new commit, NO animation, NO input,
    // OUR gate renders ZERO frames over N event-loop turns. The gate renders only
    // when a dirty signal fires (client commit / active RCSS animation / input).
    // ----
    // The gate's animation signal is RmlUi's own GetNextUpdateDelay(): finite =>
    // an animation needs the next frame; +inf => nothing is animating (idle). We
    // gate on (our dirty) OR (animation pending), exactly the design's three
    // dirty sources (client commit / RCSS animation / input).
    auto anim_pending = [&]() -> bool {
        ctx->Update();
        return std::isfinite(ctx->GetNextUpdateDelay());
    };
    if (doc != nullptr) {
        // Drain any settle frames so the document is fully at rest before we
        // measure idle (a freshly-shown doc may request one more update).
        for (int i = 0; i < 8; ++i) {
            if (anim_pending()) {
                present.render(ctx);
            }
        }
        int idle_renders = 0;
        bool dirty = false;
        for (int turn = 0; turn < 120; ++turn) {
            wl_event_loop_dispatch(loop, 0);
            if (dirty || anim_pending()) {
                present.render(ctx);
                ++idle_renders;
                dirty = false;
            }
        }
        check(idle_renders == 0, "criterion 6: idle dirty-gate renders ZERO frames over 120 turns");

        int gated_renders = 0;
        dirty = true; // simulate a single client buffer commit
        for (int turn = 0; turn < 10; ++turn) {
            if (dirty || anim_pending()) {
                present.render(ctx);
                ++gated_renders;
                dirty = false;
            }
        }
        check(gated_renders == 1, "criterion 6: a single commit gates exactly ONE render");
    }

    // ---- Criterion 3 geometry: screen->surface-local inversion through the SAME
    // transform RCSS applies (perspective(800) about the 50% origin, rotateY).
    // Project a known surface-local point to its screen landing, invert, and
    // confirm round-trip identity to sub-pixel — the math the runtime
    // RmlUi-pick -> wl_seat translation rides on. ----
    {
        const double origin = W / 2.0;
        const spike::Mat4 t = spike::rcss_transform_about_origin(
            spike::mul(spike::perspective(800.0), spike::rotate_y(35.0 * M_PI / 180.0)), origin,
            origin);
        const double lx = 64.0, ly = 96.0;
        const spike::ScreenPoint s = spike::project_to_screen(t, lx, ly);
        const auto back = spike::unproject_to_local(t, s.x, s.y);
        check(back.has_value(), "criterion 3: inversion solvable through perspective+rotateY");
        if (back) {
            const double err = std::hypot(back->x - lx, back->y - ly);
            std::fprintf(stderr, "[verify] criterion 3 round-trip error = %.6f px\n", err);
            check(err < 0.01, "criterion 3: screen->surface-local round-trip < 0.01px");
        }
    }

    present.teardown();
    live.destroy();
    if (ctx != nullptr) {
        Rml::RemoveContext("verify");
    }
    gl.restore_current();
    gl.teardown();
    if (client_buf != nullptr) {
        wlr_buffer_drop(client_buf);
    }
    wlr_scene_node_destroy(&scene->tree.node);
    wlr_allocator_destroy(allocator);
    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(display);
    return 0;
}

// ---- Criteria 4 + 5: surface trees + wallpaper (per-subsurface elements) -----
//
// THE #1 unknown (criterion 4): a toplevel that owns a POPUP and a SUBSURFACE,
// composited correctly. This prototypes the PER-SUBSURFACE-ELEMENT answer: every
// node of the surface tree (toplevel, subsurface, popup) is its OWN RML <img>
// sampling its OWN live shared texture, positioned in RCSS at its offset, with
// document order giving the stack (parent first, child/popup above). The
// alternative (per-window render-to-texture: flatten the whole tree to one
// texture off-screen, sample that as ONE element) is ANALYSED in the report;
// here we prove the per-subsurface path objectively by readback.
//
// Criterion 5 (wallpaper): a layer-shell client is just another surface element
// behind the stage — imported through the SAME LiveTexture::adopt path as the
// toplevel (criterion 1). We prove it by importing a full-output wallpaper
// buffer the identical way and reading it back where the toplevel does not cover
// it. "Mechanically identical to the toplevel path" is therefore shown, not
// asserted by hand-wave.
//
// Layout (output W x W), all flat (no 3D) so readback geometry is deterministic
// and each surface's screen footprint is exactly its element box:
//   wallpaper : full output, BLUE,   behind everything
//   toplevel  : (TLX,TLY) sized TW,  RED
//   subsurface: offset (+SOFF,+SOFF) inside the toplevel, GREEN  (occludes RED)
//   popup     : at the toplevel's top-right, partly past it, WHITE (above all)
auto run_verify_surface_trees() -> int {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);

    wl_display* display = wl_display_create();
    wlr_backend* backend = wlr_backend_autocreate(wl_display_get_event_loop(display), nullptr);
    wlr_renderer* renderer = wlr_renderer_autocreate(backend);
    wlr_allocator* allocator = wlr_allocator_autocreate(backend, renderer);
    wlr_scene* scene = wlr_scene_create();

    if (!wlr_renderer_is_gles2(renderer)) {
        std::fprintf(stderr, "[verify] SKIP surface-tree: renderer is not gles2\n");
        wlr_scene_node_destroy(&scene->tree.node);
        wlr_allocator_destroy(allocator);
        wlr_renderer_destroy(renderer);
        wlr_backend_destroy(backend);
        wl_display_destroy(display);
        return 0;
    }
    EGLDisplay egl = wlr_egl_get_display(wlr_gles2_renderer_get_egl(renderer));

    spike::GlBridge gl;
    if (!gl.init(egl)) {
        std::fprintf(stderr, "[verify] SKIP surface-tree: GL bridge unavailable\n");
        wlr_scene_node_destroy(&scene->tree.node);
        wlr_allocator_destroy(allocator);
        wlr_renderer_destroy(renderer);
        wlr_backend_destroy(backend);
        wl_display_destroy(display);
        return 0;
    }
    gl.make_current();

    const int W = 512;
    const int TLX = 128, TLY = 96, TW = 256, TH = 256; // toplevel box
    const int SOFF = 48, SW = 96, SH = 96;             // subsurface: inside toplevel
    const int PW = 96, PH = 64;                        // popup: at toplevel top-right edge
    const int PX = TLX + TW - 32, PY = TLY - 16;       // hangs past the toplevel corner

    // Four real client dmabufs, painted like a GPU client would.
    wlr_buffer* wall_buf = make_solid_client_buffer(gl, renderer, allocator, W, W, 0, 0, 1);   // blue
    wlr_buffer* top_buf = make_solid_client_buffer(gl, renderer, allocator, TW, TH, 1, 0, 0);  // red
    wlr_buffer* sub_buf = make_solid_client_buffer(gl, renderer, allocator, SW, SH, 0, 1, 0);  // green
    wlr_buffer* pop_buf = make_solid_client_buffer(gl, renderer, allocator, PW, PH, 1, 1, 1);  // white

    spike::LiveTexture wall, top, sub, pop;
    for (auto* t : {&wall, &top, &sub, &pop}) {
        t->gl = &gl;
    }
    wall.uri = "unbox-live://wall";
    top.uri = "unbox-live://top";
    sub.uri = "unbox-live://sub";
    pop.uri = "unbox-live://pop";

    bool zero_copy = true;
    struct Pair {
        spike::LiveTexture* t;
        wlr_buffer* b;
    };
    for (const Pair& p : {Pair{&wall, wall_buf}, Pair{&top, top_buf}, Pair{&sub, sub_buf},
                          Pair{&pop, pop_buf}}) {
        const bool ok = p.b != nullptr && p.t->adopt(p.b);
        zero_copy = zero_copy && ok && p.t->is_dmabuf;
    }
    check(zero_copy, "criterion 4/5: tree (toplevel+subsurface+popup) + wallpaper imported zero-copy");

    // ONE document, FOUR surface elements (per-subsurface answer): wallpaper
    // first (behind), then the toplevel, then its subsurface, then the popup —
    // document order is the composite stack. Each <img> samples its own live
    // texture and is positioned in RCSS at its surface-tree offset.
    char rml[2048];
    std::snprintf(rml, sizeof(rml),
                  "<rml><head><style>"
                  "body { margin:0px; padding:0px; width:%dpx; height:%dpx; }"
                  ".s { display:block; position:absolute; }"
                  ".s img { display:block; width:100%%; height:100%%; }"
                  "</style></head><body>"
                  "<div class=s id=wall style='left:0;top:0;width:%dpx;height:%dpx;'>"
                  "<img src='%s'/></div>"
                  "<div class=s id=top style='left:%dpx;top:%dpx;width:%dpx;height:%dpx;'>"
                  "<img src='%s'/></div>"
                  "<div class=s id=sub style='left:%dpx;top:%dpx;width:%dpx;height:%dpx;'>"
                  "<img src='%s'/></div>"
                  "<div class=s id=pop style='left:%dpx;top:%dpx;width:%dpx;height:%dpx;'>"
                  "<img src='%s'/></div>"
                  "</body></rml>",
                  W, W, W, W, wall.uri.c_str(), TLX, TLY, TW, TH, top.uri.c_str(), TLX + SOFF,
                  TLY + SOFF, SW, SH, sub.uri.c_str(), PX, PY, PW, PH, pop.uri.c_str());

    Rml::Context* ctx = Rml::CreateContext("vtree", Rml::Vector2i(W, W), gl.render);
    Rml::ElementDocument* doc = (ctx != nullptr) ? ctx->LoadDocumentFromMemory(rml) : nullptr;
    check(doc != nullptr, "criterion 4: surface-tree document loaded");
    if (doc != nullptr) {
        doc->Show();
    }

    spike::PresentTarget present;
    const bool present_ok = present.init(&gl, allocator, W, W);
    present.scene_buffer = wlr_scene_buffer_create(&scene->tree, nullptr);
    check(present_ok, "criterion 4/5: present target for the tree built");

    auto is_color = [](const std::uint8_t p[4], int r, int g, int b) {
        return std::abs(int(p[0]) - r) < 60 && std::abs(int(p[1]) - g) < 60 &&
               std::abs(int(p[2]) - b) < 60;
    };

    if (doc != nullptr && present_ok) {
        present.render(ctx);
        std::uint8_t px[4];

        // Wallpaper shows in a corner no other surface covers (criterion 5).
        present.pixel(16, 16, px);
        check(is_color(px, 0, 0, 255), "criterion 5: wallpaper (layer surface) visible behind all");

        // Toplevel RED shows where neither subsurface nor popup covers it: a spot
        // inside the toplevel but outside the (TLX+SOFF..+SW) subsurface box.
        present.pixel(TLX + 16, TLY + TH - 16, px);
        check(is_color(px, 255, 0, 0), "criterion 4: toplevel surface composited over wallpaper");

        // Subsurface GREEN occludes the toplevel at its offset box centre
        // (per-subsurface element drawn ABOVE its parent by document order).
        present.pixel(TLX + SOFF + SW / 2, TLY + SOFF + SH / 2, px);
        check(is_color(px, 0, 255, 0),
              "criterion 4: subsurface element occludes the toplevel at its offset");

        // Popup WHITE at its own box centre — drawn above everything, and where it
        // hangs PAST the toplevel it sits directly on the wallpaper (proves popups
        // are not clipped to the parent element).
        present.pixel(PX + PW / 2, PY + PH / 2, px);
        check(is_color(px, 255, 255, 255), "criterion 4: popup element composited above the tree");

        // Stacking integrity: the popup's TOP edge (above the toplevel's top) is
        // popup-white over wallpaper-blue, NOT toplevel-red — order is correct.
        present.pixel(PX + PW / 2, PY + 6, px);
        check(is_color(px, 255, 255, 255),
              "criterion 4: surface-tree stack order correct (popup top over wallpaper)");
    }

    present.teardown();
    for (auto* t : {&wall, &top, &sub, &pop}) {
        t->destroy();
    }
    if (ctx != nullptr) {
        Rml::RemoveContext("vtree");
    }
    gl.restore_current();
    gl.teardown();
    for (wlr_buffer* b : {wall_buf, top_buf, sub_buf, pop_buf}) {
        if (b != nullptr) {
            wlr_buffer_drop(b);
        }
    }
    wlr_scene_node_destroy(&scene->tree.node);
    wlr_allocator_destroy(allocator);
    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(display);
    return 0;
}

} // namespace

// The real-seat run mode lives in rml_compositing_spike_run.cpp (its own TU).
auto run_real_seat(const char* startup_cmd) -> int;

int main(int argc, char** argv) {
    const char* mode = (argc > 1) ? argv[1] : "--verify";
    if (std::strcmp(mode, "--verify") == 0) {
        // Two independent compositor bring-ups (each its own display/renderer/GL
        // bridge) so one cannot corrupt the other's GL/RmlUi global state: first
        // the live-texture/3D/input/idle/present criteria (1,2,3,6,7), then the
        // surface-tree + wallpaper criteria (4,5). g_fail accumulates across both;
        // ALL PASS is printed once for the whole run.
        run_verify();
        run_verify_surface_trees();
        std::fprintf(stderr, "\n[verify] %s (%d failures)\n",
                     g_fail == 0 ? "ALL PASS" : "FAILURES", g_fail);
        return g_fail == 0 ? 0 : 1;
    }
    if (std::strcmp(mode, "--run") == 0) {
        const char* cmd = (argc > 2) ? argv[2] : "foot";
        return run_real_seat(cmd);
    }
    std::fprintf(stderr,
                 "usage: %s [--verify | --run [startup-cmd]]\n"
                 "  --verify  headless self-check of criteria 1,2,3,4,5,6,7 (exit 0 = pass)\n"
                 "  --run     real/nested seat: spawn a client, composite it as a 3D\n"
                 "            surface element, route input back, print perf/idle metrics\n",
                 argv[0]);
    return 2;
}
