# cajeta-font — sfnt parsing, metrics, kerning, and subsetting

## 1. Definition

### 1.1 Purpose

Cajeta cannot read a font. There is no font loading, no glyph outline access, no
text measurement, and no shaping anywhere in the tree — `cajeta.gfx` is a
Vulkan-level surface whose only "text" hits are the word *texture*.

`dev.cajeta.font` is an **external library** providing the sfnt (TrueType /
OpenType) surface: table parsing, character-to-glyph mapping, metrics, kerning,
outline extraction, and PDF-ready subsetting.

### 1.2 Why its own library, and why first

It is a **general facility**, not a charting one — text rendering has consumers
across documents, reports, IDE surfaces, and anything that draws or extracts
text. `cajeta-chart` §3 originally specified it; that section is superseded by
this spec and should be reduced to a reference.

It is also **the critical path for all charting** (`cajeta-chart` §1.7): layout
cannot resolve without measured text, because you cannot place an axis without
knowing how wide its labels are. Any plan that sequences charts before fonts is
wrong.

### 1.3 Two consumers, two different needs

| Consumer | Needs |
|---|---|
| `cajeta-chart` | metrics for layout, outlines to rasterize, subsets to embed in PDF |
| `cajeta-docs` | the **inverse** — glyph codes back to Unicode, to extract text *from* a PDF (§7) |

The second is easy to overlook and shapes §7: this library must map both
directions.

### 1.4 Scope

sfnt container and table access; `cmap`; horizontal metrics; `GPOS` pair
kerning; `glyf` (TrueType) and CFF/Type 2 (OpenType) outlines; subsetting for
PDF embedding; WOFF/WOFF2 unwrapping; TrueType Collections; reverse glyph→
Unicode mapping for text extraction.

### 1.5 Non-goals

- **1.5.1** **Rasterization.** This library emits outlines as paths and metrics
  as numbers. Filling those paths is the consumer's scanline rasterizer
  (`cajeta-chart` §13.2) — glyphs are not special-cased geometry.
- **1.5.2** **Hinting.** Unnecessary at chart sizes with good anti-aliasing,
  and a large body of work on its own.
- **1.5.3** **Complex-script shaping.** Arabic, Devanagari, and CJK vertical
  layout need a HarfBuzz-class shaper. §6.5 states the limit honestly rather
  than pretending otherwise.
- **1.5.4** **Variable fonts** (`fvar`/`gvar`, `CFF2`). Static instances only.
- **1.5.5** Font *authoring*, editing, or format conversion beyond subsetting.
- **1.5.6** System font discovery. Deliberately excluded — see §2.6.

### 1.6 Systems

`cajeta.io`, `cajeta.wire` (zlib and Brotli — WOFF/WOFF2, §8),
`cajeta.resource` (shipping the bundled default, §9), `cajeta.math` (path
geometry), `dev.cajeta.unit`.

---

## 2. Feature: the sfnt container

**TTF and OTF are the same container.** They differ only in which table holds
outlines. Everything in §3–§5 is shared; only §6 branches.

- **2.1** When a font is opened, the table directory is read and any table is
  addressable by tag.
- **2.2** When the outline format is determined, dispatch is on the **sfnt
  version tag** — `0x00010000` or `true` → `glyf`, `OTTO` → CFF, `ttcf` →
  collection — and **never on the file extension**. A `.otf` may hold `glyf`
  and a `.ttf` may hold CFF; extensions lie.
- **2.3** When a required table is missing or a table's offsets fall outside
  the file, loading fails with a diagnostic naming the table.
- **2.4** When a font is malformed, truncated, or hostile, parsing fails
  cleanly — no unbounded loop, no unbounded allocation. **A font file is
  untrusted input**; historically this is one of the most exploited parsers in
  computing.
- **2.5** When font-level metadata (family, style, version, units-per-em) is
  read, it is available from `name` and `head`.
- **2.6** When a font is loaded, it comes from supplied bytes or a bundled
  resource. **The library never searches a system font directory** — output
  must not depend on what happens to be installed (`cajeta-chart` §3.9.1).

---

## 3. Feature: character-to-glyph mapping

