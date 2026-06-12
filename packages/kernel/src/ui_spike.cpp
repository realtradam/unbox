#include "ui_spike.hpp"

#include "rmlui_renderer_gl3.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/SystemInterface.h>

// The kernel owns GL; system EGL/GLES headers are allowed here (brief).
// wlr.hpp (via ui_spike.hpp) already pulled <EGL/egl.h>+<EGL/eglext.h>
// through wlr/render/egl.h, and GLES through the adapted renderer; we add
// the dmabuf import entrypoints explicitly.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h> // glEGLImageTargetTexture2DOES

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace unbox::kernel {

namespace {

constexpr int kSpikeWidth = 320;
constexpr int kSpikeHeight = 200;
constexpr int kSpikeX = 40; // layout-space origin of the node
constexpr int kSpikeY = 40;

// DRM FourCC for the buffers we allocate / wrap. ARGB8888 is universally
// render+sample-able and matches RMLUi's premultiplied RGBA8 output once
// channel order is accounted for. (FourCC AR24 = little-endian B,G,R,A.)
constexpr std::uint32_t kDrmFormatArgb8888 = 0x34325241; // 'AR24'

// Distinctive solid bands at the document's top and bottom edges. They are
// full-width, unique colors that appear NOWHERE else in the document, so the
// orientation assertion can prove the submitted buffer is upright: the top
// band must land in the TOP rows of the buffer, the bottom band in the
// BOTTOM rows. (A vertical flip would swap them — the bug.)
constexpr int kBandHeight = 12; // px, each band
// Top band #18e0a0 (teal-green); bottom band #e09018 (amber). Stored as the
// RGB byte triplets the Plan-B readback produces (R,G,B order).
constexpr std::uint8_t kTopBandRGB[3] = {0x18, 0xe0, 0xa0};
constexpr std::uint8_t kBottomBandRGB[3] = {0xe0, 0x90, 0x18};

// In-memory hello-world document. Distinctive top/bottom bands (orientation
// proof), a title, a live frame counter via data binding, and a button that
// reacts to hover/click (input proof).
const char* kHelloRml = R"RML(<rml>
<head>
<style>
body { font-family: "Noto Sans"; background: #1e2230; color: #e8ecff;
       width: 320px; height: 200px; }
#topband    { display: block; width: 320px; height: 12px; background: #18e0a0; }
#bottomband { display: block; width: 320px; height: 12px; background: #e09018;
              position: absolute; bottom: 0px; left: 0px; }
h1 { font-size: 22px; margin: 16px; color: #9ecbff; }
p  { font-size: 15px; margin: 0 16px 12px 16px; }
button { font-size: 15px; margin: 16px; padding: 8px 16px;
         background: #3a4670; color: #ffffff; border-radius: 6px; }
button:hover { background: #5468b0; }
button:active { background: #7e93e0; }
</style>
</head>
<body data-model="spike">
<div id="topband"></div>
<h1>unbox ui spike</h1>
<p>frame {{frame}}</p>
<button>{{label}}</button>
<div id="bottomband"></div>
</body>
</rml>)RML";

// --- SystemInterface: elapsed time + route RmlUi logs to wlr_log ----------

class SpikeSystemInterface final : public Rml::SystemInterface {
public:
    auto GetElapsedTime() -> double override {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (start_ == 0.0) {
            start_ = static_cast<double>(now.tv_sec) + now.tv_nsec / 1e9;
        }
        return (static_cast<double>(now.tv_sec) + now.tv_nsec / 1e9) - start_;
    }

    auto LogMessage(Rml::Log::Type type, const Rml::String& message) -> bool override {
        const wlr_log_importance imp = (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT)
                                           ? WLR_ERROR
                                           : (type == Rml::Log::LT_WARNING ? WLR_INFO : WLR_DEBUG);
        wlr_log(imp, "[rmlui] %s", message.c_str());
        return true;
    }

private:
    double start_ = 0.0;
};

// --- A data-ptr wlr_buffer wrapping heap memory (Plan B target) -----------
//
// The wlr GLES2 renderer can sample a WLR_BUFFER_CAP_DATA_PTR buffer (it
// uploads via begin/end_data_ptr_access). Works on both the headless/pixman
// and GPU/gles2 backends, which is why this is the robust spike landing.

struct ShmBuffer {
    wlr_buffer base{};
    std::vector<std::uint8_t> data;
    std::uint32_t format = kDrmFormatArgb8888;
    std::size_t stride = 0;
    bool dropped = false;
};

void shm_buffer_destroy(wlr_buffer* wlr_buf) {
    auto* buf = reinterpret_cast<ShmBuffer*>(wlr_buf);
    wlr_buffer_finish(&buf->base);
    delete buf;
}

auto shm_buffer_begin_data_ptr_access(wlr_buffer* wlr_buf, std::uint32_t /*flags*/, void** data,
                                      std::uint32_t* format, std::size_t* stride) -> bool {
    auto* buf = reinterpret_cast<ShmBuffer*>(wlr_buf);
    *data = buf->data.data();
    *format = buf->format;
    *stride = buf->stride;
    return true;
}

void shm_buffer_end_data_ptr_access(wlr_buffer* /*wlr_buf*/) {}

const wlr_buffer_impl kShmBufferImpl = {
    .destroy = shm_buffer_destroy,
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = shm_buffer_begin_data_ptr_access,
    .end_data_ptr_access = shm_buffer_end_data_ptr_access,
};

auto make_shm_buffer(int width, int height) -> ShmBuffer* {
    auto* buf = new ShmBuffer();
    buf->stride = static_cast<std::size_t>(width) * 4;
    buf->data.assign(buf->stride * static_cast<std::size_t>(height), 0);
    wlr_buffer_init(&buf->base, &kShmBufferImpl, width, height);
    return buf;
}

} // namespace

// --- Impl -----------------------------------------------------------------

struct UiSpike::Impl {
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLContext saved_context = EGL_NO_CONTEXT;
    EGLSurface saved_draw = EGL_NO_SURFACE;
    EGLSurface saved_read = EGL_NO_SURFACE;

    wlr_allocator* allocator = nullptr; // borrowed (server-owned)
    wlr_renderer* renderer = nullptr;   // borrowed (server-owned)

    // Sibling-context GL objects.
    GLuint fbo = 0;
    GLuint color_tex = 0;

    // Plan A (dmabuf) state — populated only if A engages.
    wlr_buffer* dmabuf = nullptr; // the swapchain-acquired render target
    EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;

    // Plan B (shm copy) state.
    ShmBuffer* shm = nullptr;
    std::vector<std::uint8_t> readback; // glReadPixels scratch

    // RMLUi.
    std::unique_ptr<SpikeSystemInterface> system;
    std::unique_ptr<RenderInterface_GL3> render_iface;
    Rml::Context* context = nullptr;       // owned by Rml (RemoveContext)
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle model;

    // Data-bound document state.
    int frame = 0;
    Rml::String label = "hover me";

    // Scene.
    wlr_scene_buffer* scene_buffer = nullptr;

    Plan plan = Plan::Disabled;
    int frame_count = 0;

    // EGL extension entrypoints (loaded once).
    PFNEGLCREATEIMAGEKHRPROC egl_create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_image_target_texture = nullptr;

    bool make_current() {
        saved_context = eglGetCurrentContext();
        saved_draw = eglGetCurrentSurface(EGL_DRAW);
        saved_read = eglGetCurrentSurface(EGL_READ);
        return eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context) == EGL_TRUE;
    }

    void restore_current() {
        eglMakeCurrent(egl_display, saved_draw, saved_read, saved_context);
    }

    bool init(wlr_scene_tree* parent, EGLDisplay display, wlr_allocator* alloc,
              wlr_renderer* rend);
    bool try_plan_a();
    void setup_plan_b();
    void render_locked();
    void teardown();
};

bool UiSpike::Impl::init(wlr_scene_tree* parent, EGLDisplay display, wlr_allocator* alloc,
                         wlr_renderer* rend) {
    egl_display = display;
    allocator = alloc;
    renderer = rend;

    // 1. Sibling GLES 3.2 context sharing the wlr EGLDisplay. No GL object
    //    sharing — buffers cross via dmabuf/EGLImage or CPU copy only, so we
    //    do NOT pass the wlr context as share_context.
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        wlr_log(WLR_ERROR, "ui-spike: eglBindAPI(ES) failed");
        return false;
    }
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,     8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint num_config = 0;
    if (eglChooseConfig(egl_display, config_attribs, &config, 1, &num_config) != EGL_TRUE ||
        num_config < 1) {
        wlr_log(WLR_ERROR, "ui-spike: eglChooseConfig found no ES3 config");
        return false;
    }
    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE,
    };
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        wlr_log(WLR_ERROR, "ui-spike: eglCreateContext(ES 3.2) failed (0x%x)", eglGetError());
        return false;
    }

    if (!make_current()) {
        wlr_log(WLR_ERROR, "ui-spike: eglMakeCurrent (surfaceless) failed (0x%x)", eglGetError());
        restore_current();
        return false;
    }

    // Load EGLImage entrypoints for the Plan-A attempt.
    egl_create_image =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    egl_destroy_image =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    gl_image_target_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    // 2. RMLUi render interface (our GLES3-adapted GL3 backend).
    Rml::String gl_msg;
    if (!RmlGL3::Initialize(&gl_msg)) {
        wlr_log(WLR_ERROR, "ui-spike: RmlGL3::Initialize failed");
        restore_current();
        return false;
    }
    wlr_log(WLR_INFO, "ui-spike: %s", gl_msg.c_str());

    render_iface = std::make_unique<RenderInterface_GL3>();
    if (!*render_iface) {
        wlr_log(WLR_ERROR, "ui-spike: RenderInterface_GL3 construction failed");
        restore_current();
        return false;
    }
    render_iface->SetViewport(kSpikeWidth, kSpikeHeight);

    // 3. Offscreen FBO + color target. Plan A first, Plan B on any failure.
    glGenFramebuffers(1, &fbo);
    if (!try_plan_a()) {
        setup_plan_b();
    }
    // flip_y: the FBO color attachment (dmabuf in Plan A, GL texture read
    // back in Plan B) is sampled/scanned-out row 0 = top, but GL renders with
    // a bottom-left origin. Flip the final composite so the submitted buffer
    // is upright; display then matches document coords, so pointer input is
    // forwarded unflipped (on-screen button == document button).
    render_iface->SetOutputFramebuffer(fbo, /*flip_y=*/true);

    // Verify the FBO is complete before committing to RMLUi init.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    const GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
        wlr_log(WLR_ERROR, "ui-spike: output FBO incomplete (0x%x)", fb_status);
        restore_current();
        return false;
    }

    // 4. RMLUi core + font + context + document.
    system = std::make_unique<SpikeSystemInterface>();
    Rml::SetSystemInterface(system.get());
    Rml::SetRenderInterface(render_iface.get());
    if (!Rml::Initialise()) {
        wlr_log(WLR_ERROR, "ui-spike: Rml::Initialise failed");
        restore_current();
        return false;
    }

    if (!Rml::LoadFontFace("/usr/share/fonts/noto/NotoSans-Regular.ttf")) {
        wlr_log(WLR_INFO, "ui-spike: NotoSans not found; disabling spike gracefully");
        Rml::Shutdown();
        restore_current();
        return false;
    }

    context = Rml::CreateContext("spike", Rml::Vector2i(kSpikeWidth, kSpikeHeight));
    if (context == nullptr) {
        wlr_log(WLR_ERROR, "ui-spike: CreateContext failed");
        Rml::Shutdown();
        restore_current();
        return false;
    }

    if (Rml::DataModelConstructor ctor = context->CreateDataModel("spike")) {
        ctor.Bind("frame", &frame);
        ctor.Bind("label", &label);
        model = ctor.GetModelHandle();
    }

    document = context->LoadDocumentFromMemory(kHelloRml);
    if (document == nullptr) {
        wlr_log(WLR_ERROR, "ui-spike: LoadDocumentFromMemory failed");
        Rml::Shutdown();
        restore_current();
        return false;
    }
    document->Show();

    // 5. Scene node. Start with a transparent/empty buffer; tick() fills it.
    scene_buffer = wlr_scene_buffer_create(parent, nullptr);
    if (scene_buffer == nullptr) {
        wlr_log(WLR_ERROR, "ui-spike: wlr_scene_buffer_create failed");
        Rml::Shutdown();
        restore_current();
        return false;
    }
    wlr_scene_node_set_position(&scene_buffer->node, kSpikeX, kSpikeY);

    restore_current();
    wlr_log(WLR_INFO, "ui-spike: bridge up (plan %s, %dx%d)",
            plan == Plan::Dmabuf ? "A/dmabuf" : "B/shm-copy", kSpikeWidth, kSpikeHeight);
    return true;
}

