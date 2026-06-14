#pragma once

// SPIKE (rml-compositing, Phase 0) — shared GL glue for the runnable target.
// THROWAWAY. The sibling GLES 3.2 bridge, the LIVE zero-copy surface-element
// import, and the RmlUi-FBO -> wlr_buffer present target, shared by the
// --verify TU and the --run (real-seat) TU. A trimmed copy of the substrate's
// proven GlBridge mechanics; we deliberately do NOT refactor the real substrate
// to share it (this is a spike). wlroots only via the kernel's wrapper.

#include <unbox/kernel/wlr.hpp>

#include "../rmlui_renderer_gl3.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/SystemInterface.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace unbox::kernel::spike {

constexpr std::uint32_t kArgb8888 = 0x34325241; // 'AR24' = LE {B,G,R,A}

inline auto now_sec() -> double {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// --- RmlUi SystemInterface: elapsed time + logs to stderr --------------------
class SpikeSystem final : public Rml::SystemInterface {
public:
    auto GetElapsedTime() -> double override {
        const double t = now_sec();
        if (start_ == 0.0) {
            start_ = t;
        }
        return t - start_;
    }
    auto LogMessage(Rml::Log::Type type, const Rml::String& msg) -> bool override {
        if (type <= Rml::Log::LT_WARNING) {
            std::fprintf(stderr, "[rmlui] %s\n", msg.c_str());
        }
        return true;
    }

private:
    double start_ = 0.0;
};

// --- A data-ptr wlr_buffer wrapping heap memory (Plan-B present / test src) ---
struct DataBuffer {
    wlr_buffer base{};
    std::vector<std::uint8_t> data;
    std::size_t stride = 0;
};
inline void db_destroy(wlr_buffer* b) {
    auto* d = reinterpret_cast<DataBuffer*>(b);
    wlr_buffer_finish(&d->base);
    delete d;
}
inline bool db_access(wlr_buffer* b, std::uint32_t, void** data, std::uint32_t* fmt,
                      std::size_t* stride) {
    auto* d = reinterpret_cast<DataBuffer*>(b);
    *data = d->data.data();
    *fmt = kArgb8888;
    *stride = d->stride;
    return true;
}
inline void db_end(wlr_buffer*) {}
inline const wlr_buffer_impl kDataImpl = {
    .destroy = db_destroy,
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = db_access,
    .end_data_ptr_access = db_end,
};
inline auto make_data_buffer(int w, int h) -> DataBuffer* {
    auto* d = new DataBuffer();
    d->stride = static_cast<std::size_t>(w) * 4;
    d->data.assign(d->stride * static_cast<std::size_t>(h), 0);
    wlr_buffer_init(&d->base, &kDataImpl, w, h);
    return d;
}

// --- The sibling GLES 3.2 bridge on the wlr EGLDisplay ------------------------
struct GlBridge {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLConfig config = nullptr;

    EGLContext saved_ctx = EGL_NO_CONTEXT;
    EGLSurface saved_draw = EGL_NO_SURFACE;
    EGLSurface saved_read = EGL_NO_SURFACE;

    SpikeSystem system;
    RenderInterface_GL3* render = nullptr;
    bool rml_init = false;
    bool ok = false;
    bool dmabuf_ok = false;
    bool fence_ok = false;

    PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target = nullptr;
    PFNEGLCREATESYNCKHRPROC create_sync = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC wait_sync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync = nullptr;

    auto make_current() -> bool {
        saved_ctx = eglGetCurrentContext();
        saved_draw = eglGetCurrentSurface(EGL_DRAW);
        saved_read = eglGetCurrentSurface(EGL_READ);
        return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_TRUE;
    }
    void restore_current() { eglMakeCurrent(dpy, saved_draw, saved_read, saved_ctx); }

    void submit_sync() {
        if (fence_ok) {
            EGLSyncKHR s = create_sync(dpy, EGL_SYNC_FENCE_KHR, nullptr);
            if (s != EGL_NO_SYNC_KHR) {
                glFlush();
                wait_sync(dpy, s, 0, EGL_FOREVER_KHR);
                destroy_sync(dpy, s);
                return;
            }
        }
        glFinish();
    }

    auto init(EGLDisplay display) -> bool {
        dpy = display;
        if (dpy == EGL_NO_DISPLAY || eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            return false;
        }
        const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                    EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                    EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint n = 0;
        if (eglChooseConfig(dpy, cfg_attrs, &config, 1, &n) != EGL_TRUE || n < 1) {
            return false;
        }
        const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2,
                                    EGL_NONE};
        ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
        if (ctx == EGL_NO_CONTEXT || !make_current()) {
            return false;
        }
        create_image =
            reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroy_image =
            reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        const char* exts = eglQueryString(dpy, EGL_EXTENSIONS);
        dmabuf_ok = exts != nullptr &&
                    std::strstr(exts, "EGL_EXT_image_dma_buf_import") != nullptr &&
                    create_image != nullptr && image_target != nullptr;
        create_sync =
            reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        wait_sync =
            reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
        destroy_sync =
            reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        fence_ok = exts != nullptr && std::strstr(exts, "EGL_KHR_fence_sync") != nullptr &&
                   create_sync != nullptr && wait_sync != nullptr && destroy_sync != nullptr;

        if (!RmlGL3::Initialize(nullptr)) {
            restore_current();
            return false;
        }
        render = new RenderInterface_GL3();
        if (!*render) {
            restore_current();
            return false;
        }
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(render);
        if (!Rml::Initialise()) {
            restore_current();
            return false;
        }
        rml_init = true;
        if (!Rml::LoadFontFace("/usr/share/fonts/noto/NotoSans-Regular.ttf")) {
            std::fprintf(stderr, "[spike] NotoSans not found; text labels will be blank\n");
        }
        restore_current();
        ok = true;
        std::fprintf(stderr, "[spike] GL bridge up (dmabuf_import=%d fence=%d)\n", dmabuf_ok,
                     fence_ok);
        return true;
    }

    void teardown() {
        const bool cur = (ctx != EGL_NO_CONTEXT) && make_current();
        if (rml_init) {
            Rml::Shutdown();
            rml_init = false;
        }
        delete render;
        render = nullptr;
        if (cur) {
            restore_current();
        }
        if (ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(dpy, ctx);
            ctx = EGL_NO_CONTEXT;
        }
    }
};

