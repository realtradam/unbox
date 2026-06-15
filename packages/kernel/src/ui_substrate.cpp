#include "ui_substrate.hpp"

#include "file_watcher.hpp"
#include "input_core.hpp" // pure surface-element input-back geometry (place_child_box)
#include "listener.hpp" // RAII wl_listener for the surface-element commit hook
#include "rmlui_renderer_gl3.h"

#include <RmlUi/Core/Animation.h> // Transition / TransitionList / Tween
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/DataVariable.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/ID.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StyleSheetSpecification.h> // GetPropertyId
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Tween.h>
#include <RmlUi/Core/Variant.h>

// The kernel owns GL; system EGL/GLES headers are allowed here (same as the
// retired spike). wlr.hpp already pulled <EGL/egl.h>+<EGL/eglext.h> via
// wlr/render/egl.h and GLES via the adapted renderer.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h> // glEGLImageTargetTexture2DOES
#include <GLES3/gl32.h>

// BTN_LEFT for the surface-element input-back pointer-button forward. A pure
// value header (input-event codes), not a wlroots/GL effect surface.
#include <linux/input-event-codes.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The installed asset root, resolved against for a RELATIVE UiSurfaceSpec::
// rml_path when $UNBOX_ASSET_DIR is unset. The orchestrator adds
// -DUNBOX_ASSET_DIR_DEFAULT="<installed data dir>" to the kernel build; until
// then "." lets the kernel build + run from the source/working dir.
#ifndef UNBOX_ASSET_DIR_DEFAULT
#define UNBOX_ASSET_DIR_DEFAULT "."
#endif

namespace unbox::kernel {

namespace {

constexpr std::uint32_t kDrmFormatArgb8888 = 0x34325241; // 'AR24' = LE {B,G,R,A}

// Resolve UiSurfaceSpec::rml_path to an ABSOLUTE filesystem path so RmlUi can
// load it and resolve the document's relative <link>/<style>/image refs against
// its own directory. Absolute paths are used as-is; a relative path resolves
// against $UNBOX_ASSET_DIR (dev) else UNBOX_ASSET_DIR_DEFAULT (installed data
// dir, "." fallback). Pure string/path math — no I/O, no throw.
auto resolve_asset_path(const std::string& rml_path) -> std::string {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(rml_path);
    if (p.is_absolute()) {
        return p.lexically_normal().string();
    }
    const char* env = std::getenv("UNBOX_ASSET_DIR");
    const fs::path root = (env != nullptr && env[0] != '\0') ? fs::path(env)
                                                             : fs::path(UNBOX_ASSET_DIR_DEFAULT);
    fs::path joined = (root / p).lexically_normal();
    // Make it absolute against the cwd if the root itself was relative (e.g. the
    // "." fallback), so RmlUi's relative-ref resolution has a stable base.
    if (!joined.is_absolute()) {
        joined = (fs::current_path(ec) / joined).lexically_normal();
    }
    return joined.string();
}

// True if the dev hot-reload watcher should run for this process. Gated so a
// production build does zero watching (no inotify fd, no overhead).
auto hot_reload_enabled() -> bool {
    return std::getenv("UNBOX_DEV") != nullptr || std::getenv("UNBOX_HOT_RELOAD") != nullptr;
}

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
        // RmlUi's LoadDocument returns a (possibly empty) document even when the
        // XML fails to parse — the failure is only LOGGED. So the hot-reload path
        // counts parse errors here to decide whether a fresh load was actually
        // good (see reload_surface): a "XML parse error" warning or any load-time
        // ERROR during a reload means keep the previous document.
        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ||
            (type == Rml::Log::LT_WARNING && message.find("parse error") != Rml::String::npos)) {
            ++parse_errors_;
        }
        return true;
    }

    // Snapshot/read the parse-error counter (hot-reload validity check).
    [[nodiscard]] auto parse_errors() const -> int { return parse_errors_; }

private:
    double start_ = 0.0;
    int parse_errors_ = 0;
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
    std::string rml_path;        // the spec's path (as the extension passed it)
    std::string resolved_path;   // absolute path actually loaded (file-backed only)
    bool doc_loaded = false;
    // Dev asset hot-reload (UNBOX_DEV): a watch on resolved_path's dir whose
    // callback flags this surface for reload. Released when the Surface dies
    // (stops the underlying inotify watch). Inactive for inline/non-dev surfaces.
    FileWatch asset_watch;

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
    // Drag bindings: one callback fed by RMLUi's Dragstart/Drag/Dragend for the
    // element(s) authoring data-event-drag{start,,end}=<name>. The phase is read
    // off the live Rml::Event id; x/y are the event's mouse_x/mouse_y which the
    // substrate already feeds the context in surface-LOCAL coords (ctx_motion
    // subtracts s.x/s.y), so they ARE surface-local px (origin top-left). Same
    // error-isolation boundary as EventBinding.
    struct DragBinding {
        std::function<void(UiSurface::DragPhase, double, double)> cb;
        ExtensionId who;
        Substrate::Impl* owner;
    };
    std::list<DragBinding> drag_bindings;

    // List bindings (slice 10 / b2). A bound list is a runtime-sized indexed
    // sequence the document iterates with data-for; each row exposes named
    // string/int/double/bool FIELDS read as {{ row.<field> }}. The shape maps
    // onto RmlUi's data-binding type system via three owned VariableDefinitions
    // per list (Array -> row Struct -> per-field Scalar); the row index is
    // smuggled through the DataVariable `void* ptr` (no per-row heap object).
    // All getters/count follow the scalar contract (cheap, pure, lifetime =
    // surface, throw => isolate). Stored in a std::list so addresses are stable
    // (the VariableDefinitions hold a ListBinding*). Defined below the Surface.
    struct ListBinding;
    std::list<ListBinding> list_bindings;
    // Per-list event callbacks (keyed by event name). A row event delivers the
    // row index extracted from the data expression's first argument (it_index).
    struct ListEventBinding {
        std::function<void(std::size_t)> cb;
        ExtensionId who;
        Substrate::Impl* owner;
    };
    std::list<ListEventBinding> list_event_bindings;
    // Per-row drag callbacks (bind_list_drag): combine the drag phase + surface-
    // local x/y (like DragBinding) with the row index from the data expression's
    // first argument (like ListEventBinding). Same error-isolation boundary.
    struct ListDragBinding {
        std::function<void(std::size_t, UiSurface::DragPhase, double, double)> cb;
        ExtensionId who;
        Substrate::Impl* owner;
    };
    std::list<ListDragBinding> list_drag_bindings;

    // touch-mode-changed notification (one per surface; see ui.hpp). Fired on a
    // transition, error-isolated to `who`. touch-mode does NO visual scaling
    // (user decision) — this is purely an opt-in signal for extensions.
    std::function<void(bool)> touch_mode_cb;

    // Scene.
    wlr_scene_buffer* scene_buffer = nullptr;

    int frame_count = 0;
};

// ---- List bindings: the RMLUi-free list shape -> RmlUi data-binding types ---
//
// data-for="row : <name>" makes RmlUi ask the named variable for its Size() and
// then a Child per index; {{ row.<field> }} asks that child (a row) for a Child
// per field name; the field child is a scalar that yields a Variant. We satisfy
// all three with custom VariableDefinitions (NonCopyMoveable, owned by the
// Surface for its whole life — they outlive the RmlUi context, which is torn
// down first in destroy_surface). The row index is carried through the
// DataVariable's `void* ptr` as an encoded integer, so there is NO per-row heap
// object and rows cost nothing until rendered. count()/getters are called
// straight out of the ListBinding; a throw is isolated to the owning extension.
namespace {
// Encode/decode a row index in the opaque DataVariable ptr (index + 1 so the
// encoded value is never the null we hand RmlUi for an out-of-range child).
inline auto encode_row(std::size_t row) -> void* {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(row) + 1);
}
inline auto decode_row(void* ptr) -> std::size_t {
    return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(ptr)) - 1;
}
} // namespace

// One bound list's full state: the count getter, the per-field getters (by
// name+type), and the three VariableDefinitions wired Array -> Struct -> Scalar.
// `isolate` lets a getter throw without taking down the session (it calls the
// substrate's DisableSink for `who`). Lives in Surface::list_bindings.
struct Surface::ListBinding {
    std::string name;
    std::function<std::size_t()> count;
    ExtensionId who{};
    // A copy of the substrate's DisableSink so a throwing count/getter isolates
    // the owning extension WITHOUT this struct (defined before Substrate::Impl)
    // needing the complete Impl type.
    SubstrateDisableFn disable;

    std::unordered_map<std::string, std::function<bool(std::size_t, Rml::Variant&)>> fields;

    // Run a field/count call, isolating a throw to the owning extension.
    template <typename Fn>
    auto isolate(Fn&& fn) -> bool {
        try {
            fn();
            return true;
        } catch (...) {
            if (disable) {
                disable(who);
            }
            return false;
        }
    }

    // The scalar at (row, field): decode the row, call the field getter.
    struct FieldDef final : Rml::VariableDefinition {
        FieldDef(ListBinding* b, std::function<bool(std::size_t, Rml::Variant&)>* f)
            : Rml::VariableDefinition(Rml::DataVariableType::Scalar), binding(b), field(f) {}
        bool Get(void* ptr, Rml::Variant& variant) override {
            const std::size_t row = decode_row(ptr);
            bool got = false;
            binding->isolate([&] { got = (*field)(row, variant); });
            return got;
        }
        ListBinding* binding;
        std::function<bool(std::size_t, Rml::Variant&)>* field;
    };

    // The row struct: a Child per field name (passing the encoded row through).
    struct RowDef final : Rml::VariableDefinition {
        explicit RowDef(ListBinding* b)
            : Rml::VariableDefinition(Rml::DataVariableType::Struct), binding(b) {}
        Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override {
            auto it = binding->field_defs.find(address.name);
            if (it == binding->field_defs.end()) {
                return Rml::DataVariable();
            }
            return Rml::DataVariable(it->second.get(), ptr); // ptr already encodes the row
        }
        Rml::StringList ReflectMemberNames() override {
            Rml::StringList names;
            for (const auto& [n, def] : binding->field_defs) {
                names.push_back(n);
            }
            return names;
        }
        ListBinding* binding;
    };

    // The array: Size() = count(); Child(i) = a row encoding index i.
    struct ArrayDef final : Rml::VariableDefinition {
        explicit ArrayDef(ListBinding* b)
            : Rml::VariableDefinition(Rml::DataVariableType::Array), binding(b) {}
        int Size(void* /*ptr*/) override {
            std::size_t n = 0;
            if (binding->count) {
                binding->isolate([&] { n = binding->count(); });
            }
            return static_cast<int>(n);
        }
        Rml::DataVariable Child(void* /*ptr*/, const Rml::DataAddressEntry& address) override {
            if (address.index < 0) {
                return Rml::DataVariable();
            }
            return Rml::DataVariable(&binding->row_def, encode_row(static_cast<std::size_t>(address.index)));
        }
        ListBinding* binding;
    };

    // The owned definitions (constructed in init(); addresses stable thereafter
    // because ListBinding lives in a std::list). field_defs maps field name ->
    // its scalar definition; row_def/array_def are the single struct/array.
    std::unordered_map<std::string, std::unique_ptr<FieldDef>> field_defs;
    RowDef row_def{nullptr};
    ArrayDef array_def{nullptr};

    void init() {
        // Re-seat the back-pointers now that the ListBinding has its final
        // address (it was emplaced into the std::list before init()).
        row_def.binding = this;
        array_def.binding = this;
    }
    auto add_field(const std::string& field, std::function<bool(std::size_t, Rml::Variant&)> fn)
        -> void {
        auto [it, inserted] = fields.insert_or_assign(field, std::move(fn));
        auto def_it = field_defs.find(field);
        if (def_it == field_defs.end()) {
            field_defs.emplace(field, std::make_unique<FieldDef>(this, &it->second));
        } else {
            def_it->second->field = &it->second; // re-seat after insert_or_assign
        }
    }
};

// ---- PreviewState -----------------------------------------------------------
//
// A frozen snapshot of a scene subtree, imported as a sampled GL texture in the
// RMLUi sibling context and registered under an "unbox-preview://N" URI. The
// snapshot is captured into an ARGB8888 LINEAR dmabuf by the wlr renderer
// (wlr_renderer_begin_buffer_pass), then that dmabuf is imported into the
// sibling context exactly like the surface path (EGLImage -> texture), but here
// the texture is SAMPLED by RmlUi rather than used as an FBO color attachment.
// This is the slice-3 bridge run in reverse (wlr pixels -> dmabuf -> EGLImage ->
// RmlUi texture). Lives in Substrate::Impl::previews (stable addresses).
struct PreviewState {
    Substrate::Impl* owner = nullptr;
    int id = 0;
    std::string uri;

