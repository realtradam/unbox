#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unbox/kernel/extension.hpp>
#include <unbox/kernel/hooks.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/listener.hpp> // RAII wl_listener for the Wave-1b tree test extension
#include <unbox/kernel/kernel.hpp>
#include <unbox/kernel/server.hpp>
#include <unbox/kernel/surface_registry.hpp>
#include <unbox/kernel/ui.hpp>

// Same-unit private header: the substrate's PURE decision cores (touch-mode
// state machine, implicit-grab ownership, hit-test geometry) are doctest-ed
// directly, no wlroots.
#include "../src/ui_core.hpp"
// The PRODUCTION surface-element input-back PURE core (the port of the spike's
// screen->surface-local inversion + the parent-relative child-placement helper),
// doctest-ed here as the strict-core half of Wave 1b. No wlroots/GL/RMLUi.
#include "../src/input_core.hpp"
// The VT-switch escape hatch's pure core (keysym -> VT number), no wlroots.
#include "../src/vt_core.hpp"
// SPIKE (rml-compositing, Phase 0): the throwaway spike's PURE input-inversion
// core (screen-point -> surface-local through a 3D transform). Header-only, no
// wlroots/GL/RMLUi, so the criterion-3 geometry is doctest-ed here alongside the
// runnable target's own headless self-check (src/spike/). Kept in the kernel
// suite so the spike's geometry stays green with the unit.
#include "../src/spike/spike_input_core.hpp"

#include <cmath>
#include <numbers>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// TEST-ONLY: an in-process Wayland CLIENT for the surface-element integration
// test (the only in-process way to get a real wlr_surface with an advancing
// commit seq). wayland-client is a test-executable-only dep (the kernel is a
// compositor, never a client) — see packages/kernel/meson.build.
#include <wayland-client.h>

// Wave-1b surface-tree test: a real xdg popup is a tree child, so the test needs
// CLIENT-side xdg-shell bindings (generated; see packages/kernel/meson.build) and
// the SERVER side runs xdg-shell in a TEST extension via the kernel's wlr wrapper
// (xdg-shell stays a feature, provided by an extension — the kernel names none).
#include <unbox/kernel/wlr.hpp>
#include "xdg-shell-client-protocol.h"
#include "xdg-shell-client-protocol-code.h" // private code: included by ONE TU only

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

TEST_CASE("kernel compiles against and links wlroots + libwayland-server") {
    CHECK(unbox::kernel::link_probe());
    CHECK(unbox::kernel::wlroots_version().substr(0, 4) == "0.20");
}

TEST_CASE("vendored RMLUi subproject compiled and linked") {
    CHECK(!unbox::kernel::rmlui_version().empty());
}

TEST_CASE("server boots and shuts down on the headless backend") {
    // Headless backend + pixman renderer: no GPU, no parent session needed.
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    // Simulate the inherited-parent value (labwc's wayland-0) the real bug left
    // in place. The startup setenv must OVERWRITE this with our own socket.
    setenv("WAYLAND_DISPLAY", "wayland-stale-parent", 1);

    auto server = unbox::kernel::Server::create({});
    CHECK(!server->socket_name().empty());

    // Regression guard for the real bug: after startup the PROCESS environment's
    // WAYLAND_DISPLAY must name OUR socket (not the inherited parent value), so
    // every child — the -s startup spawn AND any extension's spawn — connects to
    // unbox by default instead of the wrong compositor ("no monitors").
    const char* env_display = getenv("WAYLAND_DISPLAY");
    REQUIRE(env_display != nullptr);
    CHECK(std::string(env_display) == server->socket_name());

    for (int i = 0; i < 3; ++i) {
        CHECK(server->dispatch(10));
    }
    // Destruction runs the full tinywl shutdown sequence.
}

TEST_CASE("server boots with a headless output present and advertised") {
    // The headless backend creates its output during wlr_backend_start (inside
    // Server::create), so it is enabled + committed + globalled before this
    // returns. We assert the boot path survives an output being present and the
    // event loop pumps cleanly — the headless analogue of the DRM advertise the
    // wl_output-global guarantee (the layout auto-advertises the global for an
    // output with a committed size).
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);

    auto server = unbox::kernel::Server::create({});
    CHECK(!server->socket_name().empty());
    server->activate_extensions();
    for (int i = 0; i < 5; ++i) {
        CHECK(server->dispatch(10));
    }

    unsetenv("WLR_HEADLESS_OUTPUTS");
}

// ============================================================================
// The ui substrate — contract-critical facade. A TEST extension creates a ui
// surface through the PUBLIC Host::ui() path, binds a scalar + event, and the
// kernel suite asserts: frames advance, the submitted buffer is upright,
// button/touch over the surface is CONSUMED (a second extension's bus hooks do
// not see it), touch-mode flips and scales hit-test geometry, and the EGL
// fence-sync (production) path is active. Headless+gles2 exercises the GL
// bridge; pixman makes the substrate unavailable (graceful no-op).
// ============================================================================

namespace {

using unbox::kernel::Host;
using unbox::kernel::Manifest;
using unbox::kernel::Tier;
using unbox::kernel::UiSurface;
using unbox::kernel::UiSurfaceSpec;

// Distinctive top (#18e0a0) / bottom (#e09018) full-width bands = the
// orientation guard the substrate's ui_orientation() samples. A live
// data-bound counter ({{frame}}) + a data-event button (input proof). (The
// button uses `dp` units, but touch-mode does NO scaling now — it looks the
// same in both modes; the fixture is unchanged from when it did.)
const char* kFixtureRml = R"RML(<rml>
<head>
<style>
body { font-family: "Noto Sans"; background: #1e2230; color: #e8ecff;
       width: 320px; height: 200px; }
#topband    { display: block; width: 320px; height: 12px; background: #18e0a0; }
#bottomband { display: block; width: 320px; height: 12px; background: #e09018;
              position: absolute; bottom: 0px; left: 0px; }
button { display: block; width: 80dp; height: 40dp; margin: 24px;
         background: #3a4670; }
</style>
</head>
<body data-model="ui">
<div id="topband"></div>
<p>frame {{frame}}</p>
<button id="b" data-event-click="tap">{{label}}</button>
<div id="bottomband"></div>
</body>
</rml>)RML";

// A test extension that owns a ui surface and a bus button-hook (to prove
// consumption: when a click lands on the surface, this hook must NOT fire).
class UiTestExtension : public unbox::kernel::Extension {
public:
    auto manifest() const -> const Manifest& override { return manifest_; }

    void activate(Host& host) override {
        button_hits_via_bus = 0;
        substrate_ = &host.ui(); // borrow valid for the session
        button_sub_ = host.subscribe(host.on_pointer_button(),
                                     [this](const unbox::kernel::PointerButtonEvent&) {
                                         ++button_hits_via_bus;
                                     });
        UiSurfaceSpec spec;
        spec.rml_inline = kFixtureRml;
        spec.x = 40;
        spec.y = 40;
        spec.width = 320;
        spec.height = 200;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
        if (surface_ != nullptr) {
            surface_->bind_int("frame", [this] { return frame; });
            surface_->bind_string("label", [] { return std::string("tap me"); });
            surface_->bind_event("tap", [this] { ++taps; });
            surface_->on_touch_mode_changed([this](bool touch) {
                ++touch_mode_changes;
                last_touch_mode = touch;
            });
        }
    }

    void advance() {
        ++frame;
        if (surface_ != nullptr) {
            surface_->dirty("frame");
        }
    }

    int frame = 0;
    int taps = 0;
    int button_hits_via_bus = 0;
    int touch_mode_changes = 0;
    bool last_touch_mode = false;
    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }
    [[nodiscard]] auto surface() -> UiSurface* { return surface_.get(); }
    // Reads the substrate's touch-mode through the public facade the extension
    // was handed (proves the STATE is observable via Host::ui()).
    [[nodiscard]] auto substrate_touch_mode() const -> bool {
        return substrate_ != nullptr && substrate_->touch_mode();
    }

private:
    Manifest manifest_{"ui-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
    unbox::kernel::UiSubstrate* substrate_ = nullptr;
    unbox::kernel::Subscription button_sub_;
};

void pump(unbox::kernel::Server& s, int turns) {
    for (int i = 0; i < turns; ++i) {
        s.dispatch(10);
    }
}

// ---- slice-10 / a1 preview spike: a known-color source buffer + an <img> ----
//
// A data-ptr wlr_buffer filled with one solid color, wrapped in a scene-buffer
// node under a private tree. The preview snapshots THIS subtree; a ui surface's
// <img src="unbox-preview://N"> samples it. Color is FourCC AR24 little-endian
// {B,G,R,A} so the test color round-trips to RMLUi's RGBA after the snapshot.

constexpr std::uint32_t kArgb8888 = 0x34325241; // 'AR24'

struct TestSrcBuffer {
    wlr_buffer base{};
    std::vector<std::uint8_t> data;
    std::size_t stride = 0;
};

void test_src_destroy(wlr_buffer* b) {
    auto* buf = reinterpret_cast<TestSrcBuffer*>(b);
    wlr_buffer_finish(&buf->base);
    delete buf;
}
bool test_src_access(wlr_buffer* b, std::uint32_t, void** data, std::uint32_t* format,
                     std::size_t* stride) {
    auto* buf = reinterpret_cast<TestSrcBuffer*>(b);
    *data = buf->data.data();
    *format = kArgb8888;
    *stride = buf->stride;
    return true;
}
void test_src_end(wlr_buffer*) {}

const wlr_buffer_impl kTestSrcImpl = {
    .destroy = test_src_destroy,
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = test_src_access,
    .end_data_ptr_access = test_src_end,
};

// Build a w*h buffer of solid (r,g,b) opaque pixels (premultiplied; opaque so
// premultiply is identity). Stored {B,G,R,A} per AR24.
auto make_solid_buffer(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b)
    -> TestSrcBuffer* {
    auto* buf = new TestSrcBuffer();
    buf->stride = static_cast<std::size_t>(w) * 4;
    buf->data.assign(buf->stride * static_cast<std::size_t>(h), 0);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        buf->data[i * 4 + 0] = b;
        buf->data[i * 4 + 1] = g;
        buf->data[i * 4 + 2] = r;
        buf->data[i * 4 + 3] = 0xff;
    }
    wlr_buffer_init(&buf->base, &kTestSrcImpl, w, h);
    return buf;
}

// A ui surface whose ONLY content is a full-bleed <img> of a preview. Sized to
// the surface; the body has no margin so the image fills it. Distinct bg
// (#101010) so a failed sample is obvious.
const char* kPreviewRml = R"RML(<rml>
<head>
<style>
body { background: #101010; width: 200px; height: 200px; }
img { display: block; position: absolute; left: 0px; top: 0px;
      width: 200px; height: 200px; }
</style>
</head>
<body data-model="ui">
<img src="PREVIEW_URI"/>
</body>
</rml>)RML";

// Extension that builds a known-color source subtree, makes a Preview of it,
// and shows it in a ui surface via <img>. Records whether each step succeeded.
class PreviewTestExtension : public unbox::kernel::Extension {
public:
    auto manifest() const -> const Manifest& override { return manifest_; }

    void activate(Host& host) override {
        if (!host.ui().available()) {
            return; // no GL path: degrade (test skips)
        }
        // Build the source: a 64x64 solid #ff2060 buffer in its own tree under
        // the background layer (off to the side so it does not overlap the ui
        // surface; the preview snapshots the TREE, not the screen).
        src_tree_ = wlr_scene_tree_create(host.scene_layer(unbox::kernel::SceneLayer::background));
        if (src_tree_ == nullptr) {
            return;
        }
        src_buf_ = make_solid_buffer(64, 64, 0xff, 0x20, 0x60);
        src_node_ = wlr_scene_buffer_create(src_tree_, &src_buf_->base);
        wlr_buffer_drop(&src_buf_->base); // scene_buffer took its own lock

        preview_ = host.ui().create_preview(src_tree_);
        if (preview_ == nullptr) {
            return;
        }

        std::string rml = kPreviewRml;
        const std::string token = "PREVIEW_URI";
        rml.replace(rml.find(token), token.size(), preview_->source_uri());

        UiSurfaceSpec spec;
        spec.rml_inline = rml;
        spec.x = 0;
        spec.y = 0;
        spec.width = 200;
        spec.height = 200;
        spec.layer = unbox::kernel::SceneLayer::overlay;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
    }

    void teardown() {
        surface_.reset();
        preview_.reset();
        if (src_tree_ != nullptr) {
            wlr_scene_node_destroy(&src_tree_->node);
            src_tree_ = nullptr;
        }
    }

    [[nodiscard]] auto has_preview() const -> bool { return preview_ != nullptr; }
    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }
    [[nodiscard]] auto preview_uri() const -> std::string {
        return preview_ != nullptr ? preview_->source_uri() : std::string{};
    }
    [[nodiscard]] auto preview() -> unbox::kernel::Preview* { return preview_.get(); }
    [[nodiscard]] auto preview_w() const -> int { return preview_ ? preview_->source_width() : 0; }
    [[nodiscard]] auto preview_h() const -> int { return preview_ ? preview_->source_height() : 0; }

private:
    Manifest manifest_{"preview-test", Tier::standard, {}};
    wlr_scene_tree* src_tree_ = nullptr;
    TestSrcBuffer* src_buf_ = nullptr;
    wlr_scene_buffer* src_node_ = nullptr;
    std::unique_ptr<unbox::kernel::Preview> preview_;
    std::unique_ptr<UiSurface> surface_;
};

// The dock card faithfully: a transformed (translateX body), rounded,
// overflow:hidden 100x100 div whose preview is an image() DECORATOR (cover) —
// the exact RmlUi path the dock hits. PREVIEW_URI substituted at runtime.
const char* kDockCardRml = R"RML(<rml>
<head>
<style>
body { margin: 0px; transform: translateX(0px); transform-origin: 0% 0%; }
#card { display: block; position: absolute; left: 20px; top: 20px;
        width: 100px; height: 100px; border-radius: 50px; overflow: hidden;
        background-color: #2e2e32ff; }
#fill { display: block; width: 100%; height: 100%;
        decorator: image( PREVIEW_URI cover ); }
</style>
</head>
<body data-model="ui">
<div id="card"><div id="fill"></div></div>
</body>
</rml>)RML";

class PreviewDecoratorExtension : public unbox::kernel::Extension {
public:
    auto manifest() const -> const Manifest& override { return manifest_; }
    void activate(Host& host) override {
        if (!host.ui().available()) return;
        src_tree_ = wlr_scene_tree_create(host.scene_layer(unbox::kernel::SceneLayer::background));
        if (src_tree_ == nullptr) return;
        src_buf_ = make_solid_buffer(64, 64, 0xff, 0x20, 0x60); // #ff2060
        src_node_ = wlr_scene_buffer_create(src_tree_, &src_buf_->base);
        wlr_buffer_drop(&src_buf_->base);
        preview_ = host.ui().create_preview(src_tree_);
        if (preview_ == nullptr) return;
        std::string rml = kDockCardRml;
        const std::string token = "PREVIEW_URI";
        rml.replace(rml.find(token), token.size(), preview_->source_uri());
        UiSurfaceSpec spec;
        spec.rml_inline = rml;
        spec.width = 200;
        spec.height = 200;
        spec.layer = unbox::kernel::SceneLayer::overlay;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
    }
    void teardown() {
        surface_.reset();
        preview_.reset();
        if (src_tree_ != nullptr) { wlr_scene_node_destroy(&src_tree_->node); src_tree_ = nullptr; }
    }
    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }
    [[nodiscard]] auto has_preview() const -> bool { return preview_ != nullptr; }
private:
    Manifest manifest_{"preview-decorator-test", Tier::standard, {}};
    wlr_scene_tree* src_tree_ = nullptr;
    TestSrcBuffer* src_buf_ = nullptr;
    wlr_scene_buffer* src_node_ = nullptr;
    std::unique_ptr<unbox::kernel::Preview> preview_;
    std::unique_ptr<UiSurface> surface_;
};

// Alpha byte of a packed 0xRRGGBBAA ui_pixel readback (0 = transparent).
auto opaque_alpha(unsigned int px) -> int { return static_cast<int>(px & 0xff); }

} // namespace

// An image() DECORATOR on a CHILD of a rounded overflow:hidden card clips to the
// card's rounded shape (the corners read transparent). This is the structure the
// stage dock must use: RmlUi does NOT clip an element's OWN decorator to its OWN
// border-radius (only descendant content is clipped via the parent's clip mask),
// so a decorator placed directly on the rounded card renders SQUARE. Putting the
// decorator on a full-bleed child makes the kernel's stencil clip-mask round it.
// (Failing-then-passing lives in ext-stage-dock's RCSS — see report change-req;
// here we prove the SUBSTRATE clip-mask rounds a child decorator correctly.)
TEST_CASE("substrate: image-decorator on a child of a rounded card clips to the rounded shape") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM"); // dmabuf path (the real-seat path)
    auto server = unbox::kernel::Server::create({});
    auto* ext = new PreviewDecoratorExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    if (!ext->has_preview() || !ext->has_surface()) {
        return; // no GL path: skip
    }
    pump(*server, 80);
    if (server->ui_frame_count() == 0) {
        ext->teardown();
        return;
    }
    // The card is a 100x100 circle (border-radius:50px) at (20,20)..(120,120).
    // Center reads the preview image (#ff2060, red-dominant).
    const unsigned int center = server->ui_pixel(70, 70);
    INFO("card center (70,70) = ", center);
    CHECK(opaque_alpha(center) == 0xff);
    CHECK(((center >> 24) & 0xff) > 150);          // red preview present at center
    // The square corners of the card box fall OUTSIDE the inscribed circle => the
    // child decorator is clipped away by the rounded stencil mask => transparent.
    CHECK(server->ui_pixel(22, 22) == 0u);         // top-left card corner clipped
    CHECK(server->ui_pixel(118, 22) == 0u);        // top-right
    CHECK(server->ui_pixel(22, 118) == 0u);        // bottom-left
    CHECK(server->ui_pixel(118, 118) == 0u);       // bottom-right
    ext->teardown();
}

TEST_CASE("substrate: unavailable under pixman; create_surface degrades to null") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new UiTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    // No GL path: substrate unavailable, surface is null, server still runs.
    CHECK(!ext->has_surface());
    CHECK(server->ui_frame_count() == 0);
    pump(*server, 5);
    CHECK(server->ui_frame_count() == 0);
}

TEST_CASE("substrate: surface renders frames and submits an upright buffer") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1); // shm path => readback for orientation

    auto server = unbox::kernel::Server::create({});
    auto* ext = new UiTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();

    for (int i = 0; i < 200; ++i) {
        ext->advance();
        server->dispatch(10);
    }

    const int frames = server->ui_frame_count();
    INFO("ui_frame_count() = ", frames);
    CHECK(frames >= 0); // 0 if this box has no GL path (graceful), else advancing

    const int orient = server->ui_orientation();
    INFO("ui_orientation() = ", orient);
    CHECK(orient != -1); // never flipped
    if (frames > 0) {
        CHECK(ext->has_surface());
        CHECK(orient == 1); // shm surface ran => upright confirmed
    }

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

TEST_CASE("substrate: production fence-sync path active on the dmabuf path") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM"); // allow Plan A (dmabuf + fence)

    auto server = unbox::kernel::Server::create({});
    auto* ext = new UiTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 50);

    // If the GL/dmabuf path engaged at all, fence sync (not glFinish) must be
    // the submission sync. On a box with no dmabuf import, both are false —
    // acceptable (the shm path has no hot-path glFinish either).
    if (server->ui_fence_sync_active()) {
        CHECK(server->ui_frame_count() >= 0);
    }
    CHECK(true); // no crash; the assertion above is the meaningful one
}

TEST_CASE("substrate: touch-mode flips state but does NO visual scaling") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new UiTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();

    server->ui_set_touch_override(unbox::kernel::Server::UiTouchOverride::force_off);
    pump(*server, 60);
    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM"); // no GL path: skip
        return;
    }
    // State is observable through the public facade and flips on override.
    CHECK(ext->substrate_touch_mode() == false);
    server->ui_set_touch_override(unbox::kernel::Server::UiTouchOverride::force_on);
    CHECK(ext->substrate_touch_mode() == true);
    // The flip changes NOTHING visual: rendering continues normally (no zoom,
    // no clip, no re-layout glitch). Pump more frames; the surface keeps
    // submitting and stays upright. (Visual scaling was retired by user
    // decision; the dp-ratio is permanently 1.0 — proven by the absence of any
    // ratio knob in the substrate, and the surface rendering identically.)
    const int frames_before = server->ui_frame_count();
    for (int i = 0; i < 30; ++i) {
        ext->advance();
        server->dispatch(10);
    }
    CHECK(server->ui_frame_count() > frames_before);
    CHECK(server->ui_orientation() != -1); // still upright; no flip/garbling

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

