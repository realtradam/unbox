#include "ui_substrate.hpp"

#include "rmlui_renderer_gl3.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/SystemInterface.h>

// The kernel owns GL; system EGL/GLES headers are allowed here (same as the
// retired spike). wlr.hpp already pulled <EGL/egl.h>+<EGL/eglext.h> via
// wlr/render/egl.h and GLES via the adapted renderer.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h> // glEGLImageTargetTexture2DOES
#include <GLES3/gl32.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace unbox::kernel {

namespace {

constexpr std::uint32_t kDrmFormatArgb8888 = 0x34325241; // 'AR24' = LE {B,G,R,A}

// Orientation regression guard (kept from the spike): the test fixture document
// carries full-width solid bands at top (#18e0a0) and bottom (#e09018). The
// substrate's orientation() samples a shm-path surface's submitted buffer and
// proves the top band lands in the TOP rows (upright) — GL's bottom-left FBO
// origin vs wlr_buffer top-first convention makes a flip the default failure.
constexpr int kBandHeight = 12;
constexpr std::uint8_t kTopBandRGB[3] = {0x18, 0xe0, 0xa0};
constexpr std::uint8_t kBottomBandRGB[3] = {0xe0, 0x90, 0x18};

// --- SystemInterface: elapsed time + route RmlUi logs to wlr_log ----------
class SubstrateSystemInterface final : public Rml::SystemInterface {
public:
    auto GetElapsedTime() -> double override {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double t = static_cast<double>(now.tv_sec) + now.tv_nsec / 1e9;
        if (start_ == 0.0) {
            start_ = t;
        }
        return t - start_;
    }
    auto LogMessage(Rml::Log::Type type, const Rml::String& message) -> bool override {
        const wlr_log_importance imp =
            (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT) ? WLR_ERROR
            : (type == Rml::Log::LT_WARNING ? WLR_INFO : WLR_DEBUG);
        wlr_log(imp, "[rmlui] %s", message.c_str());
        return true;
    }

private:
    double start_ = 0.0;
};

// --- A data-ptr wlr_buffer wrapping heap memory (Plan B target) -----------
struct ShmBuffer {
    wlr_buffer base{};
    std::vector<std::uint8_t> data;
    std::uint32_t format = kDrmFormatArgb8888;
    std::size_t stride = 0;
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

// ---- GL bridge (shared sibling context) -------------------------------------
//
// One EGL context + Rml::Initialise + font shared by all surfaces. Owns the EGL
// extension entrypoints (image import for Plan A, fence sync for production
// submission) and the current-context save/restore around every GL section.

struct GlBridge {
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLConfig config = nullptr;

    EGLContext saved_context = EGL_NO_CONTEXT;
    EGLSurface saved_draw = EGL_NO_SURFACE;
    EGLSurface saved_read = EGL_NO_SURFACE;

    std::unique_ptr<SubstrateSystemInterface> system;
    std::unique_ptr<RenderInterface_GL3> render_iface;
    bool rml_initialised = false;
    bool ok = false;

    bool dmabuf_import_ok = false; // Plan A preconditions met
    bool fence_ok = false;         // EGL_KHR_fence_sync usable

    PFNEGLCREATEIMAGEKHRPROC egl_create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_image_target_texture = nullptr;
    PFNEGLCREATESYNCKHRPROC egl_create_sync = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC egl_client_wait_sync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC egl_destroy_sync = nullptr;

    bool make_current() {
        saved_context = eglGetCurrentContext();
        saved_draw = eglGetCurrentSurface(EGL_DRAW);
        saved_read = eglGetCurrentSurface(EGL_READ);
        return eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context) == EGL_TRUE;
    }
    void restore_current() {
        eglMakeCurrent(egl_display, saved_draw, saved_read, saved_context);
    }

    // Block until GL writes to the current target have completed, using an EGL
    // fence (production sync; replaces the spike's glFinish on the hot path).
    // Falls back to glFinish only if the fence extension is unusable.
    void submit_sync() {
        if (fence_ok) {
            EGLSyncKHR sync = egl_create_sync(egl_display, EGL_SYNC_FENCE_KHR, nullptr);
            if (sync != EGL_NO_SYNC_KHR) {
                glFlush();
                egl_client_wait_sync(egl_display, sync, 0, EGL_FOREVER_KHR);
                egl_destroy_sync(egl_display, sync);
                return;
            }
        }
        glFinish();
    }