    wlr_scene_tree* source = nullptr; // borrow; valid only per call (caller's concern)
    int width = 0;
    int height = 0;

    // The snapshot dmabuf (held alive for the texture's life) + its import.
    wlr_buffer* buffer = nullptr;      // ARGB8888 LINEAR dmabuf (own_buffer)
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint tex = 0;                    // sampled by RmlUi via the URI registration
    bool dmabuf = false;               // true once a dmabuf import succeeded
};

// ---- SurfaceElementState (a surface TREE) -----------------------------------
//
// A LIVE surface element: a client wl_surface ROOT + its subsurfaces + (if it is
// an xdg surface) its xdg popups, each imported zero-copy as its OWN sampled GL
// texture in the RMLUi sibling context and registered under its OWN
// "unbox-surface://N" / "unbox-surface://N.K" URI — the live sibling of
// PreviewState, generalised to a TREE (Wave 1b). Ports spike `LiveTexture`
// (src/spike/spike_gl.hpp) + the per-LiveSurface subsurface/popup model
// (rml_compositing_spike_run.cpp): the seq-gated re-import (§0d frozen-frame fix)
// + the double-buffered wlr_buffer lock/unlock lifecycle (at most ONE buffer
// pinned PER NODE). Re-import + frame-done run on the sibling context inside
// tick_all (the caller makes it current); the RAII commit Listener (kernel-
// private, never crosses the contract) marks the element dirty + asks the kernel
// to schedule a frame. Lives in Substrate::Impl::surface_elements (stable addrs).

// One node of the surface tree: a single wl_surface's live import (root, a
// subsurface, or a popup's surface). Its `surface` is a BORROW valid only while
// the node exists in the tree — the substrate re-enumerates the live tree each
// frame (adopt_surface_element) and drops a node the instant its surface stops
// appearing in the walk, so it never holds a freed child surface across frames.
struct SurfaceNode {
    wlr_surface* surface = nullptr; // BORROW (see above)
    std::string uri;                // this node's <img src> URI
    int width = 0;                  // node surface natural px (tracks commits)
    int height = 0;
    int sx = 0;                     // tree offset within the ROOT surface (px)
    int sy = 0;
    bool is_popup = false;          // popups are NOT parent-clipped (spike crit 4)

    // The live import (port of spike LiveTexture). `current` is the buffer we
    // hold LOCKED + have imported; `current_seq` its surface commit seq.
    wlr_buffer* current = nullptr;
    std::uint32_t current_seq = 0;
    bool have_seq = false;             // false until the first adopt()
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint tex = 0;

    bool seen = false;                 // tree-walk mark (this frame) — GC sweep
};

struct SurfaceElementState {
    Substrate::Impl* owner = nullptr;
    int id = 0;
    // The OWNING extension (for error isolation of on_pressed). A throwing
    // on_pressed handler disables THIS extension only, via owner->disable(who).
    ExtensionId who{};

    // The click/tap-to-focus SIGNAL handler (ui.hpp SurfaceElement::on_pressed):
    // invoked, error-isolated, when a pointer-button PRESS or a touch DOWN is
    // routed to this element (root OR a child node). Default none (no-op).
    std::function<void()> on_pressed_cb;

    // The ROOT node. `root.surface` is the wl_surface passed to
    // create_surface_element — a BORROW the caller outlives (ui.hpp). `root.uri`
    // is the element's source_uri() (the single <img src> a consumer authors);
    // child nodes get sibling URIs and the substrate places their <img> elements
    // relative to the root's resolved box (parent-relative child placement).
    SurfaceNode root;
    // Per-subsurface / per-popup child nodes, in TREE (= composite) order. Each
    // its own live texture + URI; placed at its tree offset relative to the root.
    std::list<SurfaceNode> children;
    int next_child_id = 0;             // numbers child URIs "unbox-surface://N.K"

    bool dmabuf = false;               // last import (any node) took the dmabuf path
    int reimports = 0;                 // REAL re-imports across ALL nodes — test probe
    int frame_done_sends = 0;          // frame-done calls across ALL nodes — test probe

    // RAII commit hook on the ROOT surface: a client commit dirties this element
    // + kicks the dirty-gate (the whole tree is re-walked + re-imported next
    // tick). Destruction (element drop) unsubscribes — never a bare wl_listener
    // across the contract (.unbox/rules/listener-lifetime.md).
    Listener commit_l;
    bool needs_reimport = true;        // a commit happened => re-walk + re-adopt next tick

    // Convenience: the element's stable source_uri() (== root.uri).
    [[nodiscard]] auto uri() const -> const std::string& { return root.uri; }
};

// ---- Substrate::Impl --------------------------------------------------------

struct Substrate::Impl {
    GlBridge gl;
    wlr_allocator* allocator = nullptr;
    wlr_renderer* renderer = nullptr;
    // The kernel's seat — a BORROW (the seat is destroyed with the wl_display,
    // AFTER the substrate, so it outlives every input-back call). Used ONLY by
    // surface-element input-back (route_* forwarding a pick to the client via
    // wlr_seat_pointer/touch/keyboard_notify_*). May be null (no seat / test).
    wlr_seat* seat = nullptr;
    SubstrateDisableFn disable;
    // Ask the kernel to schedule an output frame (the dirty-gate kick for live
    // surface elements: a client commit, or the continuous frame-callback loop
    // while >=1 element exists). No-op-safe before any output exists.
    SubstrateScheduleFn schedule;

    TouchModeTracker touch_mode_tracker;

    std::list<Surface> surfaces; // stable addresses (handles borrow Surface*)

    // ---- Dev asset hot-reload (UNBOX_DEV-gated) ----
    // The substrate does NOT own an inotify fd: it borrows the kernel's ONE
    // shared FileWatcher (injected at create). Each file-backed surface holds a
    // FileWatch whose callback flags the surface in `pending_reloads`; the
    // reload itself is applied (coalesced) at the next tick_all so it happens
    // inside the frame, on the GL context, like every other render step. Only
    // the DECISION to watch UI assets is UNBOX_DEV-gated — the watcher infra is
    // always available (config watching via Host::watch_file is ungated).
    FileWatcher* watcher = nullptr; // kernel-owned borrow (may be null: no loop)
    // surfaces flagged dirty by a file event, drained (coalesced) at tick_all.
    std::vector<Surface*> pending_reloads;

    // Stop flagging `s` for reload (on destroy). The FileWatch on the Surface
    // itself stops the inotify watch when the Surface is erased.
    void unwatch_surface(Surface* s);
    // Reload `s`'s document from its file, preserving context/model/bindings/
    // geometry/visibility/previews; error-isolated (keeps the old doc on a bad
    // parse). Returns true if a NEW document was installed. Caller holds nothing;
    // this makes the GL context current itself.
    bool reload_surface(Surface& s);
    // Load `s`'s document for the FIRST time (file via resolved path, else
    // inline), Show() it, and register the hot-reload watch if file-backed.
    // Caller holds the sibling context current. Returns the loaded document or
    // nullptr (logged, never throws).
    Rml::ElementDocument* load_document_first(Surface& s);

    // Previews (slice-10 spike): stable addresses (PreviewHandle borrows a
    // PreviewState*). `next_preview_id` numbers the "unbox-preview://N" URIs.
    std::list<PreviewState> previews;
    int next_preview_id = 0;
    bool last_preview_dmabuf = false; // test probe: last import took the dmabuf path
    int resize_realloc_count = 0;     // test probe: # of set_size GL-target reallocs

    // Live surface elements (RML compositing Wave 1/1b): stable addresses
    // (SurfaceElementHandle borrows a SurfaceElementState*). next_surface_id
    // numbers the "unbox-surface://N" URIs (root nodes).
    std::list<SurfaceElementState> surface_elements;
    int next_surface_id = 0;
    bool last_surface_element_dmabuf = false; // test probe: last import path

    // The wl_surface that currently holds keyboard focus via a surface element
    // (set by SurfaceElement::focus_keyboard, cleared when that element/its node
    // is destroyed). A BORROW used only to clear focus on destroy; never deref'd
    // for routing. Focus POLICY is a later wave — this is only the mechanism.
    wlr_surface* keyboard_focus = nullptr;

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
    // Free ONLY the GL render-target resources of `s` (FBO, swapchain + its
    // cached EGLImages/textures, Plan-B texture/shm buffer, readback scratch) —
    // leaves the context/document/scene_buffer/bindings intact. Caller holds the
    // sibling context current. Shared by destroy_surface and the resize path.
    void free_surface_gl(Surface& s);
    // Reallocate `s`'s render target to w×h (FBO + swapchain/shm + EGLImage +
    // texture). Updates s.width/s.height and the RmlUi context dimensions. Caller
    // must guarantee w>0 && h>0. Returns false if the rebuild failed (the surface
    // is then left with no GL target and will not render until a later resize
    // succeeds). Caller holds the sibling context current.
    bool resize_surface_gl(Surface& s, int w, int h);
    void render_surface(Surface& s); // caller holds context current
    void destroy_surface(Surface* s);

    // Preview snapshot + import (caller holds the sibling context current).
    // snapshot_into_buffer composites every WLR_SCENE_NODE_BUFFER under `source`
    // into `p.buffer` via the wlr renderer; import_snapshot (re)imports that
    // dmabuf as the sampled GL texture and registers the URI. Both return false
    // (and clean up) on any failure.
    bool snapshot_into_buffer(PreviewState& p);
    bool import_snapshot(PreviewState& p);
    void destroy_preview(PreviewState* p);

    // Surface elements (RML compositing Wave 1/1b).
    //
    // adopt_node re-imports ONE node's surface CURRENT buffer iff its commit seq
    // advanced (seq-gate); manages the double-buffered wlr_buffer lock + registers
    // the node URI. Caller holds the sibling context current. Returns true if the
    // node's sampled texture reflects its current buffer afterwards.
    bool adopt_node(SurfaceElementState& el, SurfaceNode& node);
    // Re-walk `el`'s live surface TREE (root + subsurfaces + xdg popups): create
    // child nodes for newly-appeared surfaces, drop nodes whose surface vanished,
    // refresh each node's tree offset, and re-import every node (seq-gated). This
    // is the Wave-1b tree generalisation of adopt: composite order = tree order,
    // popups NOT parent-clipped. Caller holds the sibling context current.
    bool adopt_surface_element(SurfaceElementState& el);
    // Drop ONE node's GL import (texture/EGLImage/held lock) + URI registration.
    // Caller holds the sibling context current for the GL/URI part.
    void destroy_node(SurfaceNode& node);
    // Drop the whole element: every node's import + the root commit listener, and
    // clear keyboard focus if it pointed at any of this element's surfaces.
    void destroy_surface_element(SurfaceElementState* s);
    // Place the per-node child <img> elements in EVERY ui surface document that
    // hosts this element's root <img src=root.uri>, at each child's tree offset
    // relative to the root's resolved box (parent-relative placement). Removes a
    // child <img> whose node is gone. Pure DOM glue (no GL); called from tick_all
    // after the tree was re-walked. Idempotent per frame.
    void layout_surface_element_children(SurfaceElementState& el);

    // The resolved on-screen content box (px) of this element's ROOT <img> in the
    // FIRST ui surface document that hosts it (ui.hpp SurfaceElement::
    // rendered_width/height — the kernel reading back RCSS's computed rectangle).
    // {0,0} when no laid-out document hosts the element yet.
    [[nodiscard]] auto surface_element_rendered_size(const SurfaceElementState& el) const
        -> std::pair<int, int>;

