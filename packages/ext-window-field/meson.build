# ext-window-field — RML compositing Phase 2, Wave 3 (GLOSSARY: "window field").
# The core extension that composites application toplevels as RCSS SURFACE
# ELEMENTS inside ONE ui surface (the "window field"), instead of as wlr_scene
# nodes: it subscribes to ext-xdg-shell's toplevel lifecycle, turns each
# toplevel's root wl_surface into a kernel SurfaceElement, lists them into its
# RML document, and lets RCSS lay them out + animate. Layout/animation are RCSS
# (the user's contract decision); this glue only DRIVES the document + focus.
#
# Installed by host-bin ONLY when RML compositing is enabled (the flag) — so the
# classic wlr_scene path stays the default until this is proven on the real seat.
# Tier: core; depends_on "xdg-shell".

ext_window_field_inc = include_directories('include')

# Glue library: needs the kernel ABI (ui substrate + SurfaceElement) and
# ext-xdg-shell's public contract (Toplevel::wl_surface(), the Service events).
ext_window_field_lib = static_library(
  'unbox-ext-window-field',
  'src/extension.cpp',
  include_directories: ext_window_field_inc,
  dependencies: [kernel_dep, ext_xdg_shell_dep],
)

# What host-bin links against (the factory). kernel_dep rides through for the
# Extension ABI the factory returns; ext-xdg-shell stays a build-time dep of OUR
# lib only.
ext_window_field_dep = declare_dependency(
  link_with: ext_window_field_lib,
  include_directories: ext_window_field_inc,
  dependencies: [kernel_dep],
)

# Tests, asymmetric (AGENTS.md: strict cores, lenient shell). This unit is PURE
# GLUE — its only policy (window layout) lives in RCSS, not C++, so there is no
# doctest-able decision core; the SMOKE test (factory/manifest) + a lenient
# HEADLESS GLUE test (below) are the whole suite.

# SMOKE test: the factory yields the window-field extension with the right
# manifest. Cheap, no kernel/wlroots running.
ext_window_field_smoke_test = executable(
  'ext-window-field-smoke-tests',
  'tests/test_window_field.cpp',
  include_directories: [ext_window_field_inc, include_directories('src')],
  dependencies: [ext_window_field_dep, doctest_dep],
)
test(
  'ext-window-field-smoke',
  ext_window_field_smoke_test,
  suite: 'ext-window-field',
)

# HEADLESS GLUE test: install ext-xdg-shell + ext-window-field on the wlr
# headless backend, map REAL in-process client toplevels, then drive the
# map/unmap/focus PIPELINE and assert the bound window MODEL via the private test
# probe (src/probe.hpp) + that Toplevel::hide() was driven. Lenient ext-tier
# glue: the substrate is null on pixman (no GL), so the field UiSurface +
# SurfaceElements are null — this exercises the model + hide(), not the visual
# (same posture as ext-stage-dock's test_glue.cpp). Needs CLIENT-side xdg-shell
# bindings generated from the canonical xdg-shell.xml exactly as ext-stage-dock's
# test_glue.cpp does (header + private-code-as-header, #included once by the
# single C++ TU). `src` on the include path so the test reaches the private probe
# factory (own src/ only).
wf_wayland_client_dep = dependency('wayland-client')
wf_wayland_protocols_dir = dependency('wayland-protocols').get_variable('pkgdatadir')
wf_xdg_shell_xml = wf_wayland_protocols_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml'

wf_xdg_shell_client_header = custom_target(
  'wf-xdg-shell-client-header',
  input: wf_xdg_shell_xml,
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
# private-code emitted as a .h so the C++ TU #includes it exactly once.
wf_xdg_shell_client_code = custom_target(
  'wf-xdg-shell-client-code-impl',
  input: wf_xdg_shell_xml,
  output: 'xdg-shell-client-protocol-code.h',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

ext_window_field_glue_test = executable(
  'ext-window-field-glue-tests',
  'tests/test_glue.cpp',
  wf_xdg_shell_client_header,
  wf_xdg_shell_client_code,
  include_directories: [ext_window_field_inc, include_directories('src')],
  dependencies: [ext_window_field_dep, ext_xdg_shell_dep, wf_wayland_client_dep, doctest_dep],
)
test(
  'ext-window-field-glue',
  ext_window_field_glue_test,
  suite: 'ext-window-field',
  # Real socket handshake + cooperative event-loop pump; generous timeout so a
  # slow CI box does not flake (the test fails fast on its own logic).
  timeout: 60,
)

# Aggregate alias: `ninja -C build ext-window-field-tests`.
alias_target('ext-window-field-tests',
  ext_window_field_smoke_test,
  ext_window_field_glue_test,
)
