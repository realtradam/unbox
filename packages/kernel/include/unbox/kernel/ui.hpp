#pragma once

#include <unbox/kernel/host.hpp> // SceneLayer (and, transitively, wlr.hpp-free types)

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// The ui substrate contract — the extension-facing face of the kernel's RMLUi
// subsystem (GLOSSARY: "ui substrate"). Every UI extension from slice 6 on
// (taskbar, launcher, OSK, …) builds on THIS and nothing lower: it contributes
// a **ui surface** (an RML document composited as a scene node) and reaches its
// state into the document through **data bindings** — never GL, never RMLUi
// types (architecture rule: RMLUi stays kernel-private).
//
// Reach the substrate via Host::ui(). It is kernel-owned and per-session; the
// returned reference is a borrow valid for your extension's lifetime. Your
// extension identity flows with it, so a data-event callback that throws
// disables YOUR extension only (error isolation), never the session.
//
// DEFERRED (documented, not built this slice):
//  - Keyboard into ui surfaces (text input + focus): OUT of slice 5. OSK is
//    slice 8, launcher text slice 6 — those slices add the keyboard path.
//  - Keyboard into ui surfaces only (text input + focus); see above. List /
//    container data bindings now SHIP (slice 10's stage dock forced the shape):
//    see UiSurface::bind_list / bind_list_<type> / bind_list_event. NESTED
//    lists (a list field whose value is itself a list) remain unsupported.
//
// Everything runs on the single wl_event_loop thread. RML assets live under
// assets/<unit>/ per the harness; pass either inline RML or an asset path.
//
// TOUCH-MODE DOES NOTHING VISUAL (user decision). The substrate never changes
// the RmlUi dp-ratio (it stays 1.0), so a document looks identical in pointer
// and touch mode — `dp` behaves like `px` in practice. touch-mode is purely a
// STATE signal: if your extension wants to adapt to finger input (bigger hit
// zones, extra spacing, a different layout), subscribe to
// UiSurface::on_touch_mode_changed and make the change yourself (e.g. set_size,
// toggle a bound bool your RCSS keys off). Authoring needs no special idiom:
// size for the look you want; nothing grows out from under you.

namespace unbox::kernel {

// A live ui surface: one RML document composited as a scene-node in the scene.
// OWNED by the contributing extension via unique_ptr — destroying it removes
// the document AND its scene node (so hold it as a member; it dies with you,
// in reverse declaration order, while the kernel's scene is still alive).
// All methods are event-loop-thread only.
//
// PER-PIXEL ALPHA (transparency). The surface composites with per-pixel alpha:
// any pixel your document does NOT paint is fully transparent, and the scene
// BELOW the surface composites through it (the substrate never marks the buffer
// opaque). So a document with a transparent <body> that paints, say, only a
// card in one corner shows the windows beneath it everywhere else. If you want
// a solid panel, paint an opaque background in your RCSS (e.g. body {
// background-color: #rrggbb; }) — a fully-opaque document is pixel-identical to
// a fully-opaque surface and occludes whatever it covers, as before.
class UiSurface {
public:
    virtual ~UiSurface() = default;
    UiSurface(const UiSurface&) = delete;
    auto operator=(const UiSurface&) -> UiSurface& = delete;

    // ---- Geometry & visibility (layout coordinates) ----
    // Move the surface: the node sits at (x,y) in layout space. Cheap (no
    // realloc); takes effect on the next frame.
    virtual void set_position(int x, int y) = 0;
    // Resize the surface to w×h. This RESIZES THE RENDER TARGET — the document
    // is laid out to w×h AND draws into a buffer of matching size, so the
    // surface renders fully at the new size (grow AND shrink both work; the
    // composited node and the input hit-test rect track the new size). It is
    // HEAVIER than set_position: on an actual size change it reallocates GL
    // resources (the offscreen FBO + dmabuf swapchain / shm buffer), so call it
    // on size changes, not every frame. A no-op same-size call is cheap (no
    // realloc). Non-positive w/h is rejected (the surface keeps its size). Takes
    // effect on the next frame; resizing a hidden surface is fine (it still is
    // not composited until shown).
    virtual void set_size(int width, int height) = 0;
    // Show/hide without destroying. Hidden surfaces are not composited and do
    // not receive input. Default after create is the spec's `visible`.
    virtual void set_visible(bool visible) = 0;
    [[nodiscard]] virtual auto visible() const -> bool = 0;