// Plan A: allocate a dmabuf wlr_buffer via the server allocator, import it
// into the sibling context as an EGLImage, bind that as the FBO color
// attachment. Returns false (cleaning up) on any failure so init() falls to B.
bool UiSpike::Impl::try_plan_a() {
    // Spike instrumentation: force the Plan-B fallback for testing the CPU
    // copy path even on hardware where Plan A works. Harmless in production.
    if (std::getenv("UNBOX_UI_SPIKE_FORCE_SHM") != nullptr) {
        wlr_log(WLR_INFO, "ui-spike: plan A skipped — UNBOX_UI_SPIKE_FORCE_SHM set");
        return false;
    }
    if ((allocator->buffer_caps & WLR_BUFFER_CAP_DMABUF) == 0) {
        wlr_log(WLR_INFO, "ui-spike: plan A skipped — allocator has no DMABUF cap");
        return false;
    }
    if (egl_create_image == nullptr || gl_image_target_texture == nullptr) {
        wlr_log(WLR_INFO, "ui-spike: plan A skipped — no EGLImage dmabuf-import entrypoints");
        return false;
    }
    const char* exts = eglQueryString(egl_display, EGL_EXTENSIONS);
    if (exts == nullptr || std::strstr(exts, "EGL_EXT_image_dma_buf_import") == nullptr) {
        wlr_log(WLR_INFO, "ui-spike: plan A skipped — no EGL_EXT_image_dma_buf_import");
        return false;
    }

    // Allocate one dmabuf via the allocator using a LINEAR/INVALID modifier
    // list (legacy-driver-safe; crocus is fine with linear).
    wlr_drm_format fmt{};
    fmt.format = kDrmFormatArgb8888;
    const std::uint64_t modifiers[] = {0 /* DRM_FORMAT_MOD_LINEAR */};
    fmt.len = 1;
    fmt.capacity = 1;
    fmt.modifiers = const_cast<std::uint64_t*>(modifiers);

    wlr_buffer* buf = wlr_allocator_create_buffer(allocator, kSpikeWidth, kSpikeHeight, &fmt);
    if (buf == nullptr) {
        wlr_log(WLR_INFO, "ui-spike: plan A — allocator could not create dmabuf");
        return false;
    }

    wlr_dmabuf_attributes attribs{};
    if (!wlr_buffer_get_dmabuf(buf, &attribs) || attribs.n_planes < 1) {
        wlr_log(WLR_INFO, "ui-spike: plan A — buffer has no dmabuf attrs");
        wlr_buffer_drop(buf);
        return false;
    }

    // Build the EGLImage from the dmabuf (single-plane fast path).
    EGLint img_attribs[] = {
        EGL_WIDTH,                     attribs.width,
        EGL_HEIGHT,                    attribs.height,
        EGL_LINUX_DRM_FOURCC_EXT,      static_cast<EGLint>(attribs.format),
        EGL_DMA_BUF_PLANE0_FD_EXT,     attribs.fd[0],
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(attribs.offset[0]),
        EGL_DMA_BUF_PLANE0_PITCH_EXT,  static_cast<EGLint>(attribs.stride[0]),
        EGL_NONE,
    };
    egl_image = egl_create_image(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                 static_cast<EGLClientBuffer>(nullptr), img_attribs);
    if (egl_image == EGL_NO_IMAGE_KHR) {
        wlr_log(WLR_INFO, "ui-spike: plan A — eglCreateImageKHR failed (0x%x)", eglGetError());
        wlr_buffer_drop(buf);
        return false;
    }

    glGenTextures(1, &color_tex);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    gl_image_target_texture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(egl_image));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        wlr_log(WLR_INFO, "ui-spike: plan A — FBO from EGLImage incomplete (0x%x)", status);
        egl_destroy_image(egl_display, egl_image);
        egl_image = EGL_NO_IMAGE_KHR;
        glDeleteTextures(1, &color_tex);
        color_tex = 0;
        wlr_buffer_drop(buf);
        return false;
    }

    dmabuf = buf;
    plan = Plan::Dmabuf;
    wlr_log(WLR_INFO, "ui-spike: plan A engaged (dmabuf-backed FBO)");
    return true;
}