    bool init(EGLDisplay display);
    void teardown();
};

bool GlBridge::init(EGLDisplay display) {
    egl_display = display;
    if (egl_display == EGL_NO_DISPLAY) {
        return false;
    }
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        wlr_log(WLR_ERROR, "ui-substrate: eglBindAPI(ES) failed");
        return false;
    }
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,     8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint num_config = 0;
    if (eglChooseConfig(egl_display, config_attribs, &config, 1, &num_config) != EGL_TRUE ||
        num_config < 1) {
        wlr_log(WLR_ERROR, "ui-substrate: eglChooseConfig found no ES3 config");
        return false;
    }
    const EGLint ctx_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2,
                                  EGL_NONE};
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        wlr_log(WLR_ERROR, "ui-substrate: eglCreateContext(ES 3.2) failed (0x%x)", eglGetError());
        return false;
    }
    if (!make_current()) {
        wlr_log(WLR_ERROR, "ui-substrate: surfaceless eglMakeCurrent failed (0x%x)", eglGetError());
        restore_current();
        return false;
    }

    egl_create_image =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    egl_destroy_image =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    gl_image_target_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    const char* exts = eglQueryString(egl_display, EGL_EXTENSIONS);
    const bool has_dmabuf_import =
        exts != nullptr && std::strstr(exts, "EGL_EXT_image_dma_buf_import") != nullptr;
    dmabuf_import_ok = has_dmabuf_import && egl_create_image != nullptr &&
                       gl_image_target_texture != nullptr &&
                       std::getenv("UNBOX_UI_SUBSTRATE_FORCE_SHM") == nullptr;

    // EGL fence sync (production submission sync — notes/plan.md §7).
    const bool has_fence =
        exts != nullptr && std::strstr(exts, "EGL_KHR_fence_sync") != nullptr;
    egl_create_sync =
        reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    egl_client_wait_sync =
        reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
    egl_destroy_sync =
        reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    fence_ok = has_fence && egl_create_sync != nullptr && egl_client_wait_sync != nullptr &&
               egl_destroy_sync != nullptr;

    Rml::String gl_msg;
    if (!RmlGL3::Initialize(&gl_msg)) {
        wlr_log(WLR_ERROR, "ui-substrate: RmlGL3::Initialize failed");
        restore_current();
        return false;
    }
    wlr_log(WLR_INFO, "ui-substrate: %s", gl_msg.c_str());

    render_iface = std::make_unique<RenderInterface_GL3>();
    if (!*render_iface) {
        wlr_log(WLR_ERROR, "ui-substrate: RenderInterface_GL3 construction failed");
        restore_current();
        return false;
    }

    system = std::make_unique<SubstrateSystemInterface>();
    Rml::SetSystemInterface(system.get());
    Rml::SetRenderInterface(render_iface.get());
    if (!Rml::Initialise()) {
        wlr_log(WLR_ERROR, "ui-substrate: Rml::Initialise failed");
        restore_current();
        return false;
    }
    rml_initialised = true;

    if (!Rml::LoadFontFace("/usr/share/fonts/noto/NotoSans-Regular.ttf")) {
        wlr_log(WLR_INFO, "ui-substrate: NotoSans not found; substrate unavailable");
        Rml::Shutdown();
        rml_initialised = false;
        restore_current();
        return false;
    }

    restore_current();
    ok = true;
    wlr_log(WLR_INFO, "ui-substrate: up (dmabuf=%d fence=%d)", dmabuf_import_ok, fence_ok);
    return true;
}

void GlBridge::teardown() {
    const bool cur = (egl_context != EGL_NO_CONTEXT) && make_current();
    if (rml_initialised) {
        Rml::Shutdown();
        rml_initialised = false;
    }
    render_iface.reset();
    if (cur) {
        restore_current();
    }
    if (egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display, egl_context);
        egl_context = EGL_NO_CONTEXT;
    }
}

// ---- Surface ----------------------------------------------------------------

struct Surface {
    Substrate::Impl* owner = nullptr;
    ExtensionId who{};

    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    bool is_visible = true;

    // Plan: dmabuf swapchain (A) or single shm buffer (B).
    bool dmabuf = false;

    // GL target.
    GLuint fbo = 0;
    GLuint shm_tex = 0; // Plan B color attachment

    // Plan A: 2-deep swapchain + per-buffer cached EGLImage/texture.
    wlr_swapchain* swapchain = nullptr;
    struct SlotGl {
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint tex = 0;
    };
    std::unordered_map<wlr_buffer*, SlotGl> slot_gl;

    // Plan B: one shm buffer + readback scratch.
    ShmBuffer* shm = nullptr;
    std::vector<std::uint8_t> readback;

    // RMLUi.
    Rml::Context* context = nullptr; // owned by Rml (RemoveContext)
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelConstructor ctor;  // open until the document loads (lazy)
    Rml::DataModelHandle model;
    std::string model_name;

    // Deferred document source (loaded on first tick, after binds are set).
    std::string rml_inline;
    std::string rml_path;
    bool doc_loaded = false;

