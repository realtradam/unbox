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
xkbcommon_dep = dependency('xkbcommon')

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

# Sanitizer test environment. The substrate's GL tests pull in Mesa/EGL/DRM,
# which keep process-lifetime driver globals (reclaimed at exit, not leaks), and
# vendored RmlUi trips a benign vptr downcast during element teardown. Point
# LSan/UBSan at suppression files scoped to THOSE third-party frames only — our
# own code stays fully checked, so an unbox:: leak/UB still fails the suite.
# Applied by default to every `meson test` (harmless in the non-sanitized build).
add_test_setup(
  'suppressed',
  is_default: true,
  env: [
    'LSAN_OPTIONS=suppressions=' + (meson.project_source_root() / 'suppressions' / 'lsan.txt'),
    'UBSAN_OPTIONS=suppressions=' + (meson.project_source_root() / 'suppressions' / 'ubsan.txt') + ':print_stacktrace=1:halt_on_error=1',
  ],
)

# Units. Adding one? ALL FOUR steps of .unbox/rules/unit-registration.md.
subdir('packages/kernel')
subdir('packages/ext-xdg-shell')
subdir('packages/ext-layer-shell')
subdir('packages/ext-keybindings')
subdir('packages/host-bin')
