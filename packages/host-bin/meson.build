# host-bin — the composition root (orchestrator-owned). The ONLY place that
# names every extension; activates them in dependency order (from slice 4).

executable(
  'unbox',
  'src/main.cpp',
  dependencies: [kernel_dep, ext_xdg_shell_dep, ext_layer_shell_dep, ext_keybindings_dep, ext_stage_dock_dep],
)