// --- A LIVE surface element: a client buffer imported zero-copy as a sampled
// texture, registered under a URI, re-imported on each NEW surface commit. ---
//
// FROZEN-FRAME FIX. The re-import was gated on the wlr_buffer POINTER changing
// (`buf == current`). That is WRONG for real clients: Wayland clients (foot)
// recycle a SMALL POOL of buffers, and wlroots re-uses the SAME wlr_client_buffer
// for a re-attached wl_buffer — so the identical pointer is re-committed with
// BRAND-NEW contents. The pointer-equality early-return then wrongly skipped the
// update and the displayed texture stayed stuck on buffer #1 (`commits=3` but
// `reimports=1` in the headless log). The correct dirty signal is the surface's
// COMMIT SEQUENCE (`wlr_surface_state.seq`), which increments on EVERY commit
// regardless of pool reuse. We re-import whenever the seq advances, re-binding
// the EGLImage to the current buffer (a live dmabuf view => new pixels) or
// re-uploading for shm, so new contents show even on a reused buffer pointer.
//
// BUFFER LIFECYCLE. `surface->buffer` is a wlr_client_buffer (the renderer-side
// import); wlroots has ALREADY released the client's underlying wl_buffer back
// to its pool, so reading it never starves the client. We still LOCK the buffer
// we are importing (so its dmabuf FDs stay valid while we build the EGLImage and
// sample it) and UNLOCK the PREVIOUS one once the new import is live — a
// double-buffered lock that mirrors wlroots' consumer lock/release discipline
// and guarantees we never pin more than one buffer at a time.
struct LiveTexture {
    GlBridge* gl = nullptr;
    std::string uri;
    int width = 0, height = 0;
    wlr_buffer* current = nullptr;       // the buffer currently imported + LOCKED
    std::uint32_t current_seq = 0;       // surface commit seq of `current`
    bool have_seq = false;               // false until the first adopt()
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint tex = 0;
    bool is_dmabuf = false;
    int reimports = 0;
    int commits_seen = 0;

