# Diagnostic engine + error types (collect-and-continue) — spec

## 1. Definition

### 1.1 Purpose
Replace the abort-on-first `throw cajeta::Exception` model for **recoverable semantic
errors** with a designed diagnostics architecture: a **diagnostic engine** that collects
diagnostics as data, and an **error type** (poison) in the semantic domain that lets
analysis continue past a failure. A file surfaces **all** its semantic errors at once,
each precisely located (Phase 1), with cascades suppressed — the way a language compiler
should report.

### 1.2 The two mechanisms
- **`DiagnosticEngine`** — a first-class collector. Semantic analysis calls
  `report(diagnostic)`; the engine accumulates, dedups, sorts by span, caps, and emits at
  the end. It replaces scattered `throw` for recoverable errors. Truly unrecoverable
  conditions (I/O, internal compiler error) keep a **fatal** channel that still aborts.
- **`ErrorType`** — a distinguished singleton `CajetaType`. When resolution fails it
  reports **one** diagnostic and returns `ErrorType` (never `null`, never `throw`). The
  error type is:
  - **absorbing** — every operation on it (member access, call, arithmetic, assignment,
    subtyping) yields `ErrorType` and reports **no further diagnostic**;
  - **universally compatible** — assignable to and from any type, satisfies any expected
    type — so it never triggers a secondary "type mismatch".

  Absorption is the cascade-suppression mechanism: one unresolved `Foo` used in five
  places is **one** diagnostic, by construction.

### 1.3 Why this removes the try/catch question
Because `ErrorType` is a well-behaved value, analysis walks the rest of the tree without
null-dereferences — **recovery needs no try/catch and no recovery boundaries**; the poison
discipline *is* the recovery. There is no exception unwinding, so no half-restored global
state (`activeModule` etc.). Codegen never sees `null`; it is simply **skipped whenever the
engine holds any error**, so `ErrorType` is never lowered.

### 1.4 Scope
- Introduce `DiagnosticEngine` (collect / dedup / sort / cap / emit) and route recoverable
  semantic diagnostics through it.
- Introduce `ErrorType` with absorbing + universally-compatible semantics.
- Migrate the flagship resolution sites (unresolved type first) from `throw` →
  `report + return ErrorType`.
- Gate codegen on `engine.hasErrors()`; both lint and full compile report **all**
  collected diagnostics then fail; a clean file compiles/lints as today.
- Full analysis now runs safely under lint, so lint sees the whole file's errors (not just
  those before the first abort).

### 1.5 Non-goals
- **Migrating every `throw` site at once** — incremental; un-migrated sites keep throwing
  (treated as fatal / abort) until converted. No regression: a still-throwing site behaves
  exactly as today.
- **Warning/lint-rule expansion** — this is about the reporting architecture, not new
  checks.
- **Cross-file multi-target** — one linted/compiled file's engine at a time.

## 2. DiagnosticEngine

### 2.1
As semantic analysis, when I detect a recoverable error, I call `report(severity, code,
message, span)` on the active engine instead of throwing; analysis continues.

### 2.2
As the engine, at end of analysis I emit every collected diagnostic **sorted by span**
(file, then line, then column), **deduped** by (file, line, column, code), in the active
`--diag-format`.

### 2.3
As the engine, when collected errors exceed a cap (default 100), I emit the first cap and
one trailing note "…and N more"; I never flood.

### 2.4
As the driver, `engine.hasErrors()` (any error-severity diagnostic) gates codegen and the
process exit code; warnings/notes never gate.

### 2.5
As `--source-root` context analysis, sibling files run under a **suppressed** engine whose
diagnostics are discarded — so a broken sibling contributes none to the target's engine.

## 3. ErrorType

### 3.1
As type resolution, when a type name does not resolve I `report` one diagnostic and return
`ErrorType` (not null).

### 3.2
As any operation on an `ErrorType` value (member access, method call, index, operator,
cast), I return `ErrorType` and report **no** diagnostic (absorption).

### 3.3
As subtyping / assignability, `ErrorType` is assignable to and from every type and
satisfies every expected type, so it never causes a secondary "incompatible type" error.

### 3.4
As codegen, `ErrorType` is never lowered — codegen runs only when the engine has no
errors. (Defensively, `ErrorType::getLlvmType()` yields an opaque pointer so a stray path
cannot crash.)

## 4. Recovery & control flow

### 4.1
As analysis, after a recoverable error I keep visiting sibling declarations/statements —
no statement is skipped merely because an earlier one erred (the error value flows on).

### 4.2
As a fatal condition (internal invariant broken, unreadable input), I still `throw`; the
top level reports it and aborts. Recoverable semantic errors never abort.

### 4.3
As a **syntactically** malformed file, semantic analysis is skipped for the malformed unit
(as today) — a broken parse tree cannot be soundly analyzed; syntax diagnostics report and
gate.

## 5. End-to-end behavior

### 5.1
As a user linting a file with several independent errors (two unresolved types, an
unresolved type plus an unrelated one), I receive **one NDJSON diagnostic per error**, each
located, in source order, exit non-zero.

### 5.2
As a user with one root cause used many times (an unresolved `Foo` referenced five times),
I receive **one** diagnostic (absorption), not five.

### 5.3
As a user compiling a clean file, I see no diagnostics, codegen runs, exit 0 — unchanged.

### 5.4
As a user compiling a broken file, I see all migrated semantic errors, no artifact is
produced, exit non-zero — where today I saw only the first.