    // ---- surface-element input-back ----
    // Resolve a hovered RmlUi element to the surface-element NODE it samples (its
    // <img src> is a node URI), walking up parents. Returns null if the pick is
    // not over a surface element (then normal ui routing / data-events apply).
    // `out_el` receives the owning element. Borrows valid only during the call.
    auto node_for_hovered(Rml::Element* hovered, SurfaceElementState*& out_el) -> SurfaceNode*;
    // Forward a transform-aware pick on `surf`'s context at surface-local doc
    // coords (lx-s.x, ly-s.y) to the client surface under it, via the seat, at
    // surface-LOCAL px (Element::Project through the node <img>'s real transform,
    // then box->surface scale — the spike route_point fix). Returns true if a
    // client surface was found + entered (so the caller knows a client was hit).
    // `pressed`/`is_button` drive the pointer button + implicit-grab semantics.
    enum class PointerKind { motion, button_down, button_up };
    auto forward_pointer_to_client(Surface& surf, double lx, double ly, PointerKind kind,
                                   std::uint32_t button, std::uint32_t time_msec) -> bool;
    auto forward_touch_to_client(Surface& surf, double lx, double ly, std::int32_t id,
                                 bool down, std::uint32_t time_msec) -> bool;
    // Fire `el`'s on_pressed handler (the click/tap-to-focus SIGNAL), if set.
    // Called ONLY for a pointer-button PRESS / touch DOWN routed to a surface
    // element (root OR child) — never on motion/release/miss. Error-isolated to
    // the owning extension (a throw => disable(el.who)), like the other ui
    // callbacks; never takes down the session.
    void fire_pressed(SurfaceElementState& el);
    // Keyboard-focus PRIMITIVE (mechanism only — focus POLICY is a later wave).
    // Give `el`'s ROOT client surface seat keyboard focus + send the enter with
    // the active keyboard's pressed keys + modifiers, so the kernel's existing
    // key passthrough (input.cpp) routes subsequent keys to it. Idempotent.
    // Clearing focus on element/node destroy is handled in destroy_*.
    void focus_keyboard(SurfaceElementState& el);

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

// ---- Document load (first) + dev asset hot-reload --------------------------

Rml::ElementDocument* Substrate::Impl::load_document_first(Surface& s) {
    Rml::ElementDocument* doc = nullptr;
    if (!s.rml_path.empty()) {
        s.resolved_path = resolve_asset_path(s.rml_path);
        doc = s.context->LoadDocument(s.resolved_path);
        if (doc == nullptr) {
            wlr_log(WLR_ERROR, "ui-substrate: failed to load document '%s' (resolved '%s')",
                    s.rml_path.c_str(), s.resolved_path.c_str());
            return nullptr;
        }
        // Dev-only: register an asset hot-reload watch on the kernel's SHARED
        // file watcher (the same machinery Host::watch_file uses). We watch the
        // document's whole DIRECTORY (add_dir), NOT just the .rml basename:
        // RmlUi resolves `<link href="x.rcss">` and image srcs relative to the
        // document dir, and the dock keeps its dock.rcss BESIDE dock.rml — so a
        // change to ANY file in that dir (the .rml OR a linked .rcss/asset) must
        // reload. (Watching only the .rml basename was the regression: editing
        // dock.rcss fired no reload.) The callback flags this surface for reload,
        // applied (coalesced) at the next tick_all on the GL context. Only this
        // DECISION is UNBOX_DEV-gated; the watcher infra itself is always
        // available (Host::watch_file is ungated).
        if (hot_reload_enabled() && watcher != nullptr) {
            Surface* sp = &s;
            const std::string dir =
                std::filesystem::path(s.resolved_path).parent_path().string();
            s.asset_watch = watcher->add_dir(
                dir,
                [this, sp] {
                    if (std::find(pending_reloads.begin(), pending_reloads.end(), sp) ==
                        pending_reloads.end()) {
                        pending_reloads.push_back(sp);
                    }
                },
                s.who);
            wlr_log(WLR_INFO,
                    "ui-substrate: dev hot-reload ON (inotify watching asset dir '%s')",
                    dir.c_str());
        }
    } else {
        doc = s.context->LoadDocumentFromMemory(s.rml_inline);
        if (doc == nullptr) {
            wlr_log(WLR_ERROR, "ui-substrate: failed to load inline document");
            return nullptr;
        }
    }
    doc->Show();
    return doc;
}

void Substrate::Impl::unwatch_surface(Surface* s) {
    // The Surface's own FileWatch (asset_watch) stops the inotify watch when the
    // Surface is erased; here we just drop any queued reload so a destroyed
    // surface is never reloaded.
    pending_reloads.erase(std::remove(pending_reloads.begin(), pending_reloads.end(), s),
                          pending_reloads.end());
}

bool Substrate::Impl::reload_surface(Surface& s) {
    // Only file-backed, already-loaded surfaces reload. The data model + the
    // extension's registered bind_*/bind_list*/bind_event/bind_drag getters are
    // CONTEXT- and substrate-owned, so they survive — we never touch
    // s.ctor/s.*_bindings (the reloaded document re-binds data-event-drag* to
    // the still-present model callback by name).
    if (s.context == nullptr || s.resolved_path.empty() || !s.doc_loaded) {
        return false;
    }
    const bool cur = gl.make_current();
    bool installed = false;
    try {
        // RCSS edits only re-parse if the stylesheet cache is dropped first.
        // Load the NEW document BEFORE unloading the old one, so a broken file
        // leaves the previous good document rendering (error isolation). RmlUi
        // returns a (possibly empty) document even on a parse failure — it only
        // LOGS the error — so we bracket the load with the SystemInterface's
        // parse-error counter and treat any increase as a failed reload.
        Rml::Factory::ClearStyleSheetCache();
        const int errors_before = gl.system ? gl.system->parse_errors() : 0;
        Rml::ElementDocument* fresh = s.context->LoadDocument(s.resolved_path);
        const bool parse_failed = gl.system && gl.system->parse_errors() != errors_before;
        if (fresh != nullptr && parse_failed) {
            // The "fresh" document is broken (empty/partial); discard it and keep
            // the previous good one rendering.
            s.context->UnloadDocument(fresh);
            fresh = nullptr;
        }
        if (fresh == nullptr) {
            wlr_log(WLR_ERROR, "ui-substrate: hot-reload parse failed for '%s'; keeping previous",
                    s.resolved_path.c_str());
        } else {
            if (s.document != nullptr) {
                s.context->UnloadDocument(s.document);
            }
            s.document = fresh;
            // Re-apply geometry/visibility (a reload must not move/resize/hide).
            // The context was already laid out to s.width/s.height; visibility is
            // the surface's current state, not the document default.
            if (s.is_visible) {
                s.document->Show();
            } else {
                s.document->Hide();
            }
            // The fresh document re-binds to the still-present data model; force
            // every bound variable to re-read on the next frame.
            if (s.model) {
                s.model.DirtyAllVariables();
            }
            installed = true;
        }
    } catch (...) {
        // A throwing reload is contained to the owning extension exactly like a
        // throwing getter/hook — never the session.
        wlr_log(WLR_ERROR, "ui-substrate: hot-reload threw for '%s'; keeping previous",
                s.resolved_path.c_str());
        if (disable) {
            disable(s.who);
        }
    }
    if (cur) {
        gl.restore_current();
    }
    return installed;
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
        s.document = load_document_first(s);
        if (s.document == nullptr) {
            return; // logged inside; nothing to render this frame
        }
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
    // PER-PIXEL ALPHA: the output FBO is the surface's ARGB8888 wlr_buffer. It
    // must start FULLY TRANSPARENT (0,0,0,0) so a pixel the RML document never
    // paints stays alpha=0 and wlr_scene composites the scene below through it
    // (no opaque region is ever set on the scene_buffer node). EndFrame()
    // composites the document over this with the premultiplied-alpha blend
    // (GL_ONE, GL_ONE_MINUS_SRC_ALPHA), so an opaque-bodied document still
    // fully overwrites to opaque — identical to before. Clearing here (output
    // FBO bound) also wipes stale swapchain content each frame. NOTE: we do NOT
    // call render_iface->Clear() (the vendored helper clears whatever FBO is
    // currently bound — after BeginFrame that is the internal render layer, and
    // it clears to OPAQUE black (0,0,0,1), which is exactly what made every
    // un-painted pixel reach the screen opaque).
    glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s.context->Update();
    gl.render_iface->BeginFrame();
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

void Substrate::Impl::free_surface_gl(Surface& s) {
    // Caller holds the sibling context current. Free every GL render-target
    // resource; the scene_buffer keeps its OWN lock on whatever buffer it was
    // last given (wlr_scene_buffer_set_buffer locked it), so dropping our locks
    // here does not pull the buffer out from under the scene before the next
    // wlr_scene_buffer_set_buffer replaces it.
    for (auto& [buf, slot] : s.slot_gl) {
        if (slot.tex != 0) {
            glDeleteTextures(1, &slot.tex);
        }
        if (slot.image != EGL_NO_IMAGE_KHR && gl.egl_destroy_image != nullptr) {
            gl.egl_destroy_image(gl.egl_display, slot.image);
        }
    }
    s.slot_gl.clear();
    if (s.shm_tex != 0) {
        glDeleteTextures(1, &s.shm_tex);
        s.shm_tex = 0;
    }
    if (s.fbo != 0) {
        glDeleteFramebuffers(1, &s.fbo);
        s.fbo = 0;
    }
    if (s.swapchain != nullptr) {
        wlr_swapchain_destroy(s.swapchain);
        s.swapchain = nullptr;
    }
    if (s.shm != nullptr) {
        wlr_buffer_drop(&s.shm->base);
        s.shm = nullptr;
    }
    s.readback.clear();
    s.readback.shrink_to_fit();
    s.dmabuf = false;
}

bool Substrate::Impl::resize_surface_gl(Surface& s, int w, int h) {
    // Caller guarantees positive geometry + sibling context current. Tear the
    // old GL target down and rebuild at the new size; init_surface_gl re-decides
    // Plan A vs B (it sets s.dmabuf). The RmlUi context is laid out to w×h so the
    // document draws into a matching-size target. Per-pixel-alpha transparent
    // clear, upright (V-flip) composite, premultiplied blend, and the fence-sync
    // submit all live in render_surface/EndFrame and are unaffected — the new
    // target goes through the exact same render path.
    free_surface_gl(s);
    s.width = w;
    s.height = h;
    if (s.context != nullptr) {
        s.context->SetDimensions(Rml::Vector2i(w, h));
    }
    if (!init_surface_gl(s)) {
        wlr_log(WLR_ERROR, "ui-substrate: resize realloc failed (%dx%d)", w, h);
        return false;
    }
    return true;
}

void Substrate::Impl::destroy_surface(Surface* s) {
    // Drop any queued reload for this surface FIRST (so a coalesced file event
    // can never reload a dying surface). The surface's FileWatch (asset_watch)
    // stops the underlying inotify watch when the Surface is erased below.
    unwatch_surface(s);
    s->asset_watch.reset();
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
    free_surface_gl(*s);
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

// ---- Preview snapshot + import ----------------------------------------------
//
// The snapshot is captured by the wlr GLES2 renderer (NOT the sibling RMLUi
// context) into a LINEAR ARGB8888 dmabuf, then that dmabuf is imported into the
// sibling context as a sampled GL texture (slice-3 bridge in reverse). All wlr
// renderer work happens on the WLR EGL context; all import/texture work after
// the snapshot happens on the sibling context (the caller makes it current).

namespace {

// Recursively composite every enabled WLR_SCENE_NODE_BUFFER under `node` into
// `pass`, offset by the accumulated (ox,oy) from the snapshot tree's origin.
// Single-surface toplevels and simple subsurface stacks composite; per-node
// transform/clip/opacity beyond position is a documented follow-up.
void composite_buffers(wlr_scene_node* node, int ox, int oy, wlr_render_pass* pass,
                       wlr_renderer* renderer) {
    if (!node->enabled) {
        return;
    }
    const int x = ox + node->x;
    const int y = oy + node->y;
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        auto* sb = wlr_scene_buffer_from_node(node);
        if (sb->buffer != nullptr) {
            wlr_texture* tex = wlr_texture_from_buffer(renderer, sb->buffer);
            if (tex != nullptr) {
                const int w = sb->dst_width > 0 ? sb->dst_width : static_cast<int>(tex->width);
                const int h = sb->dst_height > 0 ? sb->dst_height : static_cast<int>(tex->height);
                wlr_render_texture_options opts{};
                opts.texture = tex;
                opts.dst_box = wlr_box{x, y, w, h};
                opts.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED;
                wlr_render_pass_add_texture(pass, &opts);
                wlr_texture_destroy(tex);
            }
        }
    } else if (node->type == WLR_SCENE_NODE_TREE) {
        auto* tree = wlr_scene_tree_from_node(node);
        wlr_scene_node* child = nullptr;
        wl_list_for_each(child, &tree->children, link) {
            composite_buffers(child, x, y, pass, renderer);
        }
    }
}

// Natural pixel extent (max right/bottom of buffer nodes) of the subtree, in
// the tree's own coordinate space (origin 0,0). 0x0 if the tree has no buffers.
void tree_extent(wlr_scene_node* node, int ox, int oy, int& max_w, int& max_h) {
    if (!node->enabled) {
        return;
    }
    const int x = ox + node->x;
    const int y = oy + node->y;
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        auto* sb = wlr_scene_buffer_from_node(node);
        if (sb->buffer != nullptr) {
            const int w = sb->dst_width > 0 ? sb->dst_width : sb->buffer->width;
            const int h = sb->dst_height > 0 ? sb->dst_height : sb->buffer->height;
            max_w = std::max(max_w, x + w);
            max_h = std::max(max_h, y + h);
        }
    } else if (node->type == WLR_SCENE_NODE_TREE) {
        auto* tree = wlr_scene_tree_from_node(node);
        wlr_scene_node* child = nullptr;
        wl_list_for_each(child, &tree->children, link) {
            tree_extent(child, x, y, max_w, max_h);
        }
    }
}

} // namespace

bool Substrate::Impl::snapshot_into_buffer(PreviewState& p) {
    // Size the snapshot to the subtree's natural extent (relative to the tree
    // origin: children offsets are relative to `source`, so start at 0,0).
    int w = 0;
    int h = 0;
    tree_extent(&p.source->node, -p.source->node.x, -p.source->node.y, w, h);
    if (w <= 0 || h <= 0) {
        wlr_log(WLR_INFO, "ui-substrate: preview source has no pixels to snapshot");
        return false;
    }

    // (Re)allocate the dmabuf if the extent changed (refresh of a resized
    // toplevel). The buffer is LINEAR ARGB8888, same format the surface path
    // uses, so the same EGL import preconditions apply.
    if (p.buffer == nullptr || p.width != w || p.height != h) {
        if (p.buffer != nullptr) {
            wlr_buffer_drop(p.buffer);
            p.buffer = nullptr;
        }
        wlr_drm_format fmt{};
        fmt.format = kDrmFormatArgb8888;
        std::uint64_t modifiers[] = {0 /* DRM_FORMAT_MOD_LINEAR */};
        fmt.len = 1;
        fmt.capacity = 1;
        fmt.modifiers = modifiers;
        p.buffer = wlr_allocator_create_buffer(allocator, w, h, &fmt);
        if (p.buffer == nullptr) {
            wlr_log(WLR_ERROR, "ui-substrate: preview dmabuf allocation failed");
            return false;
        }
        p.width = w;
        p.height = h;
    }

    // Composite the subtree's buffers into the dmabuf via the WLR renderer. This
    // runs on the wlr renderer's own EGL context (the caller has the SIBLING
    // context current for the import that follows; begin_buffer_pass switches to
    // the wlr context internally and restores nothing — so we re-make-current
    // the sibling context after submit, in import_snapshot's caller).
    wlr_buffer_pass_options pass_opts{};
    wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(renderer, p.buffer, &pass_opts);
    if (pass == nullptr) {
        wlr_log(WLR_ERROR, "ui-substrate: preview begin_buffer_pass failed");
        return false;
    }
    // Clear to transparent first (the toplevel may not cover the whole extent).
    wlr_render_rect_options clear{};
    clear.box = wlr_box{0, 0, w, h};
    clear.color = wlr_render_color{0.f, 0.f, 0.f, 0.f};
    clear.blend_mode = WLR_RENDER_BLEND_MODE_NONE;
    wlr_render_pass_add_rect(pass, &clear);
    composite_buffers(&p.source->node, -p.source->node.x, -p.source->node.y, pass, renderer);
    if (!wlr_render_pass_submit(pass)) {
        wlr_log(WLR_ERROR, "ui-substrate: preview render_pass_submit failed");
        return false;
    }
    return true;
}

bool Substrate::Impl::import_snapshot(PreviewState& p) {
    // The caller holds the sibling context current. Re-import the dmabuf as a
    // sampled texture (EGLImage -> glEGLImageTargetTexture2DOES), then register
    // it under the URI so RmlUi's LoadTexture resolves <img src="unbox-...">.
    if (!gl.dmabuf_import_ok || gl.egl_create_image == nullptr ||
        gl.gl_image_target_texture == nullptr) {
        return false;
    }
    wlr_dmabuf_attributes attribs{};
    if (!wlr_buffer_get_dmabuf(p.buffer, &attribs) || attribs.n_planes < 1) {
        wlr_log(WLR_ERROR, "ui-substrate: preview buffer has no dmabuf");
        return false;
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
    EGLImageKHR img =
        gl.egl_create_image(gl.egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, ia);
    if (img == EGL_NO_IMAGE_KHR) {
        wlr_log(WLR_ERROR, "ui-substrate: preview eglCreateImageKHR failed (0x%x)", eglGetError());
        return false;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    gl.gl_image_target_texture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(img));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Replace any prior import (refresh): drop old registration + GL objects.
    if (gl.render_iface) {
        gl.render_iface->unregister_preview_texture(p.uri);
    }
    if (p.tex != 0) {
        glDeleteTextures(1, &p.tex);
    }
    if (p.image != EGL_NO_IMAGE_KHR && gl.egl_destroy_image != nullptr) {
        gl.egl_destroy_image(gl.egl_display, p.image);
    }
    p.tex = tex;
    p.image = img;
    p.dmabuf = true;
    if (gl.render_iface) {
        gl.render_iface->register_preview_texture(p.uri, p.tex,
                                                  Rml::Vector2i(p.width, p.height));
    }
    return true;
}

void Substrate::Impl::destroy_preview(PreviewState* p) {
    const bool cur = gl.make_current();
    if (gl.render_iface) {
        gl.render_iface->unregister_preview_texture(p->uri);
    }
    if (p->tex != 0) {
        glDeleteTextures(1, &p->tex);
        p->tex = 0;
    }
    if (p->image != EGL_NO_IMAGE_KHR && gl.egl_destroy_image != nullptr) {
        gl.egl_destroy_image(gl.egl_display, p->image);
        p->image = EGL_NO_IMAGE_KHR;
    }
    if (cur) {
        gl.restore_current();
    }
    if (p->buffer != nullptr) {
        wlr_buffer_drop(p->buffer);
        p->buffer = nullptr;
    }
    previews.remove_if([p](const PreviewState& e) { return &e == p; });
}

// ---- Surface elements: seq-gated live import (port of spike LiveTexture) -----
//
// Each NODE's CURRENT committed buffer is `node.surface->buffer` — a
// wlr_client_buffer (the renderer-side import; wlroots has ALREADY released the
// client's underlying pool wl_buffer, so reading/locking it can never starve the
// client). We re-import it ONLY when the surface commit SEQUENCE advances
// (surface_element_needs_reimport, ui_core.hpp): a static client never commits =>
// its seq never advances => zero work (idle dirty-gate intact); a pooled client
// re-commits the SAME wlr_buffer pointer with NEW contents and a bumped seq =>
// re-import (the §0d frozen-frame fix). The buffer we import is LOCKED for the
// import+sample lifetime and the PREVIOUS one unlocked once the new import is
// live — double-buffered, at most one buffer pinned PER NODE, balanced in EVERY
// case incl. the pooled same-pointer re-commit (prev == buf: net +1 then -1).

bool Substrate::Impl::adopt_node(SurfaceElementState& el, SurfaceNode& node) {
    if (node.surface == nullptr) {
        return node.tex != 0;
    }
    wlr_buffer* buf = nullptr;
    if (node.surface->buffer != nullptr) {
        buf = &node.surface->buffer->base;
    }
    if (buf == nullptr) {
        // No buffer committed yet (e.g. a 0x0 configure-ack commit) — nothing to
        // import; keep any prior texture. Not a failure.
        return node.tex != 0;
    }
    const std::uint32_t seq = node.surface->current.seq;
    if (!surface_element_needs_reimport(node.have_seq, node.current_seq, seq, buf == node.current,
                                        node.tex != 0)) {
        return true; // truly unchanged surface state: zero re-import, zero copy
    }

    // Lock the buffer we are about to sample (its dmabuf FDs / shm storage must
    // stay valid for the whole import+sample); release the PREVIOUS one once the
    // new import is live (double-buffered: at most one buffer pinned per node).
    wlr_buffer* prev = node.current;
    wlr_buffer_lock(buf);

    auto commit = [&](bool is_dmabuf) {
        node.current = buf;
        node.current_seq = seq;
        node.have_seq = true;
        el.dmabuf = is_dmabuf;
        last_surface_element_dmabuf = is_dmabuf;
        ++el.reimports;
        if (prev != nullptr) {
            wlr_buffer_unlock(prev); // prev may == buf (pooled re-commit): net +1/-1
        }
        if (gl.render_iface) {
            gl.render_iface->register_preview_texture(node.uri, node.tex,
                                                      Rml::Vector2i(node.width, node.height));
        }
    };

    wlr_dmabuf_attributes attribs{};
    if (gl.dmabuf_import_ok && gl.egl_create_image != nullptr &&
        gl.gl_image_target_texture != nullptr && wlr_buffer_get_dmabuf(buf, &attribs) &&
        attribs.n_planes >= 1) {
        EGLint ia[] = {
            EGL_WIDTH,                     attribs.width,
            EGL_HEIGHT,                    attribs.height,
            EGL_LINUX_DRM_FOURCC_EXT,      static_cast<EGLint>(attribs.format),
            EGL_DMA_BUF_PLANE0_FD_EXT,     attribs.fd[0],
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(attribs.offset[0]),
            EGL_DMA_BUF_PLANE0_PITCH_EXT,  static_cast<EGLint>(attribs.stride[0]),
            EGL_NONE,
        };
        EGLImageKHR img =
            gl.egl_create_image(gl.egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, ia);
        if (img != EGL_NO_IMAGE_KHR) {
            // STABLE TEXTURE ID across re-imports. RmlUi's image()/<img> decorator
            // caches the texture HANDLE it gets from LoadTexture(uri) ONCE (at load
            // time) and draws that handle every frame — so if we regenerated the GL
            // id on each re-import, a live window would show its FIRST frame then go
            // blank (it would draw a DELETED texture). Allocate node.tex ONCE and
            // just re-point it at the NEW EGLImage on every re-import, destroying the
            // PREVIOUS image after the rebind. The id stays constant => the cached
            // RmlUi handle stays valid => the window updates live.
            EGLImageKHR old_image = node.image;
            if (node.tex == 0) {
                glGenTextures(1, &node.tex);
            }
            glBindTexture(GL_TEXTURE_2D, node.tex);
            gl.gl_image_target_texture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(img));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            if (old_image != EGL_NO_IMAGE_KHR && gl.egl_destroy_image != nullptr) {
                gl.egl_destroy_image(gl.egl_display, old_image);
            }
            node.image = img;
            node.width = attribs.width;
            node.height = attribs.height;
            commit(true);
            return true;
        }
    }

    // Fallback: one CPU upload for an shm client (no dmabuf path). R<->B swizzle
    // like Preview/the spike so AR24 {B,G,R,A} samples with correct colors.
    void* data = nullptr;
    std::uint32_t fmt = 0;
    std::size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buf, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt,
                                          &stride)) {
        wlr_buffer_unlock(buf); // import failed: drop the lock we just took
        return node.tex != 0;
    }
    // STABLE TEXTURE ID (see the dmabuf path above): reuse node.tex so RmlUi's
    // cached decorator handle stays valid; a re-upload (glTexImage2D) into the
    // same id refreshes the pixels for an shm client's live updates.
    if (node.image != EGL_NO_IMAGE_KHR && gl.egl_destroy_image != nullptr) {
        gl.egl_destroy_image(gl.egl_display, node.image);
        node.image = EGL_NO_IMAGE_KHR;
    }
    if (node.tex == 0) {
        glGenTextures(1, &node.tex);
    }
    glBindTexture(GL_TEXTURE_2D, node.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride / 4));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, buf->width, buf->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    wlr_buffer_end_data_ptr_access(buf);
    node.width = buf->width;
    node.height = buf->height;
    commit(false);
    return true;
}

