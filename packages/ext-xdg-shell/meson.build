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
