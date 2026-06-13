Use when a feature works headless/nested but fails ONLY on the real DRM seat (input, spawning, outputs) — i.e. under the bare-TTY `~/start-unbox.sh`.
---
# /diagnose-real-seat — real-seat-only bug checklist

1. Reproduce headless first: `WLR_BACKENDS=headless ./build/packages/host-bin/unbox`.
   Works headless but not on the seat → seat/DRM/spawn-specific, not logic.
2. Capture logs without leaving the seat: `~/start-unbox.sh --debug [-s foot]`
   writes /tmp/unbox.log and prints the diagnostic lines on exit.
3. What a key REALLY emits: `~/capture-keys.sh` (evtest over ALL input devices)
   → /tmp/keycap.log. (Both CF-AX3 Super keys = KEY_LEFTMETA → Super_L 0xffeb.)
4. What clients RECEIVE (globals/outputs): `~/dump-outputs.sh` runs `~/wloutdump`
   inside unbox → /tmp/wlout.txt (wayland-info/weston-info are NOT installed).
   Compare the real output against the headless baseline.
5. A live process's EFFECTIVE env: /proc/<pid>/environ does NOT reflect runtime
   setenv(). Read it by launching the binary as gdb's OWN child (ptrace_scope=1
   allows tracing your child): `gdb --batch -ex 'break wl_display_run' -ex run
   -ex 'call (char*)getenv("WAYLAND_DISPLAY")' -ex kill ./build/.../unbox`.
6. Spawned client reached the WRONG compositor → check WAYLAND_DISPLAY in the
   spawn env (.unbox/rules/spawn-env.md); libwayland defaults to wayland-0 (parent).
7. Escape a stuck session: Ctrl+Alt+Backspace (VT-switch isn't implemented yet);
   from another VT `pkill -x unbox` (never `-f` — it kills your own shell).