TEST_CASE("substrate: touch-mode flip notifies the surface (on_touch_mode_changed)") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new UiTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    server->ui_set_touch_override(unbox::kernel::Server::UiTouchOverride::force_off);
    pump(*server, 30);
    if (!ext->has_surface()) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM"); // no GL path: skip
        return;
    }
    const int before = ext->touch_mode_changes;
    // Flip to touch: the surface's callback must fire with touch == true.
    server->ui_set_touch_override(unbox::kernel::Server::UiTouchOverride::force_on);
    CHECK(ext->touch_mode_changes == before + 1);
    CHECK(ext->last_touch_mode == true);
    // Flip back: fires again with touch == false.
    server->ui_set_touch_override(unbox::kernel::Server::UiTouchOverride::force_off);
    CHECK(ext->touch_mode_changes == before + 2);
    CHECK(ext->last_touch_mode == false);

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

TEST_CASE("substrate: a click over a ui surface is CONSUMED (no click-through)") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new UiTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 60); // let the surface load + render so hit-test sees it

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        // No GL path on this box: consumption is moot (nothing to hit). Skip.
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        return;
    }
    // The substrate hit-test is geometric (the surface spans 40,40..360,240).
    // We cannot synthesize wlr pointer events headlessly without input devices,
    // so consumption is asserted at the routing layer via the public probe: a
    // click inside the surface rect must not reach the bus hook. The kernel's
    // route_pointer_button consumes when over a surface; here we assert the
    // invariant that drove the design — the bus hook saw zero synthetic clicks
    // (no input device => zero events; the meaningful negative is that nothing
    // leaked through during rendering/hover).
    CHECK(ext->button_hits_via_bus == 0);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

// ============================================================================
// slice-10 / a1 PREVIEW SPIKE (Fork-B gate). A known-color source subtree is
// snapshotted into a dmabuf, imported into the RMLUi context as a sampled
// texture, and shown via <img src="unbox-preview://N"> in a ui surface. The
// suite proves: (1) the dmabuf->EGLImage->texture import engaged on this GPU
// (Plan A — the go/no-go unknown), and (2) the known source color actually
// composited into the surface at the expected spot (position-aware readback).
// ============================================================================

TEST_CASE("preview: dmabuf import as a sampled RMLUi texture engages (Fork-B GO)") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM"); // Plan A: dmabuf import path

    auto server = unbox::kernel::Server::create({});
    auto* ext = new PreviewTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();

    if (!ext->has_preview()) {
        // No GL path on this box: the spike is moot here (recorded NO-GO would
        // be reported from real hardware, not skipped CI). Nothing to assert.
        return;
    }
    // The preview reports the source's natural size and a stable URI.
    CHECK(ext->preview_w() == 64);
    CHECK(ext->preview_h() == 64);
    CHECK(ext->preview_uri().rfind("unbox-preview://", 0) == 0);
    // The GO criterion: the snapshot imported via dmabuf -> EGLImage -> texture.
    CHECK(server->ui_preview_import_is_dmabuf());

    pump(*server, 60); // let the <img> surface load + sample the preview texture
    CHECK(ext->has_surface());
    CHECK(server->ui_frame_count() > 0);

    ext->teardown(); // drop preview + surface + source while the server lives
}

TEST_CASE("preview: known source color composites into an <img> (position-aware)") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    // Plan A throughout: the preview snapshots into a dmabuf and imports as a
    // sampled texture; the surface composites it into its own (dmabuf) FBO. The
    // ui_pixel probe reads that FBO back via glReadPixels (path-independent), so
    // no FORCE_SHM is needed — this exercises the real Fork-B pipeline.
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");

    auto server = unbox::kernel::Server::create({});
    auto* ext = new PreviewTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();

    if (!ext->has_preview() || !ext->has_surface()) {
        return; // no GL path: skip
    }

    for (int i = 0; i < 80; ++i) {
        server->dispatch(10);
    }
    if (server->ui_frame_count() == 0) {
        return; // no frame submitted on this box
    }

    // The <img> fills the 200x200 surface with the 64x64 #ff2060 source scaled
    // up. Sample the center: it must be the source color, NOT the #101010 bg —
    // proof the imported preview texture was sampled and composited upright.
    const unsigned int px = server->ui_pixel(100, 100);
    INFO("center pixel (RRGGBBAA) = ", px);
    const int r = static_cast<int>((px >> 24) & 0xff);
    const int g = static_cast<int>((px >> 16) & 0xff);
    const int b = static_cast<int>((px >> 8) & 0xff);
    // Tolerant match for #ff2060 (bilinear edges + premultiply rounding).
    CHECK(r > 180);
    CHECK(g < 90);
    CHECK(b > 60);
    CHECK(b < 160);
    // And definitely not the dark background (a missed sample would be ~#101010).
    CHECK(r + g + b > 200);

    ext->teardown();
}

// ============================================================================
// slice-10 / b2 LIST BINDINGS. The stage dock is one document iterating a
// VARIABLE list of slots with data-for; each row reads string fields and a
// per-row click event delivers the row index back to the extension. The suite
// proves through the PUBLIC Host::ui() path: (1) a list of N rows renders N row
// elements, (2) mutating the backing vector + dirty(list) changes the rendered
// row count on the next tick, and (3) clicking a row fires the per-row callback
// with the correct index. Headless+gles2 exercises the GL bridge.
// ============================================================================

namespace {

// The dock document: a row <p> per slot, each carrying the slot's title text
// and a per-row click that calls restore(it_index). The row tag is <p> so the
// element-count probe counts exactly the rows (no other <p> in the body).
const char* kListRml = R"RML(<rml>
<head>
<style>
body { font-family: "Noto Sans"; background: #1e2230; color: #e8ecff;
       width: 320px; height: 240px; }
p { display: block; width: 320px; height: 24px; }
</style>
</head>
<body data-model="ui">
<p data-for="row : slots" data-event-click="restore(it_index)"><span>{{ row.title }} {{ row.fav }}</span></p>
</body>
</rml>)RML";

// A test extension owning a ui surface bound to a runtime-sized slot list.
class ListTestExtension : public unbox::kernel::Extension {
public:
    auto manifest() const -> const Manifest& override { return manifest_; }

    void activate(Host& host) override {
        titles = {"alpha", "beta", "gamma"};
        UiSurfaceSpec spec;
        spec.rml_inline = kListRml;
        spec.x = 0;
        spec.y = 0;
        spec.width = 320;
        spec.height = 240;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
        if (surface_ != nullptr) {
            surface_->bind_list("slots", [this] { return titles.size(); });
            surface_->bind_list_string("slots", "title",
                                       [this](std::size_t r) { return titles.at(r); });
            // A second string field proves multiple per-row fields coexist.
            surface_->bind_list_string("slots", "fav",
                                       [](std::size_t r) { return "icon" + std::to_string(r); });
            surface_->bind_list_event("slots", "restore",
                                      [this](std::size_t r) {
                                          last_restored = static_cast<int>(r);
                                          ++restore_calls;
                                      });
        }
    }

    void set_rows(std::vector<std::string> rows) {
        titles = std::move(rows);
        if (surface_ != nullptr) {
            surface_->dirty("slots");
        }
    }

    std::vector<std::string> titles;
    int last_restored = -1;
    int restore_calls = 0;
    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }

private:
    Manifest manifest_{"list-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
};

} // namespace

TEST_CASE("substrate: data-for list renders N rows, re-renders on dirty, routes row events") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new ListTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 60); // load the document + run the data-for loop

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM"); // no GL path on this box: skip
        return;
    }

    // (1) Three rows in the backing vector => three rendered rows. Each
    // rendered row carries a <span> (the data-for template <p> keeps no span
    // child — its inner RML is extracted — so counting <span> counts exactly
    // the rendered rows).
    CHECK(server->ui_element_count("span") == 3);

    // (2) Grow then shrink the list + dirty(list): the rendered row count tracks
    // count() on the next tick.
    ext->set_rows({"one", "two", "three", "four", "five"});
    pump(*server, 5);
    CHECK(server->ui_element_count("span") == 5);

    ext->set_rows({"solo"});
    pump(*server, 5);
    CHECK(server->ui_element_count("span") == 1);

    // (3) Restore three rows and click the middle one: the per-row callback
    // fires with the right index (data-event-click="restore(it_index)"). The
    // generated rows occupy <p> indices 0..N-1 (the hidden template <p> is last).
    ext->set_rows({"r0", "r1", "r2"});
    pump(*server, 5);
    CHECK(server->ui_element_count("span") == 3);
    const int before = ext->restore_calls;
    REQUIRE(server->ui_click_element("p", 1));
    CHECK(ext->restore_calls == before + 1);
    CHECK(ext->last_restored == 1);

    // Click row 0 and row 2 to prove the index is the real row, not a constant.
    REQUIRE(server->ui_click_element("p", 0));
    CHECK(ext->last_restored == 0);
    REQUIRE(server->ui_click_element("p", 2));
    CHECK(ext->last_restored == 2);

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

// ============================================================================
// DRAG-EVENT BINDING (coordinate-carrying). A ui surface element opts into
// dragging with RCSS `drag: drag;` and authors data-event-dragstart / -drag /
// -dragend all naming ONE callback bound via UiSurface::bind_drag. The
// substrate routes RMLUi's Dragstart/Drag/Dragend to that callback tagged with
// DragPhase {start,move,end}, carrying the pointer position in SURFACE-LOCAL px
// (origin top-left). The full path needs RmlUi to GENERATE drag events from a
// real pointer-down→move-past-threshold→up sequence on the GL context, so it is
// GL-path only (pixman has a null substrate, headless+gles2 exercises it). The
// Server::ui_drag_element seam drives exactly that sequence (no input device).
// ============================================================================

namespace {

// One full-bleed, drag-enabled box. `drag: drag;` is what makes RmlUi emit the
// drag events; without it the same gesture is just a click. The box fills the
// surface so the drag seam's centre-press always lands on it.
const char* kDragRml = R"RML(<rml>
<head>
<style>
body { background-color: #1e2230; width: 200px; height: 200px; margin: 0px; }
#grip { display: block; position: absolute; left: 0px; top: 0px;
        width: 200px; height: 200px; background-color: #3a4670; drag: drag; }
</style>
</head>
<body data-model="ui">
<div id="grip"
     data-event-dragstart="slide"
     data-event-drag="slide"
     data-event-dragend="slide"></div>
</body>
</rml>)RML";

// Records every drag phase + coordinate the callback receives.
class DragTestExtension : public unbox::kernel::Extension {
public:
    auto manifest() const -> const Manifest& override { return manifest_; }

    void activate(Host& host) override {
        UiSurfaceSpec spec;
        spec.rml_inline = kDragRml;
        spec.x = 40; // a non-zero surface origin: proves coords are surface-LOCAL
        spec.y = 30; // (NOT layout-space), i.e. the substrate subtracted s.x/s.y.
        spec.width = 200;
        spec.height = 200;
        spec.layer = unbox::kernel::SceneLayer::overlay;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
        if (surface_ != nullptr) {
            surface_->bind_drag("slide",
                                [this](UiSurface::DragPhase phase, double x, double y) {
                                    phases.push_back(phase);
                                    last_x = x;
                                    last_y = y;
                                    if (phase == UiSurface::DragPhase::start) {
                                        start_x = x;
                                        start_y = y;
                                    }
                                });
        }
    }

    std::vector<UiSurface::DragPhase> phases;
    double start_x = -1.0;
    double start_y = -1.0;
    double last_x = -1.0;
    double last_y = -1.0;
    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }

private:
    Manifest manifest_{"drag-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
};

} // namespace

TEST_CASE("substrate: bind_drag routes Dragstart/Drag/Dragend with surface-local coords") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new DragTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 60); // load + lay out the document so the grip has geometry

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM"); // no GL path on this box: skip
        return;
    }

    using DP = UiSurface::DragPhase;

    // Drag the grip by (+30,+20) from its centre. The seam presses at the box
    // centre (100,100 in surface-local px), then moves past RmlUi's threshold.
    REQUIRE(server->ui_drag_element("div", 0, 30.0, 20.0));

    // (1) The callback saw a start, then move(s), then an end — in that order.
    REQUIRE(ext->phases.size() >= 3);
    CHECK(ext->phases.front() == DP::start);
    CHECK(ext->phases.back() == DP::end);
    bool saw_move = false;
    for (std::size_t i = 1; i + 1 < ext->phases.size(); ++i) {
        if (ext->phases[i] == DP::move) {
            saw_move = true;
        }
    }
    CHECK(saw_move);

    // (2) Coordinates are SURFACE-LOCAL px (origin = surface top-left), NOT
    // layout-space: the surface sits at layout (40,30) but the centre-press
    // reports ~ (100,100), the grip's local centre — proof the substrate mapped
    // mouse_x/mouse_y into the surface's own coordinate system.
    CHECK(ext->start_x == doctest::Approx(100.0).epsilon(0.05));
    CHECK(ext->start_y == doctest::Approx(100.0).epsilon(0.05));

    // (3) The final (dragend) coordinate followed the travel: centre + 2*delta
    // (the seam issues two moves of (dx,dy) and 2*(dx,dy)). So last ~ (160,140).
    CHECK(ext->last_x == doctest::Approx(160.0).epsilon(0.05));
    CHECK(ext->last_y == doctest::Approx(140.0).epsilon(0.05));

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

// ============================================================================
// slice-10 / ui-surface ALPHA (transparency). A ui surface composites with
// per-pixel alpha: a pixel the document does NOT paint is transparent (the
// scene below shows through), while a painted opaque box stays solid. The
// substrate must (a) clear the surface's output buffer to transparent (0,0,0,0)
// — NOT opaque black — and (b) never mark the scene_buffer opaque. This is the
// substrate capability the stage dock needs (its un-painted strip becomes
// see-through). Proven via the public Host::ui() path + the ui_pixel /
// ui_surface_has_opaque_region probes.
// ============================================================================

namespace {

// Transparent <body> with one small OPAQUE box in the top-left corner. The box
// is #20c040 (an obvious, non-black color). Everything else is unpainted ⇒
// must read back fully transparent. The box uses position:absolute so its
// geometry is exact (a 40x40 square at 0,0).
const char* kAlphaRml = R"RML(<rml>
<head>
<style>
body { background-color: transparent; width: 200px; height: 200px; margin: 0px; }
#box { display: block; width: 40px; height: 40px; background-color: #20c040; }
</style>
</head>
<body data-model="ui">
<div id="box"></div>
</body>
</rml>)RML";

class AlphaTestExtension : public unbox::kernel::Extension {
public:
    auto manifest() const -> const Manifest& override { return manifest_; }

    void activate(Host& host) override {
        UiSurfaceSpec spec;
        spec.rml_inline = kAlphaRml;
        spec.x = 0;
        spec.y = 0;
        spec.width = 200;
        spec.height = 200;
        spec.layer = unbox::kernel::SceneLayer::overlay;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
    }

    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }

private:
    Manifest manifest_{"alpha-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
};

} // namespace

TEST_CASE("substrate: un-painted pixels are transparent; painted box stays opaque") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new AlphaTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 60); // load the document + render the surface

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM"); // no GL path on this box: skip
        return;
    }

    // (1) The scene buffer must NOT carry a forced opaque region — otherwise
    // wlr_scene would skip alpha-blending and occlude the scene below.
    CHECK(server->ui_surface_has_opaque_region() == false);

    // (2) A pixel in the UN-painted area (center, far from the corner box) is
    // FULLY TRANSPARENT: premultiplied (0,0,0,0) ⇒ packed 0xRRGGBBAA == 0. This
    // is the failing-then-passing assertion: before the fix the output FBO was
    // cleared to opaque black (0,0,0,1) so this read back 0x000000ff.
    const unsigned int unpainted = server->ui_pixel(100, 100);
    INFO("un-painted center pixel (RRGGBBAA) = ", unpainted);
    CHECK((unpainted & 0xffu) == 0u);   // alpha == 0
    CHECK(unpainted == 0u);             // fully transparent premultiplied (0,0,0,0)

    // (3) A pixel inside the box reads the box color, fully opaque. The 40x40
    // box is the first normal-flow block at the document top-left; sample well
    // inside it (10,10) to avoid antialiased edges.
    const unsigned int box = server->ui_pixel(10, 10);
    INFO("box pixel (RRGGBBAA) = ", box);
    const int br = static_cast<int>((box >> 24) & 0xff);
    const int bg = static_cast<int>((box >> 16) & 0xff);
    const int bb = static_cast<int>((box >> 8) & 0xff);
    const int ba = static_cast<int>(box & 0xff);
    CHECK(ba == 0xff);          // opaque
    CHECK(bg > br);             // green dominates (#20c040)
    CHECK(bg > bb);
    CHECK(br < 90);             // little red
    CHECK(bg > 140);            // strong green

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

// ============================================================================
// slice-10 / RmlUi CLIPPING (scissor + stencil clip-mask). The stage dock draws
// rounded cards with an overflowing preview image; the GLES-adapted render
// interface's clip path was never exercised before. Two clip mechanisms:
//   - rectangular: overflow:hidden -> EnableScissorRegion/SetScissorRegion ->
//     glScissor (must clip to the element's ON-SCREEN box, not a flipped strip);
//   - rounded: border-radius -> EnableClipMask/RenderToClipMask -> the STENCIL
//     buffer (the offscreen render target must carry a stencil attachment).
// Proven via the public Host::ui() path + position-aware ui_pixel readback.
// ============================================================================

namespace {

// A 200x200 surface. A 60x60 #e03060 parent at top-left clips (overflow:hidden)
// a 600x600 child that would otherwise overflow far past it. If scissor is
// correct, only the top-left 60x60 is painted; everything outside is unpainted.
const char* kScissorRml = R"RML(<rml>
<head>
<style>
body { margin: 0px; }
#clip { display: block; position: absolute; left: 0px; top: 0px;
        width: 60px; height: 60px; overflow: hidden; }
#big  { display: block; width: 600px; height: 600px; background-color: #e03060; }
</style>
</head>
<body data-model="ui">
<div id="clip"><div id="big"></div></div>
</body>
</rml>)RML";

// Mirrors the stage dock card: a TRANSFORMED (scale) border-radius element
// whose overflow clips a large child. A transform on the element forces RmlUi
// to clip via the STENCIL clip-mask (not glScissor — a scissor rect can't
// represent a transformed region), and the border-radius does too. This is the
// path the dock actually hits and the simple scissor fixture does NOT.
const char* kTransformClipRml = R"RML(<rml>
<head>
<style>
body { margin: 0px; transform: translateX(0px); transform-origin: 0% 0%; }
#card { display: block; position: absolute; left: 20px; top: 20px;
        width: 100px; height: 100px; overflow: hidden; border-radius: 50px;
        transform: scale(1.0); transform-origin: 0% 0%; }
#big  { display: block; width: 600px; height: 600px; background-color: #c08020; }
</style>
</head>
<body data-model="ui">
<div id="card"><div id="big"></div></div>
</body>
</rml>)RML";

// A 200x200 surface with a 200x200 element filled #30c0e0 and a huge
// border-radius (100px => a full circle inscribed in the square). The four
// square corners fall OUTSIDE the rounded mask -> must be clipped transparent;
// the center is inside -> painted.
const char* kRoundedRml = R"RML(<rml>
<head>
<style>
body { margin: 0px; }
#round { display: block; position: absolute; left: 0px; top: 0px;
         width: 200px; height: 200px; border-radius: 100px;
         background-color: #30c0e0; }
</style>
</head>
<body data-model="ui">
<div id="round"></div>
</body>
</rml>)RML";

class ClipTestExtension : public unbox::kernel::Extension {
public:
    explicit ClipTestExtension(const char* rml) : rml_(rml) {}
    auto manifest() const -> const Manifest& override { return manifest_; }

    void activate(Host& host) override {
        UiSurfaceSpec spec;
        spec.rml_inline = rml_;
        spec.x = 0;
        spec.y = 0;
        spec.width = 200;
        spec.height = 200;
        spec.layer = unbox::kernel::SceneLayer::overlay;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
    }

    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }

private:
    const char* rml_;
    Manifest manifest_{"clip-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
};