// Plan B: a plain GL texture color attachment; results read back to a
// data-ptr wlr_buffer with glReadPixels each frame.
void UiSpike::Impl::setup_plan_b() {
    glGenTextures(1, &color_tex);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSpikeWidth, kSpikeHeight, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    shm = make_shm_buffer(kSpikeWidth, kSpikeHeight);
    readback.assign(static_cast<std::size_t>(kSpikeWidth) * kSpikeHeight * 4, 0);
    plan = Plan::ShmCopy;
    wlr_log(WLR_INFO, "ui-spike: plan B engaged (FBO + glReadPixels -> shm)");
}

// Render one dirty frame. Caller holds the sibling context current.
void UiSpike::Impl::render_locked() {
    // Tick the bound state; dirtying drives the repeated render proof.
    frame += 1;
    if (model) {
        model.DirtyVariable("frame");
    }

    context->Update();
    render_iface->BeginFrame();
    render_iface->Clear();
    context->Render();
    render_iface->EndFrame(); // composites into `fbo` (SetOutputFramebuffer)

    if (plan == Plan::Dmabuf) {
        // The wlr renderer will sample the dmabuf directly; ensure all GL
        // writes have landed before the compositor reads it. A spike uses
        // glFinish; the real substrate will use an EGL fence.
        glFinish();
        wlr_scene_buffer_set_buffer(scene_buffer, dmabuf);
    } else {
        // Plan B: read the FBO back into the data-ptr buffer.
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, kSpikeWidth, kSpikeHeight, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // RMLUi outputs premultiplied RGBA8 (R,G,B,A byte order). The shm
        // buffer is FourCC AR24 = little-endian {B,G,R,A}. Swap R<->B; the
        // result is already premultiplied which wlroots expects.
        const std::size_t px = static_cast<std::size_t>(kSpikeWidth) * kSpikeHeight;
        std::uint8_t* dst = shm->data.data();
        const std::uint8_t* src = readback.data();
        for (std::size_t i = 0; i < px; ++i) {
            dst[i * 4 + 0] = src[i * 4 + 2]; // B
            dst[i * 4 + 1] = src[i * 4 + 1]; // G
            dst[i * 4 + 2] = src[i * 4 + 0]; // R
            dst[i * 4 + 3] = src[i * 4 + 3]; // A
        }
        wlr_scene_buffer_set_buffer(scene_buffer, &shm->base);
    }

    frame_count += 1;
}

