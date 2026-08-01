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
- **1.3.1** RESOLVED (Julian, 2026-07-28): binary JSON ("jsonb") can wait —
  it is the plan's FINAL unit. The codec seam + MessagePack/CBOR land there;
  the concrete build-emitted format is confirmed when that unit starts. The
  console's role in run/debug sessions is the focus.
- **1.3.2** JSON5 full support vs JSONC-only (comments + trailing commas).
  Drafted: JSONC-level leniency reading; full JSON5 deferred.
- **1.3.3** RESOLVED (Julian, 2026-07-28): the JSON view is an IN-PLACE
  toggle on the session console (default ON for cajeta configurations,
  available elsewhere). Implementation note: a card wrapper AROUND the real
  platform ConsoleView — raw view IS the platform console, untouched;
  structured is the table; the toggle flips cards (the shipped
  JsonlConsolePanel's exact discipline), so platform console behaviors are
  never reproduced by hand.

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
  trailing JSON object after a plain-text prefix (a logger prefix, ANSI
  color codes) renders the prefix verbatim and the JSON part structured.
  ANSI escapes are tolerated anywhere. A trailing array stays a raw
  passthrough — the record row model is object-shaped; revisit if a real
  feed emits array tails.

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
- **3.1.1** The structured view attaches to any run/debug console as an
  IN-PLACE toggle: one console surface whose toolbar button flips between
  the untouched platform ConsoleView and the structured table (card
  wrapper), both fed live from the same process stream (the existing
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

- **3.1.6** **Diagnostic row navigation**: a structured row carrying source
  coordinates (file/line fields, as the compiler's NDJSON diagnostics and
  lint output emit) is clickable and navigates to the location, like a
  console file link — the payoff of tools encoding output as JSONL.

- **3.1.7** **Column selection.** The table opens on a SUBSET of the
  discovered fields — the first few of the deterministic column order
  (preferred log keys first) — so a wide record shape does not present as an
  unreadable wall of columns. A **Fields** control lists every field
  discovered so far and toggles each column on or off.
  - **3.1.7.1** The available list is discovery-driven and grows with the
    stream: a field first seen at line 10,000 joins the chooser with no
    reload, and the list never shrinks within a session.
  - **3.1.7.2** Until the reader makes an explicit choice, the visible set is
    the default RECOMPUTED over everything discovered so far, so a stream
    whose first lines carry only metadata does not fix a poor column set.
    The first explicit toggle pins the selection; fields discovered after
    that are listed but stay off until chosen.
  - **3.1.7.3** Deselecting every field is allowed and renders a single
    line-text column, so raw passthrough rows stay visible (3.1.4).
- **3.1.8** **No column width ceiling.** No column is clamped to a maximum
  width: a column sizes to its widest rendered cell and the table scrolls
  horizontally, so a long `message` is read in full rather than clipped to
  the viewport. Widths only ever grow as content arrives — a streaming
  console never reflows narrower under the reader.
  - **3.1.8.1** Width is measured over RECORD cells only. A raw passthrough
    row's text renders in the first data column and carries a full-text
    tooltip, but does not stretch that column — one long stack-trace line
    must not push every structured column off-screen.

- **3.1.9** **Layout persists per run/debug profile.** A reader who has set
  up the table once should not set it up again. The chosen columns, their
  ORDER, and any width they resized by hand are remembered against the
  run/debug configuration the console belongs to, and are the starting
  layout for that configuration's next session.
  - **3.1.9.1** The key is the run/debug configuration, not the project or
    the IDE: a build console and a debug session of the same project keep
    separate layouts, because they carry different record shapes.
  - **3.1.9.2** A remembered layout is authoritative over the defaults of
    3.1.7: it arrives pinned, so a field the reader switched off stays off
    even when later records carry it.
  - **3.1.9.3** Only widths the reader set by DRAGGING are remembered.
    A column the viewer sized to content is not pinned by that, so
    content-sizing (3.1.8) keeps working on the columns nobody has touched.
  - **3.1.9.4** A remembered column that a later run never emits is kept in
    the layout, not dropped — the run that lacks it may be the anomaly, and
    silently forgetting it would make the setting feel unreliable.
  - **3.1.9.5** Corrupt or unreadable stored layout degrades to the 3.1.7
    defaults. Layout is a convenience; it never blocks a console.

### 3.2 Use cases
- **3.2.1** As a developer debugging the tour, I flip the running console to
  JSON view and watch structured rows stream; flipping back loses nothing.
- **3.2.2** As a developer, I filter to `level >= warn` during a noisy run
  and clear the filter afterwards; raw rows reappear.
- **3.2.3** As a developer whose build fails, I click the structured
  diagnostic row and land on the offending line.
- **3.2.4** As a developer watching a wide record shape, I get a few readable
  columns rather than every field, then open **Fields** and switch on `file`
  once I care about it — including a field that only showed up mid-run.
- **3.2.5** As a developer reading a long `message`, I scroll right and see
  all of it instead of an ellipsis at the viewport edge.
- **3.2.6** As a developer who set up `level`, `message`, `file` in that
  order with a wide `message` column, when I debug that configuration again
  tomorrow, then the console opens exactly that way without my touching it.
- **3.2.7** As a developer who tuned my debug console, when I run a
  different configuration, then its console is untouched by my choices.

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
- **4.1.6** The editor's table view carries the same column chooser and width
  rule as the console (3.1.7, 3.1.8). Across paged windows the available
  field list ACCUMULATES — paging forward may reveal fields the first window
  never had, and paging back does not retract them.

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
