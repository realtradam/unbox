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

# Tests: the pure decision cores doctest-hard (the reveal recognizer + the dock
# layout geometry) with NO kernel/wlroots running. The TU compiles against the
# header-only cores directly and needs `src` on the include path (a unit may read
# its own src/). No glue/headless test this step — activate() is a no-op.
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

# Aggregate alias the brief builds: `ninja -C build ext-stage-dock-tests`.
alias_target('ext-stage-dock-tests',
  ext_stage_dock_policy_test,
)
