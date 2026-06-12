# unit-registration
Meson does not auto-discover. A new unit exists only when ALL FOUR are done:
its `packages/<unit>/meson.build` · `subdir()` in the root `meson.build` ·
registration in host-bin's `main.cpp` (extensions only) · its test suite
wired into `meson test`. Miss one and it silently doesn't exist. Check all
four, every time.
