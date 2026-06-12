# kernel — package notes

Slice-4 state: the kernel **names no concrete feature** and boots
featureless. It owns generic plumbing (compositor/subcompositor/data-device,
output+scene glue, cursor + xcursor-mgr + seat, the kernel-private ui spike)
plus the **extension host + typed bus**. ALL shell policy (xdg-shell
toplevels/popups, focus, alt-cycle, terminate, interactive move/resize,
keybindings) was EXTRACTED — `src/toplevel.cpp` is deleted; ext-xdg-shell /
ext-layer-shell recreate it from the contract alone.

Public contract (the ABI): `hooks.hpp` (typed `Event<Args...>` /
`Filter<T>` + RAII `Subscription`), `extension.hpp` (`Tier`, `Manifest`,
`Extension`), `host.hpp` (`Host` facade: borrows + event catalogue + scene
layers + services + typed surface→tree association), `listener.hpp` (the RAII
`wl_listener` wrapper, now public), `surface_registry.hpp` (`SurfaceRegistration`
RAII handle + the pure `detail::PointerAssoc` core), `server.hpp` (`install` +
`activate_extensions`).

Side-effect graph (who emits / who routes):
- The kernel EMITS typed Events for its glue (output add/remove; pointer
  motion/button/axis/frame; touch down/motion/up/cancel/frame) and applies
  `key_filter` to every key. It moves the cursor, runs seat-capability and
  seat-protocol glue (request_set_cursor/selection, focus_change default
  cursor), and forwards a key to the focused client ONLY if no filter link
  set `handled`. It routes NOTHING else to client surfaces and makes NO
  focus decision — extensions do that via the bus + the seat borrow.
- `Server::install()` transfers ownership; `activate_extensions()` (called
  by `run()`, or earlier by host-bin/tests) topo-sorts by `Manifest
  depends_on` (ties: tier then install order), then calls each `activate`.
  Missing dep / cycle / duplicate id = `std::runtime_error` at startup. An
  `activate()` throw is FATAL (propagates) — a core ext that can't start is
  a broken session, not an isolated one. RUNTIME callback throws ARE
  isolated (see below).
- Scene z-bands live in `Impl::scene_layers[]` (SceneLayer order, created
  over `scene->tree` background→overlay so stacking is correct). The ui
  spike now sits in the `overlay` band. Extensions attach via
  `Host::scene_layer()`.

Gotchas the headers can't express:

- **Error isolation = deferred purge.** A hook callback that throws is
  caught at the bus boundary; `Server::Impl` (a `detail::DisableSink`) marks
  the owning extension disabled and `purge()`s its subscriptions from EVERY
  registered hook (`all_hooks`). Purge during a live dispatch only
  tombstones (`dead=true`); physical erase happens when that hook's dispatch
  depth returns to 0 (`compact_if_idle`). So disabling an extension from
  inside its own callback, and an ext subscribed to multiple hooks, are both
  safe. Hooks are PINNED (Subscriptions hold a raw `HookBase*`): never move
  an `Event`/`Filter`; hold them as stable members.
- **Extensions are destroyed FIRST in `shutdown()`**, reverse of install
  order, so their RAII members (Subscriptions, Listeners, scene nodes)
  release while the wlr objects they borrow are still alive. Then the spike,
  then clients, then server-level Listeners, then wlr objects.
- **`wlr.hpp` blanks `static` around the wlr includes.** wlroots headers
  use C99 array-parameter syntax (`float color[static 4]`), invalid in
  C++. With `static` blanked, `static inline` helpers become `inline`
  (ODR-merged, safe). Re-audit when ADDING includes to the wrapper.
- **RMLUi is kernel-private.** `rmlui_dep` is deliberately absent from
  `kernel_dep` propagation: extensions contribute RML documents + data
  bindings via the (future) ui substrate, never RMLUi API calls. Do not
  "fix" a missing-RMLUi-header error downstream by propagating it.
- **Server-level Listener disconnect order is load-bearing**
  (`Impl::shutdown()`): a Listener outliving the wlr object owning its
  signal is a use-after-free (`wl_list_remove` touches neighbor links).
  Entity-level Listeners (Output/Keyboard/TouchDevice) are exempt: their
  destroy events fire during `wl_display_destroy_clients` / backend destroy.
- **A Listener handler may destroy its own Listener** (the destroy-event
  pattern) but the erase/delete must be the handler's LAST action.
- **No cross-unit `wlr_surface.data`.** The surface→scene-tree association is
  a typed kernel contract (`Host::host_surface`/`scene_tree_for`, backed by
  `Server::Impl::surface_assoc`). The map is kernel-owned but the VALUE tree is
  an extension's; the returned tree is a borrow valid only while the hosting
  extension's `SurfaceRegistration` lives. Re-hosting a surface supersedes the
  old handle (token defense), so a stale handle never tears down the new
  mapping. Private intra-unit `.data` use is still fine; cross-unit must route
  through the contract.
- **Pointer button & axis are NOT forwarded by the kernel** (it only moves the
  cursor and emits `ev_pointer_button`/`ev_pointer_axis`). The pointer-routing
  extension forwards them via `wlr_seat_pointer_notify_button/_axis`, same as
  enter/motion/frame — not notifying during a grab is the suppression mechanism.
  (The old "kernel forwards button/axis" doc comment was a verified lie; fixed.)
- Everything runs on the single `wl_event_loop` thread.

Slice-3 spike gotchas (EGL/dmabuf — read before touching `ui_spike.cpp`):

