# Plugin output protocol

**Status:** draft · authored 2026-08-23
**Owner:** Julian
**Plan:** `agents/plugin-output-protocol-plan.md`

## 1. Definition

A build-tool plugin communicates with the build tool over a line-oriented
record stream: `log`, `warn`, `finding`, `output`, `write`, `result`, `error`.
The build tool ships a typed plugin API — `ActionContext`, `ActionResult`,
`Finding`, `Severity` — and **stops short of the output channel**. Emission is
unshipped, so every plugin author writes the wire format by hand.

`dev.cajeta.coverage` does exactly that:

```cajeta
static void log(String message) {
    System.stdout.println("{\"kind\":\"log\",\"level\":\"info\",\"message\":\""
        + message + "\"}");
}
```

No escaping. A message containing `"`, `\` or a newline emits malformed JSON —
a Windows path or a quoted compiler diagnostic is enough. The build tool then
fails the parse or silently mis-reads the record, and the plugin appears broken
for a reason nothing reports.

The structured channel therefore exists and is discarded twice: once by plugins
that build it as text, once by the build tool, which flattens every record to
`[plugin] <text>` for the console regardless of `--diag-format`.

**This spec makes the shipped API the only way a plugin emits, and makes plugin
output a first-class citizen of the diagnostic stream.**

### 1.1 Guiding principle

*A plugin should not know the wire format exists.* Every property this protocol
claims — escaping, required fields, a terminating result — is a property the
plugin cannot violate because it never spells the record itself.

### 1.2 Provenance is not the plugin's to claim

Every record carries the identity of who produced it, and **the producer does
not supply that identity** — the build tool stamps it from the plugin whose
subprocess emitted the record. A plugin has no API for the field and cannot
write it: an origin appearing in a plugin's own output is overwritten, never
trusted.

**The identity is the plugin's Olla library key** (resolved 2026-08-23) — the
same string the manifest declares and the resolver resolves, e.g.
`dev.cajeta.coverage`. That choice is what makes a diagnostic traceable end to
end: from a line in the build output, to the `plugins` entry that asked for it,
to the published artifact it came from. An identity the build tool invented, or
one derived from the action name, would break that chain.

This is what makes §5 safe. Once plugin findings enter the diagnostic stream
they look like diagnostics *because they are* — but a reader, human or machine,
can always answer two questions:

1. Did the compiler say this, or a plugin?
2. **Which** plugin?

Neither answer depends on the plugin's cooperation.

### 1.3 Scope

| In | Out |
|---|---|
| Shipped emitter in `cajeta.buildtool.plugin` | The plugin *invocation* protocol (stdin request shape) |
| Non-forgeable record provenance (§1.2) | Signing or authenticating the plugin artifact itself |
| Additive schema extension for `source` + `output`/`write` kinds (§5.1) | Restructuring the diagnostic schema or bumping its major |
| Record validation + diagnostics for malformed input | New record kinds beyond those listed in §1 |
| `--diag-format=json` on the plugin path | Compiler diagnostic schema changes |
| Diagnostic-style rendering of findings in text mode | IDE-side presentation (consumes what this emits) |
| Migrating `dev.cajeta.coverage` onto the API | Other published plugins (none hand-roll emission today) |
| coco's `warn` coverage level (§7.1) | Other coco thresholds (`min-score`, mutation gates) |

## 2. The emitter ships with the API

`PluginHost` moves into `cajeta.buildtool.plugin` beside `ActionResult`, and
serializes through one writer that escapes correctly.

**Use cases**

1. A plugin logs progress; the message contains a `"` and a `\` and the record
   is still well-formed.
2. A plugin reports a finding by constructing a `Finding`, never a string.
3. A plugin sets an output key whose value contains a newline.
4. A plugin author writes no JSON at any point.
5. A second plugin (not coco) adopts the API and inherits every guarantee
   without copying code.

## 3. Result is guaranteed

**Use cases**

1. An action returns without calling `result`; the runtime emits one from the
   returned `ActionResult`, and the task sees a well-formed completion.
2. An action returns a failing `ActionResult`; the emitted result carries the
   error message.
3. An action calls `result` explicitly; exactly one result is emitted, not two.

## 4. Validation, and what happens to bad input

Nothing published breaks. A plugin that emits a malformed record — including
`dev.cajeta.coverage` 0.5.2 in the wild — keeps working.

**Use cases**

1. A record is not valid JSON: warn **once**, naming the plugin, drop the
   record, continue the action.