    // Data bindings. Each bound scalar pairs a getter with a stable slot the
    // getter writes into; RmlUi binds to the slot's address. Bound BEFORE the
    // document loads (RmlUi requires the model complete at parse time), so we
    // use std::list for address stability across pushes.
    template <typename T>
    struct ScalarBinding {
        std::function<T()> getter;
        T slot{};
    };
    std::list<ScalarBinding<int>> int_bindings;
    std::list<ScalarBinding<double>> double_bindings;
    std::list<ScalarBinding<bool>> bool_bindings;
    std::list<ScalarBinding<Rml::String>> string_bindings;
    struct EventBinding {
        std::function<void()> cb;
        ExtensionId who;
        Substrate::Impl* owner;
    };
    std::list<EventBinding> event_bindings;

    // touch-mode-changed notification (one per surface; see ui.hpp). Fired on a
    // transition, error-isolated to `who`. touch-mode does NO visual scaling
    // (user decision) — this is purely an opt-in signal for extensions.
    std::function<void(bool)> touch_mode_cb;

    // Scene.
    wlr_scene_buffer* scene_buffer = nullptr;

    int frame_count = 0;
};

// ---- Substrate::Impl --------------------------------------------------------

struct Substrate::Impl {
    GlBridge gl;
    wlr_allocator* allocator = nullptr;
    wlr_renderer* renderer = nullptr;
    SubstrateDisableFn disable;

    TouchModeTracker touch_mode_tracker;

    std::list<Surface> surfaces; // stable addresses (handles borrow Surface*)

    // Pointer implicit grab: the consumer of the first button press owns the
    // whole press..release stream (standard seat behavior). `pointer_grab`
    // (pure) tracks owner + down-count; `pointer_grab_surface` is the ui surface
    // the substrate routes the grabbed stream to (null if a grabbed surface was
    // destroyed mid-stream — then the substrate still CONSUMES the tail but
    // delivers nothing, never leaking mid-grab events to the bus).
    PointerButtonGrab pointer_grab;
    Surface* pointer_grab_surface = nullptr;

    // Touch routing: which surface a given touch id is captured by (down ->
    // up/cancel). The down's consumer owns that point's motion/up/cancel; a
    // down that fell through to the bus has NO entry (bus owns it). Cleared on
    // up/cancel and on surface destruction.
    std::unordered_map<std::int32_t, Surface*> touch_capture;

    [[nodiscard]] auto available() const -> bool { return gl.ok; }

    // Topmost visible surface containing (lx,ly). Surfaces are kept in
    // creation order; later surfaces composite above earlier within a layer, so
    // scan back-to-front. (Cross-layer correctness is the scene's job; for the
    // input hit-test, last-created-wins matches the overlay-stacked default.)
    auto surface_at(double lx, double ly) -> Surface* {
        Surface* hit = nullptr;
        for (Surface& s : surfaces) {
            if (s.is_visible && point_in_rect(lx, ly, s.x, s.y, s.width, s.height)) {
                hit = &s; // keep scanning: later = on top
            }
        }
        return hit;
    }

    // Notify every surface that touch-mode flipped. touch-mode does NO visual
    // scaling (user decision) — the substrate never touches the dp-ratio, so
    // this is purely the opt-in signal. Called only on a real transition.
    // Error-isolated per surface.
    void notify_touch_mode_changed() {
        const bool touch = touch_mode_tracker.is_touch();
        for (Surface& s : surfaces) {
            if (s.touch_mode_cb) {
                try {
                    s.touch_mode_cb(touch);
                } catch (...) {
                    if (disable) {
                        disable(s.who);
                    }
                }
            }
        }
    }

    // Re-read every bound getter for `s` into its scratch slots + dirty the
    // model. Getter exceptions isolate the owning extension.
    void refresh_bindings(Surface& s);

    bool init_surface_gl(Surface& s);
    void render_surface(Surface& s); // caller holds context current
    void destroy_surface(Surface* s);

    // Forward a synthesized pointer event into a surface's Rml context. Returns
    // whether RmlUi (or our hit-test) treats it as consumed.
    void ctx_motion(Surface& s, double lx, double ly);
    void ctx_button(Surface& s, bool pressed);
};

void Substrate::Impl::refresh_bindings(Surface& s) {
    if (!s.model) {
        return;
    }
    auto isolate = [&](auto&& fn) {
        try {
            fn();
        } catch (...) {
            if (disable) {
                disable(s.who);
            }
        }
    };
    for (auto& b : s.int_bindings) {
        if (b.getter) {
            isolate([&] { b.slot = b.getter(); });
        }
    }
    for (auto& b : s.double_bindings) {
        if (b.getter) {
            isolate([&] { b.slot = b.getter(); });
        }
    }
    for (auto& b : s.bool_bindings) {
        if (b.getter) {
            isolate([&] { b.slot = b.getter(); });
        }
    }
    for (auto& b : s.string_bindings) {
        if (b.getter) {
            isolate([&] { b.slot = b.getter(); });
        }
    }
}