- **The sibling GLES 3.2 context shares the EGLDisplay, NOT GL objects.**
  `eglCreateContext` is called with `share_context = EGL_NO_CONTEXT` on
  `wlr_egl_get_display(wlr_gles2_renderer_get_egl(renderer))`. Buffers cross
  the boundary ONLY as dmabuf/EGLImage (Plan A) or CPU copy (Plan B) — never
  shared GL handles. The wlr renderer's current EGL context/surfaces are
  saved before `eglMakeCurrent` and restored after every tick/teardown;
  forgetting the restore corrupts wlr's own rendering.
- **The context is surfaceless** (`eglMakeCurrent(dpy, EGL_NO_SURFACE,
  EGL_NO_SURFACE, ctx)`, requires `EGL_KHR_surfaceless_context` — present on
  crocus). RMLUi's `RmlUi_Renderer_GL3` hardcodes `glBindFramebuffer(0)` in
  `EndFrame()`; framebuffer 0 is INCOMPLETE in a surfaceless context. The
  adapted copy adds `SetOutputFramebuffer()` so EndFrame composites into the
  bridge's own offscreen FBO instead. Re-apply that delta on any RMLUi bump.
- **GLES path selection is `-DUNBOX_RMLUI_GLES`** (kernel meson.build,
  scoped to the kernel lib). It mirrors upstream's `__ANDROID__` branch
  (`#version 320 es`, `<GLES3/gl32.h>`, CLAMP_TO_EDGE, no sRGB framebuffer)
  WITHOUT defining the `__ANDROID__` builtin (which would poison the whole
  TU). Four upstream `#if ...__ANDROID__` guards were widened to also test
  `UNBOX_RMLUI_GLES`; re-audit them on a bump.
- **Plan A dmabuf import is single-plane LINEAR.** We allocate ARGB8888
  (FourCC 'AR24') with a LINEAR-only modifier list via
  `wlr_allocator_create_buffer`, `wlr_buffer_get_dmabuf`, then
  `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` + `glEGLImageTargetTexture2DOES`
  as the FBO color attachment. Preconditions checked at runtime (allocator
  DMABUF cap, `EGL_EXT_image_dma_buf_import`, the two entrypoints); any miss
  or an incomplete FBO falls back to Plan B with a log line. Render formats
  are NOT public in wlroots 0.20 (`wlr_renderer_get_render_formats` is
  private; only `get_texture_formats` is exported), so we pick the format by
  hand — revisit if a future GPU rejects linear ARGB8888 as a render target.
- **Submission sync is `glFinish()` (spike fidelity), not a fence.** Plan A
  must ensure GL writes land before the compositor samples the shared
  dmabuf. The real substrate should use an EGL fence
  (`EGL_KHR_fence_sync` is advertised) instead of the full pipeline stall.
- **Plan B's wlr_buffer is a custom `WLR_BUFFER_CAP_DATA_PTR` impl**
  (`ShmBuffer` wrapping a `std::vector`, via `<wlr/interfaces/wlr_buffer.h>`).
  RMLUi outputs premultiplied RGBA8 (R,G,B,A byte order); the buffer is
  FourCC 'AR24' = little-endian {B,G,R,A}, so the copy swaps R<->B. The
  alpha is already premultiplied, which wlroots expects.
- **`UNBOX_UI_SPIKE_FORCE_SHM=1`** forces the Plan-B path even where Plan A
  works — kept as fallback-test instrumentation; harmless in production.
- **Headless (pixman) disables the spike**: no gles2 renderer ⇒ no
  EGLDisplay ⇒ `start_ui_spike()` no-ops. Headless+gles2 (render node
  present) DOES exercise Plan A — verified.
- **GL framebuffer origin is bottom-left; wlr_buffer scan-out is top-first.**
  RMLUi already maps document-y=0 to the GL framebuffer top via
  `ProjectOrtho(0,w,h,0,...)`, but reading the FBO out (glReadPixels, Plan B)
  or scanning out the dmabuf (Plan A) yields row 0 = GL bottom = document
  bottom ⇒ the whole document composited **upside-down** (caught by grim).
  Fix: the adapted renderer's `EndFrame()` flips V on the FINAL composite
  into the output FBO when `SetOutputFramebuffer(fbo, /*flip_y=*/true)`
  (vertex-uv flip on the passthrough fullscreen quad). Chosen over a scene
  transform or a flipped projection because (a) it makes the SUBMITTED buffer
  genuinely top-first — so the orientation assertion can read it back
  directly and so display == document coords, leaving pointer input
  un-transformed (an on-screen hover hits the button in document space); and
  (b) it is ONE localized change that applies identically to Plan A and Plan
  B and needs no matching scissor/clip-rect change (scissor only affects
  intermediate layer rendering, not the final composite). Re-apply on an
  RMLUi bump alongside the `SetOutputFramebuffer` delta. NOTE: a flip done as
  a display-only transform would have left on-screen hit-testing wrong even
  while a document-space input test passed — verify display+input together.
- **Orientation regression guard**: the spike document carries distinctive
  full-width solid bands at its top (`#18e0a0`) and bottom (`#e09018`) edges.
  `UiSpike::check_orientation()` (exposed as `Server::ui_spike_orientation()`)
  inspects the Plan-B readback and returns +1 upright / -1 flipped / 0
  indeterminate. The `kernel` suite asserts it is never -1 (and ==1 when the
  bridge ran). Position-aware, not just color-aware — a flip can't slip past.