    // Re-import the surface's CURRENT committed buffer for commit sequence `seq`.
    // `seq` MUST be the surface's wlr_surface_state.seq (advances every commit) —
    // NOT the buffer pointer, which a pooled client recycles. Returns true if the
    // sampled texture reflects the current buffer afterwards.
    auto adopt(wlr_buffer* buf, std::uint32_t seq) -> bool {
        ++commits_seen;
        // Idle gate: a static client never commits, so its seq never advances and
        // we do zero work (the dirty-gate stays intact). A re-committed buffer —
        // even the SAME pointer with new contents — bumps seq and re-imports.
        if (have_seq && seq == current_seq && buf == current && tex != 0) {
            return true; // truly unchanged surface state: zero re-import, zero copy
        }
        // Lock the buffer we are about to sample so its storage (dmabuf FDs / shm)
        // stays valid for the whole import+sample; unlock the PREVIOUS one once the
        // new import is live (double-buffered: at most one buffer pinned).
        wlr_buffer* prev = current;
        wlr_buffer_lock(buf);
        wlr_dmabuf_attributes attrs{};
        if (gl->dmabuf_ok && wlr_buffer_get_dmabuf(buf, &attrs) && attrs.n_planes >= 1) {
            EGLint ia[] = {EGL_WIDTH,
                           attrs.width,
                           EGL_HEIGHT,
                           attrs.height,
                           EGL_LINUX_DRM_FOURCC_EXT,
                           static_cast<EGLint>(attrs.format),
                           EGL_DMA_BUF_PLANE0_FD_EXT,
                           attrs.fd[0],
                           EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                           static_cast<EGLint>(attrs.offset[0]),
                           EGL_DMA_BUF_PLANE0_PITCH_EXT,
                           static_cast<EGLint>(attrs.stride[0]),
                           EGL_NONE};
            EGLImageKHR img =
                gl->create_image(gl->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, ia);
            if (img != EGL_NO_IMAGE_KHR) {
                release_gl();
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                gl->image_target(GL_TEXTURE_2D, static_cast<GLeglImageOES>(img));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);
                image = img;
                width = attrs.width;
                height = attrs.height;
                is_dmabuf = true;
                adopt_commit(prev, buf, seq);
                register_uri();
                return true;
            }
        }
        // Fallback: one CPU upload for an shm client.
        void* data = nullptr;
        std::uint32_t fmt = 0;
        std::size_t stride = 0;
        if (!wlr_buffer_begin_data_ptr_access(buf, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt,
                                              &stride)) {
            wlr_buffer_unlock(buf); // import failed: drop the lock we just took
            return false;
        }
        release_gl();
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride / 4));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, buf->width, buf->height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        wlr_buffer_end_data_ptr_access(buf);
        width = buf->width;
        height = buf->height;
        is_dmabuf = false;
        adopt_commit(prev, buf, seq);
        register_uri();
        return true;
    }

    // Commit a successful import: adopt `buf` (already locked) at sequence `seq`
    // and release the PREVIOUSLY-locked buffer (double-buffered lock). Counts a
    // reimport. NB: prev may equal buf when a pooled client re-commits the same
    // pointer with new contents — lock/unlock balance still holds (net +1 then
    // -1 => the single live lock we took above for THIS adopt).
    void adopt_commit(wlr_buffer* prev, wlr_buffer* buf, std::uint32_t seq) {
        current = buf;
        current_seq = seq;
        have_seq = true;
        ++reimports;
        if (prev != nullptr) {
            wlr_buffer_unlock(prev);
        }
    }

    void register_uri() {
        gl->render->register_preview_texture(uri, tex, Rml::Vector2i(width, height));
    }
    void release_gl() {
        if (tex != 0) {
            glDeleteTextures(1, &tex);
            tex = 0;
        }
        if (image != EGL_NO_IMAGE_KHR && gl->destroy_image != nullptr) {
            gl->destroy_image(gl->dpy, image);
            image = EGL_NO_IMAGE_KHR;
        }
    }
    void destroy() {
        if (gl != nullptr && gl->render != nullptr) {
            gl->render->unregister_preview_texture(uri);
        }
        release_gl();
        if (current != nullptr) {
            wlr_buffer_unlock(current); // release the buffer we held locked
            current = nullptr;
        }
        have_seq = false;
        current_seq = 0;
    }
};

// --- The RmlUi-FBO -> wlr_buffer present target (criterion 7) -----------------
struct PresentTarget {
    GlBridge* gl = nullptr;
    wlr_allocator* allocator = nullptr;
    int width = 0, height = 0;
    bool dmabuf = false;

    GLuint fbo = 0;
    GLuint shm_tex = 0;
    wlr_swapchain* swapchain = nullptr;
    std::unordered_map<wlr_buffer*, std::pair<EGLImageKHR, GLuint>> slot_gl;

    DataBuffer* shm = nullptr;
    std::vector<std::uint8_t> readback;

    wlr_scene_buffer* scene_buffer = nullptr;