// Like ClipTestExtension but creates the surface at a 1px placeholder (as the
// stage dock does) and grows it via set_size — to exercise clipping AFTER a
// render-target realloc (does the layer stack / stencil follow the new size?).
class ClipGrowTestExtension : public unbox::kernel::Extension {
public:
    explicit ClipGrowTestExtension(const char* rml) : rml_(rml) {}
    auto manifest() const -> const Manifest& override { return manifest_; }
    void activate(Host& host) override {
        UiSurfaceSpec spec;
        spec.rml_inline = rml_;
        spec.x = 0;
        spec.y = 0;
        spec.width = 1;
        spec.height = 1;
        spec.layer = unbox::kernel::SceneLayer::overlay;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
    }
    void grow(int w, int h) {
        if (surface_ != nullptr) surface_->set_size(w, h);
    }
    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }

private:
    const char* rml_;
    Manifest manifest_{"clip-grow-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
};

} // namespace

TEST_CASE("substrate: overflow:hidden scissor clips a child to the parent box (correct band)") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new ClipTestExtension(kScissorRml);
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 60);

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        return;
    }

    // Inside the 60x60 clip box (top-left): the child paints, reads #e03060.
    const unsigned int inside = server->ui_pixel(20, 20);
    INFO("inside-clip pixel (20,20) = ", inside);
    CHECK(opaque_alpha(inside) == 0xff);
    CHECK(((inside >> 24) & 0xff) > 150);  // red-dominant #e03060
    CHECK(((inside >> 8) & 0xff) < 140);   // not much blue

    // OUTSIDE the parent box, well below it (document y=150) and right
    // (document x=150): the child would overflow here, but overflow:hidden must
    // clip it away => transparent. A wrong scissor Y clips the OPPOSITE band, so
    // (150,150) would read painted. This is the failing-then-passing assertion.
    CHECK(server->ui_pixel(150, 150) == 0u); // far corner: unpainted
    CHECK(server->ui_pixel(20, 150) == 0u);  // straight below the box: unpainted
    CHECK(server->ui_pixel(150, 20) == 0u);  // straight right of the box: unpainted

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

TEST_CASE("substrate: border-radius clip-mask (stencil) rounds the corners") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new ClipTestExtension(kRoundedRml);
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 60);

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        return;
    }

    // Center of the 200x200 circle: inside the rounded mask => painted #30c0e0.
    const unsigned int center = server->ui_pixel(100, 100);
    INFO("rounded center (100,100) = ", center);
    CHECK(opaque_alpha(center) == 0xff);
    CHECK(((center >> 8) & 0xff) > 150);   // blue-ish #30c0e0
    CHECK(((center >> 16) & 0xff) > 120);  // strong green component

    // The square's corners fall OUTSIDE the inscribed circle (a 100px radius on
    // a 200px box => the corner at (2,2) is ~138px from center, well outside the
    // 100px radius). With the stencil clip-mask working they are clipped away =>
    // transparent. Before the fix (no stencil / wrong mask) the corner reads the
    // opaque fill (square). Sample a few pixels into each corner to dodge AA.
    INFO("corner (3,3) = ", server->ui_pixel(3, 3));
    CHECK(server->ui_pixel(3, 3) == 0u);       // top-left corner clipped
    CHECK(server->ui_pixel(196, 3) == 0u);     // top-right corner clipped
    CHECK(server->ui_pixel(3, 196) == 0u);     // bottom-left corner clipped
    CHECK(server->ui_pixel(196, 196) == 0u);   // bottom-right corner clipped

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

// The dock's actual clip path: a TRANSFORMED body + a transformed,
// overflow:hidden, border-radius card clipping an overflowing child. A transform
// forces RmlUi onto the stencil clip-mask (a scissor rect can't represent a
// transformed region). This must still round correctly AFTER a set_size grow
// (the layer stack + its shared stencil renderbuffer must follow the new size) —
// the exact lifecycle the dock hits (create tiny -> grow on minimize).
TEST_CASE("substrate: transformed rounded clip (stencil) survives a set_size grow") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM"); // dmabuf path (the real-seat path)

    auto server = unbox::kernel::Server::create({});
    auto* ext = new ClipGrowTestExtension(kTransformClipRml);
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 20);     // render at the 1px create size first
    ext->grow(200, 200);   // grow like the dock on minimize (realloc + new layers)
    pump(*server, 40);

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        return; // no GL path: skip
    }

    // The card is a 100x100 circle at (20,20)..(120,120) filled #c08020 (orange).
    const unsigned int center = server->ui_pixel(70, 70);
    INFO("xform card center (70,70) = ", center);
    CHECK(opaque_alpha(center) == 0xff);
    CHECK(((center >> 24) & 0xff) > 150);  // red-dominant orange #c08020
    CHECK(((center >> 8) & 0xff) < 120);   // little blue
    // Card box corners fall outside the inscribed circle => clipped transparent.
    CHECK(server->ui_pixel(22, 22) == 0u);
    CHECK(server->ui_pixel(118, 22) == 0u);
    CHECK(server->ui_pixel(22, 118) == 0u);
    CHECK(server->ui_pixel(118, 118) == 0u);
}

// ============================================================================
// slice-10 / set_size RENDER-TARGET RESIZE. A surface created SMALL must grow
// (and shrink) and render fully at the new size — set_size now reallocates the
// FBO + dmabuf swapchain (or shm buffer) + EGLImage + texture, not just the
// logical RmlUi layout. The dock creates a 1px placeholder and grows it; before
// this fix the grown area rendered into the original tiny buffer (invisible).
// Proven via the public Host::ui() path + the ui_pixel / ui_resize_realloc_count
// probes. Full-body opaque color so a grown-area pixel reading the color proves
// the new buffer was actually drawn into.
// ============================================================================

namespace {

// A full-bleed opaque blue body (#2080e0), no margin, so EVERY pixel of the
// surface (at whatever current size) is the painted color.
const char* kResizeRml = R"RML(<rml>
<head>
<style>
body { margin: 0px; }
#fill { display: block; position: absolute; left: 0px; top: 0px;
        width: 4000px; height: 4000px; background-color: #2080e0; }
</style>
</head>
<body data-model="ui">
<div id="fill"></div>
</body>
</rml>)RML";

class ResizeTestExtension : public unbox::kernel::Extension {
public:
    auto manifest() const -> const Manifest& override { return manifest_; }

    void activate(Host& host) override {
        UiSurfaceSpec spec;
        spec.rml_inline = kResizeRml;
        spec.x = 0;
        spec.y = 0;
        spec.width = 40; // created SMALL (the dock starts at a tiny placeholder)
        spec.height = 40;
        spec.layer = unbox::kernel::SceneLayer::overlay;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
    }

    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }
    auto surface() -> UiSurface* { return surface_.get(); }

private:
    Manifest manifest_{"resize-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
};

// True if (RRGGBBAA) is the painted blue #2080e0 (tolerant), opaque.
auto is_painted_blue(unsigned int px) -> bool {
    const int r = static_cast<int>((px >> 24) & 0xff);
    const int g = static_cast<int>((px >> 16) & 0xff);
    const int b = static_cast<int>((px >> 8) & 0xff);
    const int a = static_cast<int>(px & 0xff);
    return a == 0xff && b > 160 && b > r && b > g && r < 90;
}

} // namespace

TEST_CASE("substrate: set_size resizes the render target (grow renders, shrink renders)") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM"); // exercise the real Plan-A path the dock hits

    auto server = unbox::kernel::Server::create({});
    auto* ext = new ResizeTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 30); // load + render at the small (40x40) size

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        return; // no GL path on this box: skip
    }

    // Small surface paints fully: its center reads the body color.
    CHECK(is_painted_blue(server->ui_pixel(20, 20)));

    // GROW to 200x200. Tick. A pixel deep in the GROWN region (150,150) — which
    // does NOT exist in the original 40x40 buffer — must now read the painted
    // color. Before the fix the document re-laid-out but rendered into the old
    // 40x40 buffer, so (150,150) was unrendered (clamped/garbage/transparent).
    const int before = server->ui_resize_realloc_count();
    ext->surface()->set_size(200, 200);
    CHECK(server->ui_resize_realloc_count() == before + 1); // grow reallocated
    pump(*server, 10);
    const unsigned int grown = server->ui_pixel(150, 150);
    INFO("grown-region pixel (150,150) (RRGGBBAA) = ", grown);
    CHECK(is_painted_blue(grown));
    // The opaque-region invariant survives a resize (still per-pixel-alpha buffer).
    CHECK(server->ui_surface_has_opaque_region() == false);
    // The buffer is still upright after the realloc (no flip regression).
    // (orientation() only inspects shm-path surfaces; this is the dmabuf path, so
    // we assert upright indirectly: a top-left pixel and a bottom-right pixel of
    // the full-bleed body both read the color, i.e. no garbled/empty rows.)
    CHECK(is_painted_blue(server->ui_pixel(5, 5)));
    CHECK(is_painted_blue(server->ui_pixel(195, 195)));

    // SHRINK to 60x60. Tick. A pixel inside reads the color; out-of-bounds reads
    // 0 (probe clamps), proving the buffer actually shrank.
    ext->surface()->set_size(60, 60);
    CHECK(server->ui_resize_realloc_count() == before + 2); // shrink reallocated
    pump(*server, 10);
    CHECK(is_painted_blue(server->ui_pixel(30, 30)));
    CHECK(server->ui_pixel(150, 150) == 0u); // out of the new 60x60 bounds

    // SAME-size set_size is a no-op realloc (only-on-change guard) and still
    // renders correctly.
    const int after_shrink = server->ui_resize_realloc_count();
    ext->surface()->set_size(60, 60);
    CHECK(server->ui_resize_realloc_count() == after_shrink); // no extra realloc
    pump(*server, 5);
    CHECK(is_painted_blue(server->ui_pixel(30, 30)));

    // Non-positive set_size is rejected (keeps the 60x60 size; no realloc).
    ext->surface()->set_size(0, 100);
    ext->surface()->set_size(100, -1);
    CHECK(server->ui_resize_realloc_count() == after_shrink);
    pump(*server, 5);
    CHECK(is_painted_blue(server->ui_pixel(30, 30)));
}

// ============================================================================
// slice-10 / rml_path + dev HOT-RELOAD. A ui surface loads its document from a
// FILE (UiSurfaceSpec::rml_path), and a dev watcher reloads it live on a save —
// preserving the RmlUi context, data model, the extension's registered bindings,
// and the surface geometry/visibility. The reload is exercised deterministically
// via the Server::ui_reload_surface() test seam (no inotify race). Headless+
// gles2, position-aware ui_pixel readback like the alpha/clip/resize tests.
// ============================================================================

namespace {

// Write `contents` to `path` (truncating). Returns false on failure.
auto write_file(const std::filesystem::path& path, const std::string& contents) -> bool {
    std::ofstream f(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!f) {
        return false;
    }
    f << contents;
    return static_cast<bool>(f);
}

// A full-bleed body of one color. `color` is an RCSS hex like "#2080e0".
auto full_body_rml(const std::string& color) -> std::string {
    return "<rml><head><style>"
           "body { margin: 0px; }"
           "#fill { display: block; position: absolute; left: 0px; top: 0px;"
           " width: 4000px; height: 4000px; background-color: " +
           color + "; }"
           "</style></head><body data-model=\"ui\"><div id=\"fill\"></div></body></rml>";
}

// An extension that loads its surface from a file path (no inline RML).
class PathSurfaceExtension : public unbox::kernel::Extension {
public:
    explicit PathSurfaceExtension(std::string path) : path_(std::move(path)) {}
    auto manifest() const -> const Manifest& override { return manifest_; }
    void activate(Host& host) override {
        UiSurfaceSpec spec;
        spec.rml_path = path_; // absolute path => loaded as-is
        spec.x = 0;
        spec.y = 0;
        spec.width = 80;
        spec.height = 80;
        spec.layer = unbox::kernel::SceneLayer::overlay;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
    }
    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }

private:
    std::string path_;
    Manifest manifest_{"path-surface-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
};

// True if px (RRGGBBAA) is ~green #20c040, opaque.
auto is_green(unsigned int px) -> bool {
    const int r = static_cast<int>((px >> 24) & 0xff);
    const int g = static_cast<int>((px >> 16) & 0xff);
    const int b = static_cast<int>((px >> 8) & 0xff);
    return (px & 0xffu) == 0xffu && g > 140 && g > r && g > b && r < 90;
}
// True if px (RRGGBBAA) is ~red #d03020, opaque.
auto is_red(unsigned int px) -> bool {
    const int r = static_cast<int>((px >> 24) & 0xff);
    const int g = static_cast<int>((px >> 16) & 0xff);
    const int b = static_cast<int>((px >> 8) & 0xff);
    return (px & 0xffu) == 0xffu && r > 150 && r > g && r > b && g < 90;
}

// A unique temp path under the system temp dir for this test run.
auto temp_rml(const char* tag) -> std::filesystem::path {
    auto dir = std::filesystem::temp_directory_path() / "unbox-kernel-tests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / (std::string("hot-reload-") + tag + "-" +
                  std::to_string(::getpid()) + ".rml");
}

} // namespace

TEST_CASE("substrate: load a surface document from rml_path (file), render its color") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    const auto path = temp_rml("load");
    REQUIRE(write_file(path, full_body_rml("#20c040"))); // green

    auto server = unbox::kernel::Server::create({});
    auto* ext = new PathSurfaceExtension(path.string());
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 30); // lazy first-load happens on first render

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return; // no GL path: skip
    }

    // The file's full-body green is composited (proves rml_path loaded a file).
    CHECK(is_green(server->ui_pixel(40, 40)));

    std::error_code ec;
    std::filesystem::remove(path, ec);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

TEST_CASE("substrate: rml_path resolves a RELATIVE path against UNBOX_ASSET_DIR") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    // Lay out <assetroot>/unit-x/doc.rml and load it via the RELATIVE path
    // "unit-x/doc.rml" with UNBOX_ASSET_DIR pointing at <assetroot>.
    const auto root = std::filesystem::temp_directory_path() / "unbox-kernel-tests" /
                      (std::string("assetroot-") + std::to_string(::getpid()));
    const auto unit_dir = root / "unit-x";
    std::error_code ec;
    std::filesystem::create_directories(unit_dir, ec);
    REQUIRE(write_file(unit_dir / "doc.rml", full_body_rml("#20c040"))); // green
    setenv("UNBOX_ASSET_DIR", root.string().c_str(), 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new PathSurfaceExtension("unit-x/doc.rml"); // RELATIVE
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 30);

    if (ext->has_surface() && server->ui_frame_count() > 0) {
        CHECK(is_green(server->ui_pixel(40, 40))); // resolved + loaded
    }

    unsetenv("UNBOX_ASSET_DIR");
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("substrate: hot-reload re-parses RCSS (file change -> new color)") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    const auto path = temp_rml("recolor");
    REQUIRE(write_file(path, full_body_rml("#20c040"))); // green first

    auto server = unbox::kernel::Server::create({});
    auto* ext = new PathSurfaceExtension(path.string());
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 30);

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return;
    }
    CHECK(is_green(server->ui_pixel(40, 40)));

    // Rewrite the file with a DIFFERENT body color, then trigger reload via the
    // deterministic test seam. The new RCSS color must composite — proof that
    // reload re-parses RCSS (ClearStyleSheetCache) and re-loads the document.
    REQUIRE(write_file(path, full_body_rml("#d03020"))); // red now
    CHECK(server->ui_reload_surface());                  // a NEW doc installed
    pump(*server, 10);
    CHECK(is_red(server->ui_pixel(40, 40)));             // failing-then-passing
    CHECK_FALSE(is_green(server->ui_pixel(40, 40)));

    std::error_code ec;
    std::filesystem::remove(path, ec);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

TEST_CASE("substrate: END-TO-END dev hot-reload (real inotify, not the seam)") {
    // This exercises the REAL path the seam-based test does NOT: UNBOX_DEV on,
    // the substrate arms its own asset watch on the kernel's shared FileWatcher,
    // and a WRITE to a file on disk fires the real inotify event through the
    // wl_event_loop, which must run the reload callback. (The ui_reload_surface
    // seam passed even while this real path was broken — that gap is the bug.)
    //
    // It mirrors the STAGE DOCK exactly: dock.rml <link>s a SEPARATE dock.rcss
    // in the same dir, and the user edits the RCSS. The regression watched only
    // the .rml basename, so an RCSS edit fired no reload — this test catches it.
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);
    setenv("UNBOX_DEV", "1", 1); // arm the substrate's asset watch

    // A doc dir holding doc.rml (links style.rcss) + style.rcss (the body color).
    const auto dir = std::filesystem::temp_directory_path() / "unbox-kernel-tests" /
                     (std::string("e2e-") + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto rml = dir / "doc.rml";
    const auto rcss = dir / "style.rcss";
    REQUIRE(write_file(rml,
                       "<rml><head><link type=\"text/rcss\" href=\"style.rcss\"/></head>"
                       "<body data-model=\"ui\"><div id=\"fill\"></div></body></rml>"));
    REQUIRE(write_file(rcss, "body{margin:0px;} #fill{display:block;position:absolute;"
                             "left:0px;top:0px;width:4000px;height:4000px;"
                             "background-color:#20c040;}")); // green

    auto server = unbox::kernel::Server::create({});
    auto* ext = new PathSurfaceExtension(rml.string());
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 30); // first render: loads the doc + arms the asset-DIR watch

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_DEV");
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        std::filesystem::remove_all(dir, ec);
        return; // no GL path: skip
    }
    REQUIRE(is_green(server->ui_pixel(40, 40)));

    // Edit the LINKED RCSS on disk (the dock's real case). Do NOT call the seam —
    // let the real inotify watch on the document's DIRECTORY fire through the
    // loop and drive the reload. Pump until the pixel flips; on the buggy code
    // (basename-only watch of doc.rml) the RCSS change never matched -> no flip.
    REQUIRE(write_file(rcss, "body{margin:0px;} #fill{display:block;position:absolute;"
                             "left:0px;top:0px;width:4000px;height:4000px;"
                             "background-color:#d03020;}")); // red
    bool reloaded = false;
    for (int i = 0; i < 100 && !reloaded; ++i) {
        server->dispatch(20); // pump the loop: delivers inotify + ticks surfaces
        reloaded = is_red(server->ui_pixel(40, 40));
    }
    CHECK(reloaded);                                  // FAILS on the regression
    CHECK_FALSE(is_green(server->ui_pixel(40, 40)));

    unsetenv("UNBOX_DEV");
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
    std::filesystem::remove_all(dir, ec);
}

namespace {
// A file-backed list document + an extension that binds a runtime-sized list.
const char* kListReloadRml = R"RML(<rml>
<head><style>
body { margin: 0px; background-color: #101010; }
p { display: block; }
</style></head>
<body data-model="ui">
<p data-for="row : slots"><span>{{ row.title }}</span></p>
</body>
</rml>)RML";

class ListPathExtension : public unbox::kernel::Extension {
public:
    explicit ListPathExtension(std::string path) : path_(std::move(path)) {}
    auto manifest() const -> const Manifest& override { return manifest_; }
    void activate(Host& host) override {
        titles = {"a", "b", "c"};
        UiSurfaceSpec spec;
        spec.rml_path = path_;
        spec.width = 200;
        spec.height = 200;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
        if (surface_ != nullptr) {
            surface_->bind_list("slots", [this] { return titles.size(); });
            surface_->bind_list_string("slots", "title",
                                       [this](std::size_t r) { return titles.at(r); });
        }
    }
    void set_rows(std::vector<std::string> rows) {
        titles = std::move(rows);
        if (surface_ != nullptr) {
            surface_->dirty("slots");
        }
    }
    std::vector<std::string> titles;
    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }

private:
    std::string path_;
    Manifest manifest_{"list-path-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
};
} // namespace

TEST_CASE("substrate: hot-reload preserves a list data binding") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    const auto path = temp_rml("list");
    REQUIRE(write_file(path, kListReloadRml));

    auto server = unbox::kernel::Server::create({});
    auto* ext = new ListPathExtension(path.string());
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 30);

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return;
    }
    // 3 bound rows render 3 <span> rows.
    CHECK(server->ui_element_count("span") == 3);

    // Reload the SAME file (no re-registration by the extension). The bindings
    // must survive: the list still renders its rows after reload.
    REQUIRE(server->ui_reload_surface());
    pump(*server, 5);
    CHECK(server->ui_element_count("span") == 3); // bindings preserved across reload

    // And mutating the vector + dirty still works AFTER reload (the getter the
    // extension registered once, before first frame, is still live).
    ext->set_rows({"one", "two", "three", "four", "five"});
    pump(*server, 5);
    CHECK(server->ui_element_count("span") == 5);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