- **3.1** When a Unicode code point to a glyph is maped, `cmap` format 4 (BMP)
  and format 12 (full Unicode) are both supported.
- **3.2** When a code point is absent from the font, the result is the
  `.notdef` glyph explicitly, not a silent zero that renders as a blank.
- **3.3** When a whole string is maped, the result is its glyph sequence in one
  call, since per-character calls dominate measurement cost.
- **3.4** When a font carries several `cmap` subtables, selection is documented
  and deterministic.

---

## 4. Feature: metrics — the property everything else depends on

- **4.1** When a string is measured, the result is its advance width, ascent,
  descent, and bounding box **before drawing it**, so layout can resolve.
- **4.2** When per-glyph metrics is read, advance width and left side bearing
  come from `hmtx`, and vertical metrics from `hhea`/`OS/2`.
- **4.3** When at a point size is measured, scaling from font units uses
  `unitsPerEm`, and the rounding rule is stated — inconsistent rounding is how
  backends drift apart.
- **4.4** When the same string is measured for any consumer, the result is
  **identical**. This is the property that keeps SVG, PDF, PNG, and HTML5
  agreeing on label collisions (`cajeta-chart` §3.3, §14.5).
- **4.5** When a font has tabular figures, that is discoverable, because
  proportional digits make axis tick labels jitter as values change (`cajeta-
  chart` §3.9.5).

---

## 5. Feature: kerning — via `GPOS`, not `kern`

- **5.1** When text is measured or positioned, **`GPOS` pair positioning** is
  applied. The legacy `kern` table is largely absent from modern fonts; a
  parser that reads only `kern` produces visibly loose text and is the most
  commonly missed piece of this whole library.
- **5.2** When a font carries a legacy `kern` table and no `GPOS`, `kern` is
  used as a fallback.
- **5.3** When `GPOS` uses class-based pair positioning, class definitions are
  resolved — pair-by-pair coverage alone is insufficient.
- **5.4** When kerning is disabled, measurement and positioning agree with each
  other in that mode too.

---

## 6. Feature: outlines

- **6.1** When a `glyf` outline is extracted, the result is quadratic B-spline
  contours, with composite glyphs resolved into their components under their
  transforms.
- **6.2** When a composite glyph nests, recursion is bounded and a cycle fails
  rather than hanging (§2.4).
- **6.3** When a CFF outline is extracted, Type 2 charstrings are interpreted
  into cubic contours. Two things bite here: the **subroutine bias** depends on
  the subr INDEX count and is a classic off-by-one, and **hint operators must
  be parsed to be skipped correctly** even though hinting is ignored (§1.5.2).
- **6.4** When outlines are requested, both formats yield the **same path
  representation** — cubic segments — so consumers never branch on outline
  format. TrueType quadratics are elevated on the way out.
- **6.5** When out non-Latin text is layed, the supported range is stated
  plainly: Latin, Latin-Extended, Greek, and Cyrillic with correct kerning.
  Scripts requiring reordering, joining, or contextual substitution are **not**
  supported (§1.5.3), and the API says so rather than producing subtly wrong
  output.

---

## 7. Feature: subsetting and reverse mapping

Two directions, two consumers (§1.3).

- **7.1** When a font to a glyph set is subseted, only those glyphs are
  retained and the output is a valid font.
- **7.2** When a TrueType font is subseted, `glyf`/`loca` are rebuilt and the
  result embeds in PDF as `/FontFile2`.
- **7.3** When a CFF font is subseted, the charstring INDEX is rebuilt and the
  result embeds as `/FontFile3` subtype `Type1C`.
- **7.4** When a subset glyph is composite, its components are retained too —
  dropping them yields a font that renders holes.
- **7.5** When text **from** a PDF is extracted, it is possible to map glyph
  codes **back** to Unicode via the font's `cmap` reversed and any `ToUnicode`
  CMap, which is what `cajeta-docs` §4.5 needs.
- **7.6** When a glyph has no Unicode preimage, that is reported rather than
  guessed — a wrong character is worse than a known gap.

---

## 8. Feature: container variants

- **8.1** When a **WOFF** file is opened, the zlib-compressed tables are
  unwrapped into an ordinary sfnt, reusing `cajeta.wire`.