    // ---- Data bindings (typed, RMLUi-free) ----
    // Bind a named scalar the document reads via {{name}} / data-* attributes.
    // The GETTER is called by the substrate when the surface re-renders after
    // you dirty(name); it must be cheap and pure (no event-loop blocking). The
    // getter is stored and invoked for the surface's lifetime — capture only
    // state that outlives this surface (e.g. your extension's members). A
    // getter that throws is caught and isolates your extension.
    // Call these BEFORE the first frame (in activate / right after create);
    // re-binding the same name replaces the getter.
    virtual void bind_int(std::string_view name, std::function<int()> getter) = 0;
    virtual void bind_double(std::string_view name, std::function<double()> getter) = 0;
    virtual void bind_bool(std::string_view name, std::function<bool()> getter) = 0;
    virtual void bind_string(std::string_view name, std::function<std::string()> getter) = 0;

    // Bind a named RML `data-event` (e.g. data-event-click="name") to a
    // callback. Invoked on the event-loop thread when the document fires it; a
    // throwing callback is caught at the substrate boundary and disables YOUR
    // extension (its surfaces + subscriptions dropped) — never the session.
    virtual void bind_event(std::string_view name, std::function<void()> callback) = 0;

    // Drag phase for bind_drag (a single captured pointer/touch drag stream).
    enum class DragPhase { start, move, end };

    // Bind a named RML drag interaction to a C++ callback. The document opts an
    // element into dragging with the RCSS `drag` property and authors the event on
    // it (e.g. data-event-dragstart / data-event-drag / data-event-dragend all
    // naming <name>); the substrate routes RMLUi's Dragstart/Drag/Dragend for that
    // element to ONE callback, tagged with the phase. `x`/`y` are the pointer
    // position in THIS surface's LOCAL document coordinates (px, origin = surface
    // top-left), so an extension can map travel to a fraction directly. Invoked on
    // the event-loop thread; a throwing callback is caught at the substrate boundary
    // and disables YOUR extension only — never the session. Call before the first
    // frame (same rule as bind_event); re-binding the same name replaces it.
    // A tap (press-release with no drag past RmlUi's threshold) does NOT fire this —
    // it still fires data-event-click — so tap-to-restore and drag-to-close coexist.
    //
    // RML AUTHORING SHAPE (the substrate hooks NONE of these implicitly — author
    // all three you want; binding one name to several phases is the whole point):
    //
    //   <... style="drag: drag;"
    //        data-event-dragstart="<name>"
    //        data-event-drag="<name>"
    //        data-event-dragend="<name>">
    //
    // The RCSS `drag: drag;` on the element is REQUIRED — without it RMLUi emits no
    // drag events at all (the captured pointer stream stays a plain click/tap). All
    // three data-event-drag{start,,end} attributes name the SAME <name>; the
    // substrate delivers each to your one callback with phase start/move/end
    // respectively. Omit `data-event-dragstart`/`-dragend` if you only need the
    // phase(s) you author — but the dock close needs all three (start = arm,
    // move = live slide percent, end = snap), so author all three.
    virtual void bind_drag(std::string_view name,
                           std::function<void(DragPhase phase, double x, double y)> callback) = 0;

    // React to a touch-mode flip on THIS surface. `callback(touch)` is invoked
    // (event-loop thread) when the substrate's touch-mode changes — touch ==
    // true for finger mode. The substrate itself does NOTHING visual on a flip;
    // this is the only way a surface changes for touch. Use it to adapt the
    // SAME document yourself: set_size() to a roomier surface, toggle a bound
    // bool your RCSS keys off, dirty() bindings, etc. A throwing callback
    // isolates your extension. One callback per surface; re-binding replaces it.
    // Ignoring it is fine — the surface simply looks the same in both modes.
    virtual void on_touch_mode_changed(std::function<void(bool touch)> callback) = 0;