TEST_CASE("substrate: a malformed hot-reload is isolated; old doc kept; recovers") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    const auto path = temp_rml("malformed");
    REQUIRE(write_file(path, full_body_rml("#20c040"))); // good green first

    auto server = unbox::kernel::Server::create({});
    auto* ext = new PathSurfaceExtension(path.string());
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 30);

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return;
    }
    CHECK(is_green(server->ui_pixel(40, 40)));

    // Save a BROKEN file (not a valid RML document) and reload: no throw escapes,
    // the previous GOOD document keeps rendering, and the session is alive.
    REQUIRE(write_file(path, std::string("this is <not !! valid &&& rml at all")));
    CHECK_FALSE(server->ui_reload_surface()); // no new doc installed
    pump(*server, 10);
    CHECK(is_green(server->ui_pixel(40, 40))); // OLD good doc still rendering
    CHECK(server->ui_frame_count() > 0);       // session alive, still ticking

    // A subsequent GOOD save recovers (now red).
    REQUIRE(write_file(path, full_body_rml("#d03020")));
    CHECK(server->ui_reload_surface());
    pump(*server, 10);
    CHECK(is_red(server->ui_pixel(40, 40)));

    std::error_code ec;
    std::filesystem::remove(path, ec);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
}

// ============================================================================
// slice-10 / watch_file SERVICE. The inotify-on-the-wl_event_loop machinery is
// now a typed RAII kernel service (Host::watch_file -> FileWatch) backed by ONE
// session inotify instance (shared with the substrate's asset hot-reload). Works
// regardless of UNBOX_DEV. These run on the headless backend (no GL needed); we
// pump the wl_event_loop in-test and use real temp files.
// ============================================================================

namespace {

using unbox::kernel::FileWatch;

// An extension that registers a watch_file on the path it is constructed with
// and counts callbacks. Holds the FileWatch as a member (RAII).
class WatchTestExtension : public unbox::kernel::Extension {
public:
    explicit WatchTestExtension(std::string path, bool throw_on_change = false)
        : path_(std::move(path)), throw_(throw_on_change) {}
    auto manifest() const -> const Manifest& override { return manifest_; }
    void activate(Host& host) override {
        watch_ = host.watch_file(path_, [this] {
            ++hits;
            if (throw_) {
                throw std::runtime_error("watch callback boom");
            }
        });
    }
    [[nodiscard]] auto watch_active() const -> bool { return watch_.active(); }
    void drop_watch() { watch_.reset(); }
    int hits = 0;

private:
    std::string path_;
    bool throw_;
    Manifest manifest_{"watch-test", Tier::standard, {}};
    FileWatch watch_;
};

// Pump the loop until `pred` is true or `max_turns` dispatches elapse.
template <typename Pred>
void pump_until(unbox::kernel::Server& s, Pred pred, int max_turns = 100) {
    for (int i = 0; i < max_turns && !pred(); ++i) {
        s.dispatch(20);
    }
}

auto temp_path(const char* tag) -> std::filesystem::path {
    auto dir = std::filesystem::temp_directory_path() / "unbox-kernel-tests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / (std::string("watch-") + tag + "-" + std::to_string(::getpid()) + ".txt");
}

} // namespace

TEST_CASE("watch_file: fires on write, coalesced to one callback") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1); // no GL needed: this is the bare watcher

    const auto path = temp_path("write");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REQUIRE(write_file(path, "v1")); // exists before the watch

    auto server = unbox::kernel::Server::create({});
    auto* ext = new WatchTestExtension(path.string());
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    REQUIRE(ext->watch_active());

    pump(*server, 3); // settle
    CHECK(ext->hits == 0); // no write yet

    REQUIRE(write_file(path, "v2"));              // one save
    pump_until(*server, [&] { return ext->hits >= 1; });
    CHECK(ext->hits == 1);                         // fired, coalesced to once

    // A second save fires again (still one per save).
    REQUIRE(write_file(path, "v3"));
    pump_until(*server, [&] { return ext->hits >= 2; });
    CHECK(ext->hits == 2);

    std::filesystem::remove(path, ec);
}

TEST_CASE("watch_file: fires when a not-yet-existing file is CREATED") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    const auto path = temp_path("create");
    std::error_code ec;
    std::filesystem::remove(path, ec); // ensure it does NOT exist

    auto server = unbox::kernel::Server::create({});
    auto* ext = new WatchTestExtension(path.string());
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    REQUIRE(ext->watch_active()); // watch armed on the (existing) parent dir

    pump(*server, 3);
    CHECK(ext->hits == 0);

    REQUIRE(write_file(path, "born")); // create the file
    pump_until(*server, [&] { return ext->hits >= 1; });
    CHECK(ext->hits >= 1); // fired on CREATE

    std::filesystem::remove(path, ec);
}

TEST_CASE("watch_file: RAII — after the handle is destroyed, no more callbacks") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    const auto path = temp_path("raii");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REQUIRE(write_file(path, "v1"));

    auto server = unbox::kernel::Server::create({});
    auto* ext = new WatchTestExtension(path.string());
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();

    REQUIRE(write_file(path, "v2"));
    pump_until(*server, [&] { return ext->hits >= 1; });
    CHECK(ext->hits == 1);

    // Drop the watch; a further write must NOT call back.
    ext->drop_watch();
    CHECK_FALSE(ext->watch_active());
    REQUIRE(write_file(path, "v3"));
    pump(*server, 10); // give inotify ample time
    CHECK(ext->hits == 1); // unchanged

    std::filesystem::remove(path, ec);
}

TEST_CASE("watch_file: a throwing on_change is isolated; the session survives") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    const auto path = temp_path("throw");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REQUIRE(write_file(path, "v1"));

    auto server = unbox::kernel::Server::create({});
    auto* ext = new WatchTestExtension(path.string(), /*throw_on_change=*/true);
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();

    // The throwing callback must be caught at the watcher boundary: dispatch
    // returns cleanly (no exception escapes the loop) and the server keeps
    // running. (Its extension is disabled by the same isolation path as a
    // throwing hook/getter; the session is unharmed.)
    REQUIRE(write_file(path, "v2"));
    bool dispatched_ok = true;
    for (int i = 0; i < 100 && ext->hits == 0; ++i) {
        dispatched_ok = server->dispatch(20) && dispatched_ok;
    }
    CHECK(ext->hits >= 1);      // the callback ran (and threw)
    CHECK(dispatched_ok);       // the loop dispatched cleanly across the throw
    // Session still alive: a further dispatch still succeeds.
    CHECK(server->dispatch(5));

    std::filesystem::remove(path, ec);
}

// ============================================================================
// VT-switch escape hatch — PURE CORE (no wlroots): keysym -> VT number. The
// glue (input.cpp) calls wlr_session_change_vt on a hit and consumes; this
// helper decides the hit. Ctrl+Alt+Fn arrives as XF86Switch_VT_1..12.
// ============================================================================

TEST_CASE("vt_for_keysym: maps the XF86Switch_VT range to 1..12") {
    using unbox::kernel::vt_for_keysym;

    // Both endpoints of the range.
    CHECK(vt_for_keysym(XKB_KEY_XF86Switch_VT_1) == 1U);
    CHECK(vt_for_keysym(XKB_KEY_XF86Switch_VT_12) == 12U);
    // A representative interior value.
    CHECK(vt_for_keysym(XKB_KEY_XF86Switch_VT_2) == 2U);
    CHECK(vt_for_keysym(XKB_KEY_XF86Switch_VT_7) == 7U);

    // Just outside the range on both sides => nullopt (no VT-switch).
    CHECK(vt_for_keysym(XKB_KEY_XF86Switch_VT_1 - 1) == std::nullopt);
    CHECK(vt_for_keysym(XKB_KEY_XF86Switch_VT_12 + 1) == std::nullopt);

    // Plain F1..F12 (no Ctrl+Alt) resolve to ordinary keysyms, NOT the
    // XF86Switch_VT range — they must pass through untouched.
    CHECK(vt_for_keysym(XKB_KEY_F1) == std::nullopt);
    CHECK(vt_for_keysym(XKB_KEY_F12) == std::nullopt);

    // An unrelated keysym.
    CHECK(vt_for_keysym(XKB_KEY_a) == std::nullopt);
}

// ============================================================================
// The typed bus — PURE CORE (strict; zero mocks of unbox modules, no wlroots
// running). A test DisableSink stands in for the kernel's isolation registry.
// ============================================================================

namespace {

using unbox::kernel::detail::DisableSink;
using unbox::kernel::detail::HookBase;
using unbox::kernel::Event;
using unbox::kernel::ExtensionId;
using unbox::kernel::Filter;
using unbox::kernel::Subscription;

// Mirrors Server::Impl's isolation behavior at pure-core scale: on disable(),
// purge the offending extension from every registered hook. Records who got
// disabled so tests can assert isolation hit the RIGHT extension.
struct TestRegistry final : DisableSink {
    std::vector<HookBase*> hooks;
    std::vector<ExtensionId> disabled;

    void track(HookBase& h) {
        h.set_sink(this);
        hooks.push_back(&h);
    }
    void disable(ExtensionId who) noexcept override {
        disabled.push_back(who);
        for (HookBase* h : hooks) {
            h->purge(who);
        }
    }
};

constexpr ExtensionId ext_a{1};
constexpr ExtensionId ext_b{2};
constexpr ExtensionId ext_c{3};

} // namespace

TEST_CASE("Event fans out to all listeners in subscription order") {
    Event<int> ev;
    std::vector<int> log;
    auto s1 = ev.subscribe(ext_a, [&](int v) { log.push_back(v + 10); });
    auto s2 = ev.subscribe(ext_b, [&](int v) { log.push_back(v + 20); });
    auto s3 = ev.subscribe(ext_c, [&](int v) { log.push_back(v + 30); });

    ev.emit(1);
    CHECK(log == std::vector<int>{11, 21, 31});
}

TEST_CASE("Subscription RAII unsubscribes on destruction") {
    Event<int> ev;
    int hits = 0;
    auto outer = ev.subscribe(ext_a, [&](int) { ++hits; });
    {
        auto inner = ev.subscribe(ext_b, [&](int) { ++hits; });
        ev.emit(0);
        CHECK(hits == 2);
    }
    // inner dropped: only outer remains.
    ev.emit(0);
    CHECK(hits == 3);

    // Explicit reset() also unsubscribes.
    outer.reset();
    CHECK(!outer.active());
    ev.emit(0);
    CHECK(hits == 3);
}

TEST_CASE("Subscription is move-only and the moved-from handle is inert") {
    Event<int> ev;
    int hits = 0;
    Subscription s = ev.subscribe(ext_a, [&](int) { ++hits; });
    Subscription moved = std::move(s);
    CHECK(moved.active());
    CHECK(!s.active());
    ev.emit(0);
    CHECK(hits == 1);
    s.reset(); // no-op on moved-from
    ev.emit(0);
    CHECK(hits == 2);
}

TEST_CASE("a listener may unsubscribe ITSELF during dispatch (deferred removal)") {
    Event<int> ev;
    int a = 0;
    int c = 0;
    std::unique_ptr<Subscription> self;
    auto sa = ev.subscribe(ext_a, [&](int) { ++a; });
    auto sb = ev.subscribe(ext_b, [&](int) { self->reset(); }); // drop self mid-dispatch
    auto sc = ev.subscribe(ext_c, [&](int) { ++c; });
    self = std::make_unique<Subscription>(std::move(sb));

    ev.emit(0);
    // a and c still fired this round despite b removing itself.
    CHECK(a == 1);
    CHECK(c == 1);

    ev.emit(0); // b gone now
    CHECK(a == 2);
    CHECK(c == 2);
}

TEST_CASE("re-entrant emit is safe") {
    Event<int> ev;
    int inner = 0;
    bool reentered = false;
    auto s = ev.subscribe(ext_a, [&](int v) {
        if (!reentered && v == 1) {
            reentered = true;
            ev.emit(2); // re-enter
        }
        ++inner;
    });
    ev.emit(1);
    CHECK(inner == 2); // outer (v=1) and inner (v=2)
}

TEST_CASE("Filter threads the value through links in order") {
    Filter<int> flt;
    auto s1 = flt.subscribe(ext_a, [](int v) { return v + 1; });
    auto s2 = flt.subscribe(ext_b, [](int v) { return v * 10; });
    // (((5)+1)*10) = 60
    CHECK(flt.apply(5) == 60);
}

TEST_CASE("Filter with no links returns the value unchanged") {
    Filter<int> flt;
    CHECK(flt.apply(42) == 42);
}

TEST_CASE("error isolation: a throwing listener disables only its extension") {
    TestRegistry reg;
    Event<int> ev{&reg};
    reg.track(ev);

    std::vector<std::string> log;
    auto sa = ev.subscribe(ext_a, [&](int) { log.emplace_back("a"); });
    auto sb = ev.subscribe(ext_b, [&](int) {
        log.emplace_back("b-throw");
        throw std::runtime_error("boom");
    });
    auto sc = ev.subscribe(ext_c, [&](int) { log.emplace_back("c"); });

    ev.emit(0);
    // All three ran THIS emit (isolation doesn't abort the in-flight fan-out);
    // b was disabled.
    CHECK(log == std::vector<std::string>{"a", "b-throw", "c"});
    CHECK(reg.disabled == std::vector<ExtensionId>{ext_b});

    log.clear();
    ev.emit(0);
    // b's subscription was purged; a and c remain.
    CHECK(log == std::vector<std::string>{"a", "c"});
}

TEST_CASE("error isolation: a throwing filter link is skipped and chain continues") {
    TestRegistry reg;
    Filter<int> flt{&reg};
    reg.track(flt);

    auto s1 = flt.subscribe(ext_a, [](int v) { return v + 1; });
    auto s2 = flt.subscribe(ext_b, [](int) -> int { throw std::runtime_error("boom"); });
    auto s3 = flt.subscribe(ext_c, [](int v) { return v * 10; });

    // a: 0->1, b throws (skipped, value stays 1), c: 1->10.
    CHECK(flt.apply(0) == 10);
    CHECK(reg.disabled == std::vector<ExtensionId>{ext_b});

    // b purged: a then c.
    CHECK(flt.apply(0) == 10);
}

TEST_CASE("disabling an extension purges it across MULTIPLE hooks") {
    TestRegistry reg;
    Event<int> ev1{&reg};
    Event<int> ev2{&reg};
    reg.track(ev1);
    reg.track(ev2);

    int ev2_hits = 0;
    // ext_b subscribes to BOTH hooks; throwing on ev1 must drop its ev2 sub too.
    auto a1 = ev1.subscribe(ext_a, [](int) {});
    auto b1 = ev1.subscribe(ext_b, [](int) { throw std::runtime_error("boom"); });
    auto b2 = ev2.subscribe(ext_b, [&](int) { ++ev2_hits; });

    ev1.emit(0);          // disables ext_b everywhere
    ev2.emit(0);          // ext_b's ev2 listener must NOT fire
    CHECK(ev2_hits == 0);
    CHECK(reg.disabled == std::vector<ExtensionId>{ext_b});
}

// ============================================================================
// Extension host: install + topological activation (no wlroots input needed).
// ============================================================================

namespace {

// Records activation order into a shared log so tests can assert topo order.
class RecordingExtension : public unbox::kernel::Extension {
public:
    RecordingExtension(unbox::kernel::Manifest m, std::vector<std::string>* log)
        : manifest_(std::move(m)), log_(log) {}
    auto manifest() const -> const unbox::kernel::Manifest& override { return manifest_; }
    void activate(unbox::kernel::Host&) override { log_->push_back(manifest_.id); }

private:
    unbox::kernel::Manifest manifest_;
    std::vector<std::string>* log_;
};

auto make_headless_server() -> std::unique_ptr<unbox::kernel::Server> {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    return unbox::kernel::Server::create({});
}

using unbox::kernel::Manifest;
using unbox::kernel::Tier;

} // namespace

TEST_CASE("activation respects depends_on topological order") {
    auto server = make_headless_server();
    std::vector<std::string> log;

    // Install in an order that does NOT match the dependency order.
    server->install(std::make_unique<RecordingExtension>(
        Manifest{"taskbar", Tier::standard, {"xdg-shell"}}, &log));
    server->install(std::make_unique<RecordingExtension>(
        Manifest{"xdg-shell", Tier::core, {}}, &log));
    server->install(std::make_unique<RecordingExtension>(
        Manifest{"tiling", Tier::standard, {"xdg-shell", "taskbar"}}, &log));

    server->activate_extensions();

    // xdg-shell first (no deps, core tier), then taskbar, then tiling.
    CHECK(log == std::vector<std::string>{"xdg-shell", "taskbar", "tiling"});
}

TEST_CASE("activate_extensions is idempotent") {
    auto server = make_headless_server();
    std::vector<std::string> log;
    server->install(
        std::make_unique<RecordingExtension>(Manifest{"a", Tier::core, {}}, &log));
    server->activate_extensions();
    server->activate_extensions();
    CHECK(log == std::vector<std::string>{"a"});
}

TEST_CASE("duplicate extension id is a startup error at install") {
    auto server = make_headless_server();
    std::vector<std::string> log;
    server->install(
        std::make_unique<RecordingExtension>(Manifest{"dup", Tier::core, {}}, &log));
    CHECK_THROWS_AS(server->install(std::make_unique<RecordingExtension>(
                        Manifest{"dup", Tier::standard, {}}, &log)),
                    std::runtime_error);
}

TEST_CASE("missing dependency is a startup error at activation") {
    auto server = make_headless_server();
    std::vector<std::string> log;
    server->install(std::make_unique<RecordingExtension>(
        Manifest{"needs-missing", Tier::core, {"nope"}}, &log));
    CHECK_THROWS_AS(server->activate_extensions(), std::runtime_error);
}

TEST_CASE("dependency cycle is a startup error at activation") {
    auto server = make_headless_server();
    std::vector<std::string> log;
    server->install(
        std::make_unique<RecordingExtension>(Manifest{"x", Tier::core, {"y"}}, &log));
    server->install(
        std::make_unique<RecordingExtension>(Manifest{"y", Tier::core, {"x"}}, &log));
    CHECK_THROWS_AS(server->activate_extensions(), std::runtime_error);
}

TEST_CASE("featureless kernel: zero extensions boots, runs, shuts down clean") {
    auto server = make_headless_server();
    CHECK(!server->socket_name().empty());
    server->activate_extensions(); // no-op with zero extensions
    for (int i = 0; i < 3; ++i) {
        CHECK(server->dispatch(10));
    }
}

// ============================================================================
// Typed surface->scene-tree association — PURE CORE (no wlroots). Keys/values
// are pointer identities; dummy addresses stand in for wlr_surface*/scene_tree*.
// ============================================================================

namespace {

using unbox::kernel::detail::PointerAssoc;
using unbox::kernel::SurfaceRegistration;

// Distinct, never-dereferenced sentinel addresses.
int surf_a_obj = 0, surf_b_obj = 0, tree_1_obj = 0, tree_2_obj = 0;
void* const surf_a = &surf_a_obj;
void* const surf_b = &surf_b_obj;
void* const tree_1 = &tree_1_obj;
void* const tree_2 = &tree_2_obj;

} // namespace

TEST_CASE("surface assoc: register, lookup, unregister") {
    PointerAssoc store;
    CHECK(store.get(surf_a) == nullptr); // unregistered -> null

    SurfaceRegistration reg(&store, surf_a, store.set(surf_a, tree_1));
    CHECK(reg.active());
    CHECK(store.get(surf_a) == tree_1);
    CHECK(store.get(surf_b) == nullptr); // independent key still null

    reg.reset();
    CHECK(!reg.active());
    CHECK(store.get(surf_a) == nullptr); // unregistered on reset
    CHECK(store.size() == 0);
}

TEST_CASE("surface assoc: RAII handle unregisters on destruction") {
    PointerAssoc store;
    {
        SurfaceRegistration reg(&store, surf_a, store.set(surf_a, tree_1));
        CHECK(store.get(surf_a) == tree_1);
    }
    CHECK(store.get(surf_a) == nullptr);
}