    auto init(GlBridge* g, wlr_allocator* alloc, int w, int h) -> bool {
        gl = g;
        allocator = alloc;
        width = w;
        height = h;
        glGenFramebuffers(1, &fbo);
        if (gl->dmabuf_ok && (allocator->buffer_caps & WLR_BUFFER_CAP_DMABUF) != 0) {
            wlr_drm_format fmt{};
            fmt.format = kArgb8888;
            std::uint64_t mods[] = {0};
            fmt.len = 1;
            fmt.capacity = 1;
            fmt.modifiers = mods;
            swapchain = wlr_swapchain_create(allocator, w, h, &fmt);
            if (swapchain != nullptr) {
                dmabuf = true;
            }
        }
        if (!dmabuf) {
            glGenTextures(1, &shm_tex);
            glBindTexture(GL_TEXTURE_2D, shm_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shm_tex, 0);
            const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (st != GL_FRAMEBUFFER_COMPLETE) {
                return false;
            }
            shm = make_data_buffer(w, h);
            readback.assign(static_cast<std::size_t>(w) * h * 4, 0);
        }
        return true;
    }

    auto render(Rml::Context* ctx) -> wlr_buffer* {
        GLuint target = fbo;
        wlr_buffer* dmabuf_target = nullptr;
        if (dmabuf) {
            wlr_buffer* buf = wlr_swapchain_acquire(swapchain);
            if (buf == nullptr) {
                return nullptr;
            }
            dmabuf_target = buf;
            auto it = slot_gl.find(buf);
            if (it == slot_gl.end()) {
                wlr_dmabuf_attributes a{};
                if (!wlr_buffer_get_dmabuf(buf, &a) || a.n_planes < 1) {
                    wlr_buffer_unlock(buf);
                    return nullptr;
                }
                EGLint ia[] = {EGL_WIDTH,
                               a.width,
                               EGL_HEIGHT,
                               a.height,
                               EGL_LINUX_DRM_FOURCC_EXT,
                               static_cast<EGLint>(a.format),
                               EGL_DMA_BUF_PLANE0_FD_EXT,
                               a.fd[0],
                               EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                               static_cast<EGLint>(a.offset[0]),
                               EGL_DMA_BUF_PLANE0_PITCH_EXT,
                               static_cast<EGLint>(a.stride[0]),
                               EGL_NONE};
                EGLImageKHR img =
                    gl->create_image(gl->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, ia);
                if (img == EGL_NO_IMAGE_KHR) {
                    wlr_buffer_unlock(buf);
                    return nullptr;
                }
                GLuint t = 0;
                glGenTextures(1, &t);
                glBindTexture(GL_TEXTURE_2D, t);
                gl->image_target(GL_TEXTURE_2D, static_cast<GLeglImageOES>(img));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                it = slot_gl.emplace(buf, std::make_pair(img, t)).first;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   it->second.second, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                wlr_buffer_unlock(buf);
                return nullptr;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        gl->render->SetViewport(width, height);
        gl->render->SetOutputFramebuffer(target, /*flip_y=*/true);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ctx->Update();
        gl->render->BeginFrame();
        ctx->Render();
        gl->render->EndFrame();

        if (dmabuf) {
            gl->submit_sync();
            if (scene_buffer != nullptr) {
                wlr_scene_buffer_set_buffer(scene_buffer, dmabuf_target);
            }
            wlr_buffer_unlock(dmabuf_target);
            return dmabuf_target;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        const std::size_t px = static_cast<std::size_t>(width) * height;
        for (std::size_t i = 0; i < px; ++i) {
            shm->data[i * 4 + 0] = readback[i * 4 + 2];
            shm->data[i * 4 + 1] = readback[i * 4 + 1];
            shm->data[i * 4 + 2] = readback[i * 4 + 0];
            shm->data[i * 4 + 3] = readback[i * 4 + 3];
        }
        if (scene_buffer != nullptr) {
            wlr_scene_buffer_set_buffer(scene_buffer, &shm->base);
        }
        return &shm->base;
    }

    void pixel(int x, int y, std::uint8_t out[4]) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void teardown() {
        for (auto& [buf, slot] : slot_gl) {
            if (slot.second != 0) {
                glDeleteTextures(1, &slot.second);
            }
            if (slot.first != EGL_NO_IMAGE_KHR && gl->destroy_image != nullptr) {
                gl->destroy_image(gl->dpy, slot.first);
            }
        }
        slot_gl.clear();
        if (shm_tex != 0) {
            glDeleteTextures(1, &shm_tex);
        }
        if (fbo != 0) {
            glDeleteFramebuffers(1, &fbo);
        }
        if (swapchain != nullptr) {
            wlr_swapchain_destroy(swapchain);
        }
        if (shm != nullptr) {
            wlr_buffer_drop(&shm->base);
        }
    }
};

} // namespace unbox::kernel::spike
