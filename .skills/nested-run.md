Use to smoke-test unbox visually without leaving the live labwc session.
---
# /nested-run — nested smoke-test checklist

1. **Build** the needed targets: `ninja -C build` (or unit target + host-bin).
2. **Launch nested:** `./build/packages/host-bin/unbox` — wlroots
   auto-detects the parent Wayland session (labwc) and opens unbox as a
   window. NEVER test on a bare TTY during development; the nested window
   is the sandbox.
3. **Drive it:** note the `WAYLAND_DISPLAY` socket unbox prints, then
   `WAYLAND_DISPLAY=<that socket> foot` to open a client inside it.
4. **Verify the ONE thing you came to verify**, then exit. Visual checks:
   a single targeted `grim` screenshot of the nested output, read it, done.
   Screenshots are token-expensive — never browse around with captures.
5. **Crashes:** reproduce under `build-asan/` FIRST and read the sanitizer
   trace before reading any source (see ORCHESTRATOR.md §4).
6. **Touch caveat:** nested touch fidelity depends on what labwc forwards.
   Final touch/gesture validation only counts on the real seat (slice 9+,
   s6 service on seat0).