void UiSpike::Impl::teardown() {
    // RMLUi teardown needs the sibling context current (GL deletes).
    const bool ok = make_current();

    if (scene_buffer != nullptr) {
        wlr_scene_node_destroy(&scene_buffer->node);
        scene_buffer = nullptr;
    }

    if (context != nullptr) {
        // Document is owned by the context; Shutdown tears everything down.
        Rml::Shutdown();
        context = nullptr;
        document = nullptr;
    }
    render_iface.reset();

    if (color_tex != 0) {
        glDeleteTextures(1, &color_tex);
        color_tex = 0;
    }
    if (fbo != 0) {
        glDeleteFramebuffers(1, &fbo);
        fbo = 0;
    }
    if (egl_image != EGL_NO_IMAGE_KHR && egl_destroy_image != nullptr) {
        egl_destroy_image(egl_display, egl_image);
        egl_image = EGL_NO_IMAGE_KHR;
    }
    if (dmabuf != nullptr) {
        wlr_buffer_drop(dmabuf);
        dmabuf = nullptr;
    }
    if (shm != nullptr) {
        wlr_buffer_drop(&shm->base); // triggers shm_buffer_destroy -> delete
        shm = nullptr;
    }

    if (ok) {
        restore_current();
    }
    if (egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display, egl_context);
        egl_context = EGL_NO_CONTEXT;
    }
}

