# kernel — the minimal runtime core. Public headers (the ABI): include/unbox/kernel/

kernel_inc = include_directories('include')

kernel_lib = static_library(
  'unbox-kernel',
  'src/kernel.cpp',
  include_directories: kernel_inc,
  dependencies: [wlroots_dep, wayland_server_dep, rmlui_dep],
)

# What consumers get. wlroots/wayland propagate because wlr.hpp is a public
# header; RMLUi does NOT — it is kernel-private (the ui substrate owns it,
# extensions contribute RML documents + data bindings, never RMLUi calls).
kernel_dep = declare_dependency(
  link_with: kernel_lib,
  include_directories: kernel_inc,
  dependencies: [wlroots_dep, wayland_server_dep],
)

kernel_test = executable(
  'kernel-tests',
  'tests/test_kernel.cpp',
  dependencies: [kernel_dep, doctest_dep],
)
test('kernel', kernel_test, suite: 'kernel')