namespace {
// Tree-walk scratch: collect every (surface, tree-offset, is_popup) under a
// root, in tree (= composite) order, so adopt_surface_element can reconcile it
// against the element's existing nodes. wlr_surface_for_each_surface visits the
// surface + all subsurfaces root->leaves (z-order); xdg popups are walked
// separately (each its own tree under the toplevel), exactly as the spike's
// send_frame_done_to_clients walks them. The ROOT itself (sx=sy=0) is the FIRST
// visited subsurface entry and is handled as el.root, so it is skipped here.
struct TreeEntry {
    wlr_surface* surface = nullptr;
    int sx = 0;
    int sy = 0;
    bool is_popup = false;
};
struct TreeWalk {
    wlr_surface* root = nullptr;
    std::vector<TreeEntry>* out = nullptr;
    bool popup_pass = false;
    int popup_ox = 0; // popup tree offset within the root (toplevel coords)
    int popup_oy = 0;
};
void tree_walk_cb(wlr_surface* surface, int sx, int sy, void* data) {
    auto* w = static_cast<TreeWalk*>(data);
    if (!w->popup_pass && surface == w->root) {
        return; // the root is el.root, not a child node
    }
    w->out->push_back(TreeEntry{surface, w->popup_ox + sx, w->popup_oy + sy, w->popup_pass});
}
} // namespace

