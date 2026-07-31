# compiler-jsonl — one machine-readable stream for compiler output

Related: `json-diagnostics`, `ide-symbol-index` (§2.0.2 xref stream),
`lint-server-spec` (§2 protocol), `json-viewer-spec` (the consumer),
`diagnostic-engine-spec`.

Status: **active** (approved 2026-07-31). Section 9 records the decisions taken.

## 1. Definition

### 1.1 Purpose
Give the compiler one machine-readable output format, so a consumer parses
records rather than sniffing them, and so anything the compiler wants to say
can be said structurally.

### 1.2 Problem
The format already exists — three times, in three shapes, plus two more in
neighbouring processes. `--diag-format=json` puts diagnostics, progress and
cache records on stderr as NDJSON today; the xref stream rides the same
channel; the lint server and the build-tool plugin runtime each speak their
own NDJSON dialect. They disagree on the three things a stream format has to
settle:

- **Self-description.** `progress`, `cache`, `xref`, and every plugin-runtime
  record carry `kind`. Diagnostics do not — they are recognised by *having* a
  `severity` field. Consumers therefore sniff: the IDE console derives a row's
  level as `fields["level"] ?: fields["severity"]`, probing field names because
  there is no discriminator to dispatch on.
- **Versioning.** The xref sub-stream carries `{major, minor}` and requires
  consumers to REFUSE an unknown major. Diagnostics, progress and cache carry
  no version at all. One careful sub-stream inside three unversioned ones.
- **What `kind` means.** The plugin runtime uses it for both record type
  (`finding`, `result`) and severity (`warn` alongside
  `log`+`level:"warn"`). Two axes on one field.

Coverage is the other half. Plenty of compiler output has no structured form
at all and reaches the IDE console as opaque text: `[jit-run] entry … returned
42`, `[step-armed] …`, `cajeta: --classpath read failed for …`, `cajeta jit:
dependency resolution failed: …`, `cajeta: compile finished`. These are
exactly the lines a structured console cannot filter, level-tint, or navigate
from.

### 1.3 Scope
The compiler's own output on its diagnostic channel: diagnostics, phase
progress, cache activity, xref records, and the currently-unstructured
narration listed above. The envelope is specified once and adopted by the
lint-server and plugin-runtime protocols where they overlap.

### 1.4 Constraints
- **1.4.1 Text stays the default and stays byte-identical.** JSONL is opt-in.
  A terminal user sees exactly what they see today.
- **1.4.2 Migration is additive.** The plugin ships separately from the
  compiler, and a new compiler must not break an installed plugin (nor the
  reverse). No field is removed or renamed in the same major that adds its
  replacement.
- **1.4.3 One record per line, flushed per record.** Already the discipline in
  `emitJsonDiagnostic` / `emitJsonProgress`: consumers read a pipe live, so a
  record must be visible when it happens, not at process exit.
- **1.4.4 The emitter never throws.** `ProgressPhase`'s destructor runs while a
  fatal diagnostic unwinds; a phase must still close.

### 1.5 Non-goals
- The debuggee's own stdout. A program's output (e.g. the tour's
  `JsonlEncoder`) reaches the console as DAP `output` events. The viewer
  renders both; they do not share a schema and this spec does not govern it.
- A new flag. `--diag-format=json` is the switch (§5).
- Replacing the DAP wire protocol, which is DAP's own spec.

## 2. The envelope

### 2.1 Requirements
- **2.1.1** Every record is a single-line JSON object carrying a `kind` string.
  No record is identified by the presence or absence of a payload field.
- **2.1.2** `kind` names the record TYPE only. Severity, level and status are
  separate fields. `{"kind":"warn"}` is not a record type; a warning is
  `{"kind":"diagnostic","severity":"warning"}`.