- **8.2** When a **WOFF2** file is opened, the Brotli stream is decompressed
  likewise. *(`cajeta.wire.Compressor` already lists zlib and Brotli, so this
  is cheap.)*
- **8.3** When a **TrueType Collection** is opened, it is possible to enumerate
  the fonts and select one by index or name.
- **8.4** When a container's checksums fail, it is rejected.

---

## 9. Feature: the bundled default

- **9.1** When no font is supplied, a bundled default is used, so a first chart
  needs no configuration.
- **9.2** When the default ships, it is a **bundled resource** read with no
  filesystem capability (`buildtool-resources` §3.2).
- **9.3** When the default is chosen, it has **tabular figures**, stays legible
  at small sizes, and visually disambiguates `0`/`O` and `1`/`l`/`I` (`cajeta-
  chart` §3.9.5).
- **9.4** When the default ships, its licence permits redistribution **and PDF
  embedding** — SIL OFL or Apache-2.0 — and the licence text ships with it.
- **9.5** When the default ships, it is **subset** to Latin, Latin-Extended,
  Greek, Cyrillic and common symbols, and limited to two weights. Full Unicode
  coverage would dominate the artifact with glyphs no chart draws.

---

## 10. Open questions (resolve at plan time)

- **10.1** *(resolved 2026-08-01 — Inter.)* The bundled fallback is **Inter**
  (OFL, screen-designed, genuine tabular figures, disambiguating stylistic set)
  with a **mono sibling** for table views. Tabular figures decided it: axis
  labels and table columns must align on the digit, which proportional figures
  break.
- **10.2** *(resolved 2026-08-01.)* v1 bundles **static instances**, since
  §1.5.4 defers variable-font support. Inter's variable build would put every
  weight in one file; that is an optimization to revisit, not a dependency.
  Follow-on for the plan: decide which weights ship — regular and bold at
  minimum, and whether the mono sibling needs more than one.
- **10.3** *(resolved 2026-08-01 — the library ships it.)* `dev.cajeta.font`
  bundles the default font through `cajeta.resource`. The alternative makes
  every consumer re-solve the same licensing and subsetting problem.
- **10.4** *(resolved 2026-08-01.)* **`cajeta-docs`** owns the `ToUnicode` CMap
  parser — it is a PDF construct, not an sfnt one. This library owns only the
  reversed `cmap`.
- **10.5** *(new 2026-08-01 — from `cajeta-chart` §13.1.)* This library gains
  the **text shaping engine**: full bidirectional text, Arabic joining, Indic
  reordering, and CJK, driven by the font's own **GSUB/GPOS** tables. It is
  specified separately as **`cajeta-text-shaping-spec`** rather than folded in
  here, because it is comparable in size to this whole spec and has three
  independent consumers (`cajeta-chart` §3, `cajeta-docs` PDF output, and this
  library's own rendering). Shaping belongs with the font, not with any one
  renderer — the same separation HarfBuzz has from every engine that uses it.
  **That spec is not yet written.**

---

## 11. Acceptance criteria (spec-level)

- **11.1** The same string measures **identically** across every consumer
  (§4.4) — the property `cajeta-chart` §14.5 depends on.
- **11.2** TTF and OTF fonts yield the same path representation (§6.4),
  verified by rendering one glyph from each and comparing geometry.
- **11.3** Kerned text measures narrower than unkerned for a known kerning pair
  (§5.1) — the assertion that catches a `GPOS`-less parser.
- **11.4** A subset font embeds in a PDF that opens correctly in a standards-
  compliant reader **with no external font available**.
- **11.5** A composite glyph survives subsetting with its components (§7.4).
- **11.6** Malformed, truncated, and adversarially crafted fonts fail with
  diagnostics — no hangs, no unbounded memory (§2.4). **Fuzz the parser.**
- **11.7** Round-trip: a string → glyphs → back to Unicode recovers the
  original for the supported scripts (§7.5).
- **11.8** Dispatch is by sfnt version tag, verified with a `.ttf` containing
  CFF outlines and a `.otf` containing `glyf` (§2.2).
