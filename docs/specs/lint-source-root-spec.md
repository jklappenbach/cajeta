# `cajeta --lint --source-root` — project-context resolution — spec

## 1. Definition

### 1.1 Purpose
Extend the single-file lint mode (`docs/specs/compiler-lint-mode-spec.md`) with an
optional `--source-root <root>` so a linted file's references to **sibling project
files** — types, methods, fields — resolve, eliminating the false "unresolved" squiggles
that single-file lint produces. Diagnostics are still reported **only** for the linted
file.

### 1.2 Background
Single-file lint resolves only against the stdlib and the declarations *within* the
file, so `Foo x` (a sibling type) — and worse, `foo.bar()` / `foo.field` (sibling
members) — report false errors. The compiler already retrieves other units' **signatures
via its front end**: `ingestClasspath` (`Compiler.cpp:916`) full-parses each `.cja`
class's source to register its structure into the canonical map and *throws the LLVM
module away* — signature retrieval is parse-only, no codegen. `--source-root` applies
that same front-end work to the project's own source files.

Note: **prescan (type-name registration) is not sufficient** — it resolves `Foo x` but
not `foo.bar()`, because a member signature exists only after `Foo` is parsed (its
prototype built). Context resolution therefore parses siblings, it does not merely
prescan them.

### 1.3 Scope
- `--source-root <root>`: parse every `.cajeta` under `<root>` for **signatures**
  (front-end: prescan + parse → structures/prototypes; **no codegen, no emit**), then
  parse + diagnose only the linted file.
- **Resilient context**: a sibling with a syntax or semantic error must not abort the
  linted file's diagnostics; its diagnostics are suppressed and it simply contributes no
  (or partial) signatures.
- **Shadow**: `--shadow <realpath>` lets the linted (staged, unsaved) buffer replace its
  on-disk twin under `<root>`, so the edited content is what's analyzed.

### 1.4 Non-goals
- **Located / multiple semantic diagnostics for the linted file.** Semantic errors are
  still `throw cajeta::Exception` — single-shot and location-less; the linted file still
  yields one semantic diagnostic at no precise location. Making semantic diagnostics
  *collect-and-continue with spans* is a **separate follow-up** (its own spec); this work
  only removes false *cross-file* positives.
- **Codegen / emit / entry-method** — unchanged from single-file lint.
- **Multi-root / cross-project** resolution (one `--source-root`).
- **Incremental caching** — each invocation reparses `<root>`; perf tuning is later.

## 2. CLI surface

### 2.1
As the IDE plugin, when I run `cajeta --lint <file> --source-root <root> --shadow
<realpath> --diag-format=json`, then references in `<file>` to sibling types/methods/
fields under `<root>` resolve, and only `<file>`'s diagnostics are emitted as NDJSON.

### 2.2
As a user, when I run `--lint <file>` **without** `--source-root`, then behavior is
exactly today's single-file lint (stdlib + in-file only).

### 2.3
As a user, when `--source-root` names a path that does not exist or is not a directory,
then a clear error is reported (not the compile usage banner) and exit is non-zero.

## 3. Context resolution (signatures)

### 3.1
As the compiler, when linting with `--source-root <root>`, then I parse every `.cajeta`
under `<root>` far enough to register its class structure + member signatures, then
parse the linted file so its cross-file references resolve — without generating or
emitting any code for `<root>`'s files.

### 3.2
As a user, when `<file>` calls `foo.bar(x)` where `Foo` and `bar` live in a sibling
file under `<root>`, then no "unresolved" diagnostic is reported for `Foo`, `foo`, or
`bar`.

## 4. Isolation / resilience

### 4.1
As a user, when a **sibling** file under `<root>` has a syntax or semantic error, then
linting `<file>` still completes and reports `<file>`'s own diagnostics; the sibling's
error neither aborts the run nor appears in the output.

### 4.2
As the compiler, when a context file fails to parse/prototype, then it is skipped (it
contributes no signatures) and context resolution proceeds with the remaining files.

### 4.3
As a user, when `<file>` itself is clean but a sibling is broken, then `<file>` lints
clean (exit 0, no diagnostics) — the broken sibling is invisible except insofar as a
reference into it may go unresolved.

## 5. Shadow / dirty buffer

### 5.1
As the plugin, when I lint an unsaved buffer staged to `<tempfile>` for the project file
at `<realpath>`, then I pass `--source-root <root> --shadow <realpath>`; the `<root>`
walk **skips** `<realpath>`, and `<tempfile>` is parsed in its place — so the edited
content is diagnosed, not the stale on-disk file, and the class is not defined twice.

### 5.2
As a user, when `--shadow` names a path not under `<root>`, then it is a no-op skip (no
error) — `<tempfile>` is simply added as an extra unit.

## 6. Output contract
Unchanged from single-file lint: exit 0 iff no error-severity diagnostic **for the
linted file**; JSON mode is pure NDJSON with nothing on stdout; only `<file>`'s
diagnostics are emitted.

## 7. Plugin integration

### 7.1
As `CajetacRunner`, when the edited file belongs to a resolvable project source root,
then I pass `--source-root <root> --shadow <realpath>` alongside `--lint <tempfile>
--diag-format=json`; when no root is determinable, I omit them (degrading to single-file
lint).
