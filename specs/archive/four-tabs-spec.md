# four-tabs — Tour / Guide / Specification / Stdlib navigation (spec)

## 1. Definition
Navigation for the reorganized docs tree (tour doc + `guide/`,
`specification/`, `stdlib/`). Four top tabs, each opening directly into
content — no card-grid interstitials for these sections.

## 2. Tabs

- 2.1 The header shows, in order: **Tour**, **Guide**, **Specification**,
  **Stdlib** (any other top-level dirs follow; `history` last; the loose-file
  "Guides" tab is dropped — the tour is promoted to its own tab).
- 2.2 **Tour** opens `/tour/` — the tour document rendered as a normal doc
  page (content only, with its table of contents).
- 2.3 **Guide** opens the book reader at the guide introduction (left chapter
  index, right view) — `/guide/` renders it directly.
- 2.4 **Specification** and **Stdlib** open a reference browser: a left
  index of the section's documents grouped by subdirectory, the current
  document on the right; `/specification/` and `/stdlib/` land on the
  section README ("Overview") in the same shell.
- 2.5 Every doc page inside those two sections shows the same left index
  with the current document highlighted.
- 2.6 Tabs still derive from the manifest: trees without a tour or these
  dirs fall back to the generic per-section tabs (six's own tree keeps
  working).