// A stable DOM id for a child node's <img> in a hosting document: derived from
// the node URI (unique per node, stable for the node's life), prefixed so it
// never collides with author ids. The substrate creates/removes the child <img>
// under this id; consumers do NOT author or address it (the element represents
// the whole tree — ui.hpp). Defined here so destroy_surface_element +
// layout_surface_element_children agree on the id.
namespace {
auto child_dom_id(const SurfaceElementState& /*el*/, const SurfaceNode& node) -> std::string {
    // node.uri is "unbox-surface://N.K"; turn the scheme-y chars into id-safe
    // ones so RmlUi's id lookup is well-formed.
    std::string id = "unbox-se-child-";
    for (char ch : node.uri) {
        id.push_back((ch == ':' || ch == '/' || ch == '.') ? '-' : ch);
    }
    return id;
}
} // namespace

bool Substrate::Impl::adopt_surface_element(SurfaceElementState& el) {
    // 1) Re-import the ROOT node first (seq-gated).
    bool any = adopt_node(el, el.root);

    if (el.root.surface == nullptr) {
        return any;
    }

    // 2) Walk the live tree: subsurfaces (any surface) + xdg popups (if the root
    //    is an xdg surface). Collect entries in tree order, then reconcile.
    std::vector<TreeEntry> entries;
    TreeWalk walk;
    walk.root = el.root.surface;
    walk.out = &entries;
    wlr_surface_for_each_surface(el.root.surface, tree_walk_cb, &walk);

    if (wlr_xdg_surface* xdg = wlr_xdg_surface_try_from_wlr_surface(el.root.surface)) {
        // Popups: each popup's surface (+ its own subsurfaces) at coords relative
        // to the toplevel. wlr_xdg_surface_for_each_popup_surface gives the
        // popup's surface-tree with sx/sy already in the parent xdg surface's
        // coordinate space (== root surface space here). Popups are NOT parent-
        // clipped — they become their own absolutely-placed child <img> (crit 4).
        walk.popup_pass = true;
        wlr_xdg_surface_for_each_popup_surface(xdg, tree_walk_cb, &walk);
    }

    // 3) Reconcile entries -> el.children (match by wlr_surface identity). Mark
    //    every existing child unseen, walk entries (create/refresh), then sweep
    //    unseen children (their surface vanished — drop the node + its import).
    for (SurfaceNode& c : el.children) {
        c.seen = false;
    }
    for (const TreeEntry& e : entries) {
        SurfaceNode* node = nullptr;
        for (SurfaceNode& c : el.children) {
            if (c.surface == e.surface) {
                node = &c;
                break;
            }
        }
        if (node == nullptr) {
            el.children.emplace_back();
            node = &el.children.back();
            node->surface = e.surface;
            node->uri = el.root.uri + "." + std::to_string(++el.next_child_id);
        }
        node->sx = e.sx;
        node->sy = e.sy;
        node->is_popup = e.is_popup;
        node->seen = true;
        any = adopt_node(el, *node) || any;
    }
    for (auto it = el.children.begin(); it != el.children.end();) {
        if (it->seen) {
            ++it;
            continue;
        }
        // This child's surface left the tree (subsurface/popup destroyed): drop
        // its import + URI. If it held keyboard focus, clear it (mechanism).
        if (keyboard_focus == it->surface) {
            if (seat != nullptr) {
                wlr_seat_keyboard_notify_clear_focus(seat);
            }
            keyboard_focus = nullptr;
        }
        destroy_node(*it);
        it = el.children.erase(it);
    }
    return any;
}

void Substrate::Impl::destroy_node(SurfaceNode& node) {
    if (gl.render_iface) {
        gl.render_iface->unregister_preview_texture(node.uri);
    }
    if (node.tex != 0) {
        glDeleteTextures(1, &node.tex);
        node.tex = 0;
    }
    if (node.image != EGL_NO_IMAGE_KHR && gl.egl_destroy_image != nullptr) {
        gl.egl_destroy_image(gl.egl_display, node.image);
        node.image = EGL_NO_IMAGE_KHR;
    }
    if (node.current != nullptr) {
        wlr_buffer_unlock(node.current); // release the buffer we held locked
        node.current = nullptr;
    }
    node.have_seq = false;
}

void Substrate::Impl::destroy_surface_element(SurfaceElementState* s) {
    s->commit_l.disconnect(); // stop dirtying after the element is gone
    // Clear keyboard focus if it pointed at any of this element's surfaces (the
    // mechanism's clean-up half — never leave the seat focused on a dead surface).
    auto owns_focus = [&](wlr_surface* f) {
        if (s->root.surface == f) {
            return true;
        }
        for (const SurfaceNode& c : s->children) {
            if (c.surface == f) {
                return true;
            }
        }
        return false;
    };
    if (keyboard_focus != nullptr && owns_focus(keyboard_focus)) {
        if (seat != nullptr) {
            wlr_seat_keyboard_notify_clear_focus(seat);
        }
        keyboard_focus = nullptr;
    }
    // Remove this element's child <img> elements from every hosting document
    // (before the URIs are unregistered) so no stale <img src> survives.
    for (Surface& surf : surfaces) {
        if (surf.document == nullptr) {
            continue;
        }
        for (const SurfaceNode& c : s->children) {
            if (Rml::Element* el = surf.document->GetElementById(child_dom_id(*s, c))) {
                if (Rml::Element* parent = el->GetParentNode()) {
                    parent->RemoveChild(el);
                }
            }
        }
    }
    const bool cur = gl.make_current();
    destroy_node(s->root);
    for (SurfaceNode& c : s->children) {
        destroy_node(c);
    }
    if (cur) {
        gl.restore_current();
    }
    s->children.clear();
    surface_elements.remove_if([s](const SurfaceElementState& e) { return &e == s; });
}

// ---- Surface-element child placement (parent-relative, Wave 1b) --------------

namespace {
// Find the <img> in `doc` whose src is exactly `uri` (the root surface-element
// img the consumer authored, or a child img the substrate created). nullptr if
// the document does not host it. Cheap linear scan over <img> (documents have a
// handful). `src` resolution: RmlUi keeps the raw attribute, so an exact string
// match identifies the surface element's image robustly across reloads.
auto find_img_by_src(Rml::ElementDocument* doc, const std::string& uri) -> Rml::Element* {
    if (doc == nullptr) {
        return nullptr;
    }
    Rml::ElementList imgs;
    doc->GetElementsByTagName(imgs, "img");
    for (Rml::Element* img : imgs) {
        if (img->GetAttribute<Rml::String>("src", "") == uri) {
            return img;
        }
    }
    return nullptr;
}
} // namespace

void Substrate::Impl::layout_surface_element_children(SurfaceElementState& el) {
    // Pure DOM glue (no GL). For every ui surface document that hosts this
    // element's ROOT <img src=root.uri>, ensure each child node has its own
    // <img> placed at the child's tree offset relative to the root img's resolved
    // box (place_child_box — parent-relative placement so a moving/resized parent
    // drags its children). Composite order = tree order: we append child imgs in
    // el.children order (= the tree walk order) AFTER the root img, so later
    // (higher z) nodes draw on top. Popups are NOT parent-clipped — the child img
    // is an absolutely-positioned sibling of the root img (no overflow clip).
    for (Surface& surf : surfaces) {
        Rml::ElementDocument* doc = surf.document;
        Rml::Element* root_img = find_img_by_src(doc, el.root.uri);
        if (root_img == nullptr) {
            continue; // this document does not host the element
        }
        Rml::Element* container = root_img->GetParentNode();
        if (container == nullptr) {
            continue;
        }
        // The root img's resolved box (content area, the texture's drawn box) in
        // the container's coordinate space: place children relative to THIS.
        const Rml::Vector2f roff = root_img->GetAbsoluteOffset(Rml::BoxArea::Content);
        const Rml::Vector2f coff = container->GetAbsoluteOffset(Rml::BoxArea::Content);
        const double px = roff.x - coff.x;
        const double py = roff.y - coff.y;
        const double pw = root_img->GetClientWidth();
        const double ph = root_img->GetClientHeight();

        for (const SurfaceNode& node : el.children) {
            const std::string id = child_dom_id(el, node);
            Rml::Element* img = doc->GetElementById(id);
            if (img == nullptr) {
                Rml::ElementPtr created = doc->CreateElement("img");
                created->SetId(id);
                created->SetAttribute("src", node.uri);
                created->SetProperty("position", "absolute");
                created->SetProperty("display", "block");
                img = container->AppendChild(std::move(created));
            }
            if (img == nullptr) {
                continue;
            }
            const ChildBox box = place_child_box(px, py, pw, ph, el.root.width, el.root.height,
                                                 node.sx, node.sy, node.width, node.height);
            img->SetProperty("left", std::to_string(box.x) + "px");
            img->SetProperty("top", std::to_string(box.y) + "px");
            img->SetProperty("width", std::to_string(box.w) + "px");
            img->SetProperty("height", std::to_string(box.h) + "px");
        }
    }
}

auto Substrate::Impl::surface_element_rendered_size(const SurfaceElementState& el) const
    -> std::pair<int, int> {
    // Read back the rectangle RCSS laid the root <img> out to (the same resolved
    // content box used for child placement above). First hosting document wins;
    // {0,0} before any document has laid it out. GetClientWidth/Height are the
    // element's content-box dimensions in document px — exactly the texture's
    // drawn box. Rounded to whole px (a client buffer size is integral).
    for (const Surface& surf : surfaces) {
        Rml::Element* root_img = find_img_by_src(surf.document, el.root.uri);
        if (root_img == nullptr) {
            continue;
        }
        const int w = static_cast<int>(root_img->GetClientWidth() + 0.5F);
        const int h = static_cast<int>(root_img->GetClientHeight() + 0.5F);
        return {w, h};
    }
    return {0, 0};
}

// ---- Surface-element input-back (pick -> surface-local -> wl_seat) -----------
//
// Port of the spike's route_point: the substrate already feeds pointer/touch
// into a ui surface's Rml context (for bind_event/bind_drag). After a move, the
// hovered element is RmlUi's transform-aware pick. If that element is a surface
// element's <img> (root OR a child node), map the point THROUGH the img's own
// transform with Element::Project() (a no-op when untransformed; the spike fix),
// then box->surface-local scale, and forward to THAT client surface via the seat
// at surface-LOCAL px. A pick that lands on normal RML is untouched (data-events
// fire as before) — only surface-element picks forward to clients.

auto Substrate::Impl::node_for_hovered(Rml::Element* hovered, SurfaceElementState*& out_el)
    -> SurfaceNode* {
    for (Rml::Element* el = hovered; el != nullptr; el = el->GetParentNode()) {
        const Rml::String src = el->GetAttribute<Rml::String>("src", "");
        if (src.empty()) {
            continue;
        }
        for (SurfaceElementState& se : surface_elements) {
            if (se.root.uri == src) {
                out_el = &se;
                return &se.root;
            }
            for (SurfaceNode& c : se.children) {
                if (c.uri == src) {
                    out_el = &se;
                    return &c;
                }
            }
        }
    }
    out_el = nullptr;
    return nullptr;
}

