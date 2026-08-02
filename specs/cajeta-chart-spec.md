# cajeta-chart — a first-class charting and visualization library

## 1. Definition

### 1.1 Purpose

Cajeta has no charting capability. `cajeta.gfx` is a Vulkan-level GPU surface
(swapchain, textures, pipelines) and `cajeta.math.Color` is an RGBA record with
sRGB conversion — that is the entire relevant foundation. Nothing draws a chart.

This spec defines `dev.cajeta.chart`: a data-visualization library with the
**composability of D3**, the **declarative chart surface and interactivity of
plotly**, the **coverage of matplotlib/seaborn**, and output to **SVG, PDF,
HTML5, and PNG**. It is intended to be best-of-breed, not adequate.

### 1.2 Design intent, made testable

"Stylish and elegant" is a requirement, not a preference, and this spec refuses
to leave it subjective. §10 encodes a design method whose colour rules are
**computable and validated by a runnable checker**, whose mark geometry is
specified in pixels, and whose accessibility properties are asserted by test. A
chart that violates §10 fails CI; it is not a matter of taste.

### 1.3 The layered architecture

Five layers, each usable on its own. This is what lets one library be both D3
and plotly: the low layers are the composable primitives, the high layers are
the one-call surface built from them.

| Layer | Role | Analogue |
|---|---|---|
| **L0 Surface** | device-independent drawing: paths, text, images, clip, transform | canvas / cairo |
| **L1 Primitives** | scales, axes, ticks, shape generators, colour scales, layouts | **D3** |
| **L2 Figure** | figure/axes/subplots, layout engine, explicit composition | **matplotlib** |
| **L3 Declarative** | one call from a `Table` to a chart; facets; statistical transforms | **plotly express / seaborn** |
| **L4 Interaction** | hover, tooltip, zoom, pan, brush, legend toggle | **plotly / D3** |

An author may drop to any layer. L3 is the default entry point; L1 is the escape
hatch that keeps the library from being a walled garden.

### 1.4 Scope

The five layers; four output backends; the chart-type coverage in §9; the design
system in §10; interactivity in §11; accessibility in §12.

### 1.5 Non-goals

- **1.5.1** Geographic projections and mapping. A large domain of its own; the
  L1 scale seam is designed so it can be added later without redesign.
- **1.5.2** 3D charts. Explicitly excluded — they are almost always the wrong
  answer for quantitative reading, and `gfx` is the right home if ever wanted.
- **1.5.3** A general 2D vector-graphics API for non-chart use. L0 is scoped to
  what charts need.
- **1.5.4** Dashboard layout, data binding to live sources, and report
  generation.
- **1.5.5** GPU-accelerated rendering. L0's backend seam permits it later.

### 1.6 Systems

`dev.cajeta.font` — **both** the font machinery (`cajeta-font`) and the
**shaping engine** (`cajeta-text-shaping`); `cajeta.resource` (the bundled
default font); `cajeta.math.Tensor`, `cajeta.math.Color`,
`cajeta.math.stats.Stats` (the statistical transforms in §8),
`cajeta.nucleo.frame.Table` (the L3 data source, at L3 only),
`cajeta.wire.Compressor` (deflate, required by PNG), `cajeta.io`,
`dev.cajeta.unit`.

### 1.7 The critical path, stated up front

**Text is the hard part.** There is no font loading, no glyph metrics, and no
text shaping anywhere in cajeta. Every axis label, tick, legend entry, and title
needs measured text before layout can resolve — you cannot place an axis without
knowing how wide its labels are.

**That work is no longer in this spec.** It is `cajeta-font` (parsing, metrics,
outlines) and `cajeta-text-shaping` (string → positioned glyphs), both in
`dev.cajeta.font`. §3 states what this library requires of them and what it does
with the result.

The critical path is unchanged in substance: **both must land before phase C1**
(§13.7). Any plan that sequences charts before text is still wrong — it is now
wrong across a library boundary rather than within this one, which makes the
dependency explicit instead of implicit.

---

## 2. Feature: L0 — the rendering surface and backends

- **2.1** When drawing to a surface, one device-independent API is used — paths
  (move/line/cubic/arc/close), fills, strokes, transforms, clipping, text, and
  images — and the backend is a detail.
- **2.2** Whether rendering to SVG, PDF, PNG, and HTML5, the
  geometry is identical; only rasterization and interactivity differ.
- **2.3** When rendering to **SVG**, the result is standards-clean markup with
  real `<text>` elements, so it is searchable, restyleable, and small.