2. A record is valid JSON with an unknown `kind`: same treatment.
3. A record is missing a required field for its kind: same treatment.
4. A plugin writes raw non-JSON text to stdout: accepted, wrapped as a `log`
   record. `printf` debugging keeps working.
5. Repeated malformed records from one plugin do not repeat the warning.
6. The warning **names the offending line** (resolved 2026-08-23), so the
   author can see what was actually emitted rather than only that something
   was.

### 4.1 Echoing the offending line safely

The offending line is bytes the plugin controls, and it is by definition
malformed. Reporting it must not let it damage the stream it is reported in —
a bad record that breaks the diagnostic output would be the same class of
failure this spec exists to remove, one layer up.

**Use cases**

1. A malformed line containing `"` and `\` is quoted into a JSON diagnostic and
   the diagnostic stream stays parseable.
2. A malformed line containing control characters or a raw newline is escaped
   rather than reproduced, so it cannot forge a second record or a second
   console line.
3. A very long malformed line is truncated with an explicit marker; a plugin
   emitting a megabyte of garbage cannot flood the build log.
4. A malformed line containing invalid UTF-8 is reported without corrupting
   the output encoding.

## 5. JSON mode — one stream, one grammar

Under `--diag-format=json`, plugin output joins the diagnostic stream rather
than forming a parallel one.

| Record | Becomes |
|---|---|
| `finding` | a diagnostic, at the finding's severity, **attributed to the emitting plugin** (§1.2) |
| `warn` | a `warning` diagnostic |
| `log` | a `note` diagnostic |
| `output` | its own record kind — structural, not a message |
| `result` | its own record kind — structural, not a message |
| `write` | its own record kind — structural, not a message |

**Use cases**

1. A consumer that already parses compiler diagnostics shows coco's findings
   with no new parsing, and can tell they came from a plugin.
1a. A consumer filters to compiler-only diagnostics, or to one plugin's, using
   the provenance field alone.
1b. A plugin emits a record claiming to originate from the compiler, or from a
   different plugin; the claim is discarded and the true origin recorded.
1c. A reader takes the identity from a diagnostic and finds the `plugins` entry
   in `cajeta.json` that declared it, without translation.
2. A finding carrying file/line/column produces a navigable diagnostic.
3. A finding with no location produces a diagnostic with no location rather
   than a fabricated one.
4. `output` and `result` remain machine-readable as themselves — they are not
   messages and must not be flattened into text.
5. Text mode is unaffected by anything in this section.

## 5.1 The diagnostic schema, extended

Checked against `specs/schemas/compiler-jsonl.schema.json` on 2026-08-23. Most
of what §5 needs is already there:

| Need | Status |
|---|---|
| Severity mapping | `["error","warning","note"]` — exactly the three §5 uses, no lossy mapping |
| Unlocated finding | `file`/`line`/`column` optional; only `kind`/`severity`/`message` required |
| Adding fields | `additionalProperties` unset on every record — extension is not breaking |
| Version gate | `stream` carries `major`/`minor`; consumers can already refuse an unknown major |

**The gap is provenance.** No record carries who produced it: `diagnostic` is
`kind, severity, code, message, file, line, column`; `log` is `kind, level,
message`; `result` is `kind, status, message`. §1.2 is therefore not expressible
in the schema as it stands, which makes this a prerequisite for §4 rather than a
detail of it.

### 5.1.1 `source` on every record

*Resolved 2026-08-23.* Every record the stream carries names its originator in
the data — **compiler records included**, not only plugin ones. Provenance
stated, never inferred from absence.

The value is the plugin's **name**, which is the same string as its Olla library
key and its `plugins` entry (`PluginSpec.name`, e.g. `dev.cajeta.coverage`) —
there is no separate internal id to choose between. Compiler-produced records
name the compiler.

**Name and version are separate fields** (resolved 2026-08-23): `source` carries
the name, `sourceVersion` the version — `"cajeta"` / `"0.23.2"`, not
`"cajeta-v0.23.2"`.

Concatenation was considered and rejected on three grounds:

1. **No unambiguous split.** Plugin names contain dots and versions contain
   hyphens, so `dev.cajeta.coverage-v1.0.0-rc1` has no reliable delimiter. Every
   consumer would re-implement a parse that breaks on prereleases.
2. **Filtering and grouping.** Use case 3 below is "group a stream by
   originator". Concatenated, that means parsing every line to strip a version,
   and grouping across versions means matching a prefix.
3. **Version comparison.** A separate field can be ordered semantically; a
   substring of a display string cannot.

### 5.1.1a Three versions, three jobs

The versions in play are easy to conflate, and only one of them helps a reader
parse an old trace:

| Version | Where | Job |
|---|---|---|
| **Schema** | `stream.major` / `minor` — **already exists** | Tells a consumer how to interpret the records. "Refuse the stream if unrecognised." This is the parse-help. |
| **Producer** | `sourceVersion` (new, per record) | Which build made this claim — reproducibility and bug attribution, not parsing. |
| Producer name | `source` (new, per record) | Who made it. |

`stream.producer` (`"cajeta 0.10.0"`) already exists and stays: it is
**per-stream** provenance, and the stream is no longer single-producer. It says
who ran the build; `source` says who said each line. A display string may still
join name and version for humans — the per-record data does not.

Absence-means-compiler was considered and rejected. It is cheaper — no change to
any emitted compiler record — but it makes provenance a default rather than a
fact, so a consumer reading a record with no `source` cannot distinguish "the
compiler said this" from "this came from a producer that predates the field".
That ambiguity is exactly what §1.2 exists to remove.

**Use cases**

1. Every record in a `--diag-format=json` stream carries `source`, whoever
   produced it.
2. A compiler diagnostic and a plugin finding on the same file and line are
   told apart by `source` alone.
3. A consumer groups a whole stream by originator without heuristics.
4. A record whose `source` is absent is treated as unknown provenance — not
   silently attributed to the compiler.

### 5.1.2 Schema changes

| Change | Kind |
|---|---|
| `source` (producer name) added to `diagnostic`, `log`, `result`, and every other record kind | additive |
| `sourceVersion` (producer version) added alongside it | additive |
| `output` and `write` record kinds added (§5 keeps them structural; the compiler stream has no such kinds today) | additive |
| `minor` bumped, `major` unchanged | non-breaking |

Declared **optional** in the schema while being **always emitted** in practice:
that keeps every existing consumer validating, while every record this build
produces states its origin. Requiring it would be a `major` bump and would
invalidate readers for no gain the emitter does not already deliver.

## 6. Text mode — findings look like diagnostics

Progress output is unchanged: `[plugin] coco: [3/6] instrumenting …`. Findings
are not progress and stop being rendered as such.

**Use cases**

1. A finding prints in the compiler's `file:line: severity:` form so the IDE
   Build window makes it clickable, and **names the plugin** so it is never
   mistaken for compiler output — a compiler diagnostic and a plugin finding
   are visually distinguishable at a glance.
2. A finding with no location prints without a location prefix, still
   attributed.
2a. Two plugins reporting findings in one task are told apart by the reader.
3. `check-tour.sh`, which asserts on finding text, is updated in step with the
   change rather than discovering it.

## 7. coco migrates

**Use cases**

1. `dev.cajeta.coverage` deletes its `PluginHost` and calls the shipped one.
2. The tour produces byte-identical results through the new path — 6 of 10
   modules, 12 tests, 36.0% lines (18/50), 2/3 mutants killed.
3. `cajeta cover --diag-format=json` emits findings a diagnostic consumer can
   read.
4. coco republishes; the previous version keeps working under §4.

### 7.1 The coverage gate becomes severity-bearing

*Resolved 2026-08-23.* coco's gate migrates from a failing `result` to
**findings**, and gains a warning level alongside the failing one:

| Config | Severity | Effect |
|---|---|---|
| below `min` | `error` | task fails (§7a) — today's behaviour, new mechanism |
| below `warn` | `warning` | **recorded, task succeeds** |
| at or above both | — | no finding |

One mechanism instead of two, and the number lands in the diagnostic stream
where a CI consumer reads it directly rather than inferring from an exit code.

The warning level is what makes a ratchet workable: set `warn` above `min` and
a project sees coverage drift *before* the build starts failing on it, which is
the point at which the drift is still cheap to fix. A gate that only ever fails
gets raised once and then avoided.

**Use cases**

1. Coverage below `min`: an `error` finding, the task fails, the finding names
   the actual and required percentages.
2. Coverage between `min` and `warn`: a `warning` finding, the task **succeeds**,
   and the finding is recorded in both output formats.
3. Coverage at or above `warn`: no finding.
4. `warn` set below `min`: rejected as configuration nonsense rather than
   silently producing a warning that can never fire without the error also
   firing.
5. Only `min` set (every project today): unchanged behaviour — fails below it,
   silent above it.
6. Only `warn` set: reports drift and never fails, which is a legitimate
   configuration for a project not yet ready to gate.

## 7a. An `error` finding fails the task

*Resolved 2026-08-23.* A finding at `error` severity fails the task that
produced it. This is the general rule; coco's `min` coverage gate is one
instance of it — a build below the configured floor must fail.

Severity therefore becomes load-bearing rather than decorative, and a plugin
must mean it. coco's own findings span the range: dead code is informational,
a surviving mutant is a warning, a coverage floor breach is an error.

**Use cases**

1. A plugin emits an `error` finding; the task fails and names the finding.
2. A plugin emits `warning` and `note` findings only; the task succeeds and the
   findings are reported.
3. A plugin emits several findings, one of them `error`; the task fails and
   **all** of them are still reported — failing must not truncate the report
   that explains why.
4. A task fails on an error finding in `--diag-format=json`; the failure is
   visible in the diagnostic stream, not only in the exit code.
5. An error finding attributes the failure to its plugin (§1.2), so a failing
   build says which plugin failed it.

## 8. Success criteria

| # | Criterion |
|---|---|
| 8.1 | No plugin in tree spells a JSON record by hand. |
| 8.2 | A message containing `"`, `\`, newline and a non-ASCII character round-trips intact. |
| 8.3 | `--diag-format=json` yields findings as diagnostics from a plugin-driven task. |
| 8.4 | A malformed record warns once and does not fail the build. |
| 8.5 | An action that never calls `result` still produces one. |
| 8.6 | The tour's numbers are unchanged end to end. |
| 8.7 | Every record a plugin produces is attributed to that plugin, in both formats. |
| 8.8 | A plugin that writes an origin field of its own does not change the recorded origin. |
| 8.9 | An `error` finding fails its task; a `warning` finding does not. |
| 8.11 | coco's gate emits an `error` finding below `min` and a `warning` finding below `warn`; only the former fails the task. |
| 8.10a | Every attributed record names the plugin by its name — the same string as its Olla key and its manifest `plugins` entry. |
| 8.10b | EVERY record in a json stream carries `source` and `sourceVersion`, compiler records included; none relies on absence to mean anything. |
| 8.10c | A consumer filters and groups by producer without parsing a composite string, and can order two `sourceVersion` values. |
| 8.10 | A malformed record is quoted back safely: the diagnostic stream stays parseable, control characters are escaped, and an overlong line is truncated. |

