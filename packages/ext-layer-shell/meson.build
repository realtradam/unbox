# ext-layer-shell — wlr-layer-shell-unstable-v1 (v5) for external clients.
# Public headers (the contract): include/unbox/ext-layer-shell/
#   ext_layer_shell.hpp  — the extension factory (cross-extension surface)
#   arrangement.hpp      — the PURE CORE usable-area math (wlroots-free)

ext_layer_shell_inc = include_directories('include')

# Glue library: the extension implementation. Pulls kernel_dep (and through it
# wlroots/wayland + the generated layer-shell protocol header via wlr.hpp).
ext_layer_shell_lib = static_library(
  'unbox-ext-layer-shell',
  'src/ext_layer_shell.cpp',
  include_directories: ext_layer_shell_inc,
  dependencies: [kernel_dep],
)

# What host-bin (and any consumer of the usable-area model) links against. The
# arrangement.hpp pure core rides on the include path with no wlroots in its
# transitive closure; consuming the factory pulls kernel_dep for the Extension
# ABI it returns.
ext_layer_shell_dep = declare_dependency(
  link_with: ext_layer_shell_lib,
  include_directories: ext_layer_shell_inc,
  dependencies: [kernel_dep],
)

# Pure-core test: arrangement math, doctest-hard. NO kernel/wlroots — the whole
# point of the pure core is that it tests with nothing running.
ext_layer_shell_arrangement_test = executable(
  'ext-layer-shell-arrangement-tests',
  'tests/test_arrangement.cpp',
  include_directories: ext_layer_shell_inc,
  dependencies: [doctest_dep],
)
test(
  'ext-layer-shell-arrangement',
  ext_layer_shell_arrangement_test,
  suite: 'ext-layer-shell',
)

# Glue test: install + activate + dispatch + clean shutdown on the wlr headless
# backend. Lenient (a few integration checks, not coverage-chasing).
ext_layer_shell_glue_test = executable(
  'ext-layer-shell-glue-tests',
  'tests/test_glue.cpp',
  dependencies: [ext_layer_shell_dep, doctest_dep],
)
test(
  'ext-layer-shell-glue',
  ext_layer_shell_glue_test,
  suite: 'ext-layer-shell',
)

# Regression test for the "real client gets no configure" bug: a genuine
# in-process wayland-CLIENT binds zwlr_layer_shell_v1, creates a layer surface
# with a NIL output (exactly what fuzzel does), and must receive a configure.
# This requires CLIENT-side bindings for the layer-shell protocol, generated
# from the same vendored XML the kernel uses for the server side.
wayland_client_dep = dependency('wayland-client')

layer_shell_client_h = custom_target(
  'wlr-layer-shell-client-header',
  input: meson.project_source_root() / 'protocol' / 'wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
# Private code as a HEADER (not .c): the root project declares only C++, so a
# generated .c has no compiler. wayland-scanner's private-code is plain C that
# is valid C++; emitting it as a header lets the (single) C++ test TU #include
# it exactly once. The output name ends in .h purely so meson treats it as a
# header (no separate compile), not as a marshalling-code convention.
layer_shell_client_code_h = custom_target(
  'wlr-layer-shell-client-code',
  input: meson.project_source_root() / 'protocol' / 'wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-client-protocol-code.h',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# The layer-shell protocol's get_popup references xdg_popup, so its generated
# marshalling tables reference xdg_popup_interface. We don't link a client-side
# xdg-shell lib, so generate xdg-shell client code (header-form, same reason) to
# supply that symbol. wayland-protocols ships the canonical xdg-shell.xml.
wayland_protocols_dir = dependency('wayland-protocols').get_variable('pkgdatadir')
xdg_shell_client_code_h = custom_target(
  'xdg-shell-client-code',
  input: wayland_protocols_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml',
  output: 'xdg-shell-client-protocol-code.h',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

ext_layer_shell_client_test = executable(
  'ext-layer-shell-client-tests',
  'tests/test_client.cpp',
  layer_shell_client_h,
  layer_shell_client_code_h,
  xdg_shell_client_code_h,
  dependencies: [ext_layer_shell_dep, wayland_client_dep, doctest_dep],
)
test(
  'ext-layer-shell-client',
  ext_layer_shell_client_test,
  suite: 'ext-layer-shell',
  # A real socket handshake + cooperative event-loop pump; generous timeout so
  # a slow CI box does not flake (the test fails fast on its own logic).
  timeout: 60,
)
