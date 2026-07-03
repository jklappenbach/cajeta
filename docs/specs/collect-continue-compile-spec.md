# Full-compile collect-and-continue + absorption foundation — spec

## 1. Definition

### 1.1 Purpose
Extend collect-and-continue (`docs/specs/diagnostic-engine-spec.md`) from lint to **full
compile**, and lay the **absorption** foundation in the type system that the eventual
codegen-in-collect-mode needs. After this, `cajeta <entry> <src> <archive>` reports **all**
pre-codegen semantic errors (across every module) then fails without emitting an artifact —
where today it aborts on the first.

### 1.2 Scope
- **Full-compile engine.** The 3-positional compile driver installs a `DiagnosticEngine`;
  migrated resolution sites report to it (recovering with `ErrorType`) instead of throwing;
  all collected diagnostics are emitted at the end.
- **Gate codegen + emit.** After the resolution passes (through `resolveDependencyGraph`),
  if the engine has errors, skip the Phase-1/2 codegen loop and all artifact emission; exit
  non-zero. A clean compile runs codegen and emits exactly as today.
- **Absorption foundation.** `ErrorType` becomes **absorbing** and **universally
  compatible** in the type system: subtyping/assignability against any type is true both
  ways, and the designated "result type" of an operation whose operand is `ErrorType` is
  `ErrorType`. This is unit-tested at the type level; it is what will let a future
  **codegen-in-collect-mode** run codegen over error types without cascading — but that
  mode (collecting codegen-time errors like type-mismatch / unknown-method) remains a
  separate effort because it also needs IR-safe error values.

### 1.3 Non-goals
- **Codegen-in-collect-mode** (running codegen with poison to collect use-site /
  type-mismatch / unknown-method errors) — needs IR-safe error values across codegen; its
  own effort. Here codegen is *gated off* when errors exist, so error types never reach it.
- **Migrating every throw site** — incremental; un-migrated sites still throw and, with an
  engine active, are folded into it by the driver as a single fatal diagnostic.

## 2. Full-compile behavior

### 2.1
As a developer compiling a source tree with several unresolved-type errors across
different files, I receive **one diagnostic per error**, sorted by (file, line, column),
and the compile fails with **no** artifact written.

### 2.2
As a developer compiling a clean tree, codegen runs and the artifact is emitted, exit 0 —
byte-for-byte as today.

### 2.3
As a developer, when a still-`throw`-based (un-migrated) fatal error fires, it is caught by
the driver and reported as one diagnostic; the compile fails. (No worse than today.)

### 2.4
As the build tool (`--diag-format=json`), full-compile diagnostics stream as NDJSON with no
plain-text leak.

## 3. Absorption (type-system foundation)

### 3.1
As the type system, `ErrorType.isAssignableFrom(T)` and `T.isAssignableFrom(ErrorType)` are
true for every `T` — an error-typed value never triggers a secondary "incompatible type".

### 3.2
As the type system, the result type of a member/element/operation resolution whose subject
is `ErrorType` is `ErrorType` (absorption) — queried via a helper the future
codegen-in-collect-mode will call so a poisoned subtree yields no secondary diagnostic.

### 3.3
As codegen, `ErrorType` is never lowered (it is gated out); `getLlvmType()` stays safe.

## 4. Migration boundary
Same as diagnostic-engine: migrated sites report + recover; un-migrated throw. This spec
adds the full-compile driver + gate and the absorption predicates; it does not migrate
codegen-time sites.
