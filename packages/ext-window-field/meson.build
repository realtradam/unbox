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

# toml++: the APPROVED config dep (notes/plan.md §2), a project-global Meson wrap
# (subprojects/tomlplusplus.wrap). Force default_library=static so the installed
# `unbox` binary bakes it in (the wrap's own meson.build hardcodes shared, which
# would need libtomlplusplus.so at runtime). Same rationale as ext-keybindings.
tomlplusplus_dep = dependency('tomlplusplus',
                              default_options: ['default_library=static'])

# Glue library: needs the kernel ABI (ui substrate + SurfaceElement), ext-xdg-
# shell's public contract (Toplevel::wl_surface()/set_size(), the Service events),
# and toml++ (the [window-field] resize-policy loader, src/config.cpp). The policy
# parse is a PURE core in src/config.{hpp,cpp} (toml++ only; doctest-covered).
ext_window_field_lib = static_library(
  'unbox-ext-window-field',
  'src/extension.cpp',
  'src/config.cpp',
  'src/geometry.cpp',
  include_directories: ext_window_field_inc,
  dependencies: [kernel_dep, ext_xdg_shell_dep, tomlplusplus_dep],
)

# What host-bin links against (the factory). kernel_dep rides through for the
# Extension ABI the factory returns; ext-xdg-shell stays a build-time dep of OUR
# lib only.
ext_window_field_dep = declare_dependency(
  link_with: ext_window_field_lib,
  include_directories: ext_window_field_inc,
  dependencies: [kernel_dep],
)

# Tests, asymmetric (AGENTS.md: strict cores, lenient shell). Window LAYOUT lives
# in RCSS (no C++ decision core), but the resize-policy CONFIG parse (src/config.
# cpp) IS a pure core and is doctest-hard below. The SMOKE test (factory/manifest)
# + a lenient HEADLESS GLUE test round out the suite.

# CONFIG core test: the pure [window-field] toml parser (defaults, every
# resize_mode value, debounce clamping, malformed input). No kernel/wlroots; the
# core TU compiles config.cpp directly and needs `src` on the include path
# (a unit may read its own src/) plus toml++.
ext_window_field_config_test = executable(
  'ext-window-field-config-tests',
  'tests/test_config.cpp',
  'src/config.cpp',
  include_directories: [ext_window_field_inc, include_directories('src')],
  dependencies: [doctest_dep, tomlplusplus_dep],
)
test(
  'ext-window-field-config',
  ext_window_field_config_test,
  suite: 'ext-window-field',
)

# GEOMETRY core test: the pure floating-window drag/resize math (move, the two
# bottom resize grips with the anchored-opposite-edge rule, min-size clamp,
# field clamp). No kernel/wlroots.
ext_window_field_geometry_test = executable(
  'ext-window-field-geometry-tests',
  'tests/test_geometry.cpp',
  'src/geometry.cpp',
  include_directories: [ext_window_field_inc, include_directories('src')],
  dependencies: [doctest_dep],
)
test(
  'ext-window-field-geometry',
  ext_window_field_geometry_test,
  suite: 'ext-window-field',
)

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
  ext_window_field_config_test,
  ext_window_field_geometry_test,
  ext_window_field_glue_test,
)
