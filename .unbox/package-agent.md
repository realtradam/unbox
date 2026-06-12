# Owner-agent brief (every unit)

You are the SINGLE owner of `packages/<unit>/` — its `include/`, `src/`,
`tests/`, `meson.build`, and `<unit>.md`. Nobody else edits these files;
you edit nothing outside them.

## Visibility
- You MAY read: your unit; the PUBLIC headers of your dependencies
  (`packages/*/include/**`); the harness md files; `GLOSSARY.md`; assets.
- You may NOT read other units' `src/`. Needing to means their contract is
  incomplete — record the gap in your report instead of peeking.

## Contract discipline
- Your public headers ARE your contract: minimal, documented,
  lifetime-explicit. `unique_ptr` = ownership transfer; raw pointer/ref =
  borrow valid only for the call, never stored; subscriptions return RAII
  handles.
- Doc-comment every public symbol: ownership + calling context. Everything
  runs on the single `wl_event_loop` thread unless explicitly stated.
- No templates in public headers unless genuinely necessary (they drag
  implementation into the contract and bloat compile times).
- Changing a header YOU own: fine, note it in the report. Needing a change
  in one you DON'T own: a change-request in your report — never edit it,
  never work around it with casts or string lookups.

## Code discipline
- Pure decision core (no wlroots/GL/RMLUi types) + thin glue at the edge.
  doctest the core hard; zero mocks of `unbox::` modules — if you want to
  mock one, the effect should have been injected: fix the design.
- Respect the budget machine: forward-declare, pimpl concrete cross-
  boundary classes, keep includes lean. Build YOUR target, not the world.
- Vocabulary: `GLOSSARY.md` is law. A concept that needs a new name goes in
  the report for user sign-off — never coin silently.

## Done means
`ninja -C build <unit-target>` clean · `meson test -C build --suite <unit>`
green · `reports/<unit>.md` written (what you built, the public surface,
test output, contract gaps / change-requests). Reply to the orchestrator
with ONE line + the report path. No diffs, no logs.
