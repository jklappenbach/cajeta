# json-viewer — spec

## 1. Definition

One JSON viewing/editing capability across the IDE plugin's two surfaces:

- **Console** — run/debug output where loggers emit JSONL and tools (the
  cajeta compiler's NDJSON diagnostics, `--lint-server`, cajeta-mcp) emit
  line-structured JSON, interleaved with plain unstructured text.
- **Editor** — the main editor panel, for JSON-family files selected from the
  build tree or project: plain JSON documents, JSONL/NDJSON, lenient dialects,
  and binary JSON encodings — to view *and edit*.

**Prior art (reused, not rebuilt):** the buildtool-widget work shipped
`dev.cajeta.idea.jsonl`: `JsonlEngine` (pure line parser, records + raw
passthrough), `JsonlConsoleController`/`Panel` (streaming line assembly,
structured table / raw toggle, level + field filters), `JsonlWindowReader`
(bounded window over 1M-line files), `JsonlFileType` (`jsonl;ndjson;
jsonlines`) and `JsonlFileEditorProvider` (structured tab beside the text
editor). This spec generalizes that package; its unfinished live-acceptance
item (buildtool-widget 9.3, structured-during-run) is absorbed here.

### 1.1 Scope
- Format family: strict JSON documents, JSONL/NDJSON, lenient dialects
  (JSONC/JSON5 read), binary JSON encodings behind a decoder seam.
- Mixed console streams: JSON lines render structured, everything else
  passes through verbatim — nothing is ever dropped.
- The structured surface attachable to ANY run/debug console, not only the
  buildtool tool window.
- Editing: text formats in place; structured (tree/table) editing that writes
  back; binary formats decode → edit → re-encode on save.

### 1.2 Non-goals
- No JSON Schema validation/completion (the platform's JSON editor already
  covers text-level tooling for plain `.json`).
- No log persistence/indexing service — the viewer renders what a stream or
  file provides.
- No new stdlib codec work; the viewer's binary decoders are plugin-side
  (Kotlin). A cajeta-side binary emitter is separate future work.

### 1.3 Open items for Julian's review
- **1.3.1** "jsonb": no binary JSON codec exists in the stdlib today
  (`cajeta/codec` has json/csv/base64; `cajeta/wire` names MessagePack/CBOR/
  Protobuf as example formats only). The spec provides a pluggable decoder
  seam with **MessagePack and CBOR** as the first two decoders; confirm which
  concrete format(s) the build will actually emit, and its file extension.
- **1.3.2** JSON5 full support vs JSONC-only (comments + trailing commas).
  Drafted: JSONC-level leniency reading; full JSON5 deferred.
- **1.3.3** Console auto-detection default: ON for cajeta run configurations,
  opt-in toggle for arbitrary ones. Confirm.

---

## 2. Format family

### 2.1 Requirements
- **2.1.1** A **document model** shared by all surfaces: a JSON value tree
  (object/array/scalar) with source spans for text formats; the JSONL row
  model (`JsonlRow.Record|Raw`) remains for line streams.
- **2.1.2** Strict JSON documents (`.json` with objects/arrays of any depth)
  parse into the tree model; parse errors show the offending line/column and
  degrade to the raw text view, never a blank panel.
- **2.1.3** JSONL/NDJSON as today (one value per line, raw passthrough for
  non-JSON lines) via the existing engine.
- **2.1.4** Lenient read: `//`/`/* */` comments and trailing commas (JSONC)
  parse in documents and lines; the editor writes back what the user edited
  without stripping comments it did not touch.
- **2.1.5** Binary JSON via a **decoder seam**: `BinaryJsonCodec { detect
  (bytes/extension), decode(bytes) -> tree, encode(tree) -> bytes }`.
  First codecs: MessagePack, CBOR. Registration is data-driven so a future
  cajeta wire format plugs in without viewer changes.
- **2.1.6** Mixed console lines: a line that is not pure JSON but contains a
  trailing JSON object/array after a plain-text prefix (a logger prefix, ANSI
  color codes) renders the prefix verbatim and the JSON part structured.
  ANSI escapes are tolerated anywhere.

### 2.2 Use cases
- **2.2.1** As a developer, when my program logs JSONL to stdout, each line
  appears as a structured row (expandable), with plain printlns interleaved
  verbatim in order.
- **2.2.2** As a developer running `cajeta build --diag-format=json`, the
  NDJSON diagnostics render structured in the console.
- **2.2.3** As a developer opening a MessagePack/CBOR file from the build
  tree, I see the decoded tree, not hex garbage.
- **2.2.4** As a developer opening a malformed `.json`, I see the text editor
  with the parse error located — never a dead structured tab.

---

## 3. Console surface

### 3.1 Requirements
- **3.1.1** The structured view attaches to any run/debug console: a console
  toggle ("JSON view") switches between the platform console and the
  structured surface, fed live from the same process stream (the existing
  controller's incremental line assembly).
- **3.1.2** On for cajeta run/debug/build configurations by default;
  available via the toggle elsewhere (1.3.3).
- **3.1.3** Log ergonomics (existing, kept): level filter, field-substring
  filter, structured/raw toggle; plus level-based row coloring (error/warn
  tinted like the platform console).
- **3.1.4** Ordering and completeness: rows appear in stream order; a partial
  trailing line materializes only when complete; NOTHING is dropped —
  unparseable lines are raw rows.
- **3.1.5** Throughput: a build spraying tens of thousands of lines must not
  freeze the EDT (batched appends; the table virtualizes).

### 3.2 Use cases
- **3.2.1** As a developer debugging the tour, I flip the running console to
  JSON view and watch structured rows stream; flipping back loses nothing.
- **3.2.2** As a developer, I filter to `level >= warn` during a noisy run
  and clear the filter afterwards; raw rows reappear.

---

## 4. Editor surface

### 4.1 Requirements
- **4.1.1** File types: `.json` (structured tab added beside the platform
  editor), `.jsonl/.ndjson/.jsonlines` (existing), `.jsonc`/`.json5` (read-
  lenient), binary extensions per registered codec (1.3.1). The structured
  tab is PLACE_AFTER_DEFAULT_EDITOR, as today.
- **4.1.2** Tree view for documents: expand/collapse, path breadcrumb,
  find-by-key/value; table view for a top-level array of flat objects
  (columns from the engine's column inference).
- **4.1.3** **Editing** in the structured view: scalar cells edit in place;
  add/remove of array elements and object keys; edits write back to the text
  document (text formats) preserving untouched formatting where feasible, or
  re-encode on save (binary formats). The platform text editor remains the
  editing surface of record for text formats — the structured editor never
  fights it (single source: the Document).
- **4.1.4** Binary files: the text tab shows a read-only hex/summary stub;
  the structured tab is the editor. Save re-encodes through the codec;
  a decode→encode round-trip without edits is byte-stable for the shipped
  codecs, or the file is marked read-only with the reason shown.
- **4.1.5** Large files: the windowed reader path (existing) for JSONL;
  documents above a size threshold open read-only structured with a banner.

### 4.2 Use cases
- **4.2.1** As a developer, I open a `.jsonl` log from `.cajeta/logs`, sort
  by the level column, edit nothing, and the file is untouched.
- **4.2.2** As a developer, I open a config `.json`, flip to the structured
  tab, change a scalar, and the text document updates (undo works, VCS diff
  is minimal).
- **4.2.3** As a developer, I open a `.msgpack` fixture, correct one field,
  save, and re-opening shows the change.

---

## 5. Robustness

### 5.1 Requirements
- **5.1.1** No surface ever faults on malformed input: worst case is the raw
  view with an error locator.
- **5.1.2** The console surface never reorders or drops output relative to
  the plain console.
- **5.1.3** Binary decode is bounds-checked; a truncated/corrupt file yields
  the error banner, never a partial silent tree.
- **5.1.4** All engine/controller/codec logic is pure Kotlin, unit-tested
  headless; Swing layers stay thin delegates (the existing package's
  discipline).