bool Substrate::Impl::init_surface_gl(Surface& s) {
    glGenFramebuffers(1, &s.fbo);

    if (gl.dmabuf_import_ok && (allocator->buffer_caps & WLR_BUFFER_CAP_DMABUF) != 0) {
        wlr_drm_format fmt{};
        fmt.format = kDrmFormatArgb8888;
        std::uint64_t modifiers[] = {0 /* DRM_FORMAT_MOD_LINEAR */};
        fmt.len = 1;
        fmt.capacity = 1;
        fmt.modifiers = modifiers;
        // 2-deep swapchain (production: double-buffer so the compositor can be
        // sampling slot N while we render slot N+1). WLR_SWAPCHAIN_CAP caps it.
        s.swapchain = wlr_swapchain_create(allocator, s.width, s.height, &fmt);
        if (s.swapchain != nullptr) {
            s.dmabuf = true;
        }
    }

    if (!s.dmabuf) {
        // Plan B: single GL texture color attachment, read back to a shm buffer.
        glGenTextures(1, &s.shm_tex);
        glBindTexture(GL_TEXTURE_2D, s.shm_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s.width, s.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.shm_tex, 0);
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            wlr_log(WLR_ERROR, "ui-substrate: Plan B FBO incomplete (0x%x)", st);
            return false;
        }
        s.shm = make_shm_buffer(s.width, s.height);
        s.readback.assign(static_cast<std::size_t>(s.width) * s.height * 4, 0);
    }
    return true;
}

