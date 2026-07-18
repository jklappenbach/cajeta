# home-and-benchmarks — Home tab + Benchmarks tab (spec)

## 1. Definition
Give the site a real front door and surface the benchmark docs. Content
lives in the docs tree (cajeta-five); rendering/site code in this clone.

## 2. Home

- 2.1 A `docs/home/` directory holds the home page source (`README.md`).
  Its tab, labeled **Home**, is pinned first and links `/`; **Guide** comes
  next (alphabetical among directories, home pinned ahead of it).
- 2.2 The site root renders the home document — hero + styled feature
  sections consistent with the site's editorial design — with the Home tab
  active. Trees without `home/` keep the previous behavior (guide reader,
  then the landing fallback).
- 2.3 The page introduces Cajeta (what it is, where it specializes, what
  you can build), the toolchain (IDE plugin, `cajeta` builder, `cvm`
  version manager, Olla at https://olla.cajeta.dev), and the platform-
  independent libraries: Nucleo (data science/ML), XPU (compute), GFX
  (graphics).

## 3. Benchmarks

- 3.1 The site builder ingests `bench/**/*.md` (sibling of the docs root)
  as a **benchmarks** section: pinned as the final tab, rendered through
  the same editorial pipeline as every other doc (reference-browser
  layout, Overview from bench/README.md).
- 3.2 Relative links from bench markdown into `docs/` resolve to site URLs.
- 3.3 Doc pages state their true source path (`bench/…` vs `docs/…`).