auto Substrate::Impl::forward_pointer_to_client(Surface& surf, double lx, double ly, PointerKind kind,
                                                std::uint32_t button, std::uint32_t time_msec)
    -> bool {
    if (seat == nullptr || surf.context == nullptr) {
        return false;
    }
    // The caller has ALREADY fed this surface's context the move (ctx_motion), so
    // RmlUi's transform-aware hover pick is current; read it.
    SurfaceElementState* el = nullptr;
    SurfaceNode* node = node_for_hovered(surf.context->GetHoverElement(), el);
    if (node == nullptr || node->surface == nullptr) {
        // Not over a client surface (over plain RML / a gap): clear client
        // pointer focus so a stale client does not keep the pointer. The ui
        // surface still got the move above (data-events / hover unaffected).
        wlr_seat_pointer_notify_clear_focus(seat);
        return false;
    }
    // Click/tap-to-focus SIGNAL (ui.hpp SurfaceElement::on_pressed): a PRESS that
    // lands on this element (root OR child) notifies the OWNER, in addition to the
    // client forwarding below. `el` is the owning element (root), so a child-node
    // press fires the ROOT element's handler. Fired only for button_down (not
    // motion/release) and only on a hit — and BEFORE the coord projection, so the
    // focus signal fires even for an edge-on element where no client coord exists.
    if (kind == PointerKind::button_down && el != nullptr) {
        fire_pressed(*el);
    }
    // Project the screen point onto the node img's OWN (possibly 3D-transformed)
    // plane FIRST (Element::Project: the spike fix; no-op when untransformed),
    // then box->surface-local scale. The point fed to the context was in
    // surface-local doc coords; Project()/GetAbsoluteOffset work in the same
    // (untransformed) document space, so we project the SAME doc point.
    Rml::Element* img = find_img_by_src(surf.document, node->uri);
    if (img == nullptr) {
        return false;
    }
    Rml::Vector2f p(static_cast<float>(lx - surf.x), static_cast<float>(ly - surf.y));
    if (!img->Project(p)) {
        return false; // edge-on: ray parallel to the element plane, no valid hit
    }
    const Rml::Vector2f off = img->GetAbsoluteOffset(Rml::BoxArea::Content);
    const float bw = img->GetClientWidth();
    const float bh = img->GetClientHeight();
    if (bw <= 0 || bh <= 0) {
        return false;
    }
    const double fx = std::clamp((p.x - off.x) / bw, 0.0F, 1.0F);
    const double fy = std::clamp((p.y - off.y) / bh, 0.0F, 1.0F);
    const double sx = fx * node->width;
    const double sy = fy * node->height;
    // Implicit-grab discipline (mirror src/input.cpp / the spike): enter before
    // button, frame after. notify_enter is idempotent for the already-entered
    // surface, so calling it each event keeps focus + sends the up-to-date coords.
    wlr_seat_pointer_notify_enter(seat, node->surface, sx, sy);
    if (kind == PointerKind::motion) {
        wlr_seat_pointer_notify_motion(seat, time_msec, sx, sy);
    } else {
        wlr_seat_pointer_notify_button(
            seat, time_msec, button,
            kind == PointerKind::button_down ? WL_POINTER_BUTTON_STATE_PRESSED
                                             : WL_POINTER_BUTTON_STATE_RELEASED);
    }
    wlr_seat_pointer_notify_frame(seat);
    return true;
}

auto Substrate::Impl::forward_touch_to_client(Surface& surf, double lx, double ly, std::int32_t id,
                                              bool down, std::uint32_t time_msec) -> bool {
    if (seat == nullptr || surf.context == nullptr) {
        return false;
    }
    // The caller has ALREADY fed this surface's context the move (ctx_motion).
    SurfaceElementState* el = nullptr;
    SurfaceNode* node = node_for_hovered(surf.context->GetHoverElement(), el);
    if (node == nullptr || node->surface == nullptr) {
        return false;
    }
    // Click/tap-to-focus SIGNAL (mirror forward_pointer_to_client): a touch DOWN
    // that lands on this element (root OR child) notifies the OWNER too. `el` is
    // the root element, so a child-node down fires the ROOT element's handler.
    // Only on a DOWN (not a motion) and only on a hit, before coord projection.
    if (down && el != nullptr) {
        fire_pressed(*el);
    }
    Rml::Element* img = find_img_by_src(surf.document, node->uri);
    if (img == nullptr) {
        return false;
    }
    Rml::Vector2f p(static_cast<float>(lx - surf.x), static_cast<float>(ly - surf.y));
    if (!img->Project(p)) {
        return false;
    }
    const Rml::Vector2f off = img->GetAbsoluteOffset(Rml::BoxArea::Content);
    const float bw = img->GetClientWidth();
    const float bh = img->GetClientHeight();
    if (bw <= 0 || bh <= 0) {
        return false;
    }
    const double sx = std::clamp((p.x - off.x) / bw, 0.0F, 1.0F) * node->width;
    const double sy = std::clamp((p.y - off.y) / bh, 0.0F, 1.0F) * node->height;
    if (down) {
        wlr_seat_touch_notify_down(seat, node->surface, time_msec, id, sx, sy);
    } else {
        wlr_seat_touch_notify_motion(seat, time_msec, id, sx, sy);
    }
    return true;
}

void Substrate::Impl::fire_pressed(SurfaceElementState& el) {
    // The click/tap-to-focus SIGNAL. Error-isolated exactly like the other
    // substrate callbacks (bind_event / bind_drag): a throwing handler disables
    // the OWNING extension only (owner->disable(who)), never the session. A
    // missing handler (default) is a no-op.
    if (!el.on_pressed_cb) {
        return;
    }
    try {
        el.on_pressed_cb();
    } catch (...) {
        if (disable) {
            disable(el.who);
        }
    }
}

void Substrate::Impl::focus_keyboard(SurfaceElementState& el) {
    // Mechanism only (focus POLICY is Wave 3). Mirror src/input.cpp /
    // ext-xdg-shell discipline: pick the seat's current keyboard, set it on the
    // seat (so the enter ships the keymap), and notify_enter the root client
    // surface with the keyboard's currently-pressed keys + modifiers. The
    // kernel's key handler (input.cpp) already forwards subsequent keys to
    // whatever surface holds seat keyboard focus, so this is the whole primitive.
    if (seat == nullptr || el.root.surface == nullptr) {
        return;
    }
    wlr_keyboard* kb = wlr_seat_get_keyboard(seat);
    if (kb == nullptr) {
        // No keyboard device on the seat yet: enter with no keys/mods so the
        // client still receives focus (the keymap follows when a keyboard is set).
        wlr_seat_keyboard_notify_enter(seat, el.root.surface, nullptr, 0, nullptr);
    } else {
        wlr_seat_keyboard_notify_enter(seat, el.root.surface, kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);
    }
    keyboard_focus = el.root.surface;
}

// ---- Substrate (private surface) --------------------------------------------

auto Substrate::create(EGLDisplay egl_display, wlr_allocator* allocator, wlr_renderer* renderer,
                       wlr_seat* seat, FileWatcher* watcher, SubstrateDisableFn disable,
                       SubstrateScheduleFn schedule) -> std::unique_ptr<Substrate> {
    auto impl = std::make_unique<Impl>();
    impl->allocator = allocator;
    impl->renderer = renderer;
    impl->seat = seat; // borrow: surface-element input-back forwards picks here
    impl->disable = std::move(disable);
    impl->schedule = std::move(schedule); // dirty-gate kick for live surface elements
    impl->watcher = watcher; // shared kernel-owned file watcher (asset hot-reload)
    impl->gl.init(egl_display); // sets gl.ok; failure => unavailable substrate
    return std::unique_ptr<Substrate>(new Substrate(std::move(impl)));
}