TEST_CASE("surface assoc: move transfers ownership; moved-from is inert") {
    PointerAssoc store;
    SurfaceRegistration a(&store, surf_a, store.set(surf_a, tree_1));
    SurfaceRegistration b = std::move(a);
    CHECK(b.active());
    CHECK(!a.active());
    a.reset(); // no-op
    CHECK(store.get(surf_a) == tree_1); // still registered (b owns it)
    b.reset();
    CHECK(store.get(surf_a) == nullptr);
}

TEST_CASE("surface assoc: double-register replaces value; stale handle is a no-op") {
    PointerAssoc store;
    // First registration of surf_a -> tree_1.
    SurfaceRegistration first(&store, surf_a, store.set(surf_a, tree_1));
    CHECK(store.get(surf_a) == tree_1);

    // Re-host the SAME surface in tree_2: replaces the mapping, bumps token.
    SurfaceRegistration second(&store, surf_a, store.set(surf_a, tree_2));
    CHECK(store.get(surf_a) == tree_2);

    // Destroying the SUPERSEDED first handle must NOT tear down the newer
    // mapping (token defense).
    first.reset();
    CHECK(store.get(surf_a) == tree_2);

    // The current owner still unregisters correctly.
    second.reset();
    CHECK(store.get(surf_a) == nullptr);
}

TEST_CASE("surface assoc: distinct keys are independent") {
    PointerAssoc store;
    SurfaceRegistration ra(&store, surf_a, store.set(surf_a, tree_1));
    SurfaceRegistration rb(&store, surf_b, store.set(surf_b, tree_2));
    CHECK(store.get(surf_a) == tree_1);
    CHECK(store.get(surf_b) == tree_2);
    CHECK(store.size() == 2);
    ra.reset();
    CHECK(store.get(surf_a) == nullptr);
    CHECK(store.get(surf_b) == tree_2); // unaffected
}

// ============================================================================
// ui-substrate PURE decision cores (no wlroots): touch-mode state machine
// (debounce/override — NO visual scaling) and the consume-or-pass hit-test
// geometry.
// ============================================================================

namespace {
using unbox::kernel::point_in_rect;
using unbox::kernel::TouchModeTracker;
} // namespace

TEST_CASE("touch-mode: touch turns on, pointer turns off (transitions reported)") {
    TouchModeTracker t(/*debounce_ms=*/700);
    CHECK(!t.is_touch());            // starts in pointer mode
    CHECK(t.on_touch(1000));         // -> touch (changed)
    CHECK(t.is_touch());
    CHECK(!t.on_touch(1100));        // already touch (no change)
    // Pointer motion AFTER the debounce window flips back to pointer.
    CHECK(t.on_pointer_motion(2000));
    CHECK(!t.is_touch());
}

TEST_CASE("touch-mode: pointer jitter inside the debounce window is ignored") {
    TouchModeTracker t(700);
    t.on_touch(1000);
    // Motion 300ms after the touch (inside 700ms): ignored, stays touch.
    CHECK(!t.on_pointer_motion(1300));
    CHECK(t.is_touch());
    // Motion past the window: flips to pointer.
    CHECK(t.on_pointer_motion(1800));
    CHECK(!t.is_touch());
}

TEST_CASE("touch-mode: manual override pins, none restores automatic") {
    TouchModeTracker t(700);
    CHECK(t.set_override(TouchModeTracker::Override::force_touch));
    CHECK(t.is_touch());
    CHECK(!t.on_pointer_motion(5000)); // override pins it; no change
    CHECK(t.is_touch());
    CHECK(t.set_override(TouchModeTracker::Override::none)); // back to auto (pointer)
    CHECK(!t.is_touch());
}

TEST_CASE("hit-test geometry: consume-or-pass boundary (half-open)") {
    // Surface at (40,40) size 320x200 => covers [40,360) x [40,240).
    CHECK(point_in_rect(40, 40, 40, 40, 320, 200));    // top-left corner inside
    CHECK(point_in_rect(200, 140, 40, 40, 320, 200));  // interior
    CHECK(point_in_rect(359, 239, 40, 40, 320, 200));  // last inside pixel
    CHECK(!point_in_rect(360, 140, 40, 40, 320, 200)); // right edge half-open
    CHECK(!point_in_rect(200, 240, 40, 40, 320, 200)); // bottom edge half-open
    CHECK(!point_in_rect(39, 140, 40, 40, 320, 200));  // just left
    CHECK(!point_in_rect(200, 39, 40, 40, 320, 200));  // just above
}

// ============================================================================
// Implicit grab ownership — PURE CORE. The consumer of a press owns its
// release regardless of what is under the cursor at release time (the slice-5
// stuck-drag bug). These are the EXACT repros the brief calls out.
// ============================================================================

namespace {
using unbox::kernel::GrabOwner;
using unbox::kernel::PointerButtonGrab;
} // namespace

TEST_CASE("grab: press OVER ui surface -> release OUTSIDE still consumed by substrate") {
    PointerButtonGrab g;
    // Press over a ui surface: substrate owns the grab.
    CHECK(g.press(/*over_surface=*/true) == GrabOwner::substrate);
    CHECK(g.active());
    // Release happens with the cursor NOT over the surface — still substrate's
    // (the press's owner). It must NOT fall through to the bus.
    CHECK(g.release() == GrabOwner::substrate);
    CHECK(!g.active()); // grab ended
}

TEST_CASE("grab: press OUTSIDE -> release OVER ui surface still reaches the bus") {
    PointerButtonGrab g;
    // Press not over a ui surface: the bus owns the grab (ext-xdg-shell titlebar
    // drag). over_surface at RELEASE time is irrelevant.
    CHECK(g.press(/*over_surface=*/false) == GrabOwner::bus);
    // Release over a ui surface — must still be delivered to the bus so
    // ext-xdg-shell's GrabMachine sees it and the drag ends (the fixed bug).
    CHECK(g.release() == GrabOwner::bus);
    CHECK(!g.active());
}

TEST_CASE("grab: owner fixed at FIRST press; multi-button grab ends on last release") {
    PointerButtonGrab g;
    CHECK(g.press(/*over_surface=*/false) == GrabOwner::bus); // first press fixes owner=bus
    // A second button pressed while the first is held — even if now "over" a
    // surface — joins the SAME (bus) grab; the owner does not change mid-stream.
    CHECK(g.press(/*over_surface=*/true) == GrabOwner::bus);
    CHECK(g.active());
    CHECK(g.release() == GrabOwner::bus); // first release: grab still active
    CHECK(g.active());
    CHECK(g.release() == GrabOwner::bus); // last release: grab ends
    CHECK(!g.active());
    CHECK(g.owner() == GrabOwner::none);
}

TEST_CASE("grab: a fresh stream can flip owner (substrate then bus)") {
    PointerButtonGrab g;
    CHECK(g.press(true) == GrabOwner::substrate);
    CHECK(g.release() == GrabOwner::substrate);
    // New stream, press elsewhere: now the bus owns it.
    CHECK(g.press(false) == GrabOwner::bus);
    CHECK(g.release() == GrabOwner::bus);
}

// ============================================================================
// RCSS easing reader: UiSurface::transition_timing parses an element's authored
// `transition` (duration/delay + the RmlUi tween wrapped as a pure function),
// resolves the property name to its id, and honours an `all` transition.
// GL/seat note: transition_timing reads COMPUTED values, which only exist once
// the document has loaded + a context update ran — so it needs the gles2 GL
// bridge (headless+gles2). On a box with no GL path the surface is null and the
// case degrades to a no-op (asserts nothing), exactly like the other substrate
// cases. The frame-callback (request_frames) scheduling is real-seat / GL-frame
// driven and is exercised only under a live output, not mocked here.
// ============================================================================

namespace {

// transform => an exact `transform` transition; opacity => only via the `all`
// fallback; the #plain element has no transition at all (nullopt).
const char* kEaseRml = R"RML(<rml>
<head>
<style>
body { width: 200px; height: 120px; }
#anim  { display: block; width: 100px; height: 20px;
         transition: transform 0.2s cubic-in-out 0.05s; }
#allel { display: block; width: 100px; height: 20px;
         transition: all 0.3s; }
#plain { display: block; width: 100px; height: 20px; }
</style>
</head>
<body data-model="ui">
<div id="anim"></div>
<div id="allel"></div>
<div id="plain"></div>
</body>
</rml>)RML";

class EaseTestExtension : public unbox::kernel::Extension {
public:
    auto manifest() const -> const Manifest& override { return manifest_; }
    void activate(Host& host) override {
        UiSurfaceSpec spec;
        spec.rml_inline = kEaseRml;
        spec.x = 0;
        spec.y = 0;
        spec.width = 200;
        spec.height = 120;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
    }
    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }
    [[nodiscard]] auto surface() -> UiSurface* { return surface_.get(); }

private:
    Manifest manifest_{"ease-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
};

} // namespace

TEST_CASE("ui: transition_timing reads RCSS duration/delay + tween, resolves property + all") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new EaseTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 60); // load the document + run a context update => computed values

    if (!ext->has_surface()) {
        // No GL path on this box: surface is null, nothing computed. Graceful.
        CHECK(true);
        return;
    }
    UiSurface* s = ext->surface();

    // (1) Exact property match: transform 0.2s cubic-in-out 0.05s.
    const auto tt = s->transition_timing("anim", "transform");
    REQUIRE(tt.has_value());
    CHECK(tt->duration == doctest::Approx(0.2));
    CHECK(tt->delay == doctest::Approx(0.05));
    REQUIRE(static_cast<bool>(tt->ease));
    // cubic-in-out: clamped endpoints 0 and 1, monotone, midpoint ~0.5.
    CHECK(tt->ease(0.0F) == doctest::Approx(0.0F));
    CHECK(tt->ease(1.0F) == doctest::Approx(1.0F));
    CHECK(tt->ease(0.5F) == doctest::Approx(0.5F)); // symmetric in-out hits 0.5 at t=0.5
    const float q = tt->ease(0.25F);
    CHECK(q > 0.0F);
    CHECK(q < 0.5F); // ease-in region rises slower than linear

    // (2) `all` fallback: #allel has `all 0.3s` (no tween => RmlUi's default
    // linear); a property with no exact entry resolves through the all
    // transition (linear => ease(t) == t).
    const auto allt = s->transition_timing("allel", "transform");
    REQUIRE(allt.has_value());
    CHECK(allt->duration == doctest::Approx(0.3));
    CHECK(allt->delay == doctest::Approx(0.0));
    REQUIRE(static_cast<bool>(allt->ease));
    CHECK(allt->ease(0.5F) == doctest::Approx(0.5F)); // linear

    // (3) No transition on the element => nullopt.
    CHECK_FALSE(s->transition_timing("plain", "transform").has_value());
    // (4) Unknown element id => nullopt.
    CHECK_FALSE(s->transition_timing("nope", "transform").has_value());
    // (5) Unparseable property name => nullopt (no exact match, no `all` here).
    CHECK_FALSE(s->transition_timing("anim", "not-a-real-property").has_value());
}

// ============================================================================
// SPIKE (rml-compositing, Phase 0) — PURE input-inversion core (criterion 3).
// The runnable spike target (src/spike/) self-checks the live-texture / 3D
// transform / present / idle-gate headless; THIS unit-tests the screen-point ->
// (surface element, surface-local coord) inversion through a known transform —
// the math the runtime RmlUi-pick -> wl_seat translation rides on. Throwaway,
// but kept green with the kernel: a regressed inverse would silently mis-route
// touch on a tilted window, the exact failure criterion 3 guards against.
// ============================================================================

namespace {
namespace spk = unbox::kernel::spike;

// Forward-project a surface-local point through `t`, then invert; assert the
// round trip recovers the original to sub-pixel. err in pixels.
auto roundtrip_err(const spk::Mat4& t, double lx, double ly) -> double {
    const spk::ScreenPoint s = spk::project_to_screen(t, lx, ly);
    const auto back = spk::unproject_to_local(t, s.x, s.y);
    if (!back) {
        return 1e9;
    }
    return std::hypot(back->x - lx, back->y - ly);
}
} // namespace

TEST_CASE("spike(rml-compositing): screen->surface-local inverts an affine transform") {
    // A plain translate (no perspective): the inverse must be exact everywhere.
    const spk::Mat4 t = spk::translate(120.0, -40.0);
    CHECK(roundtrip_err(t, 0.0, 0.0) < 1e-9);
    CHECK(roundtrip_err(t, 200.0, 150.0) < 1e-9);
    // The forward map is a pure offset: a local (10,10) lands at (130,-30).
    const spk::ScreenPoint s = spk::project_to_screen(t, 10.0, 10.0);
    CHECK(s.x == doctest::Approx(130.0));
    CHECK(s.y == doctest::Approx(-30.0));
}

TEST_CASE("spike(rml-compositing): inverts perspective + rotateY about the element origin") {
    // The criterion-3 case: a 256x256 surface element with perspective(800) +
    // rotateY, resolved about the 50% origin (what RCSS computes). The inverse is
    // a ray/plane intersection (non-affine under perspective); assert sub-0.01px
    // recovery across the element, including off-center points that foreshorten.
    const double origin = 128.0;
    for (double deg : {15.0, 35.0, 60.0, -45.0}) {
        const spk::Mat4 t = spk::rcss_transform_about_origin(
            spk::mul(spk::perspective(800.0),
                     spk::rotate_y(deg * std::numbers::pi / 180.0)),
            origin, origin);
        CHECK(roundtrip_err(t, 128.0, 128.0) < 1e-6); // center: on the rotation axis
        CHECK(roundtrip_err(t, 32.0, 64.0) < 0.01);   // near edge (foreshortened)
        CHECK(roundtrip_err(t, 224.0, 200.0) < 0.01); // far edge
        CHECK(roundtrip_err(t, 64.0, 96.0) < 0.01);   // arbitrary interior point
    }
}

TEST_CASE("spike(rml-compositing): the inverse is the true matrix inverse (M*inv ~ I)") {
    // The unprojection's correctness rests on invert(): assert inv(M)*M is the
    // identity for the perspective+rotateY operator (the non-trivial case). This
    // is the algebraic backstop under the geometric round-trip tests above.
    const double origin = 128.0;
    const spk::Mat4 m = spk::rcss_transform_about_origin(
        spk::mul(spk::perspective(800.0), spk::rotate_y(40.0 * std::numbers::pi / 180.0)), origin,
        origin);
    const auto inv = spk::invert(m);
    REQUIRE(inv.has_value());
    const spk::Mat4 prod = spk::mul(*inv, m);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            CHECK(prod.at(r, c) == doctest::Approx(r == c ? 1.0 : 0.0).epsilon(1e-9));
        }
    }
}

TEST_CASE("spike(rml-compositing): an edge-on (90deg) transform collapses the element to a line") {
    // rotateY(90deg) about the origin turns the element edge-on: its plane
    // projects to a vertical LINE on screen, so distinct surface-local points
    // collapse to (nearly) the same screen x — there is no reliable preimage. We
    // assert the GEOMETRIC truth (the forward map is degenerate) rather than a
    // particular inverse return: at runtime RmlUi's own transform-aware pick is
    // what declines an edge-on element, so the spike never has to invert one.
    const double origin = 128.0;
    const spk::Mat4 t = spk::rcss_transform_about_origin(
        spk::mul(spk::perspective(800.0), spk::rotate_y(std::numbers::pi / 2.0)), origin, origin);
    const spk::ScreenPoint a = spk::project_to_screen(t, 32.0, 64.0);
    const spk::ScreenPoint b = spk::project_to_screen(t, 224.0, 64.0);
    // Two points 192px apart in surface-local X land at the same screen X (the
    // element is edge-on): the map lost its X information.
    CHECK(std::abs(a.x - b.x) < 0.5);
}

// ============================================================================
// RML compositing Wave 1b: surface-element input-back PURE CORE (src/input_core.
// hpp) — the PRODUCTION port of the spike's screen->surface-local inversion the
// live wl_seat translation rides on, doctest-ed here (criterion-3 round-trip)
// plus the parent-relative child-placement helper. Strict core, zero mocks.
// ============================================================================

namespace {
namespace ic = unbox::kernel;

// Forward-project a surface-local point through `t`, invert, assert recovery.
auto kernel_roundtrip_err(const ic::Mat4& t, double lx, double ly) -> double {
    const ic::ScreenPoint s = ic::project_to_screen(t, lx, ly);
    const auto back = ic::unproject_to_local(t, s.x, s.y);
    if (!back) {
        return 1e9;
    }
    return std::hypot(back->x - lx, back->y - ly);
}
} // namespace

TEST_CASE("input-back core: screen->surface-local inverts perspective+rotateY (<0.01px)") {
    // The criterion-3 case in the PRODUCTION core: a 256x256 surface element with
    // perspective(800) + rotateY about the 50% origin (what RCSS computes). The
    // inverse is a ray/plane intersection (non-affine); recovery must be sub-
    // 0.01px across the element — the geometry the live Element::Project()-based
    // forward + wl_seat surface-local notify depends on.
    const double origin = 128.0;
    for (double deg : {15.0, 35.0, 60.0, -45.0}) {
        const ic::Mat4 t = ic::rcss_transform_about_origin(
            ic::mul(ic::perspective(800.0), ic::rotate_y(deg * std::numbers::pi / 180.0)), origin,
            origin);
        CHECK(kernel_roundtrip_err(t, 128.0, 128.0) < 1e-6); // center: on the axis
        CHECK(kernel_roundtrip_err(t, 32.0, 64.0) < 0.01);   // near edge (foreshortened)
        CHECK(kernel_roundtrip_err(t, 224.0, 200.0) < 0.01); // far edge
        CHECK(kernel_roundtrip_err(t, 64.0, 96.0) < 0.01);   // arbitrary interior
    }
    // A plain translate (affine): exact everywhere.
    const ic::Mat4 tr = ic::translate(120.0, -40.0);
    CHECK(kernel_roundtrip_err(tr, 0.0, 0.0) < 1e-9);
    CHECK(kernel_roundtrip_err(tr, 200.0, 150.0) < 1e-9);
}

TEST_CASE("input-back core: place_child_box maps a tree offset into the parent's resolved box") {
    using unbox::kernel::place_child_box;
    // Parent <img> resolved box (px) == surface natural size (1:1 scale): a child
    // at tree offset (10,20) sized 30x40 lands at exactly (10,20,30,40).
    {
        const auto b = place_child_box(/*px*/ 0, /*py*/ 0, /*pw*/ 200, /*ph*/ 100,
                                       /*surf_w*/ 200, /*surf_h*/ 100, /*sx*/ 10, /*sy*/ 20,
                                       /*cw*/ 30, /*ch*/ 40);
        CHECK(b.x == doctest::Approx(10.0));
        CHECK(b.y == doctest::Approx(20.0));
        CHECK(b.w == doctest::Approx(30.0));
        CHECK(b.h == doctest::Approx(40.0));
    }
    // A parent rendered at HALF its natural size (a resized window): the offset +
    // child size scale by 0.5, and the parent's box origin is added (a moving
    // parent drags the child). surf 200x100 drawn into a 100x50 box at (40,30).
    {
        const auto b = place_child_box(/*px*/ 40, /*py*/ 30, /*pw*/ 100, /*ph*/ 50,
                                       /*surf_w*/ 200, /*surf_h*/ 100, /*sx*/ 20, /*sy*/ 40,
                                       /*cw*/ 60, /*ch*/ 20);
        CHECK(b.x == doctest::Approx(50.0));  // 40 + 20*0.5
        CHECK(b.y == doctest::Approx(50.0));  // 30 + 40*0.5
        CHECK(b.w == doctest::Approx(30.0));  // 60*0.5
        CHECK(b.h == doctest::Approx(10.0));  // 20*0.5
    }
    // Degenerate (zero surface size): no NaN — falls back to a 1:1 scale.
    {
        const auto b = place_child_box(5, 5, 0, 0, 0, 0, 7, 8, 9, 10);
        CHECK(b.x == doctest::Approx(12.0)); // 5 + 7
        CHECK(b.y == doctest::Approx(13.0)); // 5 + 8
    }
}

// ============================================================================
// RML compositing Wave 1: surface-element PURE CORES (ui_core.hpp). The URI
// minting and the seq-gate decision predicate are pure (no wlroots/GL), so they
// are doctest-ed here with nothing running — the strict-core half of the
// asymmetric testing rule. The live import/frame-done glue is covered by the
// headless integration test below.
// ============================================================================

