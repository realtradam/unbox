# kernel — package notes

Slice-2 state: a working tinywl port (+ touch, which tinywl lacks) wholly
inside the kernel: backend/output/scene glue, xdg-shell toplevels + popups,
click/tap-to-focus, Alt-drag-free interactive move/resize (client-requested
only), keyboard/pointer/touch via one wlr_cursor path. Slice-2 keybindings:
Alt+Escape = terminate, Alt+F1 = cycle. Slice 4 splits shell policy out
into extensions.

Gotchas the headers can't express:

- **`wlr.hpp` blanks `static` around the wlr includes.** wlroots headers
  use C99 array-parameter syntax (`float color[static 4]`), invalid in
  C++. With `static` blanked, `static inline` helpers become `inline`
  (ODR-merged, safe). Cost: a function-local `static` inside a header
  inline would silently lose persistence — none exist in our include set;
  re-audit when ADDING includes to the wrapper.
- **RMLUi is kernel-private.** `rmlui_dep` is deliberately absent from
  `kernel_dep` propagation (see meson.build): extensions contribute RML
  documents + data bindings via the ui substrate, never RMLUi API calls.
  Do not "fix" a missing-RMLUi-header error downstream by propagating it.
- **Shutdown order is load-bearing** (`Impl::shutdown()`): destroy clients
  → disconnect ALL server-level Listeners → scene/cursor/allocator/
  renderer/backend/display. A Listener outliving the wlr object owning its
  signal is a use-after-free (`wl_list_remove` touches neighbor links).
  Entity-level Listeners are exempt: their destroy events fire (and erase
  the entities) during `wl_display_destroy_clients` / backend destroy.
- **A Listener handler may destroy its own Listener** (the destroy-event
  pattern) but the erase/delete must be the handler's LAST action — see
  listener.hpp. The slice-4 bus formalizes this.
- **Touch points record their down-surface's layout origin** to derive
  surface-local motion coords; a surface moving mid-touch (interactive
  grab) skews them. Acceptable until slice 5's input routing.
- Everything runs on the single `wl_event_loop` thread.