Substrate::Substrate(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Substrate::~Substrate() {
    // Destroy previews (imported texture+EGLImage+dmabuf+URI registration) and
    // surfaces (GL + scene nodes) before the shared bridge. A surviving Preview
    // handle would dangle after this, but the contract (ui.hpp) is that the
    // substrate outlives every Preview an extension holds (it is kernel-owned
    // and torn down after extensions in Server::Impl::shutdown).
    while (!impl_->surface_elements.empty()) {
        impl_->destroy_surface_element(&impl_->surface_elements.front());
    }
    while (!impl_->previews.empty()) {
        impl_->destroy_preview(&impl_->previews.front());
    }
    while (!impl_->surfaces.empty()) {
        impl_->destroy_surface(&impl_->surfaces.front());
    }
    // The shared FileWatcher is kernel-owned (NOT the substrate's): each
    // surface's FileWatch released above already removed its asset watch.
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

auto Substrate::create_preview(wlr_scene_tree* source) -> std::unique_ptr<Preview> {
    impl_->last_preview_dmabuf = false;
    if (!impl_->available() || source == nullptr) {
        return nullptr;
    }

    impl_->previews.emplace_back();
    PreviewState& p = impl_->previews.back();
    p.owner = impl_.get();
    p.id = ++impl_->next_preview_id;
    p.uri = "unbox-preview://" + std::to_string(p.id);
    p.source = source;

    // 1) Composite the subtree into our LINEAR ARGB8888 dmabuf via the WLR
    //    renderer (its own EGL context). NO sibling context current here:
    //    begin_buffer_pass drives the wlr renderer's GL.
    if (!impl_->snapshot_into_buffer(p)) {
        impl_->destroy_preview(&p);
        return nullptr;
    }

    // 2) Import the dmabuf into the sibling RMLUi context as a sampled texture
    //    and register the URI. The sibling context must be current for the
    //    EGLImage/texture/RmlUi-registration work; save+restore the wlr context.
    if (!impl_->gl.make_current()) {
        impl_->destroy_preview(&p);
        return nullptr;
    }
    const bool imported = impl_->import_snapshot(p);
    impl_->gl.restore_current();
    if (!imported) {
        impl_->destroy_preview(&p);
        return nullptr;
    }

    impl_->last_preview_dmabuf = p.dmabuf;
    return std::make_unique<PreviewHandle>(this, &p);
}

auto Substrate::preview_import_is_dmabuf() const -> bool { return impl_->last_preview_dmabuf; }

auto Substrate::create_surface_element(ExtensionId who, wlr_surface* client)
    -> std::unique_ptr<SurfaceElement> {
    impl_->last_surface_element_dmabuf = false;
    if (!impl_->available() || client == nullptr) {
        return nullptr; // no GL path or no surface: graceful degrade (ui.hpp)
    }

    impl_->surface_elements.emplace_back();
    SurfaceElementState& s = impl_->surface_elements.back();
    s.owner = impl_.get();
    s.who = who; // for on_pressed error isolation
    s.id = ++impl_->next_surface_id;
    s.root.surface = client;
    s.root.uri = surface_element_uri(s.id); // == source_uri()

    // Import the client's current buffer + walk its tree now (so source_uri()/
    // width()/height() are valid immediately, like create_preview). The sibling
    // context must be current for the EGLImage/texture/RmlUi-registration work.
    if (!impl_->gl.make_current()) {
        impl_->destroy_surface_element(&s);
        return nullptr;
    }
    const bool imported = impl_->adopt_surface_element(s);
    impl_->gl.restore_current();
    s.needs_reimport = false;
    if (!imported || s.root.tex == 0) {
        // The surface may simply have no buffer yet (pre-first-commit); that is a
        // valid live element that will import on its first commit. Only fail if
        // there is no GL path to ever import on — but available() already gated
        // that, so a tex==0 here means no buffer yet: keep the element.
    }

    // RAII commit hook: a client commit dirties this element + kicks the
    // dirty-gate so the next frame re-imports + re-renders. The borrow received
    // in the handler is unused (we re-read s.surface->buffer in tick_all); the
    // listener unsubscribes when the element (and its SurfaceElementState) dies.
    SurfaceElementState* sp = &s;
    Substrate::Impl* impl = impl_.get();
    s.commit_l.connect(client->events.commit, [impl, sp](void*) {
        sp->needs_reimport = true; // re-adopt this surface's current buffer next tick
        if (impl->schedule) {
            impl->schedule(); // dirty-gate: ensure a frame runs to show the update
        }
    });

    // Kick the frame-callback loop on: while this element exists the kernel keeps
    // a frame scheduled (has_surface_elements()) so the client keeps drawing.
    if (impl_->schedule) {
        impl_->schedule();
    }
    return std::make_unique<SurfaceElementHandle>(this, &s);
}

auto Substrate::has_surface_elements() const -> bool {
    return !impl_->surface_elements.empty();
}

namespace {
// Frame-done tree-walk (spike §0c send_frame_done_to_clients): visit the root +
// every subsurface + every xdg popup tree, calling wlr_surface_send_frame_done
// on each mapped surface. wlr_surface_for_each_surface covers the root +
// subsurfaces; xdg popups are walked separately, as the spike does.
struct FrameDoneWalk {
    timespec* now = nullptr;
    int* counter = nullptr;
};
void frame_done_cb(wlr_surface* surface, int /*sx*/, int /*sy*/, void* data) {
    auto* w = static_cast<FrameDoneWalk*>(data);
    wlr_surface_send_frame_done(surface, w->now);
    ++*w->counter;
}
} // namespace

void Substrate::send_frame_done_to_surface_elements(const timespec& now) {
    // Frame-callback duty (the stuck-frame fix, spike §0c
    // send_frame_done_to_clients): tell every live-element-backing surface "now
    // is a good time to draw your next frame", so the client keeps producing
    // buffers. Wave 1b walks the WHOLE TREE (root + subsurfaces +
    // xdg popups), exactly as the spike does — not just the root. Sent
    // UNCONDITIONALLY per composited output frame (like
    // wlr_scene_output_send_frame_done), NOT gated by re-render: the client needs
    // callbacks to progress regardless of whether WE re-rendered.
    timespec t = now;
    for (SurfaceElementState& s : impl_->surface_elements) {
        if (s.root.surface == nullptr) {
            continue;
        }
        FrameDoneWalk w{&t, &s.frame_done_sends};
        wlr_surface_for_each_surface(s.root.surface, frame_done_cb, &w);
        if (wlr_xdg_surface* xdg = wlr_xdg_surface_try_from_wlr_surface(s.root.surface)) {
            wlr_xdg_surface_for_each_popup_surface(xdg, frame_done_cb, &w);
        }
    }
}

auto Substrate::surface_element_reimport_count() const -> int {
    int total = 0;
    for (const SurfaceElementState& s : impl_->surface_elements) {
        total += s.reimports;
    }
    return total;
}

auto Substrate::surface_element_frame_done_count() const -> int {
    int total = 0;
    for (const SurfaceElementState& s : impl_->surface_elements) {
        total += s.frame_done_sends;
    }
    return total;
}

auto Substrate::surface_element_import_is_dmabuf() const -> bool {
    return impl_->last_surface_element_dmabuf;
}

void Substrate::tick_all() {
    if (!impl_->available() ||
        (impl_->surfaces.empty() && impl_->surface_elements.empty())) {
        return;
    }
    // Apply any hot-reload requests coalesced from inotify since the last tick
    // (dev only). reload_surface manages the GL context itself + is error-
    // isolated, so a broken file here can't stop the frame.
    if (!impl_->pending_reloads.empty()) {
        std::vector<Surface*> due;
        due.swap(impl_->pending_reloads);
        for (Surface* s : due) {
            // Still alive? (unwatch_surface scrubs destroyed ones, but be safe.)
            bool live = false;
            for (Surface& e : impl_->surfaces) {
                if (&e == s) {
                    live = true;
                    break;
                }
            }
            if (live) {
                impl_->reload_surface(*s);
            }
        }
    }
    if (!impl_->gl.make_current()) {
        return;
    }
    // Re-import every live surface element's CURRENT buffer FIRST (seq-gated), so
    // a hosting ui surface's <img src=unbox-surface://N> samples the fresh
    // texture this frame. The seq-gate makes a static client cost zero work (no
    // commit => no seq advance => adopt_surface_element early-returns); a client
    // that committed since the last tick re-imports exactly once. needs_reimport
    // (set by the commit hook) is a cheap pre-filter so an idle element does not
    // even touch its surface state.
    for (SurfaceElementState& s : impl_->surface_elements) {
        if (s.needs_reimport) {
            const int before = s.reimports;
            // Re-walk the tree + re-import every node (seq-gated). A subsurface/
            // popup added or removed since the last tick is reconciled here.
            impl_->adopt_surface_element(s);
            // Clear the flag only once the seq actually caught up: adopt is
            // seq-gated, so if it did nothing (no new buffer) leaving the flag set
            // would re-check next tick, which is harmless — but a successful adopt
            // (or a no-op on an unchanged seq with a live texture) means we are
            // current, so clear it. Keep it set only when there is still no
            // texture (pre-first-buffer) so the first real buffer is picked up.
            if (s.reimports != before || s.root.tex != 0) {
                s.needs_reimport = false;
            }
        }
        // Place per-node child <img> elements in every hosting document at their
        // tree offset relative to the root img's resolved box (parent-relative
        // placement). Pure DOM glue; cheap (no-op when there are no children or
        // no hosting document yet). Done EVERY tick so a moved/resized parent
        // drags its children, and a child created this tick gets an <img>.
        impl_->layout_surface_element_children(s);
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
    // Surface-element input-back: if the move's transform-aware pick on the target
    // ui surface lands on a surface element <img>, forward the motion to THAT
    // client at surface-local coords via the seat (the move was just fed above).
    // A pick on normal RML clears client pointer focus (handled inside) — the ui
    // surface's own data-events/hover already fired, so this only ADDS the client
    // forward, never regresses existing routing.
    if (target != nullptr) {
        (void)impl_->forward_pointer_to_client(*target, lx, ly,
                                               Substrate::Impl::PointerKind::motion, 0, time_msec);
    }
}

auto Substrate::route_pointer_button(double lx, double ly, bool pressed, std::uint32_t time_msec)
    -> bool {
    if (!impl_->available()) {
        return false;
    }
    using PK = Substrate::Impl::PointerKind;
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
            // Surface-element input-back: forward the press to the client under the
            // pick (enter-before-button, frame-after — done inside the helper),
            // mirroring the spike / src/input.cpp implicit-grab discipline.
            (void)impl_->forward_pointer_to_client(*impl_->pointer_grab_surface, lx, ly,
                                                   PK::button_down, BTN_LEFT, time_msec);
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
        (void)impl_->forward_pointer_to_client(*impl_->pointer_grab_surface, lx, ly, PK::button_up,
                                               BTN_LEFT, time_msec);
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
    // Surface-element input-back: forward the touch-down to the client under the
    // pick at surface-local coords (the move was just fed above).
    (void)impl_->forward_touch_to_client(*hit, lx, ly, id, /*down=*/true, time_msec);
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
    (void)impl_->forward_touch_to_client(*it->second, lx, ly, id, /*down=*/false, time_msec);
    return true;
}

auto Substrate::route_touch_up(std::int32_t id, std::uint32_t time_msec) -> bool {
    if (!impl_->available()) {
        return false;
    }
    auto it = impl_->touch_capture.find(id);
    if (it == impl_->touch_capture.end()) {
        return false;
    }
    impl_->ctx_button(*it->second, false);
    // Surface-element input-back: end the touch point on the client too. The seat
    // tracks the down's surface per id, so a bare notify_up is enough (it is a
    // no-op for a point that was never forwarded to a client).
    if (impl_->seat != nullptr) {
        wlr_seat_touch_notify_up(impl_->seat, time_msec, id);
    }
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

auto Substrate::surface_pixel(int x, int y) const -> std::uint32_t {
    // Read the first rendered surface's current FBO at (x,y) via glReadPixels on
    // the sibling context — works for BOTH the shm and dmabuf paths (the FBO's
    // color attachment is the surface's submitted texture in either case), so
    // this probe is independent of the per-surface path choice. The renderer
    // V-flips on composite so the FBO is top-first (GL row 0 == document top,
    // consistent with orientation()'s readback row0==top), hence document-y
    // maps to GL row y DIRECTLY (a corner box at top:0 reads back at y≈0). R,G,B,A.
    for (const Surface& s : impl_->surfaces) {
        if (s.fbo == 0 || s.frame_count == 0) {
            continue;
        }
        if (x < 0 || y < 0 || x >= s.width || y >= s.height) {
            return 0;
        }
        const bool cur = impl_->gl.make_current();
        std::uint8_t rgba[4] = {0, 0, 0, 0};
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (cur) {
            impl_->gl.restore_current();
        }
        return (static_cast<std::uint32_t>(rgba[0]) << 24) |
               (static_cast<std::uint32_t>(rgba[1]) << 16) |
               (static_cast<std::uint32_t>(rgba[2]) << 8) | static_cast<std::uint32_t>(rgba[3]);
    }
    return 0;
}

auto Substrate::surface_has_opaque_region() const -> bool {
    // A per-pixel-alpha surface must NOT carry a forced opaque region: if it
    // did, wlr_scene would treat the buffer as opaque and skip blending the
    // scene below through the transparent (un-painted) pixels. We never call
    // wlr_scene_buffer_set_opaque_region, so this reads empty.
    for (const Surface& s : impl_->surfaces) {
        if (s.scene_buffer == nullptr) {
            continue;
        }
        // Read the region's extents directly (header-only; avoids linking the
        // pixman lib): an empty region has a degenerate extents box.
        const pixman_box32_t& e = s.scene_buffer->opaque_region.extents;
        return e.x2 > e.x1 && e.y2 > e.y1;
    }
    return false;
}

auto Substrate::resize_realloc_count() const -> int {
    return impl_->resize_realloc_count;
}

auto Substrate::element_count(const char* tag) const -> int {
    for (const Surface& s : impl_->surfaces) {
        if (s.document == nullptr) {
            continue;
        }
        Rml::ElementList elements;
        s.document->GetElementsByTagName(elements, Rml::String(tag));
        return static_cast<int>(elements.size());
    }
    return 0;
}

auto Substrate::click_element(const char* tag, int index) -> bool {
    for (Surface& s : impl_->surfaces) {
        if (s.document == nullptr) {
            continue;
        }
        Rml::ElementList elements;
        s.document->GetElementsByTagName(elements, Rml::String(tag));
        if (index < 0 || index >= static_cast<int>(elements.size())) {
            return false;
        }
        elements[static_cast<std::size_t>(index)]->Click();
        return true;
    }
    return false;
}

auto Substrate::drag_element(const char* tag, int index, double dx, double dy) -> bool {
    for (Surface& s : impl_->surfaces) {
        if (s.document == nullptr || s.context == nullptr) {
            continue;
        }
        Rml::ElementList elements;
        s.document->GetElementsByTagName(elements, Rml::String(tag));
        if (index < 0 || index >= static_cast<int>(elements.size())) {
            return false;
        }
        Rml::Element* el = elements[static_cast<std::size_t>(index)];
        // The element's content-box centre in context (= surface-local) coords:
        // the same space ctx_motion feeds and the same space mouse_x/mouse_y are
        // reported in, so the delivered drag coords should match these moves.
        const Rml::Vector2f origin = el->GetAbsoluteOffset(Rml::BoxArea::Content);
        const int cx = static_cast<int>(origin.x + el->GetClientWidth() / 2.0F);
        const int cy = static_cast<int>(origin.y + el->GetClientHeight() / 2.0F);
        // Press at the centre, then move PAST RmlUi's drag threshold so it emits
        // Dragstart + Drag; a second move proves move tracks travel; release ends.
        s.context->ProcessMouseMove(cx, cy, 0);
        s.context->ProcessMouseButtonDown(0, 0);
        s.context->ProcessMouseMove(cx + static_cast<int>(dx), cy + static_cast<int>(dy), 0);
        s.context->ProcessMouseMove(cx + static_cast<int>(dx) * 2,
                                    cy + static_cast<int>(dy) * 2, 0);
        s.context->ProcessMouseButtonUp(0, 0);
        return true;
    }
    return false;
}

auto Substrate::reload_first_surface() -> bool {
    for (Surface& s : impl_->surfaces) {
        return impl_->reload_surface(s);
    }
    return false;
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
    Surface& s = *surface_;
    // Reject non-positive geometry, same as create_surface — keep the old size.
    if (width <= 0 || height <= 0) {
        wlr_log(WLR_ERROR, "ui-substrate: surface needs positive geometry");
        return;
    }
    // Only-on-change: a same-size set_size is a no-op (the dock calls set_size on
    // every minimize/restore, often with the same height — never thrash the
    // swapchain). A move (set_position) never reaches here, so it stays cheap.
    if (width == s.width && height == s.height) {
        return;
    }
    // Resize the actual render target so the surface renders at w×h (grow AND
    // shrink): rebuild FBO + dmabuf swapchain (or shm buffer) + cached
    // EGLImage/texture, lay the RmlUi context out to w×h, and re-set the scene
    // node's buffer on the next render_surface tick. Heavier than set_position
    // (reallocs GL resources); call on size changes, not every frame.
    Substrate::Impl& impl = *substrate_->impl_;
    const bool cur = impl.gl.make_current();
    impl.resize_surface_gl(s, width, height); // updates s.width/s.height + ctx dims
    ++impl.resize_realloc_count;
    if (cur) {
        impl.gl.restore_current();
    }
    // The scene-node composite size follows the new buffer (set on next render);
    // the input hit-test rect uses s.width/s.height live, so it tracks too.
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

namespace {
// Map a live RMLUi drag event to the public DragPhase. Pure: only the three
// drag ids are routed here (the binding hooks no other event), so an unknown id
// is treated as a move (the safe middle phase) rather than dropped. Returns
// false for an id we should ignore entirely (none currently).
auto drag_phase_for(Rml::EventId id, UiSurface::DragPhase& out) -> bool {
    switch (id) {
    case Rml::EventId::Dragstart: out = UiSurface::DragPhase::start; return true;
    case Rml::EventId::Dragend: out = UiSurface::DragPhase::end; return true;
    case Rml::EventId::Drag: out = UiSurface::DragPhase::move; return true;
    default: out = UiSurface::DragPhase::move; return true;
    }
}
} // namespace

void SurfaceHandle::bind_drag(std::string_view name,
                              std::function<void(UiSurface::DragPhase, double, double)> callback) {
    Surface& s = *surface_;
    if (!s.ctor) {
        return;
    }
    s.drag_bindings.push_back({std::move(callback), s.who, s.owner});
    Surface::DragBinding* binding = &s.drag_bindings.back();
    // One model callback name carries all three phases; the document authors
    // data-event-dragstart / data-event-drag / data-event-dragend all naming
    // <name> on a drag-enabled element (RCSS `drag: drag;`). The phase is read
    // off the live event id; mouse_x/mouse_y are already surface-local px
    // (ctx_motion feeds the context coords relative to the surface origin), so
    // they pass straight through as x/y. Same error-isolation boundary as
    // bind_event (a throw disables the owning extension only). Survives dev
    // hot-reload like every other binding: registered once on the open ctor,
    // re-applied by the substrate against the reloaded document.
    s.ctor.BindEventCallback(
        std::string(name),
        [binding](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList&) {
            try {
                if (!binding->cb) {
                    return;
                }
                UiSurface::DragPhase phase = UiSurface::DragPhase::move;
                drag_phase_for(ev.GetId(), phase);
                const double x = static_cast<double>(ev.GetParameter<float>("mouse_x", 0.0F));
                const double y = static_cast<double>(ev.GetParameter<float>("mouse_y", 0.0F));
                binding->cb(phase, x, y);
            } catch (...) {
                if (binding->owner->disable) {
                    binding->owner->disable(binding->who);
                }
            }
        });
}

namespace {
// Find an existing list binding by name in the surface, or nullptr.
auto find_list(Surface& s, std::string_view name) -> Surface::ListBinding* {
    for (auto& b : s.list_bindings) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}
} // namespace

void SurfaceHandle::bind_list(std::string_view name, std::function<std::size_t()> count) {
    Surface& s = *surface_;
    if (!s.ctor) {
        return;
    }
    Surface::ListBinding* b = find_list(s, name);
    if (b == nullptr) {
        s.list_bindings.emplace_back();
        b = &s.list_bindings.back();
        b->name = std::string(name);
        b->who = s.who;
        b->disable = s.owner->disable;
        b->init(); // stable address now -> seat the definition back-pointers
        // Bind the array variable under the list name; data-for reads its
        // Size()/Child() to iterate, and each row's Child() resolves the fields.
        s.ctor.BindCustomDataVariable(b->name,
                                      Rml::DataVariable(&b->array_def, nullptr));
    }
    b->count = std::move(count);
}

// One template for the four typed field binds: wrap the typed getter in a
// Variant-producing closure (Variant's templated setter handles each type),
// then register it on the list's row struct under `field`.
namespace {
template <typename T, typename Getter>
void bind_list_field_impl(Surface& s, std::string_view list, std::string_view field,
                          Getter getter) {
    if (!s.ctor) {
        return;
    }
    Surface::ListBinding* b = find_list(s, list);
    if (b == nullptr) {
        // The list must be declared first (bind_list); a field on an unknown
        // list is dropped (documented: register the list before its fields...
        // they may interleave, but the list name must exist).
        wlr_log(WLR_INFO, "ui-substrate: bind_list field '%.*s' for unknown list '%.*s'",
                static_cast<int>(field.size()), field.data(),
                static_cast<int>(list.size()), list.data());
        return;
    }
    b->add_field(std::string(field),
                 [getter = std::move(getter)](std::size_t row, Rml::Variant& out) -> bool {
                     out = static_cast<T>(getter(row));
                     return true;
                 });
}
} // namespace

void SurfaceHandle::bind_list_string(std::string_view list, std::string_view field,
                                     std::function<std::string(std::size_t)> getter) {
    bind_list_field_impl<Rml::String>(*surface_, list, field, std::move(getter));
}
void SurfaceHandle::bind_list_int(std::string_view list, std::string_view field,
                                  std::function<int(std::size_t)> getter) {
    bind_list_field_impl<int>(*surface_, list, field, std::move(getter));
}
void SurfaceHandle::bind_list_double(std::string_view list, std::string_view field,
                                     std::function<double(std::size_t)> getter) {
    bind_list_field_impl<double>(*surface_, list, field, std::move(getter));
}
void SurfaceHandle::bind_list_bool(std::string_view list, std::string_view field,
                                   std::function<bool(std::size_t)> getter) {
    bind_list_field_impl<bool>(*surface_, list, field, std::move(getter));
}

void SurfaceHandle::bind_list_event(std::string_view /*list*/, std::string_view event,
                                    std::function<void(std::size_t)> callback) {
    // A row event is a normal data-event callback; the row index arrives as the
    // first data-expression argument (author it as data-event-click="ev(it_index)").
    // The event name is model-global (RmlUi has no per-list event namespace), so
    // `list` is documentary only — keep names unique per surface.
    Surface& s = *surface_;
    if (!s.ctor) {
        return;
    }
    s.list_event_bindings.push_back({std::move(callback), s.who, s.owner});
    Surface::ListEventBinding* binding = &s.list_event_bindings.back();
    s.ctor.BindEventCallback(
        std::string(event),
        [binding](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            try {
                if (binding->cb) {
                    std::size_t row = 0;
                    if (!args.empty()) {
                        row = static_cast<std::size_t>(args[0].Get<int>());
                    }
                    binding->cb(row);
                }
            } catch (...) {
                if (binding->owner->disable) {
                    binding->owner->disable(binding->who);
                }
            }
        });
}

void SurfaceHandle::bind_list_drag(
    std::string_view /*list*/, std::string_view name,
    std::function<void(std::size_t, UiSurface::DragPhase, double, double)> callback) {
    // The per-row drag binding = bind_drag (phase from the event id, x/y from the
    // event's surface-local mouse_x/mouse_y) + bind_list_event (row index from the
    // data expression's first argument, it_index). The event name is model-global
    // (like bind_list_event) so `list` is documentary; keep names unique. Survives
    // dev hot-reload like every binding (registered once on the open ctor).
    Surface& s = *surface_;
    if (!s.ctor) {
        return;
    }
    s.list_drag_bindings.push_back({std::move(callback), s.who, s.owner});
    Surface::ListDragBinding* binding = &s.list_drag_bindings.back();
    s.ctor.BindEventCallback(
        std::string(name),
        [binding](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
            try {
                if (!binding->cb) {
                    return;
                }
                UiSurface::DragPhase phase = UiSurface::DragPhase::move;
                drag_phase_for(ev.GetId(), phase);
                std::size_t row = 0;
                if (!args.empty()) {
                    row = static_cast<std::size_t>(args[0].Get<int>());
                }
                const double x = static_cast<double>(ev.GetParameter<float>("mouse_x", 0.0F));
                const double y = static_cast<double>(ev.GetParameter<float>("mouse_y", 0.0F));
                binding->cb(row, phase, x, y);
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

auto SurfaceHandle::transition_timing(std::string_view element_id, std::string_view property) const
    -> std::optional<TransitionTiming> {
    const Surface& s = *surface_;
    if (s.document == nullptr) {
        return std::nullopt; // not loaded yet => no computed values
    }
    Rml::Element* el = s.document->GetElementById(Rml::String(element_id));
    if (el == nullptr) {
        return std::nullopt;
    }
    // The computed `transition` property: a TransitionList (none/all + entries).
    const Rml::Property* prop = el->GetProperty(Rml::PropertyId::Transition);
    if (prop == nullptr) {
        return std::nullopt;
    }
    const Rml::TransitionList list = prop->Get<Rml::TransitionList>();
    if (list.none) {
        return std::nullopt;
    }

    // Resolve the requested property name (e.g. "transform") to RmlUi's id, then
    // find the matching per-property transition. An `all` transition applies to
    // every property and serves as the fallback if no exact entry exists; an
    // exact entry wins over `all`.
    const Rml::PropertyId want = Rml::StyleSheetSpecification::GetPropertyId(Rml::String(property));
    const Rml::Transition* match = nullptr;
    if (want != Rml::PropertyId::Invalid) {
        for (const Rml::Transition& t : list.transitions) {
            if (t.id == want) {
                match = &t;
                break;
            }
        }
    }
    Rml::Tween tween;
    double duration = 0.0;
    double delay = 0.0;
    if (match != nullptr) {
        tween = match->tween;
        duration = static_cast<double>(match->duration);
        delay = static_cast<double>(match->delay);
    } else if (list.all && !list.transitions.empty()) {
        // `all foo 0.2s ease` parses to a single entry flagged all=true; reuse
        // its timing/tween for the requested property.
        const Rml::Transition& t = list.transitions.front();
        tween = t.tween;
        duration = static_cast<double>(t.duration);
        delay = static_cast<double>(t.delay);
    } else {
        return std::nullopt;
    }

    // Wrap RmlUi's Tween BY VALUE — Tween::operator()(float) is the evaluator;
    // capturing it keeps RmlUi types out of the contract entirely.
    return TransitionTiming{duration, delay, [tween](float t) { return tween(t); }};
}

// ---- PreviewHandle (public Preview impl) ------------------------------------

PreviewHandle::~PreviewHandle() { substrate_->impl_->destroy_preview(state_); }

auto PreviewHandle::source_uri() const -> std::string { return state_->uri; }
auto PreviewHandle::source_width() const -> int { return state_->width; }
auto PreviewHandle::source_height() const -> int { return state_->height; }

void PreviewHandle::refresh() {
    // Re-snapshot from the original source IF it is still valid. Borrow validity
    // is the caller's concern (ui.hpp: refresh after source destruction is UB);
    // we guard only against an obviously unusable substrate. A failed re-snapshot
    // or re-import leaves the previous frozen snapshot + URI registration intact.
    Substrate::Impl& impl = *substrate_->impl_;
    if (!impl.available() || state_->source == nullptr) {
        return;
    }
    if (!impl.snapshot_into_buffer(*state_)) {
        return;
    }
    if (!impl.gl.make_current()) {
        return;
    }
    impl.import_snapshot(*state_);
    impl.gl.restore_current();
}

// ---- SurfaceElementHandle (public SurfaceElement impl) ----------------------

SurfaceElementHandle::~SurfaceElementHandle() {
    substrate_->impl_->destroy_surface_element(state_);
}

auto SurfaceElementHandle::source_uri() const -> std::string { return state_->root.uri; }
auto SurfaceElementHandle::width() const -> int { return state_->root.width; }
auto SurfaceElementHandle::height() const -> int { return state_->root.height; }
auto SurfaceElementHandle::rendered_width() const -> int {
    return substrate_->impl_->surface_element_rendered_size(*state_).first;
}
auto SurfaceElementHandle::rendered_height() const -> int {
    return substrate_->impl_->surface_element_rendered_size(*state_).second;
}

void SurfaceElementHandle::focus_keyboard() { substrate_->impl_->focus_keyboard(*state_); }

void SurfaceElementHandle::on_pressed(std::function<void()> handler) {
    // One handler per element; re-setting replaces it (the stored std::function
    // lives in the SurfaceElementState and dies with the element).
    state_->on_pressed_cb = std::move(handler);
}

} // namespace unbox::kernel
