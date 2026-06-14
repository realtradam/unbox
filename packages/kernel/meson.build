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
  'src/file_watcher.cpp',
  'src/frame_driver.cpp',
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

# wayland-client (TEST-ONLY): the surface-element integration test spins an
# in-process Wayland CLIENT thread that connects to the headless server, creates
# a wl_surface + wl_shm buffer (+ a subsurface + an xdg toplevel/popup for the
# Wave-1b surface-TREE test), and commits — the only in-process way to get a
# REAL wlr_surface tree with an advancing commit seq to exercise the live import
# + input-back. It is NOT a kernel-library dependency (the kernel is a
# compositor, never a client); scoped to the test executable only.
wayland_client_dep = dependency('wayland-client')

# CLIENT-side xdg-shell bindings for the Wave-1b surface-tree test (a real xdg
# popup is a tree child element). Generated from the canonical xdg-shell.xml the
# same way ext-xdg-shell's client test does (header + private-code-as-header,
# #included once by the single test TU). The SERVER side runs in a TEST
# extension via wlr_xdg_shell (kernel-private wlr.hpp), so the kernel itself
# still names no shell — xdg-shell stays a feature, provided by a test ext.
wayland_protocols_dir = dependency('wayland-protocols').get_variable('pkgdatadir')
xdg_shell_xml = wayland_protocols_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml'
xdg_shell_client_header = custom_target(
  'kernel-xdg-shell-client-header',
  input: xdg_shell_xml,
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_shell_client_code = custom_target(
  'kernel-xdg-shell-client-code',
  input: xdg_shell_xml,
  output: 'xdg-shell-client-protocol-code.h',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

kernel_test = executable(
  'kernel-tests',
  'tests/test_kernel.cpp',
  xdg_shell_client_header,
  xdg_shell_client_code,
  dependencies: [kernel_dep, doctest_dep, wayland_client_dep],
)
test('kernel', kernel_test, suite: 'kernel')

# ---- SPIKE: RML compositing (Phase 0 GO/NO-GO) -------------------------------
#
# A self-contained, RUNNABLE throwaway target (notes/rml-compositing.md §Phase 0,
# prompts/rml-compositing-spike.md). It is its OWN minimal compositor that maps
# real clients and composites them as LIVE surface elements inside an RmlUi
# document, proving the 7 acceptance criteria. Kept OUT of the shipped `unbox`
# binary: it is NOT in kernel_dep and host-bin never links it; build it
# explicitly with `ninja -C build rml-compositing-spike`.
#
# It reuses the kernel's adapted RmlUi GL3 renderer (src/rmlui_renderer_gl3.cpp)
# directly, so it needs RMLUi + EGL/GLES (kernel-private deps) AND the same
# -DUNBOX_RMLUI_GLES native-GLES selection the kernel lib compiles under. The
# generated layer-shell protocol header rides in via the source list (build
# order). It links the kernel lib for the renderer + wlr.hpp wrapper.
rml_compositing_spike = executable(
  'rml-compositing-spike',
  'src/spike/rml_compositing_spike.cpp',
  'src/spike/rml_compositing_spike_run.cpp',
  'src/rmlui_renderer_gl3.cpp',
  wlr_layer_shell_protocol_h,
  cpp_args: ['-DUNBOX_RMLUI_GLES'],
  include_directories: kernel_inc,
  dependencies: [wlroots_dep, wayland_server_dep, xkbcommon_dep, rmlui_dep,
                 egl_dep, glesv2_dep],
  build_by_default: false,
)
