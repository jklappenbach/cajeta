# cajeta-docs — the document model: parsing, structure, granularity, and text processing

## 1. Definition

### 1.1 Purpose

Cajeta cannot read a document or process text. `cajeta.codec` has Base64, CSV,
and JSON; `cajeta.search` does typo-tolerant *key* lookup; `cajeta.lang.String`
has case folding but no Unicode normalization. There is no PDF reader, no HTML
parser, no XML parser, no tokenizer, and no notion of a document at all.

`dev.cajeta.docs` is **the** text and document library: a structured document
model, readers that populate it from real formats, and the text processing that
turns any level of that structure into something a model or an application can
consume.

### 1.2 The organizing idea — one model, many granularities

A document is not a string. It has **pages**, **sections**, **paragraphs**,
**sentences**, **lines**, **tokens**, and **elements** (images, charts, tables,
figures, equations). Every consumer wants a different level:

| Consumer | Granularity wanted |
|---|---|
| RAG chunking | paragraphs or sections, with provenance |
| Sentence embedding | sentences |
| Summarization | sections |
| Layout analysis / citation | pages and lines |
| Model input | tokens |
| Figure extraction | elements |
| Content-based recommendation (`ml-recsys` §8) | whole-document vectors |

The library's job is to parse once into one model, then serve **any** of those
without re-parsing. Discarding structure at read time is the defect that cannot
be undone later.

### 1.3 Scope

The document model; format readers, including **per-language source-code
processors** (§4.7); extraction and structure recovery; outbound reference
extraction (§4.11); metadata
and provenance; text processing (normalization, tokenization, sentence
segmentation); vectorization; chunking; subword tokenization for models.

This spec supersedes and absorbs the earlier `cajeta-doc-ingest` and
`cajeta-text` drafts.

### 1.4 Non-goals

- **1.4.1** **Retrieval.** BM25, inverted indexing for search, hybrid
  retrieval, reranking, and context assembly are `cajeta-rag`. This library
  *understands and decomposes* text; RAG *finds* it.
- **1.4.2** **Embeddings.** Dense vectors come from `cajeta-ml-v3`. §9 provides
  the sparse/lexical side and the tokenizer that feeds a model.
- **1.4.3** **OCR.** Scanned pages are detected and reported (§5.7), never
  silently yielding empty text. OCR may arrive later behind the same interface.
- **1.4.4** **Stemming and lemmatization.** Deferred — see §11.4.
- **1.4.5** POS tagging, parsing, NER, sentiment, language detection.
- **1.4.6** Document *writing*. `cajeta-chart` writes PDF; this reads it. They
  share font machinery, not code paths.
- **1.4.7** Layout-faithful rendering. Content and structure, not appearance.

### 1.5 Systems

`cajeta.io`, `cajeta.wire` (deflate — ZIP-based office formats),
`cajeta.codec` (JSON, CSV, **XML** — §4.4), `cajeta.lang.String` (**Unicode
normalization** — §7.1), `cajeta.hash` (content addressing),
`cajeta.collection`, `cajeta.nucleo.sparse.CsrMatrix` (document-term matrices),
`dev.cajeta.font` (PDF text extraction — §1.6), `dev.cajeta.unit`.

### 1.6 Prerequisites that do not exist

- **1.6.1 An XML parser** in `cajeta.codec`. DOCX/PPTX/XLSX/EPUB are ZIP
  archives of XML; `codec` has JSON but no XML. Its own spec.
- **1.6.2 `cajeta-font`** for PDF. Recovering text means mapping glyph codes
  back to Unicode through the reversed `cmap` and `ToUnicode` CMaps
  (`cajeta-font` §7.5). **PDF is the hardest reader by a wide margin.**
- **1.6.3 Unicode normalization** on `cajeta.lang.String`. It has
  `toLowerCase`/`trim` but no NFC/NFD. This is *string correctness*, not NLP —
  composed and decomposed forms of identical text must compare equal — and
  belongs in stdlib, consumed here (§7.1).

---

## 2. Feature: the document model

