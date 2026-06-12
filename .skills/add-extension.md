Use when creating any new extension unit (ext-*) from scratch.
---
# /add-extension — the full checklist

1. **Overlap check.** `GLOSSARY.md` + existing public headers: does this
   concept already exist under another name? If yes, extend that unit
   instead (and that is a USER decision either way).
2. **User decisions confirmed:** tier (core/standard), unit boundary, and
   the unit's name (glossary-clean).
3. **Scaffold:**
   ```
   packages/ext-<name>/
   ├── include/unbox/ext-<name>/   # the contract — write this FIRST
   ├── src/                        # impl + private headers
   ├── tests/                      # doctest suite
   ├── meson.build
   └── ext-<name>.md               # written LAST (step 9)
   ```
4. **Contract first.** Public header: manifest, exported hook descriptors /
   service handles, lifetime semantics doc-commented. It must compile
   against the kernel ABI alone before any implementation exists.
5. **Registration — the trap, all four** (see .unbox/rules/unit-registration.md):
   unit `meson.build` · root `subdir()` · host-bin `main.cpp` registration ·
   test suite wired in.
6. **Pure core** in `src/` with its doctest suite. Glue LAST.
7. **UI (if any):** RML + RCSS under `assets/ext-<name>/`, data bindings
   via the ui substrate service, touch-mode variables honored.
8. **Verify:** `ninja -C build ext-<name>` · `meson test -C build --suite
   ext-<name>` · smoke-test via /nested-run.
9. **Package doc:** `ext-<name>.md`, ~20–30 lines — why it exists, gotchas
   the header can't express, side-effect graph (what it emits/filters).
10. **Report** to `reports/ext-<name>.md`; orchestrator updates `tasks.md`.
