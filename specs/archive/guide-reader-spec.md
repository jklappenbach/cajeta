# guide-reader — chaptered reading experience for numbered guides (spec)

## 1. Definition

### 1.1 Purpose
Guides with numbered `## N.` chapters (LanguageGuide, OperatorOverloading,
ArchiveManagement) are written to be read in order, but render as one long
page. Give them a book-style reader: chapter navigation and paging.

### 1.2 Scope
Applies automatically to any **guides-section** doc with ≥ 3 numbered H2
chapters. Other docs keep the existing single-page layout. `docs/` markdown
is not modified.

### 1.3 Non-goals
No chapterization of specification/specs docs (they use outline numbering
for a different purpose); no persistence of reading position.

## 2. Chapter navigation

- 2.1 As a reader, when I open a chaptered guide, then I see a left-hand
  index listing every chapter (numbered, in order) with links, and the main
  panel shows the current chapter only.
- 2.2 As a reader, the index highlights the chapter I am on.
- 2.3 As a reader, when I open the guide's base URL, then I get the guide's
  introduction (content before chapter 1) as the first page.
- 2.4 As a reader, each chapter lives at a stable URL derived from its
  heading (`/guides/languageguide/2-primitive-types/`).

## 3. Paging

- 3.1 As a reader, at the end of a chapter I find a centered pager block
  with a left arrow (previous chapter) and right arrow (next chapter),
  showing the adjacent chapter titles; at the ends, the missing side is
  absent.
- 3.2 As a reader, cross-references to other chapters (`#anchor` links) land
  on the correct chapter page, not a dead same-page anchor.

## 4. Regeneration

- 4.1 Chapterization derives from the markdown at build time: renumbered or
  added chapters in `docs/` are reflected by `npm run regen`/`deploy` with
  no code changes.