- **2.1** When a document is parsed, the result is a **tree** of typed nodes —
  document, section, paragraph, sentence, list, list item, table, code block,
  element — not a flat string.
- **2.2** When physical layout is needed, **pages** and **lines** are available
  as a *second, orthogonal* view over the same content. A paragraph may span
  pages and a sentence may span lines, so physical layout cannot be a parent in
  the logical tree without lying about one or the other.
- **2.3** When any node is addressed, it has a stable identifier, and its
  parent, children, and siblings are reachable.
- **2.4** When a document is asked for a granularity — sections, paragraphs,
  sentences, lines, tokens, elements — the result is that sequence **without
  re-parsing**, in document order.
- **2.5** When holding any node, it is possible to resolve its **source
  location**: page number, character range, and section path (§6).
- **2.6** When a format does not express a level (plain text has no pages; a
  spreadsheet has no sentences), that granularity is reported **absent** rather
  than fabricated.
- **2.7** When a node's text is read, the result is it with a stated policy for
  whitespace, hyphenation across line breaks, and included descendants.

---

## 3. Feature: elements — the non-text parts

- **3.1** When a document contains an **image**, it is a node with its
  position, dimensions, alt text, and caption where available, and its bytes
  are retrievable.
- **3.2** When a document contains a **table**, it is structured — rows,
  columns, headers, spans — and convertible to a `nucleo.frame.Table`, not
  flattened into whitespace-mangled prose.
- **3.3** When a document contains a **chart or figure**, it is an element with
  its caption and, where the format provides it (SVG in HTML/DOCX), its
  underlying content.
- **3.4** When a document contains an **equation**, it is an element retaining
  its source form (LaTeX, MathML, OMML) rather than mangled text.
- **3.5** When a document contains **footnotes or endnotes**, they are nodes
  linked to their reference site, not spliced inline where they corrupt
  sentence segmentation.
- **3.6** When an element cannot be interpreted, its presence and position are
  still recorded — a chunk should be able to say a figure was there.

---

## 4. Feature: format readers

- **4.1** When a document is opened, the reader is chosen by **content sniffing
  first, extension second** — extensions lie.
- **4.2** When **plain text or Markdown** is read, headings, lists, code
  blocks, and tables become structure rather than literal punctuation.
- **4.3** When **HTML** is read, boilerplate — navigation, headers, footers,
  scripts, styles — is separated from main content and the heading hierarchy
  survives. Extracting a page's chrome as content is the most common ingestion
  defect. Its `href` and `src` targets are extracted as **outbound references**
  (§4.11) with the document's base URI recorded, since resolving them needs a
  corpus this library does not have.
- **4.4** When **DOCX, PPTX, or XLSX** is read, the ZIP is opened, its XML
  parsed (§1.6.1), and paragraphs, headings, tables, slides, and sheets
  recovered.
- **4.5** When **PDF** is read, text is extracted in reading order with page
  numbers retained, via §1.6.2's font machinery.
- **4.6** When **CSV or JSON** is read, existing `cajeta.codec` readers are
  reused and rows or records become addressable nodes — no second CSV parser.
- **4.7** When **source code** is read, a **language processor** recovers its
  structure — definitions (functions, methods, classes, types), their spans,
  their signatures, attached doc comments, references, and imports — as nodes in
  the §2 model, with lines first-class throughout. Text with a language label is
  not sufficient: `cajeta-rag` §11 builds a symbol index and a dependency graph
  from exactly this extraction.
- **4.7.1** When a source file **does not parse** — a syntax error, a partial
  edit, an unsupported dialect — the processor **recovers and returns what it
  could**, reporting the failure. Real repositories contain files that do not
  compile; refusing them loses the corpus.
- **4.7.2** When a language has no processor, it falls back to text with
  line-level provenance rather than failing (`cajeta-rag` §11.3.4).
- **4.7.3** When **Cajeta** source is read, the processor uses the compiler's
  own front end rather than approximating it.
- **4.8** When a format is added, it implements one reader interface that
  populates the §2 model — every stage downstream is format-agnostic.
