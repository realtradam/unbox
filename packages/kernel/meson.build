# kernel — the minimal runtime core. Public headers (the ABI): include/unbox/kernel/

kernel_inc = include_directories('include')

# Slice-3 spike: native GLES 3.2 + EGL for the RMLUi -> wlr_scene bridge.
# Already surfaced to the user and approved (prompts/kernel.md). The kernel
# owns GL; these are kernel-private and do NOT propagate to consumers.
egl_dep = dependency('egl')
glesv2_dep = dependency('glesv2')

# ---- Wayland protocol codegen (the repo's FIRST; this is the template) -------
#
# wlroots' <wlr/types/wlr_layer_shell_v1.h> #includes the generated
# "wlr-layer-shell-unstable-v1-protocol.h" (a wlr-protocols extra not shipped
# by the system wayland-protocols package, so the XML is vendored read-only in
# the repo root protocol/). Because wlr.hpp is a PUBLIC header that pulls that
# wlroots header in, EVERY consumer of kernel_dep must compile against the
# generated header AND must not build before codegen runs — see the
# declare_dependency(sources: ...) propagation below, which carries both the
# include path and the build-ordering edge.
#
# wayland-scanner is located via its own pkg-config variable (the canonical
# pattern wlroots/sway/labwc use), run on the BUILD machine (native: true).
wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
  wayland_scanner_dep.get_variable('wayland_scanner'),
  native: true,
)

# Repo-root protocol/ holds the vendored XML (read-only; provisioned by the
# orchestrator). meson.project_source_root() keeps this robust regardless of
# this subdir's depth.
wlr_layer_shell_xml = files(
  meson.project_source_root() / 'protocol' / 'wlr-layer-shell-unstable-v1.xml',
)

# server-header only: the OUTPUT NAME must be exactly the string wlroots
# #includes. The interface symbols (wlr_layer_shell_v1.h's deps) are already
# exported by libwlroots, so the private-code glue is NOT needed here — proven
# by a clean link below (no undefined wl_*_interface references). Add a
# matching 'private-code' custom_target only if a future protocol's symbols are
# not provided by a linked library.
wlr_layer_shell_protocol_h = custom_target(
  'wlr-layer-shell-unstable-v1-protocol.h',
  input: wlr_layer_shell_xml,
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

# UNBOX_RMLUI_GLES selects the native GLES 3.2 path in the adapted RmlUi GL3
# renderer (src/rmlui_renderer_gl3.cpp) without poisoning the TU with the
# __ANDROID__ builtin. Scoped to this library only.
kernel_lib = static_library(
  'unbox-kernel',
  'src/kernel.cpp',
  'src/server.cpp',
  'src/input.cpp',
  'src/ui_substrate.cpp',
  'src/rmlui_renderer_gl3.cpp',
  # Listing the generated header as a source forces codegen before any kernel
  # TU compiles and puts its build dir on this lib's include path.
  wlr_layer_shell_protocol_h,
  cpp_args: ['-DUNBOX_RMLUI_GLES'],
  include_directories: kernel_inc,
  dependencies: [wlroots_dep, wayland_server_dep, xkbcommon_dep, rmlui_dep,
                 egl_dep, glesv2_dep],
)

# What consumers get. wlroots/wayland propagate because wlr.hpp is a public
# header; RMLUi does NOT — it is kernel-private (the ui substrate owns it,
# extensions contribute RML documents + data bindings, never RMLUi calls).
# The generated protocol header rides in `sources:` so every consumer of
# kernel_dep gets BOTH the include path for it AND a build-ordering edge to the
# codegen custom_target — a consumer can never compile wlr.hpp before the
# header exists.
kernel_dep = declare_dependency(
  link_with: kernel_lib,
  sources: [wlr_layer_shell_protocol_h],
  include_directories: kernel_inc,
  dependencies: [wlroots_dep, wayland_server_dep],
)

kernel_test = executable(
  'kernel-tests',
  'tests/test_kernel.cpp',
  dependencies: [kernel_dep, doctest_dep],
)
test('kernel', kernel_test, suite: 'kernel')