- **2.4** When rendering to **PDF**, the result is a valid vector PDF with
  embedded font subsets, so it is print-ready and portable with no external
  font dependency.
- **2.5** When rendering to **PNG**, the result is an anti-aliased raster at a
  requested pixel scale, so a 2× export is crisp on high-DPI displays.
- **2.6** When rendering to **HTML5**, the result is a self-contained document
  that carries the interactivity in §11 with no external CDN — the artifact
  must work offline.
- **2.7** When a backend cannot express something (interaction in PDF, say),
  the degradation is documented and deliberate — never a silently dropped
  element.
- **2.8** When a backend is added, it implements one surface interface and no
  chart code changes.
- **2.9** When the same figure twice is rendered, the output is byte-identical,
  so rendering is diffable and testable.

---

## 3. Feature: text, fonts, and metrics *(the gate — see §1.7)*

> **This library does not implement text handling. It consumes it.**
> Font parsing is `cajeta-font`; **shaping — turning a string into positioned
> glyphs — is `cajeta-text-shaping`**, which lives in `dev.cajeta.font` because
> GSUB/GPOS are font tables and three libraries need them (§13.1). Everything
> below is a *requirement this library places on those two*, plus what it does
> with the result. An earlier draft of this section described shaping as chart's
> own work; that was wrong and is corrected here.

- **3.1** When a font is loaded, TrueType and OpenType files are parsed and
  glyph outlines and metrics are available — via `cajeta-font`, not a second
  parser here.
- **3.2** When a string is measured, its width, ascent, descent, and bounding
  box are available **before** it is drawn, so layout can resolve. Measurement
  is **the shaped advance sum**, not a per-character metric lookup — those
  differ wherever ligatures, kerning, or marks apply, and a layout built on the
  wrong one collides.
- **3.3** When the same string for any backend is measured, the metrics agree —
  otherwise SVG and PNG disagree on label collisions.
- **3.4** When text is drawn, kerning is applied — from `cajeta-text-shaping`
  §7, which reads GPOS (or a legacy `kern` table). Broad tracking configuration
  is supported on top of it: tight, normal, wide.
- **3.5** When text is embedded in PDF, only the glyphs used are subset and
  embedded.
- **3.6** When text as geometry is needed, glyph outlines can be emitted as
  paths, so a backend without font support still renders.
- **3.7** When non-Latin text is rendered, it shapes correctly through
  `cajeta-text-shaping` — Arabic joins, Devanagari reorders, bidi resolves. This
  library states no shaping limitations of its own; whatever that spec supports,
  charts support (§13.1).
- **3.7.1** When a label is laid out, the glyph buffer's **visual order and
  positions are used as given**. This library must not reorder glyphs, sum
  per-character widths, or index text by character to place it — all three are
  correct only for simple scripts and silently wrong elsewhere.
- **3.7.2** When a label needs to wrap, line breaks come from a line-breaking
  facility, **not** from splitting on spaces or character counts. See
  `cajeta-text-shaping` §13.5: **line breaking is currently unowned**, and
  wrapped axis labels and multi-line titles both need it. This is a live
  dependency, not a detail.
- **3.7.3** When text is measured for collision detection, the measurement is
  the shaped one (§3.2), so a rotated or non-Latin tick label does not overlap
  because its width was estimated wrong.
- **3.8** When no font is supplied, a **bundled** default is used, so a first
  chart needs no configuration.

### 3.9 Font policy — bundled only, never resolved from the environment

- **3.9.1** A default TTF font should be defined for the framework, providing the default look and feel.  Everything should be configurable, however.
- **3.9.2** When the same figure on two machines is rendered, the glyphs and
  metrics are identical — this is what makes §14.2's byte-identical golden-file
  testing possible at all, and environment- resolved fonts would destroy it.
- **3.9.3** When exporting to PDF, fonts are **always** embedded as subsets,
  with no reliance on the reader supplying anything (§14.10). A PDF that
  renders differently on a machine without the font is a defect.
- **3.9.4** When a own font is supplied, it is first-class — brand typography
  is expected, not an afterthought — and it is subject to the same §3.2 metric
  requirements as the default.
- **3.9.5** When the bundled default is chosen, it must have **tabular (fixed-
  width) figures**, because proportional digits make axis tick labels shift
  horizontally as values change; it must remain legible at small sizes; and it
  must visually disambiguate `0`/`O` and `1`/`l`/`I`, since data labels are
  read as values.