- **4.9** When a document is malformed, truncated, or hostile, the reader fails
  with a diagnostic naming the document and offset, and never loops forever or
  exhausts memory. **Document input is untrusted by definition**; every reader
  is an attack surface.
- **4.10** When a document is encrypted or password-protected, that is reported
  distinctly from "no text found".
- **4.11** When a document references others — an HTML `href`, a Markdown link,
  a source `import` — those targets are extracted as **typed outbound
  references** carrying the location that produced them. They are recorded
  **unresolved**: this library sees one document at a time, and only a corpus
  knows what a reference points at (`cajeta-rag` §10.2).

---

## 5. Feature: extraction quality

- **5.1** When a PDF has multiple **columns**, reading order follows the
  columns rather than scanning across them. Column-interleaved text is the
  classic PDF failure and must be tested explicitly.
- **5.2** When headers, footers, or page numbers repeat across pages, they are
  detectable as such and can be excluded rather than polluting every chunk.
- **5.3** When a word is **hyphenated across a line or page break**, it is
  rejoined — otherwise tokenization produces two non-words.
- **5.4** When a paragraph spans a page break, it is one paragraph in the
  logical tree while both pages appear in its layout view (§2.2).
- **5.5** When reading order is ambiguous, the heuristic is documented and its
  confidence exposed.
- **5.6** When a document has no explicit sections, a hierarchy is inferred
  from heading levels, and inference is distinguishable from what the format
  stated.
- **5.7** When a PDF has no extractable text layer (a scan), it is reported as
  **requiring OCR** — never an empty string presented as success, which
  silently produces an empty index that looks like it worked.

---

## 6. Feature: metadata and provenance

- **6.1** When a document is parsed, title, author, dates, language, and page
  or section count are captured where the format provides them.
- **6.2** When holding **any node at any granularity**, it is possible to
  resolve which document it came from and where — page, section path, character
  range. Without this a downstream system cannot cite, and an uncitable answer
  is indistinguishable from a fabricated one.
- **6.3** When the same document twice is parsed, a content hash identifies it,
  so corpora can be deduplicated.
- **6.4** When a document changes, re-parsing yields a new version identifiable
  against the old, so stale derivatives can be retired.
- **6.5** When a own metadata is attached, it rides through to every node
  derived from that document.

---

## 7. Feature: text normalization

- **7.1** When text is normalized, Unicode normalization is applied to a
  **stated form** via `cajeta.lang.String` (§1.6.3), and case folding is
  explicit rather than assumed.
- **7.2** When text contains ligatures, soft hyphens, or zero-width characters,
  they are decomposed or removed — otherwise identical text fails to match
  itself.
- **7.3** When a stopword list is applied, a default English list ships, its
  contents are **documented**, and a custom list can be supplied or the default
  disabled. Stopword lists differ between libraries and silently change
  results.

---

## 8. Feature: tokenization and sentence segmentation

- **8.1** When text is tokenized, the result is terms under a **documented,
  testable** rule for case, punctuation, and whitespace, with regex and
  whitespace tokenizers available and a custom rule supplyable.
- **8.2** When text is tokenized, each token carries its **offset range** in
  the source, so any match traces back to its position (§6.2).
- **8.3** When **sentences** are segmented, abbreviations (`Dr.`, `Fig.`, `et
  al.`), decimals (`3.14`), ellipses, and quotations do not produce false
  breaks. This is genuinely hard and is where naive split-on-period
  implementations fail; §12.4 makes it an acceptance test.
- **8.4** When a sentence spans lines or a page break, it is one sentence
  (§2.2, §5.4).
- **8.5** When **n-grams** is extracted, word n-grams over a range are
  supported.
- **8.6** When the same text twice is tokenized, the result is identical
  output.

---

## 9. Feature: vectorization

- **9.1** When a **vocabulary** over a corpus is fitted, each term maps to a
  stable index and back, with document-frequency and top-`k` bounds available.
- **9.2** When an unseen document is transformed, unknown terms are ignored and
  **the vocabulary does not grow** — a vectorizer whose vocabulary drifts
  produces vectors of changing width.
