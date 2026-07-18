# Cajeta documentation site

The public website for the Cajeta language. Built with **Astro 5** and
**React islands**, it renders the canonical `../docs/` tree **in place** —
no markdown is copied into this project, so the published site can never
drift from the in-repo documentation.

## How the site derives from `docs/`

1. `scripts/build-manifest.mjs` scans `../docs/**/*.md` and writes
   `src/data/manifest.json`: per-doc title (first heading), description (first
   prose paragraph), section, URL, word count; per-section stats. It runs
   automatically before `dev` and `build`.
2. **Tabs mirror the tree**: every top-level child of `docs/` becomes a
   top-level tab — each subdirectory (`buildtool`, `cajeta`, `gpu`,
   `specification`, `specs`, `history`) is a tab, and the loose top-level
   `.md` files are grouped as the **Guides** tab. Add a new top-level
   directory to `docs/` and it appears as a tab on the next regen; no code
   changes.
3. An Astro content collection (glob loader, `src/content.config.ts`) reads
   the same files for rendering. Slugs are shared with the manifest via
   `scripts/lib/extract.mjs`, so cards always link to real pages
   (`docs/specification/lang/MemoryModel.md` → `/specification/lang/memorymodel/`).
4. Relative `*.md` links inside doc markdown are rewritten to site URLs at
   build time (`src/lib/rehype-docs-links.mjs`).

## Commands

```sh
npm install
npm run dev        # rescan docs/ + dev server on localhost:4321
npm run build      # rescan docs/ + static build to dist/
npm run regen      # same as build, installs deps if missing (scripts/regen.sh)
npm test           # unit tests for the extraction/slug/link helpers
```

**After editing anything under `../docs/`, run `npm run regen`** (or just
`npm run build`). The manifest, tabs, cards, charts, and pages all refresh
from the tree.

## Design notes

- **Typography**: Fraunces (display), Source Serif 4 (body), Inter (UI),
  JetBrains Mono (code) — all self-hosted variable fonts. Doc pages are
  typeset editorially: drop capital on the opening paragraph, ruled tables,
  set-off quotes.
- **Cajeta highlighting**: `src/grammars/cajeta.tmLanguage.json` is a custom
  TextMate grammar fed to Shiki (dual `vitesse-light`/`vitesse-dark` themes,
  switched by the site theme).
- **React islands** hydrate selectively: theme toggle (`client:load`),
  doc filter + TOC scrollspy (`client:idle`), corpus charts
  (`client:visible`). Everything else is static HTML.
- Chart colors are validated (lightness band, chroma, contrast) for both
  themes; the mark color lives in `--chart-mark` in `src/styles/global.css`.

## Layout

```
scripts/build-manifest.mjs   docs/ scanner -> src/data/manifest.json
scripts/lib/extract.mjs      pure helpers (titles, slugs, links) + tests
src/content.config.ts        content collection over ../docs
src/pages/index.astro        landing: hero, filter, starter cards, charts
src/pages/[section]/         one index per docs/ top-level child
src/pages/[...slug].astro    one page per markdown file
src/lib/rehype-*.mjs         md link rewriting + editorial HTML touches
```