    // ---- List bindings (typed, RMLUi-free) ----
    // A LIST is a runtime-sized, indexed sequence the document iterates with
    //   <div data-for="row : <name>"> … {{ row.<field> }} … </div>
    // (the iterator alias — "row" above — is the author's choice; the count and
    // the per-field getters you register here drive what each row renders).
    //
    // bind_list(name, count): declare the list. `count()` is called by the
    // substrate every time the list re-renders (after dirty(name) or dirty())
    // to size the loop; rows are indexed 0..count()-1. It must be cheap and
    // pure (no event-loop blocking). A count() that throws is caught and
    // isolates your extension (the list then renders as empty for that frame).
    //
    // bind_list_<type>(list, field, getter): declare a per-row FIELD `<field>`
    // of the named list, read in RML as {{ row.<field> }}. For each visible row
    // the substrate calls getter(row_index) to produce that cell's value. Same
    // contract as the scalar getters: cheap, pure, called for the surface's
    // lifetime, capturing only state that outlives this surface; a throwing
    // getter isolates your extension. Register a field before the first frame;
    // re-registering the same (list, field) replaces the getter. string is the
    // workhorse (preview/favicon URIs, titles); int/double/bool are provided
    // for numeric/flag cells.
    //
    // bind_list_event(list, event, callback): declare a per-row EVENT. Author
    // it on a row element as e.g. data-event-click="<event>(it_index)" — the
    // `it_index` argument is the row index the document supplies, and the
    // substrate delivers it to callback(row_index). (Omit the argument and you
    // get row 0; always pass it_index for the real row.) Invoked on the
    // event-loop thread; a throwing callback is caught at the substrate
    // boundary and disables YOUR extension only — never the session. Register
    // before the first frame; re-registering the same (list, event) replaces
    // the callback.
    //
    // ALL of bind_list* must be called BEFORE the first frame (same rule as the
    // scalar bind_*: RmlUi needs the data model complete before it parses the
    // document). Binding a field/event/list after the first frame is a no-op.
    // Row fields may be added in any order relative to bind_list for the same
    // name. NESTED lists (a list field that is itself a list) are NOT supported.
    virtual void bind_list(std::string_view name, std::function<std::size_t()> count) = 0;
    virtual void bind_list_string(std::string_view list, std::string_view field,
                                  std::function<std::string(std::size_t row)> getter) = 0;
    virtual void bind_list_int(std::string_view list, std::string_view field,
                               std::function<int(std::size_t row)> getter) = 0;
    virtual void bind_list_double(std::string_view list, std::string_view field,
                                  std::function<double(std::size_t row)> getter) = 0;
    virtual void bind_list_bool(std::string_view list, std::string_view field,
                                std::function<bool(std::size_t row)> getter) = 0;
    virtual void bind_list_event(std::string_view list, std::string_view event,
                                 std::function<void(std::size_t row)> callback) = 0;

    // Mark a bound scalar changed so the substrate re-reads its getter and
    // re-renders on the next frame. dirty() with no name marks ALL bound
    // scalars dirty (use sparingly). For a list, dirty(<list-name>) re-reads
    // its count() and re-reads every visible row's field getters, growing or
    // shrinking the rendered rows to match the new count on the next frame.
    virtual void dirty(std::string_view name) = 0;
    virtual void dirty() = 0;

protected:
    UiSurface() = default;
};

// Creation parameters for a ui surface. Provide EITHER inline RML in
// `rml_inline` OR an asset path in `rml_path` (path wins if both set). Geometry
// is layout-space; `layer` defaults to overlay (above toplevels). `visible`
// is the initial visibility.
//
// rml_path RESOLUTION. An ABSOLUTE path is loaded as-is. A RELATIVE path
// resolves against the asset root: `$UNBOX_ASSET_DIR` if set, else the
// compile-time install data dir (`UNBOX_ASSET_DIR_DEFAULT`, falling back to the
// process working dir). So a unit passes e.g. `rml_path =
// "ext-stage-dock/dock.rml"` and the dev launch sets `UNBOX_ASSET_DIR=<repo>/
// assets`. The document's own `<link>`/`<style>`/image srcs resolve RELATIVE TO
// THE DOCUMENT'S DIRECTORY, so a dock.rml that links a dock.rcss in the same dir
// just works. A missing/unreadable/malformed file yields a null surface from
// create_surface (graceful degrade, never throws) — same contract as no-GL.
//
// DEV HOT-RELOAD (zero overhead in production). When the process env sets
// `UNBOX_DEV` (or `UNBOX_HOT_RELOAD`), the substrate watches the directory of a
// file-backed surface and, on an editor save (handled via dir-watch so the usual
// temp-file+rename works), RELOADS the document live — no recompile, no restart.
// A reload PRESERVES the surface's RmlUi context, its data model, and every
// bind_*/bind_list*/bind_event getter/callback you registered (you set them once
// "before the first frame"; do NOT re-register on reload — the substrate keeps
// them and re-evaluates {{…}}/data-for/data-event against the new document), and
// PRESERVES the surface's geometry/visibility and any registered preview
// textures. RCSS edits re-parse (the stylesheet cache is dropped). A malformed
// save is ERROR-ISOLATED: the previous good document keeps rendering, one warning
// is logged, and the next good save recovers — a bad file never crashes or
// disables the session. Inline (`rml_inline`) surfaces are not watched.
struct UiSurfaceSpec {
    std::string rml_inline{};                 // inline RML document text
    std::string rml_path{};                   // path to an .rml asset (assets/<unit>/…)
    // The data-model name your document binds against: its <body> must carry
    // data-model="<this>" and {{name}} / data-event-* refer to the names you
    // bind via UiSurface::bind_*. Per-surface (each surface has its own RMLUi
    // context), so the default "ui" is fine for every surface; override only if
    // your document already uses a different data-model attribute.
    std::string model = "ui";
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    SceneLayer layer = SceneLayer::overlay;
    bool visible = true;
};

// A FROZEN, graphically scalable image of a toplevel's pixels, snapshotted from
// a wlr_scene_tree at create/refresh time and imported as a texture INTO the ui
// substrate's RMLUi context. Show it by putting source_uri() into an RML
// <img src="..."> in ANY ui surface this same substrate created (the preview's
// texture lives in the shared sibling GLES context, so every surface can sample
// it). Owned by the contributing extension via unique_ptr; destruction frees the
// imported texture + EGLImage + snapshot dmabuf and unregisters the URI. NOT
// live — it is a copy, so the source toplevel may later be hidden or destroyed
// without affecting an existing Preview. Event-loop thread only.
class Preview {
public:
    virtual ~Preview() = default;
    Preview(const Preview&) = delete;
    auto operator=(const Preview&) -> Preview& = delete;

