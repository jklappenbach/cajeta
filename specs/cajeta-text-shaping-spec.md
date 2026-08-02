# cajeta-text-shaping — Unicode text to positioned glyphs

## 1. Definition

### 1.1 Purpose

Nothing in cajeta can turn a string into glyphs. `cajeta-font` will parse the
font — `cmap`, outlines, metrics, `GPOS` kerning — but parsing a font is not
laying out text. The step between "a Unicode string and a font" and "these
glyph IDs at these positions" is **shaping**, and it is missing.

This spec owns that step. It ships in **`dev.cajeta.font`** rather than in any
renderer, because shaping is driven by **GSUB and GPOS**, which are tables in
the font file, and because three libraries need it independently.

### 1.2 What shaping is, and why it is not a lookup

The naive model — one character, one glyph, advance by its width, left to
right — holds for English and fails for most writing systems. Shaping is the
set of transformations that make the general case correct:

| Transformation | Example | Table |
|---|---|---|
| **Substitution, many→one** | `f` + `i` → a single `fi` glyph | GSUB |
| **Substitution, contextual** | Arabic ع has four forms — isolated, initial, medial, final — chosen by neighbours | GSUB |
| **Substitution, one→many** | a precomposed character decomposing into base + mark | GSUB |
| **Positioning, pairwise** | `A` `V` pulled closer — kerning | GPOS |
| **Positioning, attachment** | an accent placed over its base, at a height that depends on the base | GPOS |
| **Reordering** | Devanagari क + ि displays as कि — the vowel sign precedes the consonant it follows in memory | script rules |
| **Direction** | mixed Arabic and Latin resolved to visual order | UAX #9 |

The consequence that matters: **glyph count is not character count, and glyph
order is not character order.** An API that assumes either is wrong for most of
the world, and wrong in a way that produces different words rather than merely
ugly output.

### 1.3 The contract

> **Input:** a run of Unicode text, a font, a script, a language, a direction,
> and a size. **Output:** a glyph buffer — glyph IDs with x/y offsets,
> advances, and a cluster mapping back to the source text.
>
> This spec produces positions. **It draws nothing.**

### 1.4 Scope

