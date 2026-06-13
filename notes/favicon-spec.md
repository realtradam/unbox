# Feature spec — dock favicons (app icons on preview cards)

> STATUS: **SPEC ONLY — not scheduled, no code yet.** Written so it can be picked
> up cold. Vocabulary is canonical: **favicon** = "the application icon shown on a
> preview, resolved from the toplevel's `app_id` via the XDG icon theme"
> (GLOSSARY:67). No new terms needed.

## Goal
Each minimized window's dock preview card shows its application icon (favicon),
resolved from the toplevel `app_id` through the freedesktop XDG icon theme. Today
a card shows preview + title; the b2 list-binding design already reserves a
per-row `favicon` string field — it is just unwired.

## Architecture (mechanism/policy split, per the constitution)
Three disjoint pieces across two units:

1. **Pure lookup core — ext-stage-dock (or a shared pure core).** Input
   `(app_id, size, scale, theme, [inherited…])` → ordered candidate file paths,
   per the freedesktop Icon Theme Specification. Pure input→output, doctest-hard,
   ZERO I/O: the parsed theme index + a "does this file exist" predicate are
   INJECTED, so it tests against a fake filesystem. Real `index.theme` parsing and
   `stat`s live in a thin edge adapter.

2. **Decode + texture load — KERNEL / ui substrate (the GATING work).** The
   substrate's RmlUi render interface must turn an icon FILE PATH into a sampled
   GL texture. Today it only registers the custom `unbox-preview://N` texture; it
   (VERIFY) does not implement RmlUi `LoadTexture` for on-disk images. Add a
   decoder feeding RmlUi's texture loader so an `<img>` referencing an icon file
   resolves. Needs PNG **and** SVG (see formats below).

3. **Dock wiring — ext-stage-dock.** Per slot: resolve `app_id` → favicon path
   (piece 1) → bind the favicon `<img src>` via the existing b2
   `bind_list_string("slots","favicon", …)`. Show a generic fallback icon on miss.

## Dependencies to approve (AGENTS.md: no new deps without sign-off)
- **Themes (data, NOT a build dep):** `hicolor-icon-theme` + `adwaita-icon-theme`
  are already installed and sufficient. Optional for coverage: `papirus-icon-theme`
  (extra). No build wiring.
- **Decoders (the real new deps — vendored Meson subprojects):**
  - **stb_image** — PNG (single header, trivial). App icons in hicolor are
    commonly PNG (`foot.png`, `firefox.png` present).
  - **lunasvg** — SVG. NON-OPTIONAL: modern themes are SVG-first (adwaita ships
    **715 .svg vs 51 .png**; papirus/breeze all SVG), and the generic fallbacks
    are SVG. lunasvg is light, vendorable, and is RmlUi's own blessed SVG plugin
    backend. **Reject `librsvg`** (heavy GNOME/Rust dep).

## New contract surface (sketch — finalize when built)
- **Kernel `ui.hpp`:** prefer the minimal route — once `LoadTexture` decodes
  `file://` image paths, `<img src="file:///usr/share/icons/…/foot.svg">` just
  works with NO new public API. Only add a typed
  `UiSubstrate::create_icon(path,size) -> source_uri` (mirroring `create_preview`)
  if texture caching/lifetime forces kernel ownership. Decide at build time.
- **ext-stage-dock:** no public-header change — favicon is internal policy; the
  b2 `favicon` list field already exists in the contract.

## Lookup core detail (freedesktop Icon Theme Spec, minimal)
- Resolve order: requested theme's size-matched dirs → inherited themes → `hicolor`
  → `/usr/share/pixmaps/<app_id>.{png,svg,xpm}` → none.
- `app_id` normalization rules (pure): as-is; lowercased; reverse-DNS reduced to
  last component (`org.foo.Bar` → `bar`); known aliases. Try in order.
- Size selection: exact size dir, else `scalable`, else nearest. Prefer full-color
  over `-symbolic` unless only symbolic exists.

## Image-format reality (measured on this box)
- hicolor app icons: mostly PNG, some SVG. adwaita/papirus/breeze: SVG-first.
- ⇒ both decoders required; SVG is the one that can't be skipped.

## Caching / lifetime
Favicons are few and immutable per app (unlike refreshable per-window previews).
Decode on first use, cache by `(path,size)` for the session. Owner (substrate vs
dock) decided when built — leans substrate if route (a) above is taken.

## Test plan
- **Lookup core (doctest, fake fs, no I/O):** exact hit; reverse-DNS last-segment;
  size 48 vs scalable; theme→inherited→hicolor fallback; pixmaps fallback; miss→none.
- **Substrate decode (headless+gles2):** load a known PNG and a known SVG → sampled
  texture; position-aware known-color pixel readback (mirror the a1 preview color
  test); decode failure degrades to none, never throws.
- **Dock glue:** resolvable `app_id` binds a non-empty favicon src; unresolvable
  binds the fallback.

## Implementation order (when scheduled — two briefs, kernel FIRST)
1. **KERNEL:** vendor lunasvg + stb_image (Meson wraps), wire RmlUi `LoadTexture`
   to decode PNG/SVG, cache, headless decode test, `ui.hpp` doc. (Gating.)
2. **ext-stage-dock:** pure lookup core + edge adapter; resolve per slot; bind the
   favicon `<img>`; fallback icon; tests. Config (theme name + size) via
   `unbox.toml` — coordinate with the ext-keybindings config pattern or a
   `[dock]` block.

## Open questions
- Theme selection: hardcoded default vs `unbox.toml [dock] icon_theme = "…"`
  (lean: config with a sane default — adwaita/hicolor).
- Does RmlUi 6.2 `<img>` accept `file://` once `LoadTexture` is wired, or do we
  need a custom `unbox-icon://` scheme like the preview? Verify against the
  vendored source.
- SVG rasterization size: render at the card's favicon box (dp==px at 1.0 ratio);
  re-decode on the (rare) size change.
