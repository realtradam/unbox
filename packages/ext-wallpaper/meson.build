# ext-wallpaper — config-driven desktop background (GLOSSARY: "wallpaper").
# Standard-tier extension. Composites a solid colour + optional image file at
# SceneLayer::background (below everything else). Image path, fit mode, and
# background colour are driven by the [wallpaper] section of unbox.toml;
# hot-reloaded via Host::watch_file by DROP + RECREATE of the ui surface.
# input_transparent = true always (never steals clicks from windows above).
# Depends only on the kernel (Host::ui(), output events, watch_file).

ext_wallpaper_inc = include_directories('include')

# toml++: the APPROVED config dep (notes/plan.md §2), a project-global Meson wrap
# (subprojects/tomlplusplus.wrap). Force default_library=static so the installed
# `unbox` binary bakes it in (the wrap's own meson.build hardcodes shared, which
# would need libtomlplusplus.so at runtime). Same rationale as ext-keybindings.
tomlplusplus_dep = dependency('tomlplusplus',
                              default_options: ['default_library=static'])

# Glue library: needs the kernel ABI (ui substrate, output events, watch_file)
# and toml++ (the [wallpaper] config loader, src/config.cpp). The config parse
# is a PURE core in src/config.{hpp,cpp} (toml++ only; doctest-covered).
ext_wallpaper_lib = static_library(
  'unbox-ext-wallpaper',
  'src/extension.cpp',
  'src/config.cpp',
  include_directories: ext_wallpaper_inc,
  dependencies: [kernel_dep, tomlplusplus_dep],
)

# What host-bin links against (the factory). kernel_dep rides through for the
# Extension ABI the factory returns.
ext_wallpaper_dep = declare_dependency(
  link_with: ext_wallpaper_lib,
  include_directories: ext_wallpaper_inc,
  dependencies: [kernel_dep],
)

# Tests, asymmetric (AGENTS.md: strict cores, lenient shell).
#
# CONFIG core test: the pure [wallpaper] toml parser (defaults, every fit
# value, unknown fit -> cover, missing table, missing/empty path, bad color ->
# default). No kernel/wlroots; config.cpp compiles directly into the test TU.
ext_wallpaper_config_test = executable(
  'ext-wallpaper-config-tests',
  'tests/test_config.cpp',
  'src/config.cpp',
  include_directories: [ext_wallpaper_inc, include_directories('src')],
  dependencies: [doctest_dep, tomlplusplus_dep],
)
test(
  'ext-wallpaper-config',
  ext_wallpaper_config_test,
  suite: 'ext-wallpaper',
)

# SMOKE test: the factory yields the wallpaper extension with the right manifest.
# Cheap, no kernel/wlroots running.
ext_wallpaper_smoke_test = executable(
  'ext-wallpaper-smoke-tests',
  'tests/test_wallpaper.cpp',
  include_directories: [ext_wallpaper_inc, include_directories('src')],
  dependencies: [ext_wallpaper_dep, doctest_dep],
)
test(
  'ext-wallpaper-smoke',
  ext_wallpaper_smoke_test,
  suite: 'ext-wallpaper',
)

# Aggregate alias: `ninja -C build ext-wallpaper-tests`.
alias_target('ext-wallpaper-tests',
  ext_wallpaper_config_test,
  ext_wallpaper_smoke_test,
)
