# Glossary — canonical vocabulary

> One name per concept. Never invent a synonym. New term? The orchestrator
> proposes the standard/training-baked name (wlroots' own vocabulary
> preferred) and the user confirms before it lands here. "Aliases to avoid"
> maps wrong names back to the canonical one.

## Architecture

| Term | Meaning | Aliases to avoid |
|---|---|---|
| **kernel** | The minimal runtime: contracts (the ABI), extension host, event/hook/service bus, backend/output/seat/scene glue, and the UI substrate. Names no concrete feature. NOT an extension. | engine, core (when meaning the kernel) |
| **core** (tier) | The extension tier required for a usable session: xdg-shell, layer-shell, output-config, keybindings. | — |
| **standard** (tier) | The on-by-default feature tier: taskbar, launcher, OSK, tiling, … | — |
| **extension** | An in-process unit contributing capabilities via the Host API: hooks, services, ui surfaces, protocol glue. | plugin, module, addon |
| **unit** | One package under `packages/` with exactly one owner-agent. | component |
| **contract** | A unit's public headers (`packages/<unit>/include/unbox/<unit>/`) — the ONLY cross-unit surface. | API, interface (when meaning the whole surface) |
| **manifest** | An extension's declaration: id, tier, dependsOn. | — |
| **Host API** | The typed object an extension receives in `activate(host)`. | host context |
| **hook** | A typed extension point. **event** = fire-and-forget, N listeners, error-isolated. **filter** = ordered value-in→value-out chain. | callback, signal (when meaning our bus) |
| **service** | A single-responder request/response capability fetched via a typed handle. NOT a hook. | — |
| **subscription handle** | The RAII object returned by every hook subscription; destruction unsubscribes. | listener token |
| **composition root** | `packages/host-bin/` — the only place that names every extension. Owned by the orchestrator. | bootstrapper |

## Compositor domain (wlroots names are law)

| Term | Meaning | Aliases to avoid |
|---|---|---|
| **surface** | A `wl_surface`: the protocol-level pixel container. | window (when meaning wl_surface) |
| **toplevel** | An `xdg_toplevel` — an application window managed by the compositor. | window, view, app window |
| **layer surface** | A wlr-layer-shell surface from an EXTERNAL client, anchored to an output layer. | panel surface, overlay (when meaning the protocol object) |
| **output** | A display device (`wlr_output`). | monitor, display, screen |
| **seat** | The input-device collection owning keyboard/pointer/touch focus (`wlr_seat`). | — |
| **scene** | The `wlr_scene` retained-mode node tree; provides damage tracking. | render graph, scene tree (write "scene") |
| **damage** | The output region needing redraw this frame. | dirty region |
| **scene layer** | An ordered z-band of the scene tree (`background < bottom < normal < top < overlay`; wlr-layer-shell names + `normal` for toplevels). Extensions attach nodes per band, never fighting over stacking order. | z-layer, stacking layer, shell layer |
| **listener** | A `wl_listener`. Lives inside one unit's glue only; RAII-wrapped at every boundary. | — |
| **workspace** | A virtual-desktop grouping of toplevels. | desktop, tag |
| **nested session** | unbox running as a window inside the live labwc session (the dev mode). | embedded mode |

## UI substrate

| Term | Meaning | Aliases to avoid |
|---|---|---|
| **ui substrate** | The kernel subsystem owning RMLUi: contexts, render-to-scene bridge, input routing, theme variables. | shell renderer, ui engine |
| **ui surface** | One RMLUi document an extension contributes, composited as a scene node. | shell surface, overlay, RML window, panel (when meaning the object) |
| **data binding** | RMLUi's model↔document binding; the ONLY way extension state reaches RML. | — |
| **touch-mode** | The substrate state signalling finger input (auto-flipped, debounced). NO automatic visual scaling (user decision, slice 5) — extensions may adapt affordances via the change notification (spacing, invisible hit zones, OSK auto-show). | tablet mode |

## Workflow

| Term | Meaning | Aliases to avoid |
|---|---|---|
| **orchestrator** | The main agent: plans, summons, verifies; owns host-bin + harness md. Reads only public headers (the READ RULE). | — |
| **owner-agent** | The single agent assigned to one unit; edits only that unit. | subagent (alone — say which) |
| **wave** | Disjoint units summoned in parallel in one message. | batch |
| **brief** | `prompts/<unit>.md` — the task given to an owner-agent. | prompt file |
| **report** | `reports/<unit>.md` — the owner-agent's writeup. | — |
| **spike** | A slice whose only goal is de-risking one unknown. | POC, prototype (when meaning a spike) |

## Known vocabulary drift

- _None yet._ Add rows here the moment drift is caught, and keep the bad
  name in "Aliases to avoid" forever so it is never reintroduced.