    // The <img src> value resolving to this preview's texture inside any ui
    // surface of this substrate (e.g. "unbox-preview://7"). Stable for life.
    [[nodiscard]] virtual auto source_uri() const -> std::string = 0;
    // Natural pixel size of the snapshot (aspect ratio). The <img> may be sized
    // to any box via RCSS; RML scales the texture to fit.
    [[nodiscard]] virtual auto source_width() const -> int = 0;
    [[nodiscard]] virtual auto source_height() const -> int = 0;

    // Re-snapshot from the original source scene tree if it is still valid.
    // The borrow validity of the source tree is the CALLER's concern: the
    // substrate cannot detect a freed wlr_scene_tree, so calling refresh()
    // after the source has been destroyed is UNDEFINED BEHAVIOUR — the caller
    // MUST drop the Preview when its source toplevel unmaps/destroys. A
    // refresh() that fails to render (e.g. substrate lost its GL path) is a
    // no-op and leaves the previous snapshot intact. NEVER throws.
    virtual void refresh() = 0;

protected:
    Preview() = default;
};

// The kernel's ui substrate, reached via Host::ui(). Per-session, kernel-owned;
// the reference is a borrow valid for your extension's lifetime. Carries your
// extension identity for error isolation.
class UiSubstrate {
public:
    virtual ~UiSubstrate() = default;
    UiSubstrate(const UiSubstrate&) = delete;
    auto operator=(const UiSubstrate&) -> UiSubstrate& = delete;

    // Create a ui surface from `spec`. Ownership transfers to you (unique_ptr).
    // Returns nullptr if the substrate is unavailable on this backend (no GL
    // path — e.g. the headless pixman renderer) or the document failed to load;
    // a UI extension should degrade gracefully (no surface) rather than abort.
    // NEVER throws.
    [[nodiscard]] virtual auto create_surface(const UiSurfaceSpec& spec)
        -> std::unique_ptr<UiSurface> = 0;

    // Snapshot the pixels drawn under `source` (a scene subtree — typically a
    // toplevel's scene tree) into an ARGB8888 dmabuf via the wlr renderer, then
    // import that dmabuf into the RMLUi sibling context as a sampled GL texture
    // and register it under an "unbox-preview://N" URI. Show the result by
    // putting the returned Preview's source_uri() into an RML <img src="..."> in
    // any ui surface this substrate created. Ownership transfers to you
    // (unique_ptr). Returns nullptr if the substrate is unavailable (no GL path,
    // e.g. headless pixman) or the snapshot/import failed. `source` is a borrow
    // used only during this call (and on refresh()). NEVER throws.
    //
    // SCOPE (slice-10 spike): a single-surface toplevel is fully supported; the
    // snapshot composites every WLR_SCENE_NODE_BUFFER under `source` at its tree
    // offset, so simple subsurface stacks composite too, but complex
    // transform/clip/opacity per node is NOT honoured yet (a follow-up).
    [[nodiscard]] virtual auto create_preview(wlr_scene_tree* source)
        -> std::unique_ptr<Preview> = 0;

    // Whether the substrate has a working GL bridge on this backend. When
    // false, create_surface returns nullptr (degrade gracefully).
    [[nodiscard]] virtual auto available() const -> bool = 0;

    // ---- touch-mode (GLOSSARY: "touch-mode") ----
    // The substrate-level state signalling finger input. It does NO automatic
    // visual scaling (user decision) — it just flips automatically (a touch
    // event turns it on, pointer motion off, debounced) and surfaces opt in to
    // adapting via UiSurface::on_touch_mode_changed. These let tests/config
    // read or pin it.
    [[nodiscard]] virtual auto touch_mode() const -> bool = 0;
    // Pin touch-mode on/off, or release back to automatic.
    enum class TouchModeOverride { automatic, force_off, force_on };
    virtual void set_touch_mode_override(TouchModeOverride ov) = 0;

protected:
    UiSubstrate() = default;
};

} // namespace unbox::kernel