- **2.1.3** The stream opens with one `{"kind":"stream", …}` record carrying
  `{"major":M,"minor":N}` and the producer's identity. It is emitted before
  any other record, including on an otherwise-empty stream, so a consumer can
  distinguish "nothing to report" from "garbled output" — the property the
  xref stream already guarantees for itself.
- **2.1.4** A consumer that does not recognise the MAJOR refuses the whole
  stream rather than guessing at its contents, and says so once. Inherited
  from the xref rule, which is the strongest convention in the tree.
- **2.1.5** An unknown `kind` within a recognised major is SKIPPED, not fatal.
  That is what makes adding a record kind a minor bump.
- **2.1.6** Unknown fields on a known kind are ignored, for the same reason.

### 2.2 Use cases
- **2.2.1** As a console renderer, when I read a line, then I dispatch on
  `kind` in one pass and never guess from field presence.
- **2.2.2** As a plugin built against major 1, when I meet a major-2 stream,
  then I refuse it and surface one notification, instead of rendering a
  half-understood stream.
- **2.2.3** As a compiler author adding a record kind, when I ship it, then
  existing consumers skip it and keep working, and I bump only the minor.
- **2.2.4** As a consumer of an empty run, when the compile produced nothing,
  then I still receive a `stream` record and can tell success from breakage.

## 3. Record kinds

### 3.1 Requirements
- **3.1.1 `diagnostic`** — the existing payload (`severity`, `code`,
  `message`, `file`, `line`, `column`) gains `kind`. Every existing field keeps
  its name and meaning (1.4.2).
- **3.1.2 `progress`** — `phase`, `state` (`start`|`finish`), `label`,
  `elapsedMs` on finish. Already this shape; it keeps it.
- **3.1.3 `cache`** — `state`, `artifact`. Already this shape.
- **3.1.4 `xref`** — `rel`, `record`. Already this shape. Its private version
  record folds into the stream record (2.1.3) at the next major.
- **3.1.5 `log`** — the narration that has no structured form today, with
  `level` (`info`|`warn`|`debug`) and `message`. This is where `[jit-run] …`,
  `[step-armed] …`, `cajeta: compile finished` and the classpath/dependency
  failure messages go.
- **3.1.6 `result`** — terminal record: `status` (`ok`|`error`) and a
  `message` on error. Borrowed from the plugin runtime, where it already
  distinguishes a logical failure from a crash.

### 3.2 Use cases
- **3.2.1** As a developer with the JSON console open, when a build fails on a
  classpath error, then it appears as a record with a level and is caught by
  the level filter — not as untyped text.
- **3.2.2** As a developer, when a run ends, then one `result` record tells me
  whether it succeeded, without inferring it from exit code plus silence.
- **3.2.3** As the IDE, when a phase starts and finishes, then I can show live
  build progress with real elapsed times.

## 4. Adoption by the neighbouring protocols

### 4.1 Requirements
- **4.1.1** The lint-server protocol (`lint-server-spec` §2) keeps its
  request/response framing — `id` correlation and the `done` marker are the
  right shape for a request/response channel and this spec does not disturb
  them. Its *payload* records adopt §2 and §3.
- **4.1.2** The build-tool plugin runtime SHOULD adopt the envelope and drop
  the duplicated severity-as-kind records (`warn` becomes `log`+`level:"warn"`;
  `finding` keeps its richer shape but aligns field names with `diagnostic`
  where they mean the same thing). Deferred to a follow-up (9.3) — recorded
  here so the target shape is not re-derived later.
- **4.1.3** Channel split is unchanged and explicit: structured records on the
  designated channel, free-form text on the other. Neither protocol starts
  interleaving the two.

### 4.2 Use cases
- **4.2.1** As a plugin author, when I read compiler output and lint-server
  output, then the records look the same and one parser serves both.

## 5. The flag

### 5.1 Requirements
- **5.1.1** `--diag-format=json|text` remains the single switch; text is the
  default. No new flag (1.5).
