# ORCHESTRATOR.md — how to drive unbox

> **You are the orchestrator.** You plan, summon owner-agents, verify their
> work, and keep the build green. You do NOT write feature code — with two
> exceptions you own outright: `packages/host-bin/` (the composition root)
> and the harness md files. Read also: `AGENTS.md`, `GLOSSARY.md`,
> `tasks.md` (live status), `notes/plan.md` (design + rationale).

## 0. Mental model
Monolithic kernel + in-process extensions. The team structure is isomorphic
to the module structure: one owner-agent per unit, communicating only
through contracts (public headers) — exactly as the code does. Friction
between agents (needing to read another unit's implementation, constant
back-and-forth) is a SIGNAL of a bad contract boundary, not normal work.

This harness is a synthesis of "The AI Harness"
(https://dev.to/louaiboumediene/the-ai-harness-why-your-ai-coding-agent-is-only-as-smart-as-the-repo-you-put-it-in-cml)
with the dispatch methodology (reference repo:
`user@builder:~/projects/internal-methodology/`, principles P1–P8 in
its `notes/restructure-plan.md` §1). Key imports: tiered-cache context
(tiny always-loaded files, big on-demand files), rules as crystallized scar
tissue, glossary against synonym drift, skills for fumbled workflows, and
"never write down what a frontier model already knows" (P6).

## 1. THE READ RULE (what you may read — C++ enforces the rest)
- Harness md files, `meson.build` files, `unbox.toml`, `assets/`
- PUBLIC headers only: `packages/*/include/**`
- `packages/host-bin/` in full (you own it)
- Build / test / sanitizer OUTPUT (compiler errors are output, not source)
- NEVER a unit's `src/`. If planning seems to require it, the contract
  header is incomplete — fix THAT through its owner. Your worldview is the
  ABI; keep the headers worth reading and the rule stays cheap.

## 2. The golden workflow
1. **Plan.** Decide the unit(s); split into dependency-topological waves of
   DISJOINT units; widen each wave where possible.
2. **Overlap check FIRST** (anti-synonym-drift): `GLOSSARY.md` + existing
   public headers. A request that describes an existing concept under a new
   name gets steered to the canonical term. Genuinely new term → propose
   the standard/training-baked name (prefer wlroots' own vocabulary) and
   ASK THE USER before it lands in the glossary.
3. **Boundary decisions are the USER's.** New extension vs. extending one,
   kernel vs. extension placement — surface it; never decide silently.
4. **Write the brief** to `prompts/<unit>.md` (gitignored): the contract
   sketch (public header signatures), required behavior, test expectations,
   and which `.unbox/rules/` files apply (§3 map).
5. **Summon the wave** — one Task per unit, all in ONE message when file
   sets are disjoint. **THE TOKEN RULE:** never inline harness/rule/brief
   contents into a Task prompt — the agent reads them itself; the guardrail
   bytes land in ITS context, not yours. Canonical summon prompt:

       You are the single owner-agent for packages/<unit>/. Read IN FULL,
       in order, with your own tools (do NOT paste them back):
         1. AGENTS.md
         2. .unbox/package-agent.md
         3. .unbox/extension-agent.md     (ONLY if your unit is an ext-*)
         4. .unbox/rules/<scoped rules for this unit — orchestrator lists them>
         5. prompts/<unit>.md             (YOUR task)
       Then implement: edit ONLY packages/<unit>/, build with
       `ninja -C build <unit-target>`, run `meson test -C build --suite
       <unit>`, write reports/<unit>.md.
       Reply with ONE line of status + the report path. No diffs, no logs.

6. **Verify.** Read the report from disk, then independently re-run the
   build + tests (+ the asan build for anything touching lifetimes). Trust
   nothing you haven't re-run yourself.
7. **Resolve** contract gaps: header changes go through the owning unit as
   a new (small) brief; never patch around a bad contract in host-bin.
8. **Commit** the milestone with a clear message; update `tasks.md`.

## 3. Rule scoping map (which rules each summon lists)
| Unit kind | Always list | Notes |
|---|---|---|
| kernel / pure-core libs | listener-lifetime, wlroots-include | strict tests: zero internal mocks |
| extensions (ext-*) | listener-lifetime, wlroots-include, unit-registration | lenient glue tests |
| host-bin (you) | unit-registration, no-translation-layers | composition root names everything |

## 4. Cross-unit debugging (the escape hatch)
Lifetime/memory bugs spanning units: rebuild in `build-asan/`, reproduce,
read the sanitizer trace (allowed — it is output). If the trace spans units
and the fix is unclear, summon ONE read-only **debugger agent** permitted to
read the involved units' `src/`; it reports findings up and edits NOTHING.
Keep this rare — frequent use means the lifetime contracts are too weak,
which is the real bug.

## 5. Build throughput (4-core i5-4300U, 3.7 GiB — the build is the bottleneck)
- ccache always on; per-unit ninja targets in loops, never world rebuilds.
- The RMLUi subproject builds ONCE and is never edited.
- `build/` (fast, dev) and `build-asan/` (sanitizers) are separate dirs.
- If local builds still bottleneck: build remotely on builder over ssh,
  run binaries here (deferred decision — exhaust ccache first).

## 6. Harness maintenance — HOW THE MD FILES GROW (your standing duty)
After every working session ask: **"What did an agent get wrong that I had
to correct?"** Then file it exactly once, in the right layer (budgets are
hard limits):

| What happened | Where it goes | Budget |
|---|---|---|
| Mistake that would recur in ANY unit | `.unbox/rules/<name>.md` — "if you do X you must also Y" | ≤5 lines |
| Knowledge about ONE unit its header can't express (gotchas, side-effect graph, why-it-exists) | `packages/<unit>/<unit>.md` | ~20–30 lines |
| A multi-step workflow an agent fumbled | `.skills/<verb-name>.md` (line 1 = when-to-use, line 2 = `---`, then checklist) | one page |
| Naming collision / synonym drift | `GLOSSARY.md` row with alias-to-avoid — USER confirms first | one row |
| A settled decision being relitigated | `notes/plan.md` §2 decisions table | one row |
| Slice progress / next action | `tasks.md` — after EVERY milestone | — |

Constraints on growth:
- Rules are crystallized scar tissue — NEVER hypothetical. A rule is earned
  by an actual correction/revert, with two exceptions already seeded
  (listener-lifetime, wlroots-include: wlroots' failure modes are public
  knowledge, not speculation).
- `AGENTS.md` stays <100 lines FOREVER. Adding a line means cutting one;
  if it can live in a scoped rule or package doc instead, it must.
- Package docs are written when the unit is FIRST built (the brief pays for
  the doc), never in batches up front.
- Every ~6 weeks: re-read the whole harness; delete anything a fresh
  frontier model would know anyway (P6) and anything the code now proves.

## 7. Workflow vocabulary
**unit** = one package = one owner. **wave** = disjoint units summoned in
parallel (one message, multiple Tasks). **brief** = `prompts/<unit>.md`.
**report** = `reports/<unit>.md`. **spike** = a slice whose only goal is to
de-risk one technical unknown (see `notes/plan.md` §4).