- **3.9.6** When the library ships a font, its licence permits redistribution
  **and PDF embedding** — SIL OFL or Apache-2.0 — and the licence text ships
  with it.

### 3.10 Provider fonts — resolved as a dependency, never fetched at render time

A developer must be able to name a font from Google Fonts or another provider
and have the toolchain obtain it. It is resolved the way every other dependency
is resolved: **at build time, pinned by content hash, cached locally**. The
chart library itself never opens a socket.

> **Mechanism owner:** `buildtool-resources-spec.md`. That spec provides both
> the bundled-resource packaging §3.9 needs and the asset resolution §3.10
> needs; the requirements below are this library's use of it, not a
> chart-specific downloader. It is a hard prerequisite — and it in turn depends
> on `buildtool-dependency-classpath-spec`, since `cajeta.json` dependencies do
> not currently resolve at all.

- **3.10.1** When a font dependency in `cajeta.json` by provider, family,
  weight, style, and Unicode subset is declared, the build tool fetches it and
  makes it available to the chart library as a resolved asset — the same
  mechanism that already resolves `.cja` archives.
- **3.10.2** When a font is resolved, its **content hash is recorded in the
  lockfile**, and a later resolution producing different bytes is an error.
  Providers re-cut fonts without notice; unpinned fonts would break §14.2's
  golden-file tests with no visible cause.
- **3.10.3** When a font is already cached, no network access occurs — builds
  are offline after first resolution, and CI is not dependent on a third party
  being reachable.
- **3.10.4** When a chart is rendered, **no font is ever fetched**.
  `dev.cajeta.chart` requires no network capability (`settings.capabilities`),
  so charting stays a pure computation and cannot become a source of outbound
  traffic.
- **3.10.5** When a resolved font into my repository is vendored, that is
  supported and the build uses it without contacting anything — the
  reproducible-build path.
- **3.10.6** When a provider is targeted, the mechanism is not Google-specific:
  Google Fonts, Fontsource, a corporate font server, or a plain HTTPS URL plus
  hash are all expressible behind one provider abstraction.
- **3.10.7** When a font is resolved, its **licence is captured and PDF-
  embedding permission is verified**. A font whose licence forbids embedding
  fails the build with the reason named, rather than producing PDFs that cannot
  legally be distributed.
- **3.10.8** When a Unicode subset is requested, only that subset is fetched
  and stored, so a Latin-only chart does not carry CJK glyphs.
- **3.10.9** When a fetched font is malformed or fails its hash, it is rejected
  with a clear diagnostic and never partially parsed.
- **3.10.10** When resolution needs the network and it is unavailable or
  disabled, the failure names the missing font and the cache path — never a
  silent substitution, which would change every glyph and every layout without
  saying so.

---

## 4. Feature: L1 — scales and axes

Scales are the heart of D3's model: a mapping from data domain to visual range.

- **4.1** When a scale is created, linear, logarithmic, power/sqrt, time,
  ordinal, band, and point scales are available.
- **4.2** When a value is maped, the scale maps domain → range, and its inverse
  is available where meaningful — inversion is what makes §11's hover and brush
  possible.
- **4.3** When a value falls outside the domain, clamping behaviour is explicit
  rather than silently extrapolating.
- **4.4** When a scale to nice its domain is asked, endpoints round to human
  numbers.
- **4.5** When for ticks is asked, the result is human-readable values at a
  requested approximate count, and a log scale produces decade ticks rather
  than linear ones.
- **4.6** When a time scale is used, ticks fall on calendar boundaries —
  seconds, minutes, hours, days, months, years — via `cajeta.time`.
- **4.7** When an axis is rendered, ticks, labels, an optional title, and
  optional gridlines are drawn on any side, with recessive styling per §10.
- **4.8** When axis labels would collide, they are rotated, thinned, or
  abbreviated under a documented rule — never overplotted.
- **4.9** When tick labels is formated, SI prefixes, percentages, currency,
  fixed precision, and custom formatters are available.

---

## 5. Feature: L1 — marks and shape generators

- **5.1** When a line through points is generated, linear, step, basis,
  cardinal, and monotone interpolation are available, and monotone does not
  overshoot.
- **5.2** When data has gaps, a line breaks at missing values rather than
  interpolating across them — silently bridging a gap is a correctness bug, not
  a style choice.
- **5.3** When an area is generated, it supports a baseline or a lower bound,
  so confidence bands come free.
- **5.4** When series are stacked, stack, stream, and percentage offsets are
  available with a documented order.