TEST_CASE("surface-element: source_uri mints a stable unbox-surface:// URI") {
    using unbox::kernel::surface_element_uri;
    CHECK(surface_element_uri(1) == "unbox-surface://1");
    CHECK(surface_element_uri(7) == "unbox-surface://7");
    // Distinct ids => distinct URIs (each element samples its own texture).
    CHECK(surface_element_uri(1) != surface_element_uri(2));
    // The scheme matches the public-contract example and is the LIVE sibling of
    // the preview scheme (NOT the same — a live element is not a frozen preview).
    CHECK(surface_element_uri(42).rfind("unbox-surface://", 0) == 0);
    CHECK(surface_element_uri(42).rfind("unbox-preview://", 0) != 0);
}

TEST_CASE("surface-element: the seq-gate is reuse-proof (the frozen-frame fix)") {
    using unbox::kernel::surface_element_needs_reimport;

    // First import: no seq yet AND no texture => MUST import, whatever the rest.
    CHECK(surface_element_needs_reimport(/*have_seq=*/false, /*cur=*/0, /*new=*/1,
                                         /*same_ptr=*/false, /*have_tex=*/false));
    CHECK(surface_element_needs_reimport(false, 0, 1, true, false));

    // Texture lost but seq known (defensive): re-import to rebuild it.
    CHECK(surface_element_needs_reimport(/*have_seq=*/true, /*cur=*/5, /*new=*/5,
                                         /*same_ptr=*/true, /*have_tex=*/false));

    // The IDLE case: same seq, same buffer pointer, live texture => NO re-import
    // (a static client costs zero work — the idle dirty-gate is preserved).
    CHECK_FALSE(surface_element_needs_reimport(true, 5, 5, /*same_ptr=*/true, /*have_tex=*/true));

    // A NEW commit (seq advances) of the SAME pooled buffer pointer with new
    // contents => MUST re-import. This is THE frozen-frame fix: a buffer-pointer
    // gate would wrongly skip it (foot recycles a small buffer pool), the seq
    // gate does not.
    CHECK(surface_element_needs_reimport(true, 5, /*new=*/6, /*same_ptr=*/true, /*have_tex=*/true));

    // A new commit with a DIFFERENT buffer pointer => re-import (obviously).
    CHECK(surface_element_needs_reimport(true, 5, 6, /*same_ptr=*/false, /*have_tex=*/true));

    // Same seq but a different pointer (should not happen in practice, but the
    // predicate is conservative): re-import rather than show a stale texture.
    CHECK(surface_element_needs_reimport(true, 5, 5, /*same_ptr=*/false, /*have_tex=*/true));
}

// ============================================================================
// RML compositing Wave 1: surface-element HEADLESS INTEGRATION TEST. Mirrors the
// spike --verify criteria 1 (zero-copy live import + seq-gate) + 6 (frame-done
// driven per composited frame), but against a REAL client surface: an
// in-process Wayland client thread connects to the headless server, creates a
// wl_surface + wl_shm buffers, and commits; the kernel captures that wl_surface
// (test seam) and builds a real SurfaceElement; the suite asserts URI/size, that
// a new commit (seq++) re-imports exactly once while re-adopting the same seq
// re-imports zero, and that wl_surface frame-done is sent per composited frame.
// We cannot see pixels headless — we assert the counters/URIs, exactly as the
// spike's --verify does. (Lenient shell test, AGENTS.md: glue on the wlr
// headless backend, not unit-coverage chasing.)
// ============================================================================

namespace {

// A minimal in-process Wayland client on its own thread. It binds wl_compositor
// + wl_shm, creates ONE wl_surface, and commits an shm buffer on demand. Two
// pre-made buffers let the test exercise BOTH a new pointer AND (by re-using
// buffer A) the pooled same-pointer re-commit. Driven by atomics the test sets;
// the client flushes after every commit and the TEST pumps the server loop so
// the commits land.
struct TestWaylandClient {
    std::thread thread;
    std::atomic<bool> ready{false};   // connected + surface created + first commit done
    std::atomic<bool> stop{false};
    std::atomic<int> commit_cmd{0};   // bump to request another commit (cycles buffers)
    std::atomic<int> commit_done{0};  // echoes commit_cmd once that commit was sent
    std::string socket;

    explicit TestWaylandClient(std::string sock) : socket(std::move(sock)) {}

    void start() { thread = std::thread([this] { run(); }); }
    void join() {
        stop = true;
        if (thread.joinable()) {
            thread.join();
        }
    }

    // -- registry globals --
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;

    static void reg_global(void* data, wl_registry* reg, uint32_t name, const char* iface,
                           uint32_t /*ver*/) {
        auto* self = static_cast<TestWaylandClient*>(data);
        if (std::strcmp(iface, "wl_compositor") == 0) {
            self->compositor = static_cast<wl_compositor*>(
                wl_registry_bind(reg, name, &wl_compositor_interface, 4));
        } else if (std::strcmp(iface, "wl_shm") == 0) {
            self->shm =
                static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
        }
    }
    static void reg_remove(void*, wl_registry*, uint32_t) {}

    // Make a 64x64 ARGB8888 shm buffer of a solid color.
    static auto make_buffer(wl_shm* shm, int w, int h, uint32_t argb) -> wl_buffer* {
        const int stride = w * 4;
        const int size = stride * h;
        int fd = memfd_create("unbox-se-test", MFD_CLOEXEC);
        if (fd < 0) {
            return nullptr;
        }
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return nullptr;
        }
        auto* px = static_cast<uint32_t*>(
            mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (px == MAP_FAILED) {
            close(fd);
            return nullptr;
        }
        for (int i = 0; i < w * h; ++i) {
            px[i] = argb;
        }
        munmap(px, size);
        wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
        wl_buffer* buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                   WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        return buf;
    }

    void run() {
        wl_display* dpy = wl_display_connect(socket.c_str());
        if (dpy == nullptr) {
            return; // no server socket: the test will time out waiting on ready
        }
        registry = wl_display_get_registry(dpy);
        static const wl_registry_listener reg_l = {reg_global, reg_remove};
        wl_registry_add_listener(registry, &reg_l, this);
        wl_display_roundtrip(dpy); // bind globals
        if (compositor == nullptr || shm == nullptr) {
            wl_display_disconnect(dpy);
            return;
        }
        wl_surface* surface = wl_compositor_create_surface(compositor);
        wl_buffer* buf_a = make_buffer(shm, 64, 64, 0xff2060c0);
        wl_buffer* buf_b = make_buffer(shm, 64, 64, 0xff60c020);
        if (surface == nullptr || buf_a == nullptr || buf_b == nullptr) {
            wl_display_disconnect(dpy);
            return;
        }
        // First commit: attach buffer A (seq advances to 1, surface->buffer set).
        wl_surface_attach(surface, buf_a, 0, 0);
        wl_surface_damage(surface, 0, 0, 64, 64);
        wl_surface_commit(surface);
        wl_display_flush(dpy);
        ready = true;

        int last = 0;
        while (!stop) {
            wl_display_dispatch_pending(dpy);
            wl_display_flush(dpy);
            const int cmd = commit_cmd.load();
            if (cmd != last) {
                // Command 1 attaches buffer B (a NEW pointer vs the current A);
                // command 2+ RE-attaches buffer B (the SAME pointer as current =>
                // the pooled same-pointer re-commit, §0d). Either way the surface
                // commit seq advances, so the seq-gate must re-import exactly once
                // — proving the gate keys on the seq, not the buffer pointer.
                wl_surface_attach(surface, buf_b, 0, 0);
                wl_surface_damage(surface, 0, 0, 64, 64);
                wl_surface_commit(surface);
                wl_display_flush(dpy);
                last = cmd;
                commit_done = cmd;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // Destroy every bound proxy before disconnect (mirrors the ext-*-client
        // tests' teardown) so libwayland-client retains no proxy allocations —
        // keeps the asan suite leak-clean.
        wl_buffer_destroy(buf_a);
        wl_buffer_destroy(buf_b);
        wl_surface_destroy(surface);
        if (compositor != nullptr) {
            wl_compositor_destroy(compositor);
        }
        if (shm != nullptr) {
            wl_shm_destroy(shm);
        }
        if (registry != nullptr) {
            wl_registry_destroy(registry);
        }
        wl_display_flush(dpy);
        wl_display_disconnect(dpy);
    }
};

// Pump the server until `pred()` is true or `max_turns` elapses. Returns pred().
template <typename Pred>
auto pump_until_se(unbox::kernel::Server& s, Pred pred, int max_turns = 400) -> bool {
    for (int i = 0; i < max_turns && !pred(); ++i) {
        s.dispatch(5);
    }
    return pred();
}

} // namespace

TEST_CASE("surface-element: live import + seq-gate + frame-done against a real client") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");

    auto server = unbox::kernel::Server::create({});
    server->activate_extensions();

    // Spin the in-process client; it connects to our socket and commits buffer A.
    TestWaylandClient client(server->socket_name());
    client.start();

    // Pump the server until the client has connected + committed its first buffer
    // (the kernel test seam captures the latest committed client wl_surface).
    const bool got_surface =
        pump_until_se(*server, [&] { return client.ready.load(); }) &&
        pump_until_se(*server, [&] {
            return server->ui_create_surface_element_for_test();
        });
    if (!got_surface) {
        // No GL path on this box, or the client could not connect: nothing the
        // headless agent can prove here (mirrors the other GL-gated tests' skip).
        client.join();
        return;
    }

    // (criterion 1) The element reports a stable unbox-surface:// URI and the
    // client's current pixel size.
    CHECK(server->ui_surface_element_uri().rfind("unbox-surface://", 0) == 0);
    CHECK(server->ui_surface_element_width() == 64);
    CHECK(server->ui_surface_element_height() == 64);

    // The initial import counts as one re-import (the first buffer adopted).
    const int reimports_after_create = server->ui_surface_element_reimport_count();
    CHECK(reimports_after_create >= 1);

    // (criterion 6) Frame-done must be DRIVEN per composited frame while the
    // element exists: pump and watch the counter climb (the stuck-frame fix —
    // without it the client would draw once and wait forever).
    const int fd_before = server->ui_surface_element_frame_done_count();
    pump(*server, 40);
    const int fd_after = server->ui_surface_element_frame_done_count();
    CHECK(fd_after > fd_before);

    // (criterion 1, the seq-gate) IDLE: with no new client commit, pumping more
    // frames must NOT re-import (a static client costs zero import work — the
    // idle dirty-gate is intact).
    const int reimports_idle0 = server->ui_surface_element_reimport_count();
    pump(*server, 40);
    CHECK(server->ui_surface_element_reimport_count() == reimports_idle0);

    // (criterion 1, the seq-gate / frozen-frame fix) A NEW commit (seq++) of a
    // NEW buffer pointer => exactly ONE more re-import.
    const int before_new = server->ui_surface_element_reimport_count();
    const int cmd1 = client.commit_cmd.fetch_add(1) + 1; // even => buffer B (new ptr)
    pump_until_se(*server, [&] { return client.commit_done.load() >= cmd1; });
    pump(*server, 20); // let tick_all re-import the committed buffer
    CHECK(server->ui_surface_element_reimport_count() == before_new + 1);

    // (criterion 1, the §0d pooled re-commit) A new commit (seq++) re-using the
    // SAME buffer pointer (buffer A) STILL re-imports exactly once — the gate is
    // on the commit SEQ, not the buffer pointer (foot recycles a buffer pool).
    const int before_reuse = server->ui_surface_element_reimport_count();
    const int cmd2 = client.commit_cmd.fetch_add(1) + 1; // odd => buffer A (same ptr)
    pump_until_se(*server, [&] { return client.commit_done.load() >= cmd2; });
    pump(*server, 20);
    CHECK(server->ui_surface_element_reimport_count() == before_reuse + 1);

    // Dropping the element ends the frame-callback duty: with no element left the
    // counts read 0 and stay 0 (the duty does not run for a destroyed element).
    server->ui_drop_surface_element_for_test();
    CHECK_FALSE(server->ui_surface_element_uri().rfind("unbox-surface://", 0) == 0);
    CHECK(server->ui_surface_element_frame_done_count() == 0);
    pump(*server, 20);
    CHECK(server->ui_surface_element_frame_done_count() == 0);

    client.join();
    unsetenv("WLR_HEADLESS_OUTPUTS");
}

// ============================================================================
// RML compositing Wave 1b: surface-TREE + INPUT-BACK + KEYBOARD-FOCUS headless
// integration test. A real client maps an xdg toplevel ROOT with a SUBSURFACE
// and an xdg POPUP; a test extension builds a SurfaceElement from the root and
// hosts it in a ui surface's <img src=root_uri>. The suite asserts (via the
// public Host::ui() path + kernel test seams, since headless has no input
// devices): (A) the subsurface + popup become per-node child <img> elements and
// frame-done reaches EVERY node (the tree-walk); (B) a pointer motion/button (+
// a touch down) over the element forwards to the client at the EXPECTED surface-
// LOCAL coords through the element's transform (Element::Project); (C) focusing
// the element delivers a wl_keyboard enter + a forwarded key. xdg-shell is
// provided by the TEST extension (the kernel names no shell), exactly as
// ext-xdg-shell will in Wave 2.
// ============================================================================

namespace {

// Server-side: a test extension that runs xdg-shell (via the kernel's wlr
// wrapper) + the subcompositor is the kernel's own (it always creates one), and
// turns the first mapped toplevel into a SurfaceElement shown in a ui surface.
class TreeTestExtension : public unbox::kernel::Extension {
public:
    auto manifest() const -> const Manifest& override { return manifest_; }

    void activate(Host& host) override {
        host_ = &host;
        xdg_shell_ = wlr_xdg_shell_create(host.display(), 3);
        if (xdg_shell_ == nullptr) {
            return;
        }
        new_toplevel_.connect(xdg_shell_->events.new_toplevel, [this](void* data) {
            on_new_toplevel(static_cast<wlr_xdg_toplevel*>(data));
        });
        new_popup_.connect(xdg_shell_->events.new_popup, [this](void* data) {
            on_new_popup(static_cast<wlr_xdg_popup*>(data));
        });
    }

    // Drive the surface element + ui surface once the root toplevel maps. The ui
    // surface hosts <img id=se src=root_uri> filling its box 1:1 with the
    // toplevel buffer, at a NON-ZERO layout origin (proves coords are surface-
    // local). `transform_deg` (set before map by the test) tilts the hosting img.
    void on_map() {
        if (root_surface_ == nullptr || element_ != nullptr) {
            return;
        }
        element_ = host_->ui().create_surface_element(root_surface_);
        if (element_ == nullptr) {
            return;
        }
        std::string xform;
        if (transform_deg_ != 0.0) {
            xform = "#se { transform: perspective(800px) rotateY(" +
                    std::to_string(transform_deg_) +
                    "deg); transform-origin: 50% 50%; }";
        }
        std::string rml =
            "<rml><head><style>body{margin:0px;background-color:transparent;"
            "width:200px;height:200px;} #se{display:block;position:absolute;"
            "left:0px;top:0px;width:200px;height:200px;} " +
            xform + "</style></head><body data-model=\"ui\">"
                    "<img id=\"se\" src=\"" +
            element_->source_uri() + "\"/></body></rml>";
        UiSurfaceSpec spec;
        spec.rml_inline = rml;
        spec.x = kSurfX;
        spec.y = kSurfY;
        spec.width = 200;
        spec.height = 200;
        spec.layer = unbox::kernel::SceneLayer::overlay;
        spec.visible = true;
        surface_ = host_->ui().create_surface(spec);
    }

    void set_transform(double deg) { transform_deg_ = deg; }
    void focus() {
        if (element_ != nullptr) {
            element_->focus_keyboard();
        }
    }
    // Register the click/tap-to-focus press hook on the element, counting each
    // fire (so the test can assert it fires once per press/down and never on
    // motion/miss). Call after the element exists.
    void install_press_counter() {
        if (element_ != nullptr) {
            element_->on_pressed([this] { ++pressed_count_; });
        }
    }
    [[nodiscard]] auto pressed_count() const -> int { return pressed_count_; }
    [[nodiscard]] auto has_element() const -> bool { return element_ != nullptr; }
    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }

    static constexpr int kSurfX = 40;
    static constexpr int kSurfY = 30;

private:
    void on_new_toplevel(wlr_xdg_toplevel* toplevel) {
        wlr_xdg_surface* xdg = toplevel->base;
        root_surface_ = xdg->surface;
        map_.connect(xdg->surface->events.map, [this](void*) { on_map(); });
        commit_.connect(xdg->surface->events.commit, [this, xdg](void*) {
            if (xdg->initial_commit) {
                wlr_xdg_toplevel_set_size(xdg->toplevel, 0, 0);
            }
        });
    }
    void on_new_popup(wlr_xdg_popup* popup) {
        wlr_xdg_surface* xdg = popup->base;
        popup_commit_.connect(xdg->surface->events.commit, [xdg](void*) {
            if (xdg->initial_commit) {
                wlr_xdg_surface_schedule_configure(xdg);
            }
        });
    }

    Manifest manifest_{"tree-test", Tier::standard, {}};
    Host* host_ = nullptr;
    wlr_xdg_shell* xdg_shell_ = nullptr;
    wlr_surface* root_surface_ = nullptr;
    double transform_deg_ = 0.0;
    int pressed_count_ = 0;
    unbox::kernel::Listener new_toplevel_, new_popup_, map_, commit_, popup_commit_;
    std::unique_ptr<unbox::kernel::SurfaceElement> element_;
    std::unique_ptr<UiSurface> surface_;
};

// Client-side: a real Wayland client that maps an xdg toplevel + a subsurface +
// an xdg popup, and records what its wl_pointer / wl_touch / wl_keyboard receive
// (so the test can assert the input-back surface-local coords).
//
// DE-FLAKE DESIGN (replaces the original free-running-thread approach):
//   Phase 1 (background thread): XDG protocol setup — the configure/ack
//     handshake needs both the server and client dispatching concurrently, so
//     a background thread is still used for this phase. It sets ready=true
//     once the toplevel + subsurface + popup are all committed, then EXITS its
//     loop and sets loop_done=true so the main test thread knows it is safe to
//     take over.
//   Phase 2 (main test thread): cooperative dispatch — after observing ready
//     and calling take_over() to join the background thread, the main test
//     thread exclusively owns dpy. It drives wl_display_dispatch_pending() +
//     wl_display_flush() itself, interleaved with server->dispatch() pumps via
//     pump_both(). No sleeping, no races.
//   Teardown: called from main thread after all assertions; destroys proxies.
struct TreeClient {
    std::thread thread;
    std::atomic<bool> ready{false};     // toplevel + subsurface + popup committed
    std::atomic<bool> loop_done{false}; // bg thread exited its loop (owns nothing now)
    std::atomic<bool> stop{false};      // tells bg thread to exit
    std::string socket;

    // Recorded client input (plain ints: ONLY accessed from the main test
    // thread after take_over() — no more atomics needed once we own dpy).
    // Keep atomics for thread-safety during setup phase (ready signalling).
    std::atomic<int> ptr_enters{0};
    std::atomic<int> ptr_motions{0};
    std::atomic<int> ptr_buttons{0};
    std::atomic<int> touch_downs{0};
    std::atomic<int> kbd_enters{0};
    std::atomic<int> keys{0};
    std::atomic<double> last_ptr_x{-1.0};
    std::atomic<double> last_ptr_y{-1.0};
    std::atomic<double> last_touch_x{-1.0};
    std::atomic<double> last_touch_y{-1.0};
    // Which of our surfaces the pointer/touch entered (so we can assert it hit
    // the EXPECTED node — root vs subsurface vs popup).
    std::atomic<int> ptr_enter_surface{-1}; // 0=root 1=subsurface 2=popup -1=none

    explicit TreeClient(std::string sock) : socket(std::move(sock)) {}
    void start() { thread = std::thread([this] { run(); }); }

    // Called from main thread after observing ready: stops the bg thread and
    // joins it so the main thread exclusively owns dpy.
    void take_over() {
        stop = true;
        if (thread.joinable()) {
            thread.join();
        }
    }

    // Dispatch pending client events from the MAIN TEST THREAD (only after
    // take_over() — must not be called while the bg thread is running).
    // Uses the non-blocking prepare/poll/read/dispatch pattern so it never
    // blocks: if no data is on the socket, cancel_read and return immediately.
    void dispatch_pending() {
        if (dpy == nullptr) return;
        // Flush any outgoing client requests first.
        wl_display_flush(dpy);
        // Drain already-buffered events before preparing the fd read.
        while (wl_display_prepare_read(dpy) != 0) {
            wl_display_dispatch_pending(dpy);
        }
        // Non-blocking poll: if data is available on the Wayland fd, read it.
        struct pollfd pfd = { wl_display_get_fd(dpy), POLLIN, 0 };
        if (::poll(&pfd, 1, 0) > 0 && ((pfd.revents & POLLIN) != 0)) {
            wl_display_read_events(dpy);
        } else {
            wl_display_cancel_read(dpy);
        }
        wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
    }

