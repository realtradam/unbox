project(
  'unbox',
  'cpp',
  version: '0.0.1',
  meson_version: '>=1.4.0',
  default_options: [
    'cpp_std=c++23',
    'warning_level=2',
    'default_library=static',
  ],
)

# wlroots is pre-1.0: every release breaks API. The pin lives HERE and in
# the kernel's wlr.hpp wrapper — nowhere else (.unbox/rules/wlroots-include.md).
add_project_arguments('-DWLR_USE_UNSTABLE', language: 'cpp')

wlroots_dep = dependency('wlroots-0.20')
wayland_server_dep = dependency('wayland-server')

# RMLUi: CMake-only upstream, vendored as a wrap-file tarball (NO git
# submodules — settled decision, notes/plan.md §2) built via the cmake module.
cmake = import('cmake')
rmlui_opts = cmake.subproject_options()
rmlui_opts.add_cmake_defines({
  'BUILD_SHARED_LIBS': false,
  'RMLUI_SAMPLES': false,
})
rmlui_proj = cmake.subproject('rmlui', options: rmlui_opts)
rmlui_dep = rmlui_proj.dependency('rmlui_core')

doctest_dep = dependency('doctest')

# Units. Adding one? ALL FOUR steps of .unbox/rules/unit-registration.md.
subdir('packages/kernel')
subdir('packages/host-bin')