void Substrate::Impl::render_surface(Surface& s) {
    if (s.context == nullptr) {
        return;
    }
    // Lazy document load on the first render: all bind_* calls have happened by
    // now, so the data model is complete (RmlUi requires that at parse time).
    if (!s.doc_loaded) {
        s.doc_loaded = true;
        s.model = s.ctor.GetModelHandle();
        s.ctor = Rml::DataModelConstructor{}; // close the constructor
        if (!s.rml_path.empty()) {
            s.document = s.context->LoadDocument(s.rml_path);
        } else {
            s.document = s.context->LoadDocumentFromMemory(s.rml_inline);
        }
        if (s.document == nullptr) {
            wlr_log(WLR_ERROR, "ui-substrate: failed to load document");
            return;
        }
        s.document->Show();
    }
    refresh_bindings(s);
    if (s.model) {
        s.model.DirtyAllVariables();
    }

    GLuint target_fbo = s.fbo;
    wlr_buffer* dmabuf_target = nullptr;

    if (s.dmabuf) {
        wlr_buffer* buf = wlr_swapchain_acquire(s.swapchain);
        if (buf == nullptr) {
            return;
        }
        dmabuf_target = buf;
        // Cache an EGLImage+texture per swapchain buffer (re-import is costly).
        auto it = s.slot_gl.find(buf);
        if (it == s.slot_gl.end()) {
            wlr_dmabuf_attributes attribs{};
            if (!wlr_buffer_get_dmabuf(buf, &attribs) || attribs.n_planes < 1) {
                wlr_buffer_unlock(buf);
                return;
            }
            EGLint ia[] = {
                EGL_WIDTH,                     attribs.width,
                EGL_HEIGHT,                    attribs.height,
                EGL_LINUX_DRM_FOURCC_EXT,      static_cast<EGLint>(attribs.format),
                EGL_DMA_BUF_PLANE0_FD_EXT,     attribs.fd[0],
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(attribs.offset[0]),
                EGL_DMA_BUF_PLANE0_PITCH_EXT,  static_cast<EGLint>(attribs.stride[0]),
                EGL_NONE,
            };
            EGLImageKHR img = gl.egl_create_image(gl.egl_display, EGL_NO_CONTEXT,
                                                  EGL_LINUX_DMA_BUF_EXT, nullptr, ia);
            if (img == EGL_NO_IMAGE_KHR) {
                wlr_buffer_unlock(buf);
                return;
            }
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            gl.gl_image_target_texture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(img));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            Surface::SlotGl slot{img, tex};
            it = s.slot_gl.emplace(buf, slot).first;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, it->second.tex,
                               0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            wlr_buffer_unlock(buf);
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    gl.render_iface->SetViewport(s.width, s.height);
    // flip_y: GL renders bottom-left origin; the FBO is sampled/scanned-out
    // top-first, so flip the final composite for an upright submitted buffer.
    gl.render_iface->SetOutputFramebuffer(target_fbo, /*flip_y=*/true);
    s.context->Update();
    gl.render_iface->BeginFrame();
    gl.render_iface->Clear();
    s.context->Render();
    gl.render_iface->EndFrame();

    if (s.dmabuf) {
        gl.submit_sync(); // EGL fence (production), not glFinish
        wlr_scene_buffer_set_buffer(s.scene_buffer, dmabuf_target);
        wlr_buffer_unlock(dmabuf_target); // scene_buffer took its own lock
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        glReadPixels(0, 0, s.width, s.height, GL_RGBA, GL_UNSIGNED_BYTE, s.readback.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        // RMLUi premultiplied RGBA8 -> FourCC AR24 {B,G,R,A}: swap R<->B.
        const std::size_t px = static_cast<std::size_t>(s.width) * s.height;
        std::uint8_t* dst = s.shm->data.data();
        const std::uint8_t* src = s.readback.data();
        for (std::size_t i = 0; i < px; ++i) {
            dst[i * 4 + 0] = src[i * 4 + 2];
            dst[i * 4 + 1] = src[i * 4 + 1];
            dst[i * 4 + 2] = src[i * 4 + 0];
            dst[i * 4 + 3] = src[i * 4 + 3];
        }
        wlr_scene_buffer_set_buffer(s.scene_buffer, &s.shm->base);
    }
    s.frame_count += 1;
}

void Substrate::Impl::destroy_surface(Surface* s) {
    const bool cur = gl.make_current();
    if (s->scene_buffer != nullptr) {
        wlr_scene_node_destroy(&s->scene_buffer->node);
        s->scene_buffer = nullptr;
    }
    if (s->context != nullptr) {
        Rml::RemoveContext(s->context->GetName());
        s->context = nullptr;
        s->document = nullptr;
    }
    for (auto& [buf, slot] : s->slot_gl) {
        if (slot.tex != 0) {
            glDeleteTextures(1, &slot.tex);
        }
        if (slot.image != EGL_NO_IMAGE_KHR && gl.egl_destroy_image != nullptr) {
            gl.egl_destroy_image(gl.egl_display, slot.image);
        }
    }
    s->slot_gl.clear();
    if (s->shm_tex != 0) {
        glDeleteTextures(1, &s->shm_tex);
        s->shm_tex = 0;
    }
    if (s->fbo != 0) {
        glDeleteFramebuffers(1, &s->fbo);
        s->fbo = 0;
    }
    if (s->swapchain != nullptr) {
        wlr_swapchain_destroy(s->swapchain);
        s->swapchain = nullptr;
    }
    if (s->shm != nullptr) {
        wlr_buffer_drop(&s->shm->base);
        s->shm = nullptr;
    }
    if (cur) {
        gl.restore_current();
    }
    // A surface dying mid-grab must not strand the input stream. Drop any
    // capture pointing at it: the pointer grab keeps its OWNER (substrate) so
    // the tail of the stream is still consumed (not leaked to the bus mid-grab)
    // but routes to nothing; touch points captured by it are released — their
    // remaining motion/up will find no capture and (correctly) reach the bus.
    if (pointer_grab_surface == s) {
        pointer_grab_surface = nullptr;
    }
    for (auto it = touch_capture.begin(); it != touch_capture.end();) {
        it = (it->second == s) ? touch_capture.erase(it) : std::next(it);
    }
    // Erase from the owner list (Surface storage).
    surfaces.remove_if([s](const Surface& e) { return &e == s; });
}

void Substrate::Impl::ctx_motion(Surface& s, double lx, double ly) {
    if (s.context == nullptr) {
        return;
    }
    s.context->ProcessMouseMove(static_cast<int>(lx - s.x), static_cast<int>(ly - s.y), 0);
}
void Substrate::Impl::ctx_button(Surface& s, bool pressed) {
    if (s.context == nullptr) {
        return;
    }
    if (pressed) {
        s.context->ProcessMouseButtonDown(0, 0);
    } else {
        s.context->ProcessMouseButtonUp(0, 0);
    }
}

// ---- Substrate (private surface) --------------------------------------------

auto Substrate::create(EGLDisplay egl_display, wlr_allocator* allocator, wlr_renderer* renderer,
                       SubstrateDisableFn disable) -> std::unique_ptr<Substrate> {
    auto impl = std::make_unique<Impl>();
    impl->allocator = allocator;
    impl->renderer = renderer;
    impl->disable = std::move(disable);
    impl->gl.init(egl_display); // sets gl.ok; failure => unavailable substrate
    return std::unique_ptr<Substrate>(new Substrate(std::move(impl)));
}

Substrate::Substrate(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Substrate::~Substrate() {
    // Destroy surfaces (GL + scene nodes) then the shared bridge.
    while (!impl_->surfaces.empty()) {
        impl_->destroy_surface(&impl_->surfaces.front());
    }
    impl_->gl.teardown();
}

auto Substrate::available() const -> bool { return impl_->available(); }

auto Substrate::create_surface(ExtensionId who, wlr_scene_tree* parent, const UiSurfaceSpec& spec)
    -> std::unique_ptr<UiSurface> {
    if (!impl_->available() || parent == nullptr) {
        return nullptr;
    }
    if (spec.width <= 0 || spec.height <= 0) {
        wlr_log(WLR_ERROR, "ui-substrate: surface needs positive geometry");
        return nullptr;
    }
    if (!impl_->gl.make_current()) {
        return nullptr;
    }

    impl_->surfaces.emplace_back();
    Surface& s = impl_->surfaces.back();
    s.owner = impl_.get();
    s.who = who;
    s.width = spec.width;
    s.height = spec.height;
    s.x = spec.x;
    s.y = spec.y;
    s.is_visible = spec.visible;

    bool ok = impl_->init_surface_gl(s);
    if (ok) {
        // Context name must be globally unique (RmlUi namespaces contexts by
        // name); the data-model name is the document-authored spec.model.
        static int counter = 0;
        const std::string ctx_name = "ui_ctx_" + std::to_string(++counter);
        s.model_name = spec.model.empty() ? std::string("ui") : spec.model;
        s.context = Rml::CreateContext(ctx_name, Rml::Vector2i(s.width, s.height),
                                       impl_->gl.render_iface.get());
        ok = s.context != nullptr;
    }
    if (ok) {
        // touch-mode does no visual scaling: leave the context at RmlUi's
        // default dp-ratio (1.0) for the surface's whole life.
        s.scene_buffer = wlr_scene_buffer_create(parent, nullptr);
        ok = s.scene_buffer != nullptr;
    }
    if (!ok) {
        impl_->destroy_surface(&s);
        impl_->gl.restore_current();
        return nullptr;
    }

    wlr_scene_node_set_position(&s.scene_buffer->node, s.x, s.y);
    wlr_scene_node_set_enabled(&s.scene_buffer->node, s.is_visible);

    // Open the data model constructor (model name == context name == the
    // document's data-model). It stays open while the extension calls bind_*;
    // the document loads lazily on the first render once binds are complete
    // (RmlUi requires the data model fully built before it parses {{...}}).
    s.ctor = s.context->CreateDataModel(s.model_name);
    s.rml_inline = spec.rml_inline;
    s.rml_path = spec.rml_path;

    impl_->gl.restore_current();
    return std::make_unique<SurfaceHandle>(this, &s);
}

void Substrate::tick_all() {
    if (!impl_->available() || impl_->surfaces.empty()) {
        return;
    }
    if (!impl_->gl.make_current()) {
        return;
    }
    for (Surface& s : impl_->surfaces) {
        if (s.is_visible) {
            impl_->render_surface(s);
        }
    }
    impl_->gl.restore_current();
}

// ---- Input routing ----------------------------------------------------------

void Substrate::route_pointer_motion(double lx, double ly, std::uint32_t time_msec) {
    if (!impl_->available()) {
        return;
    }
    if (impl_->touch_mode_tracker.on_pointer_motion(time_msec)) {
        impl_->notify_touch_mode_changed();
    }
    // During a substrate-owned button grab, the grabbed surface keeps receiving
    // moves (RmlUi drag) even when the cursor leaves it; other surfaces get a
    // leave. Otherwise, normal hover: the hit surface gets the move.
    Surface* target = nullptr;
    if (impl_->pointer_grab.owner() == GrabOwner::substrate) {
        target = impl_->pointer_grab_surface; // may be null if destroyed mid-grab
    } else {
        target = impl_->surface_at(lx, ly);
    }
    for (Surface& s : impl_->surfaces) {
        if (&s == target) {
            impl_->ctx_motion(s, lx, ly);
        } else if (s.context != nullptr) {
            s.context->ProcessMouseLeave();
        }
    }
}

auto Substrate::route_pointer_button(double lx, double ly, bool pressed, std::uint32_t /*time*/)
    -> bool {
    if (!impl_->available()) {
        return false;
    }
    if (pressed) {
        // The press decides (or joins) the grab. Owner is fixed at the first
        // press of the stream; this press routes to that owner.
        Surface* hit = impl_->surface_at(lx, ly);
        const GrabOwner owner = impl_->pointer_grab.press(hit != nullptr);
        if (owner != GrabOwner::substrate) {
            return false; // bus owns this grab — pass through
        }
        if (impl_->pointer_grab_surface == nullptr) {
            impl_->pointer_grab_surface = hit; // first press of a substrate grab
        }
        if (impl_->pointer_grab_surface != nullptr) {
            impl_->ctx_motion(*impl_->pointer_grab_surface, lx, ly);
            impl_->ctx_button(*impl_->pointer_grab_surface, true);
        }
        return true; // consumed by the substrate
    }
    // Release: routes to the grab's owner regardless of what is under the cursor
    // now (the press's consumer owns the release).
    const GrabOwner owner = impl_->pointer_grab.release();
    if (owner != GrabOwner::substrate) {
        return false; // bus owned this grab — release reaches extensions
    }
    if (impl_->pointer_grab_surface != nullptr) {
        impl_->ctx_motion(*impl_->pointer_grab_surface, lx, ly);
        impl_->ctx_button(*impl_->pointer_grab_surface, false);
    }
    if (!impl_->pointer_grab.active()) {
        impl_->pointer_grab_surface = nullptr; // grab ended
    }
    return true; // consumed (even if the surface vanished mid-grab)
}

auto Substrate::route_pointer_axis(double lx, double ly, double delta, std::uint32_t /*time*/)
    -> bool {
    if (!impl_->available()) {
        return false;
    }
    Surface* hit = impl_->surface_at(lx, ly);
    if (hit == nullptr || hit->context == nullptr) {
        return false;
    }
    hit->context->ProcessMouseWheel(static_cast<float>(delta), 0);
    return true;
}

auto Substrate::route_touch_down(std::int32_t id, double lx, double ly, std::uint32_t time_msec)
    -> bool {
    if (!impl_->available()) {
        return false;
    }
    if (impl_->touch_mode_tracker.on_touch(time_msec)) {
        impl_->notify_touch_mode_changed();
    }
    Surface* hit = impl_->surface_at(lx, ly);
    if (hit == nullptr) {
        return false;
    }
    // Synthesize a tap = mouse move-to + button down (RmlUi single-touch model).
    impl_->touch_capture[id] = hit;
    impl_->ctx_motion(*hit, lx, ly);
    impl_->ctx_button(*hit, true);
    return true;
}

auto Substrate::route_touch_motion(std::int32_t id, double lx, double ly, std::uint32_t time_msec)
    -> bool {
    if (!impl_->available()) {
        return false;
    }
    auto it = impl_->touch_capture.find(id);
    if (it == impl_->touch_capture.end()) {
        return false; // down was not over a surface; not captured
    }
    impl_->touch_mode_tracker.on_touch(time_msec);
    impl_->ctx_motion(*it->second, lx, ly);
    return true;
}

auto Substrate::route_touch_up(std::int32_t id, std::uint32_t /*time*/) -> bool {
    if (!impl_->available()) {
        return false;
    }
    auto it = impl_->touch_capture.find(id);
    if (it == impl_->touch_capture.end()) {
        return false;
    }
    impl_->ctx_button(*it->second, false);
    impl_->touch_capture.erase(it);
    return true;
}

auto Substrate::touch_mode() const -> bool { return impl_->touch_mode_tracker.is_touch(); }

void Substrate::set_touch_mode_override(UiSubstrate::TouchModeOverride ov) {
    using TO = UiSubstrate::TouchModeOverride;
    TouchModeTracker::Override mapped = TouchModeTracker::Override::none;
    if (ov == TO::force_off) {
        mapped = TouchModeTracker::Override::force_pointer;
    } else if (ov == TO::force_on) {
        mapped = TouchModeTracker::Override::force_touch;
    }
    if (impl_->touch_mode_tracker.set_override(mapped)) {
        impl_->notify_touch_mode_changed();
    }
}

auto Substrate::frame_count() const -> int {
    int total = 0;
    for (const Surface& s : impl_->surfaces) {
        total += s.frame_count;
    }
    return total;
}

auto Substrate::fence_sync_active() const -> bool {
    return impl_->gl.fence_ok && impl_->gl.dmabuf_import_ok;
}

auto Substrate::orientation() const -> int {
    for (const Surface& s : impl_->surfaces) {
        if (s.dmabuf || s.shm == nullptr || s.frame_count == 0) {
            continue;
        }
        const std::uint8_t* base = s.readback.data(); // R,G,B,A, row0=top
        const int w = s.width;
        const int h = s.height;
        auto matches = [](const std::uint8_t* p, const std::uint8_t (&c)[3]) {
            const int dr = static_cast<int>(p[0]) - c[0];
            const int dg = static_cast<int>(p[1]) - c[1];
            const int db = static_cast<int>(p[2]) - c[2];
            return dr * dr + dg * dg + db * db < 24 * 24;
        };
        int tt = 0;
        int tb = 0;
        int bt = 0;
        int bb = 0;
        for (int row = 0; row < kBandHeight; ++row) {
            const int top_row = row;
            const int bot_row = h - 1 - row;
            for (int xx = 0; xx < w; ++xx) {
                const std::uint8_t* pt = base + (static_cast<std::size_t>(top_row) * w + xx) * 4;
                const std::uint8_t* pb = base + (static_cast<std::size_t>(bot_row) * w + xx) * 4;
                if (matches(pt, kTopBandRGB)) {
                    ++tt;
                }
                if (matches(pb, kTopBandRGB)) {
                    ++tb;
                }
                if (matches(pt, kBottomBandRGB)) {
                    ++bt;
                }
                if (matches(pb, kBottomBandRGB)) {
                    ++bb;
                }
            }
        }
        if (tt > 100 && bb > 100 && tt > tb && bb > bt) {
            return 1;
        }
        if (tb > 100 && bt > 100 && tb > tt && bt > bb) {
            return -1;
        }
        return 0;
    }
    return 0;
}

// ---- SurfaceHandle (public UiSurface impl) ----------------------------------

SurfaceHandle::~SurfaceHandle() {
    substrate_->impl_->destroy_surface(surface_);
}

void SurfaceHandle::set_position(int x, int y) {
    surface_->x = x;
    surface_->y = y;
    if (surface_->scene_buffer != nullptr) {
        wlr_scene_node_set_position(&surface_->scene_buffer->node, x, y);
    }
}

void SurfaceHandle::set_size(int width, int height) {
    // Geometry-only resize of an existing GL target is out of slice 5 (would
    // require re-allocating FBO/swapchain). Record logical size + resize the
    // Rml context; the rendered buffer keeps its allocated size. Documented in
    // ui.hpp as "takes effect on next frame"; full realloc is a slice-6 ask.
    surface_->width = width;
    surface_->height = height;
    if (surface_->context != nullptr) {
        surface_->context->SetDimensions(Rml::Vector2i(width, height));
    }
}

void SurfaceHandle::set_visible(bool visible) {
    surface_->is_visible = visible;
    if (surface_->scene_buffer != nullptr) {
        wlr_scene_node_set_enabled(&surface_->scene_buffer->node, visible);
    }
}

auto SurfaceHandle::visible() const -> bool { return surface_->is_visible; }

// All binds funnel through the surface's single open DataModelConstructor and
// MUST happen before the document loads (first render). Binding after load is a
// no-op (the constructor is closed) — documented in ui.hpp ("call before the
// first frame"). The slot lives in a std::list for stable addresses.
void SurfaceHandle::bind_int(std::string_view name, std::function<int()> getter) {
    Surface& s = *surface_;
    if (!s.ctor) {
        return;
    }
    s.int_bindings.push_back({std::move(getter), 0});
    s.ctor.Bind(std::string(name), &s.int_bindings.back().slot);
}
void SurfaceHandle::bind_double(std::string_view name, std::function<double()> getter) {
    Surface& s = *surface_;
    if (!s.ctor) {
        return;
    }
    s.double_bindings.push_back({std::move(getter), 0.0});
    s.ctor.Bind(std::string(name), &s.double_bindings.back().slot);
}
void SurfaceHandle::bind_bool(std::string_view name, std::function<bool()> getter) {
    Surface& s = *surface_;
    if (!s.ctor) {
        return;
    }
    s.bool_bindings.push_back({std::move(getter), false});
    s.ctor.Bind(std::string(name), &s.bool_bindings.back().slot);
}
void SurfaceHandle::bind_string(std::string_view name, std::function<std::string()> getter) {
    Surface& s = *surface_;
    if (!s.ctor) {
        return;
    }
    s.string_bindings.push_back({std::move(getter), Rml::String{}});
    s.ctor.Bind(std::string(name), &s.string_bindings.back().slot);
}
void SurfaceHandle::bind_event(std::string_view name, std::function<void()> callback) {
    Surface& s = *surface_;
    if (!s.ctor) {
        return;
    }
    s.event_bindings.push_back({std::move(callback), s.who, s.owner});
    Surface::EventBinding* binding = &s.event_bindings.back();
    s.ctor.BindEventCallback(
        std::string(name),
        [binding](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            try {
                if (binding->cb) {
                    binding->cb();
                }
            } catch (...) {
                if (binding->owner->disable) {
                    binding->owner->disable(binding->who);
                }
            }
        });
}

void SurfaceHandle::on_touch_mode_changed(std::function<void(bool)> callback) {
    surface_->touch_mode_cb = std::move(callback);
}

void SurfaceHandle::dirty(std::string_view name) {
    if (surface_->model) {
        surface_->model.DirtyVariable(std::string(name));
    }
}
void SurfaceHandle::dirty() {
    if (surface_->model) {
        surface_->model.DirtyAllVariables();
    }
}

} // namespace unbox::kernel
