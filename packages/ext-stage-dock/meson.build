# ext-stage-dock — the stage dock (slice 10): the left-edge ui surface of
# minimized-window PREVIEWS, REVEALED by a left-edge SWIPE (GLOSSARY). A
# STANDARD-tier extension; public header (the contract): the create() factory in
# include/unbox/ext-stage-dock/. A LEAF for now — exports no hooks/services yet.
#
# This b4 step is the skeleton + the two PURE DECISION CORES: src/reveal.hpp (the
# reversible edge-swipe recognizer) and src/dock_layout.hpp (reveal -> on-screen
# geometry), both wlroots/GL/RMLUi-free and doctest-hard. The glue
# (src/extension.cpp) is a no-op activate() for now; real wiring lands later
# (c2 static integration, d1 animation, e1 gesture).

ext_stage_dock_inc = include_directories('include')

# Glue library. Needs the kernel ABI and ext-xdg-shell's public contract (the
# manifest depends_on "xdg-shell"; c2 will consume its Service through the glue).
ext_stage_dock_lib = static_library(
  'unbox-ext-stage-dock',
  'src/extension.cpp',
  include_directories: ext_stage_dock_inc,
  dependencies: [kernel_dep, ext_xdg_shell_dep],
)

# What host-bin links against (registered at c2, orchestrator's step): the
# factory. kernel_dep rides through for the Extension ABI the factory returns.
# ext-xdg-shell stays a build-time dep of OUR lib only (factory consumers do not
# need it).
ext_stage_dock_dep = declare_dependency(
  link_with: ext_stage_dock_lib,
  include_directories: ext_stage_dock_inc,
  dependencies: [kernel_dep],
)

# Tests, asymmetric (AGENTS.md: strict cores, lenient shell). The pure decision
# cores are doctest-hard (the reveal recognizer + the dock layout geometry) with
# NO kernel/wlroots running. The TU compiles against the header-only cores
# directly and needs `src` on the include path (a unit may read its own src/).
ext_stage_dock_policy_test = executable(
  'ext-stage-dock-policy-tests',
  'tests/test_policy.cpp',
  include_directories: [ext_stage_dock_inc, include_directories('src')],
  dependencies: [doctest_dep],
)
test(
  'ext-stage-dock-policy',
  ext_stage_dock_policy_test,
  suite: 'ext-stage-dock',
)

# c2 GLUE test: install ext-xdg-shell + ext-stage-dock on the wlr headless
# backend, map a REAL in-process client toplevel, then drive the minimize/restore
# PIPELINE through the private test probe (src/probe.hpp) and assert the model +
# the scene node's hide/show enable-bit. Lenient ext-tier glue: the substrate is
# null on pixman (no GL), so this exercises the model + hide()/show(), not the
# visual. Needs CLIENT-side xdg-shell bindings generated from the canonical
# xdg-shell.xml exactly as ext-xdg-shell's test_minimize.cpp does (header +
# private-code-as-header, #included once by the single C++ TU). `src` on the
# include path so the test reaches the private probe factory (own src/ only).
wayland_client_dep = dependency('wayland-client')
sd_wayland_protocols_dir = dependency('wayland-protocols').get_variable('pkgdatadir')
sd_xdg_shell_xml = sd_wayland_protocols_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml'

sd_xdg_shell_client_header = custom_target(
  'sd-xdg-shell-client-header',
  input: sd_xdg_shell_xml,
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
# private-code emitted as a .h so the C++ TU #includes it exactly once.
sd_xdg_shell_client_code = custom_target(
  'sd-xdg-shell-client-code-impl',
  input: sd_xdg_shell_xml,
  output: 'xdg-shell-client-protocol-code.h',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

ext_stage_dock_glue_test = executable(
  'ext-stage-dock-glue-tests',
  'tests/test_glue.cpp',
  sd_xdg_shell_client_header,
  sd_xdg_shell_client_code,
  include_directories: [ext_stage_dock_inc, include_directories('src')],
  dependencies: [ext_stage_dock_dep, ext_xdg_shell_dep, wayland_client_dep, doctest_dep],
)
test(
  'ext-stage-dock-glue',
  ext_stage_dock_glue_test,
  suite: 'ext-stage-dock',
  # Real socket handshake + cooperative event-loop pump; generous timeout so a
  # slow CI box does not flake (the test fails fast on its own logic).
  timeout: 60,
)

# Aggregate alias the brief builds: `ninja -C build ext-stage-dock-tests`.
alias_target('ext-stage-dock-tests',
  ext_stage_dock_policy_test,
  ext_stage_dock_glue_test,
)