// --- UiSpike (public-ish private surface) ---------------------------------

auto UiSpike::create(wlr_scene_tree* parent, EGLDisplay egl_display, wlr_allocator* allocator,
                     wlr_renderer* renderer) -> std::unique_ptr<UiSpike> {
    auto impl = std::make_unique<Impl>();
    if (!impl->init(parent, egl_display, allocator, renderer)) {
        impl->teardown(); // safe to call after partial init
        // Hand back a Disabled bridge (never throws, never aborts the server).
        auto disabled = std::make_unique<Impl>();
        disabled->plan = Plan::Disabled;
        return std::unique_ptr<UiSpike>(new UiSpike(std::move(disabled)));
    }
    return std::unique_ptr<UiSpike>(new UiSpike(std::move(impl)));
}

UiSpike::UiSpike(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

UiSpike::~UiSpike() {
    if (impl_->plan != Plan::Disabled) {
        impl_->teardown();
    }
}

void UiSpike::tick() {
    if (impl_->plan == Plan::Disabled) {
        return;
    }
    // Render every tick (spike fidelity: the frame counter dirties the doc
    // each call, so the context is always dirty — proving repeated cycles).
    if (!impl_->make_current()) {
        return;
    }
    impl_->render_locked();
    impl_->restore_current();
}

void UiSpike::on_pointer_motion(double sx, double sy) {
    if (impl_->plan == Plan::Disabled || impl_->context == nullptr) {
        return;
    }
    impl_->context->ProcessMouseMove(static_cast<int>(sx), static_cast<int>(sy), 0);
}

void UiSpike::on_pointer_button(bool pressed) {
    if (impl_->plan == Plan::Disabled || impl_->context == nullptr) {
        return;
    }
    if (pressed) {
        impl_->context->ProcessMouseButtonDown(0, 0);
    } else {
        impl_->context->ProcessMouseButtonUp(0, 0);
    }
}

auto UiSpike::node() const -> wlr_scene_node* {
    return impl_->scene_buffer != nullptr ? &impl_->scene_buffer->node : nullptr;
}

auto UiSpike::plan() const -> Plan {
    return impl_->plan;
}

auto UiSpike::frame_count() const -> int {
    return impl_->frame_count;
}

auto UiSpike::check_orientation() const -> int {
    // Only the shm path keeps a CPU readback to inspect, and only after a
    // frame has been submitted.
    if (impl_->plan != Plan::ShmCopy || impl_->frame_count == 0) {
        return 0;
    }
    const std::uint8_t* px = impl_->readback.data(); // R,G,B,A, row 0 = top
    const int w = kSpikeWidth;
    const int h = kSpikeHeight;

    auto matches = [](const std::uint8_t* p, const std::uint8_t (&c)[3]) {
        const int dr = static_cast<int>(p[0]) - c[0];
        const int dg = static_cast<int>(p[1]) - c[1];
        const int db = static_cast<int>(p[2]) - c[2];
        return dr * dr + dg * dg + db * db < 24 * 24; // tolerant of AA edges
    };

    // Count band pixels in the top kBandHeight rows vs the bottom kBandHeight
    // rows, sampling the full width. Upright => top band dominates the top
    // rows and bottom band the bottom rows; a flip swaps them.
    int top_band_in_top = 0;
    int top_band_in_bottom = 0;
    int bottom_band_in_top = 0;
    int bottom_band_in_bottom = 0;
    for (int row = 0; row < kBandHeight; ++row) {
        const int top_row = row;
        const int bot_row = h - 1 - row;
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* pt = px + (static_cast<std::size_t>(top_row) * w + x) * 4;
            const std::uint8_t* pb = px + (static_cast<std::size_t>(bot_row) * w + x) * 4;
            if (matches(pt, kTopBandRGB)) {
                ++top_band_in_top;
            }
            if (matches(pb, kTopBandRGB)) {
                ++top_band_in_bottom;
            }
            if (matches(pt, kBottomBandRGB)) {
                ++bottom_band_in_top;
            }
            if (matches(pb, kBottomBandRGB)) {
                ++bottom_band_in_bottom;
            }
        }
    }

    // Need a clear, unambiguous signal in one orientation.
    const bool upright = top_band_in_top > 100 && bottom_band_in_bottom > 100 &&
                         top_band_in_top > top_band_in_bottom &&
                         bottom_band_in_bottom > bottom_band_in_top;
    const bool flipped = top_band_in_bottom > 100 && bottom_band_in_top > 100 &&
                         top_band_in_bottom > top_band_in_top &&
                         bottom_band_in_top > bottom_band_in_bottom;
    if (upright) {
        return 1;
    }
    if (flipped) {
        return -1;
    }
    return 0;
}

} // namespace unbox::kernel
