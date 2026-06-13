# ext-keybindings — config-driven compositor keybindings as a CORE extension.
# Public headers (the contract): include/unbox/ext-keybindings/
#   ext_keybindings.hpp — the extension factory (the WHOLE cross-extension
#   surface; this is a LEAF consumer — it exports no hooks/services).
#
# Decision cores live in src/ and are wlroots/GL-free: the combo parser
# (xkbcommon only), the toml loader (toml++), the matcher + tap state machine,
# and the focus ring. The glue (src/extension.cpp) is the thin effectful edge
# that binds the kernel key_filter, the ext-xdg-shell toplevel events, fork/exec,
# and wl_display_terminate.

ext_keybindings_inc = include_directories('include')

# toml++ (header-only): an APPROVED dep (notes/plan.md §2), first use here as a
# Meson wrap kept private to THIS unit (subprojects/tomlplusplus.wrap).
tomlplusplus_dep = dependency('tomlplusplus')

# Glue library. Needs the kernel ABI, ext-xdg-shell's public contract (the glue
# includes its header to consume the Service + Toplevel), xkbcommon (the combo
# parser resolves keysym names), and toml++ (the config loader).
ext_keybindings_lib = static_library(
  'unbox-ext-keybindings',
  'src/extension.cpp',
  'src/config.cpp',
  include_directories: ext_keybindings_inc,
  dependencies: [kernel_dep, ext_xdg_shell_dep, xkbcommon_dep, tomlplusplus_dep],
)

# What host-bin links against: the factory. kernel_dep rides through for the
# Extension ABI the factory returns. ext-xdg-shell stays a build-time dep of OUR
# lib only (consumers of the factory do not need it).
ext_keybindings_dep = declare_dependency(
  link_with: ext_keybindings_lib,
  include_directories: ext_keybindings_inc,
  dependencies: [kernel_dep],
)

# Tests, asymmetric: the pure decision cores doctest-hard (combo parser, toml
# loader, matcher + tap SM, focus ring) with NO kernel/wlroots running, plus a
# lenient headless glue smoke (install + activate + dispatch + clean shutdown).
# The pure-core TU compiles the core sources directly and needs `src` on the
# include path (a unit may read its own src/) plus xkbcommon + toml++.
ext_keybindings_policy_test = executable(
  'ext-keybindings-policy-tests',
  'tests/test_policy.cpp',
  'src/config.cpp',
  include_directories: [ext_keybindings_inc, include_directories('src')],
  dependencies: [doctest_dep, xkbcommon_dep, tomlplusplus_dep],
)
test(
  'ext-keybindings-policy',
  ext_keybindings_policy_test,
  suite: 'ext-keybindings',
)

# Glue test: install + activate + dispatch + shutdown on the wlr headless
# backend, with ext-xdg-shell present (keybindings depends on it). Lenient.
ext_keybindings_glue_test = executable(
  'ext-keybindings-glue-tests',
  'tests/test_glue.cpp',
  dependencies: [ext_keybindings_dep, ext_xdg_shell_dep, doctest_dep],
)
test(
  'ext-keybindings-glue',
  ext_keybindings_glue_test,
  suite: 'ext-keybindings',
)

# Aggregate alias the brief builds: `ninja -C build ext-keybindings-tests`.
alias_target('ext-keybindings-tests',
  ext_keybindings_policy_test,
  ext_keybindings_glue_test,
)
