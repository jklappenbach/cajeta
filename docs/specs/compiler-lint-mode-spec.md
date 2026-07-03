# Compiler `--lint` single-file diagnostics mode — spec

## 1. Definition

### 1.1 Purpose
Add a compiler mode, `cajeta --lint <file> [--diag-format=text|json]`, that runs the
diagnostic passes over **one** source file and reports the diagnostics, without a full
build (no codegen, no linking, no artifact emission, no entry-method resolution).

### 1.2 Problem
The IDE plugin's editor-annotation tier (`CajetacRunner` →
`CajetaLintAnnotator`) spawns `cajeta --emit=ir --diag-format=json <file>` with a single
positional argument. The compiler's full-compile entry requires **three** positionals
(`<entry-method> <source-root> <archive-root>`; `src/main.cpp:521`), so the invocation
falls through to the usage banner and exits 1 — producing **zero** diagnostics. The
editor never shows a squiggly. (The Build-tool-window tier is unaffected: it drives the
build tool, which supplies the three positionals.)

There is no existing single-file / lint entry point in the compiler; the plan's intended
`cajetac --lint --json` (`ide-plugins/idea/Plan.md`) was never built.

### 1.3 Scope
- A new CLI verb/flag `--lint <file>` handled in `src/main.cpp` before the
  three-positional compile path.
- A single-file diagnostic run: load the stdlib, parse `<file>`, run the
  semantic / prototype / validation / dependency-graph passes that emit diagnostics,
  then stop.
- Honor `--diag-format=text|json` exactly as the full compile does (text default;
  json = one NDJSON object per diagnostic on stderr, per
  `docs/CompilerModes.md § --diag-format`).
- Switch `CajetacRunner` to invoke `cajeta --lint <tempfile> --diag-format=json`.

### 1.4 Non-goals (v1)
- **Cross-file project resolution.** Lint sees only the stdlib and the declarations
  *within* `<file>`. A reference to a type declared in a sibling project file will
  report a false "unresolved type" diagnostic. Documented limitation; a future
  `--lint <file> --source-root <root>` (resolve against the whole project, report only
  for `<file>`) is the follow-up.
  - **Follow-up is a diagnostics refactor, not a flag.** Semantic diagnostics today are
    `throw cajeta::Exception(msg, code)` — single-shot (the first aborts the compile),
    caught once at top level and emitted with **no file/line**. `--source-root` only
    pays off once semantic diagnostics **collect-and-continue** and **carry a
    location** (so context-file errors don't abort the target's lint and diagnostics can
    be scoped to `<file>`). That rework is the real follow-up effort; syntax
    diagnostics (precise, multi-error, crash-safe) are unaffected and are what this
    mode delivers well.
- **Codegen / emit.** No IR, object, `.cja`, or executable output; no linking.
- **Entry-method resolution.** `--lint` takes no entry method and never resolves one.
- **New diagnostic *kinds*.** Lint surfaces exactly the diagnostics the existing passes
  already produce; it does not add lint rules.

## 2. CLI surface

### 2.1
As a tool author, when I run `cajeta --lint path/to/File.cajeta`, then the compiler
parses and diagnoses that one file and prints diagnostics in **text** form, exiting
non-zero iff any error diagnostic was reported.

### 2.2
As the IDE plugin, when I run `cajeta --lint <file> --diag-format=json`, then every
diagnostic is emitted as one NDJSON object per line on stderr
(`{severity,code,message,file,line,column}`) with no free text interleaved, exiting
non-zero iff any error was reported.

### 2.3
As a user, when I pass `--lint` with **no** file, or a path that does not exist, then
the compiler prints a clear error (not the generic three-positional usage banner) and
exits non-zero.

### 2.4
As a user, when I combine `--lint` with build-only flags (`--emit`, an entry method,
extra positionals), then `--lint` wins: the extra positionals are ignored and no
artifact is produced. (`--lint` is a distinct mode, like `archive`/`jit-run`.)

## 3. Diagnostic pipeline

### 3.1
As the compiler, when linting `<file>`, then I load the stdlib module, parse `<file>`
as a single module, and run the passes through dependency-graph resolution
(`Compiler.cpp`: `ensureStdlibModule` → parse → `validatePlaceholders` →
`buildPendingPrototypes` → `resolveAdviceMatches` → `resolveDependencyGraph`), then
stop before the Phase-1/2 codegen loop.

### 3.2
As a user, when `<file>` has a **syntax** error, then a `code:"syntax"` diagnostic with
a 1-based line/column is reported and the compiler exits non-zero **without crashing**
(the syntax-error visit guard from the JSON-diagnostics work applies here too).

### 3.3
As a user, when `<file>` has a **semantic** error resolvable within the file + stdlib
(e.g. an unresolved stdlib-shaped type, an invalid assignment), then the corresponding
diagnostic (its existing `code`/message) is reported and the compiler exits non-zero.

### 3.4
As a user, when `<file>` is clean (parses and resolves against stdlib + its own
declarations), then **no** diagnostics are emitted and the compiler exits 0.

## 4. Plugin integration

### 4.1
As `CajetacRunner`, when I lint a staged buffer, then I invoke
`cajeta --lint <tempfile> --diag-format=json` (dropping `--emit=ir` and the bare-file
positional) and parse the NDJSON via the existing `JsonDiagnosticParser`.

### 4.2
As `CajetaLintAnnotator`, when the compiler reports diagnostics for the buffer, then
each becomes an editor annotation at the precise `line`/`column` range, exactly as the
already-shipped `parseDiagnostics` path expects (no plugin-side parsing change).

### 4.3
As `CajetacRunner`, when the file resolves cleanly, then no annotations appear; when the
compiler is missing/misconfigured, the tier degrades silently (returns no annotations),
as today.

## 5. Output contract

### 5.1
Exit code: 0 iff no **error**-severity diagnostic was reported; non-zero otherwise
(warnings/notes alone do not fail). Matches the full-compile convention.

### 5.2
`--diag-format=json`: stderr is pure NDJSON (every non-empty line begins with `{`); no
usage banner, no `cajeta:` free-text line, no codegen output on stdout.

### 5.3
`--diag-format=text` (default): human-readable diagnostics only; no NDJSON.
