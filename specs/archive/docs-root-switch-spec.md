# docs-root-switch — build the site from an alternate docs tree (spec)

## 1. Definition

### 1.1 Purpose
The canonical docs reorganization lives in the cajeta-five clone
(`docs/guide/` numbered chapter files, `stdlib/`, `specification/`) and
cannot be committed yet. The site work stays in this clone (cajeta-six) but
must render cajeta-five's `docs/` tree.

### 1.2 Constraints
- Nothing from cajeta-five is committed here; the local pointer to it must
  be gitignored. The committed default stays `../docs` (this repo).
- All existing behavior (tabs from top-level children, manifest, link
  rewriting) must work unchanged against either tree.

## 2. Configurable docs root

- 2.1 As the developer, when I put a path in a gitignored `site/.docs-root`
  file (or set `CAJETA_DOCS_ROOT`), then the manifest scan, the content
  collection, and md-link rewriting all read that tree; with neither set,
  `../docs` is used.
- 2.2 As the developer, `npm run regen` / `deploy` against the override
  works with no other changes (tabs, cards, charts reflect the new tree).

## 3. Chapters as numbered files

- 3.1 As a reader, when a section's docs are numbered files
  (`guide/00-introduction.md` … `21-reflection.md`), then those pages render
  in the book reader (left chapter index, panel, prev/next pager), ordered
  numerically, with the section README as the introduction chapter.
- 3.2 Docs in subdirectories of such a section (e.g. `guide/drafts/`) are
  not chapters; they render as regular pages.
- 3.3 The existing numbered-H2 reader for single-file guides keeps working.