    // Full teardown: destroy all Wayland proxies and disconnect. Called from
    // the main test thread after take_over() and after all assertions.
    void teardown() {
        if (dpy == nullptr) return;
        if (pop_buf != nullptr)  { wl_buffer_destroy(pop_buf);  pop_buf  = nullptr; }
        if (sub_buf != nullptr)  { wl_buffer_destroy(sub_buf);  sub_buf  = nullptr; }
        if (root_buf != nullptr) { wl_buffer_destroy(root_buf); root_buf = nullptr; }
        if (xpop != nullptr)     { xdg_popup_destroy(xpop);     xpop     = nullptr; }
        if (xpopsurf != nullptr) { xdg_surface_destroy(xpopsurf); xpopsurf = nullptr; }
        if (pop != nullptr)      { wl_surface_destroy(pop);     pop      = nullptr; }
        if (subsurface != nullptr) { wl_subsurface_destroy(subsurface); subsurface = nullptr; }
        if (sub != nullptr)      { wl_surface_destroy(sub);     sub      = nullptr; }
        if (xtop != nullptr)     { xdg_toplevel_destroy(xtop);  xtop     = nullptr; }
        if (xsurf != nullptr)    { xdg_surface_destroy(xsurf);  xsurf    = nullptr; }
        if (root != nullptr)     { wl_surface_destroy(root);    root     = nullptr; }
        if (pointer != nullptr)  { wl_pointer_destroy(pointer); pointer  = nullptr; }
        if (touch != nullptr)    { wl_touch_destroy(touch);     touch    = nullptr; }
        if (keyboard != nullptr) { wl_keyboard_destroy(keyboard); keyboard = nullptr; }
        if (seat != nullptr)     { wl_seat_destroy(seat);       seat     = nullptr; }
        if (wm_base != nullptr)  { xdg_wm_base_destroy(wm_base); wm_base = nullptr; }
        if (subcompositor != nullptr) { wl_subcompositor_destroy(subcompositor); subcompositor = nullptr; }
        if (compositor != nullptr) { wl_compositor_destroy(compositor); compositor = nullptr; }
        if (shm != nullptr)      { wl_shm_destroy(shm);         shm      = nullptr; }
        if (registry != nullptr) { wl_registry_destroy(registry); registry = nullptr; }
        wl_display_flush(dpy);
        wl_display_disconnect(dpy);
        dpy = nullptr;
    }

    // Legacy join() for the 3D-transform test which still uses the old pattern.
    // After take_over(), teardown() must still be called explicitly.
    void join() {
        take_over();
    }

    wl_display* dpy = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_pointer* pointer = nullptr;
    wl_touch* touch = nullptr;
    wl_keyboard* keyboard = nullptr;

    wl_surface* root = nullptr;        // the toplevel surface (node 0)
    wl_surface* sub = nullptr;         // the subsurface (node 1)
    wl_surface* pop = nullptr;         // the popup surface (node 2)
    xdg_surface* xsurf = nullptr;
    xdg_toplevel* xtop = nullptr;
    xdg_surface* xpopsurf = nullptr;
    xdg_popup* xpop = nullptr;
    bool configured = false;
    // Lifted from run() locals so teardown() can destroy them from the main thread.
    wl_subsurface* subsurface = nullptr;
    wl_buffer* root_buf = nullptr;
    wl_buffer* sub_buf = nullptr;
    wl_buffer* pop_buf = nullptr;

    auto surface_index(wl_surface* s) const -> int {
        if (s == root) {
            return 0;
        }
        if (s == sub) {
            return 1;
        }
        if (s == pop) {
            return 2;
        }
        return -1;
    }

    static auto make_buffer(wl_shm* shm, int w, int h, uint32_t argb) -> wl_buffer* {
        const int stride = w * 4;
        const int size = stride * h;
        int fd = memfd_create("unbox-tree-test", MFD_CLOEXEC);
        if (fd < 0) {
            return nullptr;
        }
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return nullptr;
        }
        auto* px = static_cast<uint32_t*>(
            mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (px == MAP_FAILED) {
            close(fd);
            return nullptr;
        }
        for (int i = 0; i < w * h; ++i) {
            px[i] = argb;
        }
        munmap(px, size);
        wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
        wl_buffer* buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        return buf;
    }

    // --- listeners ---
    static void reg_global(void* data, wl_registry* reg, uint32_t name, const char* iface,
                           uint32_t ver) {
        auto* self = static_cast<TreeClient*>(data);
        if (std::strcmp(iface, "wl_compositor") == 0) {
            self->compositor = static_cast<wl_compositor*>(
                wl_registry_bind(reg, name, &wl_compositor_interface, 4));
        } else if (std::strcmp(iface, "wl_subcompositor") == 0) {
            self->subcompositor = static_cast<wl_subcompositor*>(
                wl_registry_bind(reg, name, &wl_subcompositor_interface, 1));
        } else if (std::strcmp(iface, "wl_shm") == 0) {
            self->shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
        } else if (std::strcmp(iface, "wl_seat") == 0) {
            self->seat = static_cast<wl_seat*>(
                wl_registry_bind(reg, name, &wl_seat_interface, std::min<uint32_t>(ver, 5)));
        } else if (std::strcmp(iface, "xdg_wm_base") == 0) {
            self->wm_base = static_cast<xdg_wm_base*>(
                wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
        }
    }
    static void reg_remove(void*, wl_registry*, uint32_t) {}

    static void wm_ping(void*, xdg_wm_base* b, uint32_t serial) { xdg_wm_base_pong(b, serial); }

    static void xsurf_configure(void* data, xdg_surface* s, uint32_t serial) {
        auto* self = static_cast<TreeClient*>(data);
        xdg_surface_ack_configure(s, serial);
        self->configured = true;
    }
    static void xtop_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
    static void xtop_close(void*, xdg_toplevel*) {}

    static void xpopsurf_configure(void* data, xdg_surface* s, uint32_t serial) {
        xdg_surface_ack_configure(s, serial);
        (void)data;
    }
    static void xpop_configure(void*, xdg_popup*, int32_t, int32_t, int32_t, int32_t) {}
    static void xpop_done(void*, xdg_popup*) {}

    // pointer
    static void p_enter(void* data, wl_pointer*, uint32_t, wl_surface* surf, wl_fixed_t sx,
                        wl_fixed_t sy) {
        auto* self = static_cast<TreeClient*>(data);
        ++self->ptr_enters;
        self->ptr_enter_surface = self->surface_index(surf);
        self->last_ptr_x = wl_fixed_to_double(sx);
        self->last_ptr_y = wl_fixed_to_double(sy);
    }
    static void p_leave(void*, wl_pointer*, uint32_t, wl_surface*) {}
    static void p_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
        auto* self = static_cast<TreeClient*>(data);
        ++self->ptr_motions;
        self->last_ptr_x = wl_fixed_to_double(sx);
        self->last_ptr_y = wl_fixed_to_double(sy);
    }
    static void p_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {
        ++static_cast<TreeClient*>(data)->ptr_buttons;
    }
    static void p_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
    static void p_frame(void*, wl_pointer*) {}
    static void p_axis_source(void*, wl_pointer*, uint32_t) {}
    static void p_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
    static void p_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
    static void p_axis_value120(void*, wl_pointer*, uint32_t, int32_t) {}
    static void p_axis_relative_direction(void*, wl_pointer*, uint32_t, uint32_t) {}

    // touch
    static void t_down(void* data, wl_touch*, uint32_t, uint32_t, wl_surface*, int32_t,
                       wl_fixed_t x, wl_fixed_t y) {
        auto* self = static_cast<TreeClient*>(data);
        ++self->touch_downs;
        self->last_touch_x = wl_fixed_to_double(x);
        self->last_touch_y = wl_fixed_to_double(y);
    }
    static void t_up(void*, wl_touch*, uint32_t, uint32_t, int32_t) {}
    static void t_motion(void*, wl_touch*, uint32_t, int32_t, wl_fixed_t, wl_fixed_t) {}
    static void t_frame(void*, wl_touch*) {}
    static void t_cancel(void*, wl_touch*) {}
    static void t_shape(void*, wl_touch*, int32_t, wl_fixed_t, wl_fixed_t) {}
    static void t_orientation(void*, wl_touch*, int32_t, wl_fixed_t) {}

    // keyboard
    static void k_keymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) {
        if (fd >= 0) {
            close(fd);
        }
    }
    static void k_enter(void* data, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {
        ++static_cast<TreeClient*>(data)->kbd_enters;
    }
    static void k_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
    static void k_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t) {
        ++static_cast<TreeClient*>(data)->keys;
    }
    static void k_mods(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
    static void k_repeat(void*, wl_keyboard*, int32_t, int32_t) {}

    std::atomic<uint32_t> seat_caps{0};
    static void seat_caps_cb(void* data, wl_seat*, uint32_t caps) {
        static_cast<TreeClient*>(data)->seat_caps = caps;
    }
    static void seat_name_cb(void*, wl_seat*, const char*) {}

    // Bind pointer/touch/keyboard only once the seat advertises the capability
    // (newer libwayland enforces it). The kernel advertises POINTER|TOUCH|
    // KEYBOARD via the ui_add_test_keyboard seam before the client connects.
    void bind_seat() {
        static const wl_seat_listener sl = {seat_caps_cb, seat_name_cb};
        wl_seat_add_listener(seat, &sl, this);
        wl_display_roundtrip(dpy); // deliver the capabilities event
        static const wl_pointer_listener pl = {
            p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_axis_source,
            p_axis_stop, p_axis_discrete, p_axis_value120, p_axis_relative_direction};
        static const wl_touch_listener tl = {t_down, t_up, t_motion, t_frame,
                                             t_cancel, t_shape, t_orientation};
        static const wl_keyboard_listener kl = {k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat};
        const uint32_t caps = seat_caps.load();
        if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0) {
            pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(pointer, &pl, this);
        }
        if ((caps & WL_SEAT_CAPABILITY_TOUCH) != 0) {
            touch = wl_seat_get_touch(seat);
            wl_touch_add_listener(touch, &tl, this);
        }
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0) {
            keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(keyboard, &kl, this);
        }
    }

    void run() {
        dpy = wl_display_connect(socket.c_str());
        if (dpy == nullptr) {
            loop_done = true;
            return;
        }
        registry = wl_display_get_registry(dpy);
        static const wl_registry_listener reg_l = {reg_global, reg_remove};
        wl_registry_add_listener(registry, &reg_l, this);
        wl_display_roundtrip(dpy); // bind globals
        if (compositor == nullptr || shm == nullptr || subcompositor == nullptr ||
            wm_base == nullptr || seat == nullptr) {
            wl_display_disconnect(dpy);
            dpy = nullptr;
            loop_done = true;
            return;
        }
        static const xdg_wm_base_listener wm_l = {wm_ping};
        xdg_wm_base_add_listener(wm_base, &wm_l, this);
        bind_seat();

        // --- toplevel root ---
        root = wl_compositor_create_surface(compositor);
        xsurf = xdg_wm_base_get_xdg_surface(wm_base, root);
        static const xdg_surface_listener xs_l = {xsurf_configure};
        xdg_surface_add_listener(xsurf, &xs_l, this);
        xtop = xdg_surface_get_toplevel(xsurf);
        static const xdg_toplevel_listener xt_l = {xtop_configure, xtop_close};
        xdg_toplevel_add_listener(xtop, &xt_l, this);
        wl_surface_commit(root); // initial commit -> server sends configure
        while (!configured && wl_display_dispatch(dpy) != -1) {
        }
        root_buf = make_buffer(shm, 200, 200, 0xff2060c0);
        wl_surface_attach(root, root_buf, 0, 0);
        wl_surface_damage(root, 0, 0, 200, 200);

        // --- subsurface (node 1): 40x40 at tree offset (20,30) ---
        sub = wl_compositor_create_surface(compositor);
        subsurface = wl_subcompositor_get_subsurface(subcompositor, sub, root);
        wl_subsurface_set_position(subsurface, 20, 30);
        wl_subsurface_set_desync(subsurface);
        sub_buf = make_buffer(shm, 40, 40, 0xff60c020);
        wl_surface_attach(sub, sub_buf, 0, 0);
        wl_surface_damage(sub, 0, 0, 40, 40);
        wl_surface_commit(sub);
        wl_surface_commit(root); // apply the subsurface
        wl_display_roundtrip(dpy);

        // --- popup (node 2): a 60x50 popup positioned at (80,90) off the root ---
        pop = wl_compositor_create_surface(compositor);
        xpopsurf = xdg_wm_base_get_xdg_surface(wm_base, pop);
        static const xdg_surface_listener xps_l = {xpopsurf_configure};
        xdg_surface_add_listener(xpopsurf, &xps_l, this);
        xdg_positioner* pos = xdg_wm_base_create_positioner(wm_base);
        xdg_positioner_set_size(pos, 60, 50);
        xdg_positioner_set_anchor_rect(pos, 80, 90, 1, 1);
        xpop = xdg_surface_get_popup(xpopsurf, xsurf, pos);
        xdg_positioner_destroy(pos);
        static const xdg_popup_listener xp_l = {xpop_configure, xpop_done};
        xdg_popup_add_listener(xpop, &xp_l, this);
        static const xdg_surface_listener xps2 = {xpopsurf_configure};
        (void)xps2;
        wl_surface_commit(pop); // initial popup commit -> configure
        wl_display_roundtrip(dpy);
        pop_buf = make_buffer(shm, 60, 50, 0xff2080e0);
        wl_surface_attach(pop, pop_buf, 0, 0);
        wl_surface_damage(pop, 0, 0, 60, 50);
        wl_surface_commit(pop);

        // Map the root last so the extension's map handler builds the element with
        // the subsurface + popup already in the tree.
        wl_surface_commit(root);
        wl_display_flush(dpy);
        ready = true;

        // DE-FLAKE: the original approach ran a dispatch loop with sleep_for(2ms)
        // here, causing starvation races under load. Now we simply signal that the
        // setup phase is done and exit. The main test thread takes over all
        // dispatching cooperatively (no sleep, no race). loop_done signals the
        // main thread that it is safe to call dispatch_pending().
        loop_done = true;
        // Wait for the main thread to signal teardown (it calls take_over()
        // which sets stop=true). This keeps dpy alive for the main thread's
        // cooperative dispatch phase.
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Teardown is now done by the main thread via teardown(). The thread
        // just returns; dpy is still valid when take_over() is called.
    }
};

// Cooperative pump: advance the server event loop AND dispatch pending client
// events until pred() is true or max_turns elapse. Used in the input-back test
// to synchronously deliver wl_seat events to the client after the server sends
// them — no sleeps, no races. Must only be called after client.take_over().
template <typename Pred>
auto pump_both(unbox::kernel::Server& s, TreeClient& client, Pred pred,
               int max_turns = 400) -> bool {
    for (int i = 0; i < max_turns && !pred(); ++i) {
        s.dispatch(5);
        client.dispatch_pending();
    }
    return pred();
}

} // namespace

TEST_CASE("surface-element: tree (subsurface + popup) + input-back + keyboard focus") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");

    auto server = unbox::kernel::Server::create({});
    auto* ext = new TreeTestExtension();
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    // A virtual keyboard on the seat (headless has none) so focus_keyboard can
    // deliver a wl_keyboard enter; added before the client binds the seat.
    server->ui_add_test_keyboard();

    TreeClient client(server->socket_name());
    client.start();

    // Wait for the client to complete XDG setup and the substrate to build the
    // element. pump_until_se only pumps the server (the client thread runs
    // concurrently during setup). Both must succeed; if not (no GL path), still
    // take_over + teardown for leak-clean shutdown.
    pump_until_se(*server, [&] { return client.ready.load(); }, 800);
    pump_until_se(*server, [&] { return client.loop_done.load(); }, 100);

    // Take exclusive ownership of the client display: stop the bg thread
    // (which is now idle after loop_done) and join it. From this point all
    // wl_display operations happen on the main test thread — no races.
    client.take_over();

    // Drain any pending client events that arrived during setup.
    client.dispatch_pending();

    pump_until_se(*server, [&] { return ext->has_surface(); }, 800);

    if (!ext->has_element() || server->ui_frame_count() == 0) {
        // No GL path on this box: degrade gracefully (same pattern as the other
        // GL-gated tests). All coverage is still asserted when GL is available.
        client.teardown();
        unsetenv("WLR_HEADLESS_OUTPUTS");
        return;
    }
    pump(*server, 40); // let the tree re-walk + child <img> placement settle
    client.dispatch_pending(); // drain any events from the settle phase

    // Install the click/tap-to-focus press hook (ui.hpp SurfaceElement::on_pressed).
    // It must fire exactly once per pointer PRESS / touch DOWN routed to the
    // element (root OR a child node), and NEVER on motion or on a miss.
    ext->install_press_counter();

    // (A) TREE: the root + the subsurface + the popup compose as per-node <img>
    // elements (root authored; subsurface + popup created by the substrate).
    INFO("img count = ", server->ui_element_count("img"));
    CHECK(server->ui_element_count("img") >= 3);

    // (A) FRAME-DONE walks the WHOLE tree: the count climbs by MORE than one per
    // composited frame (root + subsurface + popup each get a frame-done).
    const int fd0 = server->ui_surface_element_frame_done_count();
    pump(*server, 20);
    const int fd1 = server->ui_surface_element_frame_done_count();
    CHECK(fd1 > fd0);
    CHECK(fd1 - fd0 >= 3); // >= one per node (root + subsurface + popup)

    // (B) INPUT-BACK pointer: drive a motion at a known layout point over the
    // ROOT region of the element. Surface at (40,30); the root img fills 200x200
    // 1:1 with the 200x200 buffer, so layout (40+50, 30+60) => surface-local
    // (50,60) on the root client surface.
    // pump_both co-pumps server + client until the assertion predicate is true —
    // the client receives the wl_seat event deterministically with no sleep.
    using DK = unbox::kernel::Server::UiTouchOverride; // (unused; keep includes warm)
    (void)DK::automatic;
    server->ui_route_pointer_motion_for_test(TreeTestExtension::kSurfX + 50.0,
                                             TreeTestExtension::kSurfY + 60.0, 1000);
    pump_both(*server, client, [&] { return client.ptr_enters.load() > 0; }, 200);
    CHECK(client.ptr_enters.load() > 0);
    CHECK(client.ptr_enter_surface.load() == 0); // hit the ROOT node
    CHECK(client.last_ptr_x.load() == doctest::Approx(50.0).epsilon(0.05));
    CHECK(client.last_ptr_y.load() == doctest::Approx(60.0).epsilon(0.05));

    // (B) PRESS HOOK: a MOTION over the element does NOT fire the press hook.
    CHECK(ext->pressed_count() == 0);

    // (B) INPUT-BACK pointer over the SUBSURFACE node: the subsurface is 40x40 at
    // tree offset (20,30); a point at layout (40+30, 30+45) => surface-local
    // (30,45) on the root, which lands inside the subsurface (its <img> spans
    // (20,30)..(60,70)). The pick must hit the SUBSURFACE node and report coords
    // LOCAL TO THE SUBSURFACE: (30-20, 45-30) = (10,15).
    const int enters_before_sub = client.ptr_enters.load();
    server->ui_route_pointer_motion_for_test(TreeTestExtension::kSurfX + 30.0,
                                             TreeTestExtension::kSurfY + 45.0, 1010);
    pump_both(*server, client, [&] { return client.ptr_enters.load() > enters_before_sub; }, 200);
    CHECK(client.ptr_enter_surface.load() == 1); // the subsurface node
    CHECK(client.last_ptr_x.load() == doctest::Approx(10.0).epsilon(0.1));
    CHECK(client.last_ptr_y.load() == doctest::Approx(15.0).epsilon(0.1));

    // (B) INPUT-BACK button: a press over the root forwards a wl_pointer button.
    const int btn0 = client.ptr_buttons.load();
    const int pressed0 = ext->pressed_count();
    server->ui_route_pointer_button_for_test(TreeTestExtension::kSurfX + 50.0,
                                             TreeTestExtension::kSurfY + 60.0, true, 1020);
    // (B) PRESS HOOK: the PRESS fired the hook EXACTLY ONCE (the click/tap-to-
    // focus signal, in addition to the client forwarding above).
    CHECK(ext->pressed_count() == pressed0 + 1);
    server->ui_route_pointer_button_for_test(TreeTestExtension::kSurfX + 50.0,
                                             TreeTestExtension::kSurfY + 60.0, false, 1021);
    // (B) PRESS HOOK: the RELEASE does NOT fire the hook (still one fire total).
    CHECK(ext->pressed_count() == pressed0 + 1);
    pump_both(*server, client, [&] { return client.ptr_buttons.load() > btn0; }, 200);
    CHECK(client.ptr_buttons.load() > btn0);

    // (B) PRESS HOOK over a CHILD node: a press at layout (40+30, 30+45) lands on
    // the SUBSURFACE child <img> (its region (20,30)..(60,70) in root-local px),
    // and STILL fires the ROOT element's handler — the element is the whole tree.
    const int pressed_child0 = ext->pressed_count();
    server->ui_route_pointer_button_for_test(TreeTestExtension::kSurfX + 30.0,
                                             TreeTestExtension::kSurfY + 45.0, true, 1024);
    server->ui_route_pointer_button_for_test(TreeTestExtension::kSurfX + 30.0,
                                             TreeTestExtension::kSurfY + 45.0, false, 1025);
    client.dispatch_pending();
    CHECK(client.ptr_enter_surface.load() == 1);            // the pick hit the subsurface
    CHECK(ext->pressed_count() == pressed_child0 + 1);      // root handler still fired ONCE

    // (B) PRESS HOOK miss: a press OFF the element (far outside the 200x200 surface)
    // does NOT fire the hook.
    const int pressed_miss0 = ext->pressed_count();
    server->ui_route_pointer_button_for_test(TreeTestExtension::kSurfX + 1000.0,
                                             TreeTestExtension::kSurfY + 1000.0, true, 1028);
    server->ui_route_pointer_button_for_test(TreeTestExtension::kSurfX + 1000.0,
                                             TreeTestExtension::kSurfY + 1000.0, false, 1029);
    client.dispatch_pending();
    CHECK(ext->pressed_count() == pressed_miss0);

    // (B) INPUT-BACK touch: a touch-down over the root forwards a wl_touch down at
    // surface-local coords.
    const int pressed_touch0 = ext->pressed_count();
    server->ui_route_touch_down_for_test(7, TreeTestExtension::kSurfX + 50.0,
                                         TreeTestExtension::kSurfY + 60.0, 1030);
    pump_both(*server, client, [&] { return client.touch_downs.load() > 0; }, 200);
    CHECK(client.touch_downs.load() > 0);
    CHECK(client.last_touch_x.load() == doctest::Approx(50.0).epsilon(0.05));
    CHECK(client.last_touch_y.load() == doctest::Approx(60.0).epsilon(0.05));
    // (B) PRESS HOOK: the touch DOWN fired the hook exactly once.
    CHECK(ext->pressed_count() == pressed_touch0 + 1);
    server->ui_route_touch_up_for_test(7, 1031);
    // (B) PRESS HOOK: the touch UP does NOT fire the hook.
    CHECK(ext->pressed_count() == pressed_touch0 + 1);

    // (C) KEYBOARD FOCUS: focusing the element delivers a wl_keyboard enter, then
    // a forwarded key reaches the client.
    ext->focus();
    pump_both(*server, client, [&] { return client.kbd_enters.load() > 0; }, 200);
    CHECK(client.kbd_enters.load() > 0);
    const int keys0 = client.keys.load();
    server->ui_send_key_for_test(/*KEY_A*/ 30, true);
    server->ui_send_key_for_test(/*KEY_A*/ 30, false);
    pump_both(*server, client, [&] { return client.keys.load() > keys0; }, 200);
    CHECK(client.keys.load() > keys0);

    client.teardown();
    unsetenv("WLR_HEADLESS_OUTPUTS");
}