- **5.5** When arcs are generated, pie and donut geometry with padding and
  corner radius are available.
- **5.6** When point markers is drawn, a symbol set (circle, square, triangle,
  diamond, cross, plus) is available at a specified size — the secondary
  encoding §12 depends on.
- **5.7** When a layout is needed, histogram binning, kernel density, contours,
  treemap, and hierarchical partition are available as data transforms
  independent of drawing.

---

## 6. Feature: L2 — figure, axes, and layout

- **6.1** When a figure is created, its size is set in physical units and a
  device pixel ratio, so output is resolution-independent.
- **6.2** When axes to a figure are added, it is possible to place them on a
  grid or at explicit fractional positions.
- **6.3** When automatic layout is used, margins, tick labels, titles, and
  legends are measured (via §3) and fitted so nothing is clipped and nothing
  overlaps.
- **6.4** When axes share an x or y scale, their ranges stay locked and
  interior tick labels are suppressed.
- **6.5** When subplots are composed, per-axes titles, a figure title, and a
  shared legend are all supported.
- **6.6** When a figure is annotated, text, arrows, spans, reference lines, and
  shaded regions are placeable in data or figure coordinates.
- **6.7** When content would overflow the figure, it is reported — silently
  clipped output is the most common charting bug in every library that permits
  it.

---

## 7. Feature: L3 — the declarative surface

The one-call layer, over a `nucleo.frame.Table`.

- **7.1** When a chart function with a table and column names for x, y, colour,
  size, and symbol is called, the result is a complete figure with scales,
  axes, and legend inferred.
- **7.2** When a column's type implies a scale, it is chosen automatically —
  numeric → linear, temporal → time, categorical → ordinal — and I can override
  it.
- **7.3** When a categorical column to colour is maped, each level takes the
  next categorical slot in fixed order per §10.
- **7.4** When a chart is faceted by a column, the result is a grid of small
  multiples with shared scales and one legend — seaborn's `FacetGrid`, the
  answer §10 gives for too many series.
- **7.5** When a statistical transform is applied, aggregation, binning,
  smoothing (LOWESS), regression fit with confidence band, and bootstrap error
  bars are available, computed through `cajeta.math.stats`.
- **7.6** When customization past the declarative API is needed, the underlying
  L2 figure is retrievable and work continues on it — the escape hatch is
  guaranteed, never a rewrite.

---

## 8. Feature: chart-type coverage

Parity target: the common matplotlib/seaborn/plotly surface.

- **8.1** *Distribution* — histogram, density (KDE), box, violin, strip, swarm,
  ECDF, rug.
- **8.2** *Comparison* — bar (grouped, stacked, horizontal), lollipop, dot
  plot, diverging bar.
- **8.3** *Relationship* — scatter, bubble, line, area, stacked area, step,
  hexbin, 2-D density, regression with band, pair grid, joint plot.
- **8.4** *Composition* — pie, donut, treemap, stacked percentage.
- **8.5** *Matrix* — heatmap, correlation matrix, clustered heatmap with
  dendrogram.
- **8.6** *Time* — time series with a temporal axis, candlestick, range band.
- **8.7** *Statistical / model* — residual plot, Q-Q plot, ROC curve,
  precision-recall curve, confusion-matrix heatmap, learning curve. *(These
  pair directly with `dev.cajeta.ml`; the PR curve is also `ml-classification-
  gaps-spec` §10.1.)*
- **8.8** *Not a chart* — a stat tile / hero number is a first-class output,
  because a single value should not be drawn as a one-bar bar chart.
- **8.9** When a chart type does not fit my data's shape, I am told at call
  time with the reason, not given a misleading picture.

---

## 9. Feature: rendering quality

- **9.1** When output is rasterized, marks are anti-aliased and geometry is not
  snapped in a way that shifts data positions.
- **9.2** When marks overlap, draw order is deterministic and controllable.
- **9.3** When at 2× or 4× is exported, text and marks scale as vectors before
  rasterization — never an upscaled bitmap.
- **9.4** When many points are drawn, rendering degrades gracefully in time,
  and the point count at which a scatter should become a hexbin or density plot
  is documented rather than discovered.

---

## 10. Feature: the design system — making elegance verifiable

This section is what makes §1.2 real. The method is design-system-agnostic: the
library ships a validated default palette and consumes any brand's parameters
unchanged.

### 10.1 Colour has four jobs, and each has one rule