- **9.3** When a **count** vectorizer is fitted, the result is a sparse
  document-term matrix over `CsrMatrix`.
- **9.4** When a **TF-IDF** vectorizer is fitted, the term-frequency and
  inverse-document-frequency variants are **named explicitly**. scikit-learn's
  defaults are *not* the textbook formula — it smooths IDF and L2-normalizes
  rows — and silently picking one variant produces disagreement nobody can
  locate.
- **9.5** When at a chosen granularity is vectorized, it is possible to
  vectorize documents, sections, paragraphs, or sentences — the §2.4 property
  paying off.
- **9.6** When a large corpus is transformed, it stays sparse throughout —
  densifying a document-term matrix is how text pipelines exhaust memory.
- **9.7** When a fitted vocabulary or vectorizer is persisted, it saves and
  reloads; refitting changes every index and invalidates stored matrices.

---

## 10. Feature: chunking and model input

- **10.1** When a document is chunked, fixed-size, sentence, paragraph,
  section, and structure-aware strategies are available — each a *selection
  over the §2 model*, not a re-parse.
- **10.2** When structure-aware chunking is used, splits prefer section and
  paragraph boundaries and **never split mid-table or mid-code-block**.
- **10.2.1** When **source code** is chunked, splits fall on definition
  boundaries — function, method, class, or impl block — and never mid-body. The
  natural unit is a property of the language: one public class per file for Java
  and C#, function and impl-block boundaries for C, Go, Rust, Python, and
  JavaScript (`cajeta-rag` §11.2.2).
- **10.2.2** When a code chunk is emitted, it carries its enclosing context —
  file, module or namespace, enclosing type, and signature — the code analogue
  of §10.5's heading path.
- **10.2.3** When a **doc comment** precedes a definition, it is chunked with
  it. Separating a function from the prose describing it destroys the best
  lexical match for a natural-language query about that function.
- **10.3** When an overlap is set, adjacent chunks share that much context, so
  a fact spanning a boundary survives in at least one.
- **10.4** When a chunk is produced, it carries its §6.2 provenance. A chunk
  without provenance is not a valid output.
- **10.5** When a section carries heading context, a chunk can be emitted with
  its heading path prepended, so an isolated chunk stays interpretable.
- **10.6** When a single indivisible element exceeds the chunk size, the policy
  is explicit — split with a marker, or emit oversized — never silently
  truncated.
- **10.7** When the same document with the same settings is chunked, the result
  is identical chunks, so any derived index is reproducible.
- **10.8** When a published **subword** vocabulary is loaded, BPE and WordPiece
  tokenizers reproduce that model's tokenization, including `##` continuation
  and special tokens.
- **10.9** When subword tokenization disagrees with the reference **by one
  token**, that is a defect — embedding lookups are positional, so one extra
  token misaligns every downstream vector.
- **10.10** When chunking by token count, tokens can be **counted without
  materializing** their strings, and truncation past a model's maximum length
  is explicit.

---

## 11. Feature: corpora

- **11.1** When a directory or archive is processed, it is walked and each
  document routed to its reader, with per-document failures isolated — one bad
  PDF must not abort a 10,000-document corpus.
- **11.2** When a corpus is processed, a report lists what succeeded, what
  failed and why, and what needed OCR.
- **11.3** When documents are independent, processing parallelizes using
  existing `cajeta.concurrent` machinery.
- **11.4** When a document is very large, it streams rather than loading whole
  where the format permits.
- **11.5** When results are emited, nodes at any granularity can become a
  `nucleo.frame.Table` — text plus provenance columns — so downstream stages
  consume an ordinary frame.

---

## 12. Open questions (resolve at plan time)

