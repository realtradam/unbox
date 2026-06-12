# kernel — the minimal runtime core. Public headers (the ABI): include/unbox/kernel/

kernel_inc = include_directories('include')

# Slice-3 spike: native GLES 3.2 + EGL for the RMLUi -> wlr_scene bridge.
# Already surfaced to the user and approved (prompts/kernel.md). The kernel
# owns GL; these are kernel-private and do NOT propagate to consumers.
egl_dep = dependency('egl')
glesv2_dep = dependency('glesv2')

# UNBOX_RMLUI_GLES selects the native GLES 3.2 path in the adapted RmlUi GL3
# renderer (src/rmlui_renderer_gl3.cpp) without poisoning the TU with the
# __ANDROID__ builtin. Scoped to this library only.
kernel_lib = static_library(
  'unbox-kernel',
  'src/kernel.cpp',
  'src/server.cpp',
  'src/toplevel.cpp',
  'src/input.cpp',
  'src/ui_spike.cpp',
  'src/rmlui_renderer_gl3.cpp',
  cpp_args: ['-DUNBOX_RMLUI_GLES'],
  include_directories: kernel_inc,
  dependencies: [wlroots_dep, wayland_server_dep, xkbcommon_dep, rmlui_dep,
                 egl_dep, glesv2_dep],
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