Itemization into script/direction/font runs; the bidirectional algorithm
(UAX #9); GSUB and GPOS application with feature selection; GDEF glyph classes
and mark filtering; the default shaper; script-specific shapers for Arabic,
Hebrew, the Indic family, Khmer, Myanmar, Thai/Lao, and Hangul; the Universal
Shaping Engine for everything else complex; vertical text; the cluster model;
and shaping-plan caching.

### 1.5 Non-goals

- **1.5.1** **Rasterization.** This spec emits positions. `cajeta-chart` §13.2's
  CPU scanline rasterizer draws them.
- **1.5.2** **Font parsing.** `cajeta-font` owns sfnt, `cmap`, outlines, and
  metrics. This spec consumes them and adds only the shaping tables.
- **1.5.3** **Line breaking (UAX #14) and justification.** Deciding *where* a
  line ends is paragraph layout, not shaping — a shaper is called *per run*,
  after breaks are chosen. **Currently unowned; see §13.5.**
- **1.5.4** **Font fallback.** Choosing a different font when a glyph is
  missing is a layout-layer policy, above shaping. This spec reports
  `.notdef`; it does not resolve it. HarfBuzz draws the same line.
- **1.5.5** **Bitmap and colour fonts** (CBDT, sbix, COLR/CPAL). Emoji
  rendering is out of scope for v1.
- **1.5.6** A text *editing* model — cursor movement, selection, hit testing.
  §9's cluster map is the primitive those need, but the policy is not here.

### 1.6 Consumers

| Consumer | Needs |
|---|---|
| `cajeta-chart` §3 | axis labels, legends, titles, annotations |
| `cajeta-docs` | PDF **output**; text extraction is the inverse problem and stays there |
| `dev.cajeta.font` | its own rendering and PDF subsetting |

`cajeta-chart` §13.1 records that this was moved out of that spec. **That
spec's §3 depends on this one and does not implement it.**

### 1.7 The parity oracle — HarfBuzz

Consistent with the ecosystem's discipline (sklearn for classical ML,
statsmodels for time series, NetworkX for graphs): **HarfBuzz is the oracle and
the API muse, never a port.**

This oracle is unusually good, because `hb-shape` emits a documented plain-text
serialization of the glyph buffer — glyph name, cluster, and position per
glyph. That makes fixtures **exact and diffable** rather than approximate, for
every script, without a rendering comparison.

### 1.8 Systems

`dev.cajeta.font` (sfnt tables, `cmap`, metrics), `cajeta.lang.String`
(**Unicode normalization**, `stdlib-completion` §7), `cajeta.collection`,
`cajeta.math` (fixed-point and transform arithmetic), `dev.cajeta.unit`.

### 1.9 Prerequisites

- **1.9.1** `cajeta-font`'s sfnt container and `cmap` must land first — there is
  nothing to shape without glyph IDs.
- **1.9.2** `stdlib-completion` §7's Unicode normalization must land first.
  Shaping operates on normalized text; §4.1 depends on it.
- **1.9.3** The Unicode character database (`stdlib-completion` §8.4) supplies
  the script, joining-type, combining-class, and bidi-category properties this
  spec reads. It is the same UCD, not a second copy.

---

## 2. Feature: the glyph buffer

- **2.1** When text is shaped, the result is a **glyph buffer**: an ordered
  sequence of glyph IDs, each with an x/y offset, an x/y advance, and a
  cluster value.
- **2.2** When the buffer is read, it is in **visual order** — the order glyphs
  are drawn — not logical order, so a renderer needs no further reordering.
- **2.3** When a glyph is inspected, the font it came from is identifiable, so
  a run assembled from more than one font stays attributable.
- **2.4** When positions are read, the units are stated — font design units
  scaled to a requested size, with the scaling factor explicit — so a caller
  never guesses whether a number is em-relative or device-relative.
- **2.5** When a character has no glyph in the font, `.notdef` (glyph 0) is
  emitted rather than the character being dropped. **A missing glyph is
  visible.** Silently dropping text is the worst available failure.
- **2.6** When the buffer is empty because the input was empty, that is
  distinct from a shaping failure.

---

## 3. Feature: clusters — the mapping back to text

Clusters are what make shaping usable by anything above it: cursor placement,
selection, hit testing, and `cajeta-docs`' provenance all need to know which
glyphs came from which characters.

- **3.1** When shaping completes, each glyph carries a **cluster value** — the
  index of the source character it derives from.
- **3.2** When several characters produce one glyph (a ligature), they share
  one cluster value.
- **3.3** When one character produces several glyphs (a decomposition), those
  glyphs share that character's cluster value.
- **3.4** When glyphs are reordered, cluster values move with their glyphs, so
  the mapping survives reordering.
- **3.5** When a cluster is inspected, the **character range** it covers is
  recoverable, so a caller can map a glyph back to a substring without
  re-shaping.
- **3.6** When clusters are merged, the merging level is selectable — the
  conservative grouping that keeps grapheme clusters intact, or the finer
  grouping that exposes what actually combined. HarfBuzz exposes both and they
  serve different callers.

---

## 4. Feature: itemization

Shaping runs over a homogeneous run. Splitting arbitrary text into such runs is
the step before it.

- **4.1** When a string is prepared for shaping, it is **normalized** first
  (`cajeta.lang.String`, `stdlib-completion` §7). Composed and decomposed forms
  must shape identically, and only normalization guarantees that.
- **4.2** When text is itemized, it splits into runs of a single **script**
  (UAX #24), with common and inherited characters attaching to the surrounding
  run rather than forming their own.
- **4.3** When text is itemized, it splits on **direction** boundaries from §5.
- **4.4** When a caller supplies a script or language explicitly, that overrides
  detection — language changes shaping within one script, and the classic case
  is Turkish, where `i` behaves differently.
- **4.5** When runs are produced, their order and boundaries are deterministic
  for identical input.
- **4.6** When a run is shaped, it is shaped independently, so runs can be
  shaped concurrently and reassembled.

---

## 5. Feature: the bidirectional algorithm (UAX #9)

- **5.1** When text mixes directions, the **Unicode Bidirectional Algorithm**
  resolves embedding levels per character.
- **5.2** When a paragraph direction is not supplied, it is inferred from the
  first strong directional character; when it is supplied, it is honoured.
- **5.3** When explicit formatting characters appear (LRE, RLE, LRO, RLO, PDF,
  LRI, RLI, FSI, PDI), they are processed per the algorithm rather than
  ignored.
- **5.4** When levels are resolved, runs are reordered to visual order by the
  L2 rule.
- **5.5** When mirrored characters — brackets, parentheses — appear in a
  right-to-left run, the mirrored glyph is selected.
- **5.6** When bidi runs, it is verified against the **Unicode BidiTest and
  BidiCharacterTest** conformance files. These are published, exhaustive, and
  machine-readable; passing them is not optional (§14.3).

---

## 6. Feature: GSUB — substitution

- **6.1** When shaping runs, GSUB lookups apply in table order, and the result
  of one lookup is visible to the next.
- **6.2** When a lookup type is encountered, single, multiple, alternate,
  ligature, contextual, and chaining-contextual substitution are all supported —
  every type OpenType defines, not a common subset.
- **6.3** When extension lookups (type 7) appear, they are followed
  transparently.
- **6.4** When features are selected, the script and language system determine
  which apply, and per-script required features are always applied.
- **6.5** When a caller enables or disables a feature by tag, that is honoured,
  including over a character range rather than the whole run.
- **6.6** When default features are chosen, the set is documented per script —
  `liga`, `calt`, `ccmp`, `locl`, `rlig` and the script-specific families — and
  matches HarfBuzz's, since a different default set produces different output
  for the same font.
- **6.7** When a lookup would loop indefinitely on adversarial input, it is
  bounded and reports the bound. **A font is untrusted input** and a shaper is
  an attack surface.

---

## 7. Feature: GPOS — positioning

- **7.1** When positioning runs, single, pair, cursive, mark-to-base,
  mark-to-ligature, mark-to-mark, contextual, and chaining-contextual
  positioning are all supported.
- **7.2** When a font has no GPOS but has a legacy `kern` table, that is used
  instead, so older fonts still kern.
- **7.3** When marks attach, GDEF glyph classes distinguish base, ligature,
  mark, and component, and mark filtering sets are honoured.
- **7.4** When a mark attaches to a ligature, it attaches to the correct
  component, not to the ligature as a whole.
- **7.5** When cursive attachment applies — as it does throughout Arabic
  — connected glyphs join at their entry and exit anchors.
- **7.6** When a device table or variation delta adjusts a position, it is
  applied at the requested size.
- **7.7** When positioning completes, advances and offsets are separable, since
  a renderer needs both independently.

---

## 8. Feature: the shapers

One shaper per script family, selected by itemization. Each is a distinct
algorithm, not a configuration of one.

### 8.1 Default shaper

- **8.1.1** When a script has no dedicated shaper, the default shaper applies
  GSUB then GPOS with the standard feature set. Latin, Cyrillic, Greek, and
  Hebrew are handled here.
- **8.1.2** When Hebrew presentation forms or points appear, they position
  correctly under the default shaper plus GPOS marks.

### 8.2 Arabic

- **8.2.1** When Arabic is shaped, each letter's **joining form** — isolated,
  initial, medial, final — is selected from its joining type and its
  neighbours', per the Unicode joining algorithm.
- **8.2.2** When a non-joining or transparent character intervenes, joining is
  computed across it correctly rather than being broken by it.
- **8.2.3** When the required ligatures apply — lam-alef above all — they are
  formed.
- **8.2.4** When cursive positioning applies, §7.5's anchors join the letters.
- **8.2.5** When Arabic marks stack, they order and position by combining
  class.

### 8.3 The Indic family

- **8.3.1** When an Indic script is shaped, the text is divided into
  **syllable clusters** by the script's grammar before any substitution.
- **8.3.2** When a cluster is processed, its glyphs **reorder** per the script's
  rules — the case that makes Indic distinct: a matra stored after its
  consonant may be displayed before it.
- **8.3.3** When a conjunct forms, the virama/halant sequence produces the
  conjunct glyph the font provides.
- **8.3.4** When reph or pre-base forms apply, they move to their correct
  positions within the cluster.
- **8.3.5** When Devanagari, Bengali, Gujarati, Gurmukhi, Kannada, Malayalam,
  Oriya, Tamil, or Telugu is shaped, each is handled — they share a framework
  but differ in their reordering rules.

### 8.4 Khmer, Myanmar, Thai, Lao

- **8.4.1** When Khmer or Myanmar is shaped, its own cluster and reordering
  rules apply — they resemble the Indic family but are not it.
- **8.4.2** When Thai or Lao is shaped, mark positioning and the
  tone-mark/vowel ordering rules apply.

### 8.5 Hangul

- **8.5.1** When Hangul is shaped, jamo sequences compose to syllable glyphs
  where the font provides them, and decompose where it does not.

### 8.6 The Universal Shaping Engine

- **8.6.1** When a complex script has no dedicated shaper, the **Universal
  Shaping Engine** applies, using the Unicode character properties to cluster
  and reorder generically.
- **8.6.2** When a new script is added to Unicode, USE handles it without a new
  shaper. That is the point of it, and it is why the §8 list is not a coverage
  ceiling.

---

## 9. Feature: vertical text

- **9.1** When text is shaped vertically, `vhea`/`vmtx` advances are used, and
  `VORG` positions the origin.
- **9.2** When a glyph has a vertical alternate (`vert`, `vrt2`), it is
  substituted.
- **9.3** When Latin appears inside vertical CJK, its rotation is the caller's
  policy and the shaper reports what it did rather than deciding silently.
- **9.4** When vertical text is positioned, GPOS applies in the vertical axis.

---

## 10. Feature: variable fonts

- **10.1** When a font is variable and a variation instance is requested, the
  instance is applied before shaping so positions reflect it.
- **10.2** When `GDEF`'s item variation store adjusts anchors or advances, those
  deltas apply at the instance.
- **10.3** When `cajeta-font` §1.5.4 still defers variable support, this feature
  is inert and says so rather than silently ignoring an instance request.

---

## 11. Feature: the shaping plan and performance

Shaping the same font and script repeatedly — every axis label on a chart — must
not repeat the setup work.

- **11.1** When a font, script, language, and feature set recur, the derived
  **shaping plan** is cached and reused.
- **11.2** When a plan is cached, the cache is keyed on everything that affects
  it, so a changed feature set cannot silently reuse a stale plan.
- **11.3** When a run is shaped, no allocation occurs per glyph in the steady
  state — buffers are reused.
- **11.4** When a caller shapes many short runs, that path is efficient. It is
  the common case for charts, and the one a naive implementation handles worst.
- **11.5** When shaping is measured, per-run timing and glyph counts are
  observable, so a slow layout is diagnosable rather than mysterious.

---

## 12. Feature: robustness

- **12.1** When a font is malformed — bad offsets, cyclic lookups, truncated
  tables, absurd counts — shaping fails with a diagnostic naming the table and
  offset, and never loops forever, reads out of bounds, or exhausts memory.
- **12.2** When shaping is given adversarial input, work is bounded relative to
  input length, so a pathological string cannot hang the caller (§6.7).
- **12.3** When a font lacks the tables a script needs, shaping degrades to the
  best available result and **reports the degradation** rather than producing
  wrong text silently.
- **12.4** When the same input is shaped twice, the output is identical. Shaping
  is a pure function of its inputs.

---

## 13. Open questions (resolve at plan time)

- **13.1** **Scale, and whether the full set lands at once.** HarfBuzz is
  roughly 100k lines developed over a decade. §8's shapers are separable and
  independently testable, so the honest sequencing is: default + bidi first,
  then Arabic, then Indic, then USE. Recommendation: phase it explicitly the
  way `cajeta-chart` §13.7 is phased, and let `cajeta-chart` C1 depend only on
  the default shaper.
- **13.2** Does the UCD property access (§1.9.3) go through
  `cajeta.lang.String`'s public API, or does this library read the tables
  directly? Shaping needs joining type, combining class, and script — which
  normalization does not expose. Recommendation: extend the stdlib UCD surface
  rather than shipping a second copy of the data.
- **13.3** Is bidi (§5) part of this library or a stdlib Unicode facility? It is
  a text-processing algorithm with no font involvement at all, and `cajeta-docs`
  would want it independently of shaping. Recommendation: **stdlib**, beside
  normalization — but that adds to `stdlib-completion`'s already-large §7.
- **13.4** Should the API be a one-shot `shape(text, font, options)` or an
  explicit buffer the caller fills, shapes, and reads? HarfBuzz uses the latter
  and it is what makes buffer reuse (§11.3) possible. Recommendation: the
  explicit buffer, with a one-shot convenience wrapper.
- **13.5** **Line breaking (UAX #14) is unowned.** §1.5.3 excludes it, correctly,
  but `cajeta-chart` and PDF output both need it and no spec claims it.
  Recommendation: it belongs beside bidi in stdlib Unicode, or in a text-layout
  layer above this one. **Decide before `cajeta-chart` C1**, since wrapped axis
  labels and multi-line titles need it.
- **13.6** How are HarfBuzz fixtures generated and kept current? `hb-shape`'s
  output format is stable, but the fixtures pin a HarfBuzz version and a font
  version together. Recommendation: pin both explicitly, and vendor the test
  fonts — HarfBuzz's own test suite fonts are permissively licensed and purpose
  built.
- **13.7** Which fonts are used for testing complex scripts? Correct Arabic and
  Indic fixtures need fonts with real GSUB/GPOS coverage for those scripts, and
  Inter (§10.1 of `cajeta-font`) has none. Recommendation: vendor Noto for the
  scripts under test; it is OFL and has the broadest coverage available.

---

## 14. Acceptance criteria (spec-level)

- **14.1** Shaped output matches **`hb-shape`** exactly — glyph IDs, clusters,
  and positions — for every fixture, at pinned HarfBuzz and font versions
  (§1.7, §13.6).
- **14.2** Ligature, kerning, and mark-attachment cases are each verified
  against a font that actually exercises them, not merely against text that
  happens to contain them.
- **14.3** The **Unicode BidiTest and BidiCharacterTest** conformance suites
  pass in full (§5.6). Partial passage is failure.
- **14.4** Arabic shapes with correct joining forms across a word, verified
  glyph by glyph against the oracle (§8.2.1).
- **14.5** Devanagari reorders correctly — क + ि renders as कि with the matra
  preceding — verified as glyph order, not as an image (§8.3.2).
- **14.6** Cluster values survive reordering, and every glyph maps back to a
  character range (§3.4, §3.5).
- **14.7** A missing glyph produces `.notdef` and never a dropped character
  (§2.5), asserted by a test using a font deliberately lacking coverage.
- **14.8** A malformed and an adversarial font each fail with a diagnostic
  rather than hanging, crashing, or reading out of bounds (§12.1, §12.2).
  **Fuzz the table parsers.**
- **14.9** Shaping is deterministic: identical input yields byte-identical
  buffers across runs and platforms (§12.4).
- **14.10** Shaping the same run repeatedly performs no steady-state allocation
  (§11.3), verified by measurement.
- **14.11** `cajeta-chart` renders a chart whose labels include Latin, Arabic,
  and Devanagari, and the output matches the oracle's positions — the
  end-to-end proof that the §1.6 seam works.