- **12.1** *(resolved 2026-08-01 — full breadth, including PDF.)* v1 ships the
  whole reader set: plain text, Markdown, HTML, CSV/JSON, source code, the
  OOXML family, and **PDF**. Sequencing within that is unchanged and is now
  load-bearing rather than advisory — text/Markdown/HTML/CSV/JSON first (no new
  prerequisites), then OOXML once `cajeta.codec`'s XML parser lands (§1.6.1),
  then **PDF last**, after `cajeta-font`.

  **Consequence, accepted:** this puts two other specs on this one's critical
  path. `cajeta-codec`'s XML parser and `cajeta-font` must both complete before
  docs can finish, and PDF is described in §4.5 as the hardest reader by a wide
  margin. Docs is therefore a **late** deliverable, not an early one, and
  anything depending on it inherits that.
- **12.2** *(resolved 2026-08-01 — dual view.)* §2.2 keeps both the logical
  tree and the physical layout. Collapsing pages and lines into attributes on
  logical nodes forces a lie for any table or paragraph spanning a page
  boundary.
- **12.3** *(resolved 2026-08-01.)* §9 vectorization stays **here**. TF-IDF is
  turning text into vectors, and `ml-recsys` §8 needs it for content-based
  filtering with no retrieval involved. BM25 goes to rag, since scoring a query
  against a corpus is finding, not understanding.
- **12.4** *(resolved 2026-08-01.)* §10.8 subword tokenizers live **here**,
  with the vocabulary supplied by the consumer. They are tokenization; the
  coupling is to a vocabulary file, not to a model implementation.
- **12.5** *(resolved 2026-08-01 — accept for v1.)* Stemming stays deferred.
  Lexical retrieval is measurably worse without it, so this is revisited on
  evidence: `cajeta-rag` §8.3's evaluation is the measurement, and Porter or
  Snowball is purely additive when it lands.
- **12.6** *(resolved 2026-08-01 — rules.)* Sentence segmentation is
  rule-based in v1: testable, predictable, and no model to ship. The seam stays
  open for a trained model later.
- **12.7** *(resolved 2026-08-01 — no.)* This library does **not** implement
  `dev.cajeta.ml`'s `Transformer` protocol and takes no dependency on ml. A thin
  adapter ships **with ml** (or with the consumer), wrapping a docs vectorizer
  where `Pipeline` composition is wanted. Inverting this would make the text
  library depend on the ML library, and `recsys` and `rag` would inherit it.

---

## 13. Acceptance criteria (spec-level)

- **13.1** Every node at every granularity resolves provenance back to document
  and location (§6.2) — the property citation depends on.
- **13.2** One parse serves every granularity without re-parsing (§2.4).
- **13.3** A paragraph spanning a page break is **one** paragraph, and both
  pages appear in its layout view (§2.2, §5.4).
- **13.4** Sentence segmentation survives an adversarial fixture —
  abbreviations, decimals, ellipses, quotations, and a sentence spanning a page
  break (§8.3).
- **13.5** A scanned PDF reports "needs OCR" and never returns empty text as
  success (§5.7).
- **13.6** Multi-column PDF text extracts in column order against a known-good
  fixture (§5.1).
- **13.7** Tables survive as tables and round-trip to a `nucleo.frame.Table`
  (§3.2).
- **13.7.1** A code chunk never splits mid-definition (§10.2.1), asserted for
  every shipped language processor rather than a representative one.
- **13.7.2** A source file with a deliberate syntax error still yields the
  definitions that precede it, and the failure is reported (§4.7.1).
- **13.7.3** Outbound references are extracted unresolved, with their source
  locations, from HTML, Markdown, and source code (§4.11).
- **13.8** Chunking never splits a table or code block, and repeated runs are
  identical (§10.2, §10.7).
- **13.9** Subword tokenization matches its reference **token-for-token** on a
  published vocabulary (§10.9).
- **13.10** TF-IDF pins against scikit-learn 1.9.0 **with each variant switch
  exercised** (§9.4) — matching only on defaults would hide the formula
  differences.
- **13.11** Malformed, truncated, and adversarial inputs fail with diagnostics
  — no hangs, no unbounded memory (§4.9). **Fuzz the readers.**
- **13.12** A corpus containing failing documents completes, and the report
  names every failure (§11.1, §11.2).
- **13.13** Text extracted from different formats compares equal after
  normalization (§7.1) — the Unicode-form hazard.