## 9. Risks

| Risk | Response |
|---|---|
| Text rendering of findings breaks consumers pinned to the current strings | Only `check-tour.sh` is known to be; it is updated in §7. The IDE classifier keys on the `[plugin] ` prefix, which progress lines keep. |
| Findings-as-diagnostics could let a plugin fabricate compiler errors | **Closed by §1.2.** Provenance is stamped by the build tool from the plugin it invoked, not read from the record, so a plugin cannot claim to be the compiler or another plugin. Severity still comes from the `Finding`. |
| A published plugin's malformed output becomes newly visible as warnings | Intended. It is already malformed; today it fails silently. |

## 10. Decisions taken during authoring

Every open question raised while drafting was settled with Julian on
2026-08-23. None is left hanging, and each is recorded where it applies rather
than only here.

| # | Question | Resolution | Where |
|---|---|---|---|
| 1 | How far does this go beyond coco? | One plugin contract for all plugins; the shipped API is the only way to emit | §1, §2 |
| 2 | What does a JSON consumer receive? | Findings, logs and warnings become diagnostics; output/result/write stay structural | §5 |
| 3 | What happens to published plugins emitting bad records? | Validate, warn once, drop — nothing in the wild breaks | §4 |
| 4 | May a plugin write raw text? | Yes, wrapped as a log record | §4 |
| 5 | Does text mode change? | Progress unchanged; findings render diagnostic-style **and named** | §6 |
| 6 | What if a plugin never calls `result`? | The runtime emits one on its behalf | §3 |
| 7 | Can a plugin forge compiler output? | No — provenance is stamped by the build tool, never read from the record | §1.2 |
| 8 | Does an `error` finding fail the task? | Yes | §7a |
| 9 | Does the malformed-record warning name the line? | Yes, escaped and truncated so it cannot damage the stream reporting it | §4, §4.1 |
| 10 | What is the stamped identity? | The plugin's Olla library key | §1.2 |
| 11 | Does coco's `min` gate migrate to a finding? | Yes, and gains a non-failing `warn` level | §7.1 |