- **5.1.2** The flag means the SAME thing for every verb. It does not today:
  `dispatchJitRun` accepts `--diag-format=json` but only exports
  `CAJETA_DIAG_FORMAT` for the runtime's uncaught-throw emitter, and never
  enables JSON progress. `jit-run`, `dap`, `--lint`, `--lint-server` and a
  plain compile all honour it identically.
- **5.1.3** Turning it on turns on every record kind. Progress already rides
  the diagnostic format switch deliberately; that stays the rule.

### 5.2 Use cases
- **5.2.1** As a developer at a terminal, when I build without the flag, then
  output is exactly what it is today, byte for byte.
- **5.2.2** As a tool author, when I pass `--diag-format=json` to any verb,
  then I get the same stream shape and do not have to learn per-verb quirks.

## 6. Migration

### 6.1 Requirements
- **6.1.1** Major 1 is what ships today plus `kind` on diagnostics and the
  `stream` record. Both are additive: an existing consumer that ignores
  unknown fields keeps working unchanged.
- **6.1.2** The plugin reads `kind` when present and falls back to the field
  probe when absent. The fallback is PERMANENT, not a one-release bridge: the
  console also renders third-party JSONL that has no `kind` and never will —
  cajeta-logging's own output is `{"ts":…,"level":"INFO","logger":…,"msg":…}`.
  What `kind` buys is principled dispatch for records the COMPILER emits, not
  the removal of tolerance for everyone else's.
- **6.1.3** Removals (xref's private version record, plugin-runtime's `warn`)
  land in major 2, not alongside their replacements.

### 6.2 Use cases
- **6.2.1** As a developer with a new compiler and a stale plugin, when I
  build, then diagnostics still render.
- **6.2.2** As a developer with a stale compiler and a new plugin, when I
  build, then diagnostics still render.

## 7. Non-functional

- **7.1** Per-record cost stays negligible against a compile; the format is
  string concatenation and one flush, as today.
- **7.2** Records are deterministic for identical input — same order, same
  content — so output is diffable across runs.
- **7.3** No absolute machine paths in emitted records beyond what the existing
  `--debug-prefix-map` rules already produce (reproducibility, external-debug
  §3.1.3).

## 8. Acceptance

- **8.1** Every emitted record carries `kind`, verified by a test that parses a
  full JSON-mode run of a real project and asserts no record lacks it.
- **8.2** A text-mode run is byte-identical to the pre-change binary's, pinned
  by a golden comparison.
- **8.3** A consumer fed an unknown major refuses the stream; one fed an
  unknown `kind` skips it and keeps parsing. Both tested directly.
- **8.4** The IDE console dispatches on `kind` for compiler records, and still
  renders `kind`-less third-party JSONL through the field probe (6.1.2).
- **8.5** Every ad-hoc `std::cerr` narration site listed in §1.2 has a
  structured form under the flag.

## 9. Decisions (with Julian, 2026-07-31)

- **9.1** Version rides a stream-opening `{"kind":"stream","major":M,
  "minor":N,"producer":"…"}` record, not a per-record `"v"` field. Matches
  what xref and lint-server already do, and costs one line per run. A consumer
  that joins mid-stream is not a case we have; if it becomes one, adding `v`
  later is a minor bump under 2.1.6.
- **9.2** `log` carries the original text verbatim. `[step-armed]` and
  `[jit-run]` are debugger-internal narration; nobody queries their fields, so
  typed records for them would be schema surface with no consumer.
- **9.3** Plugin-runtime realignment (§4.2) is a FOLLOW-UP, out of scope here.
  It is a separate process boundary with its own consumers and its own
  compatibility story; folding it in widens the blast radius without helping
  the IDE console. §4.1 (lint-server payload records) stays in scope — it
  shares the compiler's own emitter.
- **9.4** Every verb emits exactly one terminal `result`, last. "Did it work"
  should be answerable from the stream alone, never inferred from an exit code
  plus silence — the failure mode this whole spec exists to remove.
