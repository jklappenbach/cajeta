# cajeta-site — public documentation website (spec)

## 1. Definition

### 1.1 Purpose
Replace the stock AstroWind template in `site/` with a purpose-built Astro
website that presents the canonical `docs/` tree as a styled, readable public
documentation site. The site renders the existing markdown — it never copies or
forks it — so published docs cannot drift from the in-repo source.

### 1.2 Problem
`docs/` holds 200+ markdown files of high-value material (guides, stdlib
specifications, GPU notes, work specs) with no presentable rendering. The
current `site/` directory is an unmodified third-party template with demo
content and a Node ≥24 requirement the dev machine does not meet.

### 1.3 Scope
- A static Astro 5 site in `site/`, with React islands for interactive pieces.
- Editorial typography: HTML-reformatted doc pages (not raw markdown dumps),
  drop capitals, a curated font set, styled tables/quotes/code, charts.
- Navigation mirrors `docs/`: its top-level children form the top-level tabs.
- A regeneration script that rescans `docs/` and rebuilds the site.

### 1.4 Constraints
- Node 22 on the build machine → Astro 5.x, not 6.x.
- Source of truth stays in `docs/`; the site reads it in place.
- `docs/` markdown has no frontmatter; titles/descriptions must be derived.
- Static output; no server runtime required to host.

### 1.5 Non-goals
- No doc content rewriting or restructuring of `docs/` itself.
- No versioned docs, i18n, CMS, or hosted search service.
- No changes to `cajeta-docs-site/` (the prior site) in this work.

## 2. Structure-mirroring navigation

The site derives its information architecture from the `docs/` tree at build
time; nothing structural is hand-maintained.

- 2.1 As a visitor, when I open the home page, then I see one top-level tab per
  top-level child of `docs/`: each subdirectory (`buildtool`, `cajeta`, `gpu`,
  `history`, `specification`, `specs`) is a tab, and the loose top-level `.md`
  files are grouped as a **Guides** tab. New top-level children appear as tabs
  on regeneration without code changes.
- 2.2 As a visitor, when I click a tab, then I get that section's index: a card
  per document (title, description, reading time), sub-grouped by
  subdirectory where the section has one (e.g. `specification/lang`).
- 2.3 As a visitor, when I open a document card, then I get a rendered page at
  a stable URL derived from the file path (`/specification/lang/memorymodel/`).
- 2.4 As a visitor, when a page's source has intra-docs relative `.md` links,
  then they resolve to the corresponding site URLs, not broken file paths.

## 3. Editorial rendering

Doc pages are typeset HTML, not raw markdown lay-ins.

- 3.1 As a reader, when I open any doc page, then the first paragraph opens
  with a drop capital, headings use a display serif, body text a readable
  serif, and code a monospace — a deliberate font set, self-hosted.
- 3.2 As a reader, when a doc contains fenced `cajeta` code, then it is
  syntax-highlighted with a Cajeta grammar (custom TextMate grammar fed to
  Shiki), and other languages highlight with their standard grammars.
- 3.3 As a reader, when a doc contains tables or blockquotes, then they render
  with styled, sophisticated layout (ruled tables, set-off quotes), readable in
  both light and dark themes.
- 3.4 As a reader on a long page, then a table of contents tracks my scroll
  position and jumps to sections.

## 4. Interactive React islands

React components hydrate only where interactivity is needed.

- 4.1 As a visitor on the home page, when I look at the overview, then charts
  summarize the corpus (documents per section, size distribution) generated
  from the scanned tree.
- 4.2 As a visitor, when I type in the site-wide filter, then document cards
  narrow live by title/description match across all sections.
- 4.3 As a reader, when I toggle the theme, then the site switches light/dark
  and remembers my choice.

## 5. Regeneration

- 5.1 As the developer, when I edit or add files under `docs/` and run one
  command (`npm run regen` in `site/`, or `site/scripts/regen.sh`), then the
  manifest (titles, descriptions, sections, stats) is rebuilt from `docs/` and
  the static site is regenerated to `site/dist/`.
- 5.2 As the developer, when a new top-level directory or loose file appears in
  `docs/`, then regeneration picks it up with no source edits (2.1).
- 5.3 As the developer, when I run the dev server, then edits to `docs/`
  markdown are reflected on reload.