- **10.1.1** When colour carries **identity**, a categorical palette is used,
  assigned in **fixed order, never cycled**.
- **10.1.2** When colour carries **magnitude**, a sequential ramp of **one hue,
  light → dark** is used. Rainbow ramps are not provided — they are
  perceptually non-monotonic and misread.
- **10.1.3** When colour carries **polarity**, a diverging pair of two hues
  with a **neutral grey midpoint** is used; a hue at the midpoint is rejected.
- **10.1.4** When colour carries **state**, a reserved status palette (good /
  warning / serious / critical) is used, never reused as a series colour, and
  always shipped with an icon or label rather than colour alone.
- **10.1.5** When a filter reduces the series count, surviving series keep
  their colours — **colour follows the entity, never its rank**.

### 10.2 The palette is validated by a runnable checker, not by eye

- **10.2.1** When a categorical palette is supplied, a validator checks
  lightness band, chroma floor, adjacent-pair separation under colour-vision
  deficiency, a normal-vision floor, and contrast against the surface — and
  reports pass/fail per check.
- **10.2.2** When an adjacent pair separates by less than the CVD threshold, it
  **fails**; between the floor and the threshold it passes only with a
  secondary encoding present (§12.2).
- **10.2.3** When a pair is indistinguishable to normal vision, it is a **hard
  failure** that secondary encoding does not excuse.
- **10.2.4** When a custom palette is shiped, the validator runs in CI — a
  palette is never merged on the strength of looking fine.
- **10.2.5** When more series than the palette has slots is needed, the
  overflow folds into "Other", facets, or a composite encoding — a ninth hue is
  **never** generated.

### 10.3 Marks and chrome

- **10.3.1** When marks are drawn, defaults are thin: 2 px lines, markers no
  smaller than 8 px, and rounded data-ends anchored to the baseline.-
  **10.3.2** When fills abut — stacked segments, adjacent bars — then a small
  surface-coloured gap separates them, and overlapping marks carry a surface-
  coloured ring.
- **10.3.3** When grid and axes are drawn, they are recessive; the data is the
  most prominent thing in the frame.
- **10.3.4** When values are labelled, labelling is selective — never a number
  on every point.
- **10.3.5** When text is drawn, it wears text colours, not
  the series colour; a colour swatch beside it carries identity.

### 10.4 Themes and dark mode

- **10.4.1** When a theme is selected, fonts, palette, surfaces, grid, and mark
  defaults change together and consistently.
- **10.4.2** When in dark mode is rendered, the palette steps are **chosen and
  validated against the dark surface** — never an inverted light palette, which
  is how dark mode goes wrong.
- **10.4.3** When a brand's ramps is supplied, the library snaps each palette
  slot to the nearest step that passes §10.2.

### 10.5 The one rule about axes

- **10.5.1** When trying to build a dual-axis chart (two y-scales in one
  frame), the library **refuses**. Two measures of different scale become two
  charts, small multiples, or an indexed common base. This is the single most
  misleading construct in charting and the library will not produce it.

---

## 11. Feature: interaction (HTML5)

- **11.1** When an HTML5 line or area chart is rendered, a crosshair and
  tooltip ship **by default** — an interactive medium should be interactive
  without asking.
- **11.2** When bars, dots, or heatmap cells is rendered, each mark has a hover
  tooltip with a hit target larger than the mark itself.
- **11.3** When a tooltip appears, it shows the series name, the x value, and
  the formatted y value, and never obscures the mark.
- **11.4** When zoom and pan is enabled, axes rescale live and a reset returns
  to the original view.
- **11.5** When brushing is enabled, a dragged region reports the selected data
  range.
- **11.6** When a legend entry is clicked, that series toggles and **the
  remaining series keep their colours** (§10.1.5).
- **11.7** When interaction is present, it is keyboard reachable and screen-
  reader announced — not mouse-only.
- **11.8** When rendering to a static backend, interaction is dropped cleanly
  and any information that lived only in tooltips is made visible instead.

---

## 12. Feature: accessibility

- **12.1** When a chart has two or more series, a legend is always present;
  with four or fewer, series are also direct-labelled, so identity never
  depends on colour alone. A single series needs no legend — the title names
  it.
- **12.2** When colour alone is insufficient, secondary encodings — marker
  symbol, dash pattern, and a texture fill — are available.
- **12.3** When a chart is produced, a **table view** of its underlying data
  can be emitted alongside it.
- **12.4** When HTML5 is rendered, the chart carries appropriate roles and
  labels and is navigable by keyboard.