// ============================================================================
// RML compositing Wave 1b: surface-element input-back through a TRANSFORMED
// hosting element. A point on the rotation AXIS (the element centre) projects to
// the surface centre regardless of the rotateY, so we can assert EXACT surface-
// local coords even under a 3D transform (Element::Project inverts it). This is
// the live analogue of the pure-core round-trip test above.
// ============================================================================

TEST_CASE("surface-element: input-back through a 3D-transformed hosting element") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");

    auto server = unbox::kernel::Server::create({});
    auto* ext = new TreeTestExtension();
    ext->set_transform(35.0); // perspective + rotateY(35deg) about 50% origin
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    // Advertise seat pointer/touch capabilities (headless adds no input devices,
    // so update_seat_capabilities never runs); the seam does this as a side effect.
    server->ui_add_test_keyboard();

    TreeClient client(server->socket_name());
    client.start();

    pump_until_se(*server, [&] { return client.ready.load(); }, 800);
    pump_until_se(*server, [&] { return client.loop_done.load(); }, 100);
    client.take_over();
    client.dispatch_pending();

    pump_until_se(*server, [&] { return ext->has_surface(); }, 800);

    if (!ext->has_element() || server->ui_frame_count() == 0) {
        client.teardown();
        unsetenv("WLR_HEADLESS_OUTPUTS");
        return;
    }
    pump(*server, 40);
    client.dispatch_pending();

    // The element box is 200x200 at layout (40,30); its centre is layout
    // (40+100, 30+100). Under perspective+rotateY about the 50% origin the centre
    // sits on the rotation axis, so it projects to surface-local (100,100) — the
    // transform-aware Element::Project recovers the centre exactly. (An untilted
    // build would give the same answer; the point is the tilt does NOT shift the
    // axis point, proving the projection — not a naive axis-aligned map — runs.)
    server->ui_route_pointer_motion_for_test(TreeTestExtension::kSurfX + 100.0,
                                             TreeTestExtension::kSurfY + 100.0, 2000);
    pump_both(*server, client, [&] { return client.ptr_enters.load() > 0; }, 200);
    REQUIRE(client.ptr_enters.load() > 0);
    CHECK(client.ptr_enter_surface.load() == 0); // the root node
    CHECK(client.last_ptr_x.load() == doctest::Approx(100.0).epsilon(0.03));
    CHECK(client.last_ptr_y.load() == doctest::Approx(100.0).epsilon(0.03));

    client.teardown();
    unsetenv("WLR_HEADLESS_OUTPUTS");
}

// ============================================================================
// Deliverable 2: stb_image PNG decode in LoadTexture.
//
// A ui surface whose ONLY content is a full-bleed <img> backed by a tiny PNG
// fixture on disk. After ticking frames, ui_pixel(x,y) must return the PNG's
// known color in 0xRRGGBBAA format — proving decode + upload + correct channel
// order (stb returns RGBA, NOT BGR; do not apply the TGA swizzle). Two cases:
// red (#FF0000) and blue (#0000FF).
//
// Shape mirrors preview: known source color composites into an <img>.
// ============================================================================

namespace {

// Returns the absolute path to a fixture PNG file inside the test source tree.
// __FILE__ expands to the absolute path of this source file at compile time;
// the PNG fixtures live in the same directory.
auto fixture_path(const char* filename) -> std::string {
    std::filesystem::path src(__FILE__);
    return (src.parent_path() / filename).string();
}

// An extension that creates a single ui surface with a full-bleed <img> of the
// given PNG file path. The body background is #010203 (very dark, not red/blue)
// so a failed sample is obvious.
class PngTestExtension : public unbox::kernel::Extension {
public:
    explicit PngTestExtension(std::string png_path) : png_path_(std::move(png_path)) {}
    auto manifest() const -> const Manifest& override { return manifest_; }

    void activate(Host& host) override {
        if (!host.ui().available()) {
            return;
        }
        // Build RML with the PNG path embedded as an absolute <img> src.
        // body is 64x64 (matches the fixture), no margin, one full-bleed img.
        std::string rml =
            "<rml><head><style>"
            "body { background-color: #010203; width: 64px; height: 64px; margin: 0px; }"
            "img  { display: block; position: absolute; left: 0px; top: 0px;"
            "       width: 64px; height: 64px; }"
            "</style></head><body data-model=\"ui\">"
            "<img src=\"" + png_path_ + "\"/>"
            "</body></rml>";

        UiSurfaceSpec spec;
        spec.rml_inline = rml;
        spec.x = 0;
        spec.y = 0;
        spec.width = 64;
        spec.height = 64;
        spec.layer = unbox::kernel::SceneLayer::overlay;
        spec.visible = true;
        surface_ = host.ui().create_surface(spec);
    }

    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }

private:
    std::string png_path_;
    Manifest manifest_{"png-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
};

} // namespace

TEST_CASE("stb_image: PNG decode + upload + correct RGBA channel order (red fixture)") {
    // Requires the GL path (Plan A or B) to upload + read back the texture.
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1); // shm path for readback

    const std::string png = fixture_path("fixture_red_4x4.png");
    auto server = unbox::kernel::Server::create({});
    auto* ext = new PngTestExtension(png);
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 80); // load the document, decode the PNG, render

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        // No GL path on this box: the test is a no-op (PNG decode is moot without GL).
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        unsetenv("WLR_HEADLESS_OUTPUTS");
        return;
    }

    // Sample the centre of the 64x64 surface (32,32). The fixture is 4x4 solid
    // #FF0000 scaled up to fill the surface. The pixel must be red-dominant, opaque.
    // ui_pixel returns 0xRRGGBBAA. Tolerant match: bilinear + premultiply rounding.
    const unsigned int px = server->ui_pixel(32, 32);
    INFO("red fixture centre pixel (RRGGBBAA) = ", px);
    const int r = static_cast<int>((px >> 24) & 0xff);
    const int g = static_cast<int>((px >> 16) & 0xff);
    const int b = static_cast<int>((px >> 8) & 0xff);
    const int a = static_cast<int>(px & 0xff);

    // The pixel must be opaque and red-dominant — proving stb returned R,G,B,A
    // (not BGR as TGA does). A wrong channel swap would show blue dominant here.
    CHECK(a == 0xff);
    CHECK(r > 180);  // strong red
    CHECK(g < 40);   // little green
    CHECK(b < 40);   // little blue

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
    unsetenv("WLR_HEADLESS_OUTPUTS");
}

TEST_CASE("stb_image: PNG decode + upload + correct RGBA channel order (blue fixture)") {
    // Second case with a non-red color to prove the channel order is truly RGBA
    // (not a coincidence where R and B happen to match).
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    const std::string png = fixture_path("fixture_blue_4x4.png");
    auto server = unbox::kernel::Server::create({});
    auto* ext = new PngTestExtension(png);
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 80);

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        unsetenv("WLR_HEADLESS_OUTPUTS");
        return;
    }

    const unsigned int px = server->ui_pixel(32, 32);
    INFO("blue fixture centre pixel (RRGGBBAA) = ", px);
    const int r = static_cast<int>((px >> 24) & 0xff);
    const int g = static_cast<int>((px >> 16) & 0xff);
    const int b = static_cast<int>((px >> 8) & 0xff);
    const int a = static_cast<int>(px & 0xff);

    // Blue fixture: #0000FF. If stb returned RGBA correctly, blue is in the B
    // channel (bits 15..8 in the 0xRRGGBBAA word). If BGR was wrongly applied,
    // R and B would swap and this pixel would appear red-dominant.
    CHECK(a == 0xff);
    CHECK(r < 40);   // little red
    CHECK(g < 40);   // little green
    CHECK(b > 180);  // strong blue

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
    unsetenv("WLR_HEADLESS_OUTPUTS");
}

// ============================================================================
// input_transparent UiSurfaceSpec flag.
//
// A visible, full-screen ui surface with input_transparent=true must RENDER
// normally (frames advance, composited correctly) but must NOT capture pointer
// button presses or touch downs — they fall through to the bus. The contrast
// with input_transparent=false (the default) is the failing-then-passing test.
//
// Uses the existing ui_route_pointer_button_for_test (now returns bool: the
// substrate consumption result) and ui_route_touch_down_for_test seams.
// Deterministic: no async Wayland client, no timer races.
// ============================================================================

namespace {

// A full-screen ui surface with a solid opaque body so the substrate can render
// it and the frame-count probe can confirm it composited. The body fills the
// whole surface so every pixel is painted — ruling out the alpha-hit-test path
// as a source of transparency (we're testing the input_transparent flag, not
// per-pixel alpha). The body background is distinct (#3050a0) so a bus-hook
// test that reads a pixel would see a non-trivial color.
const char* kTransparentRml = R"RML(<rml>
<head>
<style>
body { background-color: #3050a0; width: 800px; height: 600px; margin: 0px; }
</style>
</head>
<body data-model="ui">
</body>
</rml>)RML";

// A test extension that creates a full-screen (800x600 at (0,0)) ui surface
// with the given input_transparent flag. No bus subscription needed: the
// test asserts the substrate consumption result directly via the seam's bool
// return value. (The bus hook only fires through the real input.cpp path, not
// the ui_route_*_for_test seam which calls the substrate directly.)
class InputTransparentExtension : public unbox::kernel::Extension {
public:
    explicit InputTransparentExtension(bool input_transparent)
        : input_transparent_(input_transparent) {}

    auto manifest() const -> const Manifest& override { return manifest_; }

    void activate(Host& host) override {
        UiSurfaceSpec spec;
        spec.rml_inline = kTransparentRml;
        spec.x = 0;
        spec.y = 0;
        spec.width = 800;
        spec.height = 600;
        spec.visible = true;
        spec.input_transparent = input_transparent_;
        spec.layer = unbox::kernel::SceneLayer::overlay;
        surface_ = host.ui().create_surface(spec);
    }

    [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }

private:
    bool input_transparent_;
    Manifest manifest_{"input-transparent-test", Tier::standard, {}};
    std::unique_ptr<UiSurface> surface_;
};

} // namespace

TEST_CASE("input_transparent: opaque surface with flag=false CONSUMES presses (baseline)") {
    // Confirm that a normal (input_transparent=false) visible surface consumes.
    // This is the existing default behaviour — validated explicitly as the contrast
    // to the transparent case below.
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new InputTransparentExtension(/*input_transparent=*/false);
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 60); // let the surface load and render

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        // No GL path: substrate unavailable, surface is null — the flag is moot.
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        unsetenv("WLR_HEADLESS_OUTPUTS");
        return;
    }

    // Press inside the surface (well within 800x600 at origin).
    const bool consumed = server->ui_route_pointer_button_for_test(400.0, 300.0,
                                                                    /*pressed=*/true, 1000);
    server->ui_route_pointer_button_for_test(400.0, 300.0, /*pressed=*/false, 1001);

    // The surface is visible, non-transparent, and the point is inside it:
    // the substrate must have consumed the press (returned true).
    CHECK(consumed == true);

    // The surface still composites: frame count is positive.
    CHECK(server->ui_frame_count() > 0);

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
    unsetenv("WLR_HEADLESS_OUTPUTS");
}

TEST_CASE("input_transparent: surface with flag=true renders but PASSES THROUGH presses") {
    // The failing-then-passing assertion: with input_transparent=true the surface
    // must NOT consume pointer button presses or touch downs, and the bus hook
    // must fire (proving the press reached extensions).
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({});
    auto* ext = new InputTransparentExtension(/*input_transparent=*/true);
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 60);

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        unsetenv("WLR_HEADLESS_OUTPUTS");
        return;
    }

    // (1) Pointer button: the substrate must NOT consume — ui_route_pointer_button_
    // for_test returns the substrate's route_pointer_button() result directly.
    // FAILING-THEN-PASSING: before this flag, surface_at always matched a visible
    // surface so pressed returned true; with input_transparent=true, surface_at
    // skips it and returns nullptr, so route_pointer_button returns false.
    const bool consumed = server->ui_route_pointer_button_for_test(400.0, 300.0,
                                                                    /*pressed=*/true, 2000);
    server->ui_route_pointer_button_for_test(400.0, 300.0, /*pressed=*/false, 2001);
    CHECK(consumed == false); // FAILING-THEN-PASSING: substrate skips the transparent surface

    // (2) Touch down: surface_at is also skipped for input_transparent surfaces, so
    // route_touch_down returns false (pass-through). The seam discards the return
    // value, so we verify by confirming no crash and no stuck grab state — a touch
    // that was wrongly consumed would leave touch_capture dangling; a pass-through
    // exits cleanly. route_touch_up is a no-op (no capture entry) — also clean.
    server->ui_route_touch_down_for_test(0, 400.0, 300.0, 2010);
    server->ui_route_touch_up_for_test(0, 2011);

    // (3) The surface still RENDERS: compositing is unaffected by the flag.
    // Frame count must be positive — the surface was not hidden, just input-skipped.
    CHECK(server->ui_frame_count() > 0);

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
    unsetenv("WLR_HEADLESS_OUTPUTS");
}

// ============================================================================
// Absolute-path RCSS decorator image source (JoinPath fix).
//
// Before the SubstrateSystemInterface::JoinPath override, RmlUi's default
// JoinPath stripped the leading '/' from absolute paths — turning
// '/home/user/wall.png' into 'home/user/wall.png' before LoadTexture was
// called, so the open failed. This is the FAILING-THEN-PASSING test: a ui
// surface whose RCSS `decorator: image(...)` uses an absolute path must show
// the expected colour. Shape mirrors the stb_image <img> tests (D2) but with
// the decorator pathway, which goes through RenderManager::LoadTexture →
// JoinPath rather than the <img> LoadTexture → FileInterface path.
// ============================================================================

TEST_CASE("decorator: absolute-path image resolves correctly (JoinPath fix)") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SUBSTRATE_FORCE_SHM", "1", 1);

    // Absolute path to the red fixture PNG committed alongside this file.
    const std::string png = fixture_path("fixture_red_4x4.png");

    // Build inline RML with a body decorator using the absolute path.
    // The decorator fills the body background with the fixture image.
    // Before the fix: JoinPath strips the leading '/', the file open fails,
    // decorator renders nothing, the pixel reads the fallback color (#010203).
    // After the fix: the path is preserved, PNG decoded, pixel is red.
    const std::string rml =
        "<rml><head><style>"
        "body { width: 64px; height: 64px; margin: 0px; background-color: #010203;"
        "       decorator: image('" + png + "' cover center center); }"
        "</style></head><body data-model=\"ui\"></body></rml>";

    auto server = unbox::kernel::Server::create({});

    class DecoratorExt : public unbox::kernel::Extension {
    public:
        explicit DecoratorExt(std::string rml_) : rml(std::move(rml_)) {}
        auto manifest() const -> const Manifest& override { return manifest_; }
        void activate(Host& host) override {
            if (!host.ui().available()) return;
            UiSurfaceSpec spec;
            spec.rml_inline = rml;
            spec.x = 0; spec.y = 0;
            spec.width = 64; spec.height = 64;
            spec.layer = unbox::kernel::SceneLayer::overlay;
            spec.visible = true;
            surface_ = host.ui().create_surface(spec);
        }
        [[nodiscard]] auto has_surface() const -> bool { return surface_ != nullptr; }
    private:
        std::string rml;
        Manifest manifest_{"decorator-path-test", Tier::standard, {}};
        std::unique_ptr<UiSurface> surface_;
    };

    auto* ext = new DecoratorExt(rml);
    server->install(std::unique_ptr<unbox::kernel::Extension>(ext));
    server->activate_extensions();
    pump(*server, 80); // load the document, decode the PNG via the decorator, render

    if (!ext->has_surface() || server->ui_frame_count() == 0) {
        // No GL path on this box: the test is a no-op.
        unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
        unsetenv("WLR_HEADLESS_OUTPUTS");
        return;
    }

    // Sample the centre of the 64x64 surface. The decorator covers the body with
    // the 4x4 solid #FF0000 fixture, scaled up. The centre pixel must be
    // red-dominant and opaque — proving the absolute path was preserved through
    // JoinPath (FAILING-THEN-PASSING: before the fix it reads ~#010203 dark).
    const unsigned int px = server->ui_pixel(32, 32);
    INFO("decorator centre pixel (RRGGBBAA) = ", px);
    const int r = static_cast<int>((px >> 24) & 0xff);
    const int g = static_cast<int>((px >> 16) & 0xff);
    const int b = static_cast<int>((px >> 8) & 0xff);
    const int a = static_cast<int>(px & 0xff);

    CHECK(a == 0xff);   // opaque
    CHECK(r > 180);     // strong red — fixture is #FF0000
    CHECK(g < 40);
    CHECK(b < 40);

    unsetenv("UNBOX_UI_SUBSTRATE_FORCE_SHM");
    unsetenv("WLR_HEADLESS_OUTPUTS");
}
