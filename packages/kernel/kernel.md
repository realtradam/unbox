# kernel — package notes

Slice-1 state: a probe surface (`kernel.hpp`) proving C++ ↔ wlroots ↔ RMLUi
compile/link. Real contracts (extension host, bus, scene/seat glue, ui
substrate) land from slice 2.

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
- Everything runs on the single `wl_event_loop` thread.
