# ext-xdg-shell — window management as a core extension. Public headers (the
# contract): include/unbox/ext-xdg-shell/. Consumes the kernel ABI only.

ext_xdg_shell_inc = include_directories('include')

ext_xdg_shell_lib = static_library(
  'unbox-ext-xdg-shell',
  'src/extension.cpp',
  include_directories: ext_xdg_shell_inc,
  dependencies: [kernel_dep],
)

# What consumers (host-bin, downstream slices) link against. The kernel's
# wlroots/wayland propagation rides through kernel_dep; our public headers add
# the include path. RMLUi stays kernel-private (we contribute no UI yet).
ext_xdg_shell_dep = declare_dependency(
  link_with: ext_xdg_shell_lib,
  include_directories: ext_xdg_shell_inc,
  dependencies: [kernel_dep],
)

# Tests, asymmetric: pure policy core (doctest, no wlroots) + a headless glue
# smoke test (Server + activation probe). The test exe also compiles the glue
# source directly so it can reach the private probe factory in src/.
ext_xdg_shell_test = executable(
  'ext-xdg-shell-tests',
  'tests/test_policy.cpp',
  'tests/test_glue.cpp',
  # `src` on the path so test_glue can reach the PRIVATE probe factory
  # (src/probe.hpp) — a unit may read its own src/ (only OTHER units' src/ is
  # off-limits). policy.hpp lives there too.
  include_directories: [ext_xdg_shell_inc, include_directories('src')],
  dependencies: [ext_xdg_shell_dep, doctest_dep],
)
test('ext-xdg-shell', ext_xdg_shell_test, suite: 'ext-xdg-shell')

# slice 10 / b1: a real in-process wayland CLIENT maps an xdg_toplevel so we can
# drive the minimize MECHANISM (Toplevel::hide/show/geometry/scene_tree) on the
# wlr headless backend. Needs CLIENT-side xdg-shell bindings, generated from the
# canonical xdg-shell.xml the same way ext-layer-shell generates its protocol
# code (header + private-code-as-header, #included once by the single C++ TU).
wayland_client_dep = dependency('wayland-client')
wayland_protocols_dir = dependency('wayland-protocols').get_variable('pkgdatadir')
xdg_shell_xml = wayland_protocols_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml'

xdg_shell_client_header = custom_target(
  'xdg-shell-client-header',
  input: xdg_shell_xml,
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
# private-code emitted as a .h so the C++ TU #includes it exactly once (the root
# project declares only C++; a generated .c would have no compiler).
xdg_shell_client_code = custom_target(
  'xdg-shell-client-code-impl',
  input: xdg_shell_xml,
  output: 'xdg-shell-client-protocol-code.h',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

ext_xdg_shell_client_test = executable(
  'ext-xdg-shell-client-tests',
  'tests/test_minimize.cpp',
  xdg_shell_client_header,
  xdg_shell_client_code,
  dependencies: [ext_xdg_shell_dep, wayland_client_dep, doctest_dep],
)
test(
  'ext-xdg-shell-client',
  ext_xdg_shell_client_test,
  suite: 'ext-xdg-shell',
  # Real socket handshake + cooperative event-loop pump; generous timeout so a
  # slow CI box does not flake (the test fails fast on its own logic).
  timeout: 60,
)
