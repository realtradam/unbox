Use when a unit needs a Wayland protocol whose codegen is not yet wired into this repo.
---
# /add-protocol — wayland-scanner codegen checklist

1. **Source the XML.** Prefer the system `wayland-protocols` package
   (`pkg-config --variable=pkgdatadir wayland-protocols`). Vendor XML into
   `protocol/` ONLY for protocols not shipped there (e.g. wlr-protocols
   extras like wlr-layer-shell).
2. **Codegen in Meson.** Add the `wayland-scanner` `custom_target` pair —
   `private-code` + `server-header` — following the existing pattern in the
   kernel's `meson.build`. One pair per protocol, named consistently.
3. **Containment.** Generated headers are C: include them ONLY inside the
   owning unit's glue, wrapped the same way as wlroots includes (see
   `.unbox/rules/wlroots-include.md`). Protocol types NEVER cross a unit
   boundary — expose the capability through a typed contract in the owning
   unit's public header instead.
4. **Prove ordering.** One clean full build (`rm -rf build && meson setup
   build && ninja -C build`) to verify codegen dependency ordering, then
   back to per-unit targets.
5. **Glossary.** If the protocol introduces a user-visible concept, its
   canonical name goes through the glossary flow (user sign-off).