- **12.5** When SVG is rendered, it carries a title and description for
  assistive technology.
- **12.6** When contrast against the surface falls below
  the threshold, the validator warns, and the warning obligates a visible label
  or table view — it is not dismissable.

---

## 13. Open questions (resolve at plan time)

- **13.1** *(resolved 2026-08-01 — full shaping, and it moves out.)* Cajeta
  builds a **complete shaping engine**: bidirectional text, Arabic joining,
  Indic reordering, CJK. It does **not** live here. Shaping is driven by
  **GSUB/GPOS**, which are OpenType tables, and every text consumer needs it —
  charts, PDF output (`cajeta-docs` §4.5), and `cajeta-font`'s own rendering.
  It therefore belongs in **`dev.cajeta.font`**, specified separately as
  `cajeta-text-shaping-spec`, exactly as HarfBuzz is separate from any renderer.
  §3 becomes a *consumer* of that engine rather than an implementation of it.
  **This spec's §3 must be rewritten to depend on it before any plan opens.**
- **13.2** *(resolved 2026-08-01 — CPU.)* Raster output uses a **CPU scanline
  rasterizer with analytic anti-aliasing**, not `cajeta.gfx`. Charts must render
  headless, in CI and in containers, with no GPU or driver present — which is
  precisely the case a GPU path cannot serve.
- **13.3** *(resolved 2026-08-01 — reuse deflate.)* The PNG encoder is built on
  `cajeta.wire.Compressor`'s deflate. Remaining action is verification, not
  decision: **confirm the stream is zlib-compatible** (header + Adler-32) before
  the encoder unit opens. If it is not, that is a small `cajeta.wire` addition,
  not a redesign.
- **13.4** *(resolved 2026-08-01.)* OKLab lands in **`cajeta.math.Color`**, not
  here — it is general colour science, and `stdlib-completion` §6 already owns
  the work.
- **13.5** *(resolved 2026-08-01 — inlined vanilla JS.)* HTML5 interactivity
  emits a **self-contained inlined script**: no framework, no CDN, no external
  fetch. WASM-backed interaction is not attempted in v1. A chart must render and
  behave correctly from a single file opened off disk.
- **13.6** *(resolved 2026-08-01 — split.)* L0–L2 are separable from L3, so a
  consumer that only draws does not acquire a `nucleo.frame` dependency. The
  split is at the L2/L3 seam.
- **13.7** *(resolved 2026-08-01 — phase it.)* This is the largest spec in the
  set and is **split into phases, each with its own plan and each shipping
  something usable**:

  | Phase | Contents | Depends on |
  |---|---|---|
  | **C1** | L0–L2 core: geometry, scales, layout, the text *interface* + SVG backend | `cajeta-text-shaping` |
  | **C2** | CPU raster + PNG (§13.2, §13.3) | C1 |
  | **C3** | L3 frame binding and the §8 chart catalogue beyond the v1 five | C1 |
  | **C4** | HTML5 interactivity (§13.5) | C1 |

  The v1 chart set stays deliberately narrow — line, bar, scatter, histogram,
  heatmap — and proves the backends before the catalogue widens. Shaping is no
  longer in this spec's critical path as an implementation, only as a
  dependency (§13.1).

---

## 14. Acceptance criteria (spec-level)

- **14.1** The same figure renders to SVG, PDF, PNG, and HTML5 with identical
  geometry, verified by comparing computed layout, not by eye.
- **14.2** Rendering is deterministic and byte-identical across runs, so output
  can be golden-file tested.
- **14.3** The default palette passes every §10.2 check in both light and dark
  mode, and the validator runs in CI.
- **14.4** A dual-axis chart cannot be constructed (§10.5.1), asserted by test.
- **14.5** Text metrics agree across backends (§3.3), asserted by test — this
  is the property that keeps layout correct everywhere.
- **14.6** Automatic layout produces no clipped or overlapping labels across a
  corpus of adversarial cases: long category names, dense time axes, many
  series, extreme aspect ratios.
- **14.7** Every §8 chart type renders correctly on an empty dataset, a single-
  point dataset, and a dataset containing missing values — the three inputs
  that break charting libraries.
- **14.8** A line chart breaks at missing values rather than interpolating
  across them (§5.2).
- **14.9** Every chart with ≥ 2 series emits a legend, and charts pass an
  automated accessibility check.
- **14.10** PDFs embed subset fonts and open correctly in a standards-compliant
  reader with no external font available.
