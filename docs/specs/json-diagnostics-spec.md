# Structured (JSON) diagnostics end-to-end (spec)

## 1. Definition

### 1.1 Purpose
Make the compiler's machine-readable diagnostics (`--diag-format=json`, NDJSON on
stderr — landed in commit `56e7d646`) usable by the IDE plugin **end-to-end**:
forward the flag through the **build tool** to its compiler subprocesses, and have
the plugin **consume NDJSON** in both diagnostic tiers (real-time lint and the
Build tool window) so problems carry precise severity/code/file/line/column
instead of being regex-scraped from free text.

### 1.2 Problem
- `--diag-format=json` is a **compiler** flag. The build tool
  (`src/cajeta/buildtool`) spawns the compiler (`BuildAction`, via
  `/proc/self/exe`) but never adds the flag, so `cajeta <task>` still prints
  human text. The plugin's **build-window** path runs the build tool → no JSON.
- The plugin's **lint** path runs the compiler directly but still regex-scrapes
  stderr (`CajetacRunner.WARNING_RE`), a long-standing degraded-mode TODO.
- The **build-window** problem parser (`BuildProblemParser`) is regex over three
  brittle text shapes; two shapes carry no path, so navigation is partial
  (idea-build-toolwindow spec §4).

### 1.3 The NDJSON contract (already emitted by the compiler)
One object per line on stderr, streamed:
```
{"severity":"error"|"warning"|"note","code":<str|null>,"message":<str|null>,
 "file":<str|null>,"line":<int|null>,"column":<int|null>}
```
`code`/`message`/`file` are null when empty; `line`/`column` are 1-based, null
when ≤ 0 (`Diagnostics.h::emitJsonDiagnostic`).

### 1.4 Constraints / non-goals
- **1.4.1** Don't change the NDJSON schema — consume it as emitted.
- **1.4.2** Text mode stays the default everywhere; JSON is opt-in via the flag,
  so nothing regresses for humans at a terminal.
- **1.4.3** The build tool must **pass the compiler's stderr through unchanged**
  in JSON mode (it already inherits it) so NDJSON lines aren't reformatted.
- **1.4.4** **JSON-only — no regex fallback.** The plugin always passes
  `--diag-format=json`, so every *diagnostic* arrives as NDJSON; the legacy regex
  diagnostic parsers (`CajetacRunner.WARNING_RE`, `BuildProblemParser`'s text
  shapes) are **removed**. A stderr line that is not NDJSON is not a diagnostic —
  it stays as plain console output and is never turned into a problem (the parser
  returns null for it).

## 2. Build-tool forwarding
- **2.1** As a tool user, when I run `cajeta <task> --diag-format=json` (or
  `cajeta compile/build … --diag-format=json`), then every compiler subprocess
  the task spawns is invoked with `--diag-format=json` and its NDJSON reaches my
  stderr unmodified.
- **2.2** As a tool user, when I omit the flag (or pass `--diag-format=text`),
  then behaviour is exactly as today (human text) — the default is text.
- **2.3** As a tool user, when a task runs several build actions, then all of
  their compiler invocations honour the one flag (it's a build-invocation option,
  not per-action).
- **2.4** The flag is accepted wherever a task/build is launched from the CLI
  (`run <task>`, the `build`/`compile` builtins) and rejected with usage on a bad
  value, mirroring the compiler's own `--diag-format` parse.

## 3. Plugin — shared NDJSON parser
- **3.1** As the plugin, when I read a stderr line that is a valid NDJSON
  diagnostic, then I parse it into a structured record (severity, code, message,
  file, line, column) using the existing `lint/Diagnostic.Severity` vocabulary
  (`error→ERROR`, `warning→WARNING`, `note→WEAK_WARNING`).
- **3.2** As the plugin, when a line is not valid NDJSON, then the parser returns
  null and the line is treated as plain console output, not a diagnostic
  (no regex fallback — constraint §1.4.4).
- **3.3** As the plugin, when a diagnostic's `line`/`column` are present, then
  they are carried as 1-based (converted at each consumer to that surface's
  convention).

## 4. Plugin — lint tier (real-time editor)
- **4.1** As a developer editing a `.cajeta` file, when the compiler reports a
  diagnostic, then it appears as an editor annotation at the **precise range**
  (from NDJSON line/column), not a whole-line degraded guess.
- **4.2** As a developer, when the compiler emits a warning with a `code`, then
  the annotation carries that code as its rule id (Problems view grouping).
- **4.3** `CajetacRunner` invokes the compiler with `--diag-format=json` and
  parses NDJSON only; the old `WARNING_RE`/`parseStderr` regex path is removed.

## 5. Plugin — build-window tier
- **5.1** As a developer running a build-family task, when the compiler reports a
  diagnostic, then it appears as a Build-tool-window problem with precise
  severity and, when `file` is present, **navigation to that file:line:column**.
- **5.2** As a developer, when a JSON error is reported, then the build still
  finishes **failure** (severity `error` ⇒ build-failing), consistent with the
  existing rule (idea-build-toolwindow spec §4.5).
- **5.3** `CajetaBuildWindowLauncher` passes `--diag-format=json` to the build
  tool; `BuildProblemParser` is rewritten to consume NDJSON only (its three regex
  shapes + the stateful 2-line pairing are removed — JSON is one line per
  diagnostic). Non-NDJSON stderr stays as console output.
