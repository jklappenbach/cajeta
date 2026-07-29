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
   top-level tab — currently `guide`, `home`, `specification`, `stdlib` —
   and the loose top-level `.md` files are grouped as the **Guides** tab.
   The sibling `../bench/` tree, when present, is ingested as a
   **Benchmarks** pseudo-section. Add a new top-level directory to `docs/`
   and it appears as a tab on the next regen; no code changes.
   `docs/home/README.md`, if present, renders as the site homepage.
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
npm run deploy     # regen + ship to Cloudflare (see Deployment)
```

**After editing anything under `../docs/`, run `npm run regen`** (or just
`npm run build`). The manifest, tabs, cards, charts, and pages all refresh
from the tree.

## Which docs tree gets rendered

`scripts/lib/docs-root.mjs` resolves the source tree in this order:

1. `CAJETA_DOCS_ROOT` environment variable
2. a gitignored `site/.docs-root` file containing a path
3. `../docs` — this repository's tree (the default)

Overrides resolve relative to the site package root, so a sibling clone's
tree (`../../cajeta-five/docs`) works regardless of cwd. Leave both
overrides unset to build the canonical in-repo docs.

## Deployment

The site ships to Cloudflare as an **assets-only Worker** — static `dist/`,
no Worker script — the same model `cajeta-olla` uses for `olla.cajeta.dev`.
Config lives in `wrangler.jsonc`; `wrangler` is pinned in `devDependencies`
so deploys are reproducible.

```sh
wrangler login     # once per machine
npm run deploy     # regen.sh (manifest + build) then wrangler deploy
```

To validate config without publishing:

```sh
npx wrangler deploy --dry-run
```

> **The routes claim the apex.** `wrangler.jsonc` declares `cajeta.dev` and
> `www.cajeta.dev` with `"custom_domain": true`, so a deploy provisions DNS
> and TLS in the `cajeta.dev` zone and **takes over whatever currently serves
> those names**. Confirm what is live in the Cloudflare dashboard before the
> first deploy from a new machine or account.

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
