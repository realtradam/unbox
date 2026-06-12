# Extension-agent supplement (units named ext-*)

Read AFTER `.unbox/package-agent.md` — everything there applies, plus:

## Shape
- Export a **manifest**: id, tier (`core` | `standard`), `dependsOn`
  (extension ids, resolved topologically by the host).
- `activate(Host&)` is your ONLY entry point. Everything you touch arrives
  through the typed Host API — no globals, no reaching into the kernel's
  internals, no other extension's headers except its public contract.
- Contribute capabilities as TYPED exported symbols in your public header
  (hook descriptors, service handles) so consumers link against them — a
  missing dependency must be a compile/link error, never a runtime lookup.

## Tier rules
- Depend on the kernel + lower tiers only. Never sideways into another
  unit's privates, never upward.
- Deactivation = your RAII members being destroyed, in reverse declaration
  order. No manual teardown lists; if teardown needs choreography, your
  ownership graph is wrong.
- An exception escaping your hook callback disables YOUR extension, not the
  session (the bus catches at the boundary). That isolation is a safety
  net, not a feature — treat every trip as a bug.

## UI contributions
- UI = RML document(s) + RCSS under `assets/<unit>/` + **data bindings**
  registered through the ui substrate service. No GL calls, no direct
  RMLUi context access — the substrate owns rendering and scheduling.
- Honor **touch-mode**: hit targets scale via the substrate's theme
  variables. Never hardcode pointer-sized targets; this DE must be equally
  usable by finger, keyboard, and mouse.

## Pure-core reminder
Your policy logic (layout math, what-to-show decisions, gesture
recognition) is a pure function library inside your unit, doctest-covered
without the kernel present. Glue calls it; it never calls glue.
