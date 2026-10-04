# Diagnostic origin and stdlib warning hygiene (spec)

Requested by Julian 2026-10-04. Extends diagnostic-engine-spec.md.

## 1. Definition

### 1.1 Purpose

Every compile that has an active DiagnosticEngine also compiles the embedded
stdlib, and the stdlib reports its own warnings into the same engine. On
2026-10-03 that was 115 warnings from 70 stdlib classes: 62
CAJETA_WARN_LAST_USE_TRANSFER, 54 CAJETA_WARN_PLAIN_RETAIN_STORE and 1
CAJETA_WARN_REDUNDANT_TRANSFER. They carry no file, because an embedded source
has an empty source path, so they sort ahead of every located diagnostic. The
engine keeps the first 100 (diagnostic-engine 2.3), so the user's own
diagnostics are the ones cut. SliceLint.ResolvedStoreEmitsNote turned red when
cajeta f823b826 added three stdlib warnings and crossed the cap. That blocked
the next release, cut as v0.34.0.

A diagnostic must say where the code it describes comes from, the engine must
never let one origin crowd out another, and the stdlib must compile without
warnings.

### 1.2 Scope

- Each diagnostic records its origin: the project being compiled, a
  dependency, or the stdlib.
- A stdlib diagnostic names its class and source file, where it names nothing
  today.
- The engine caps each origin separately.
- The CLI, the lint path and the IDE show project diagnostics by default.
  Dependency and stdlib diagnostics are shown on request.
- CAJETA_WARN_PLAIN_RETAIN_STORE and CAJETA_WARN_LAST_USE_TRANSFER fire only
  where the hazard they describe can happen.
- The stdlib sites that remain after that are fixed.
- A gate keeps the stdlib warning-free.

### 1.3 Non-goals

- Changing the ownership rules themselves. The warnings are tightened to
  match the rules as they stand.
- Suppressing errors by origin. An error stops the build wherever it is.

## 2. Origin metadata

### 2.1 Requirements

Every collected diagnostic carries an origin, one of project, dependency or
stdlib. The origin is decided by the compilation unit the diagnostic is
reported from: a class from the embedded stdlib is stdlib, a class read from a
classpath archive is dependency, and anything compiled from the project's
sources is project. A stdlib or dependency diagnostic names its class, and its
source file when one is known, so it can be located.

### 2.2 Use cases

- **2.2.1** When the stdlib reports a warning during a project compile, the
  diagnostic's origin is stdlib and it names the stdlib class and line.
- **2.2.2** When a project class reports a warning, its origin is project, as
  today.
- **2.2.3** When diagnostics are printed as JSON (--diag-format json), each
  carries its origin.

## 3. Per-origin cap and ordering

### 3.1 Requirements

The cap of diagnostic-engine 2.3 applies to each origin separately. Project
diagnostics are emitted first, in source order, then dependency, then stdlib.
A positionless diagnostic never sorts ahead of a located project diagnostic.

### 3.2 Use cases

- **3.2.1** When the stdlib reports more warnings than the cap, every project
  diagnostic is still kept and emitted.
- **3.2.2** When the project itself reports more than the cap, the first 100
  project diagnostics are kept, with the "and N more" note, as today.

## 4. Default filtering

### 4.1 Requirements

The CLI, --lint, the lint server and the IDE annotator show project
diagnostics by default. A flag shows dependency and stdlib diagnostics too.
Errors are shown whatever their origin.

### 4.2 Use cases

- **4.2.1** When a project compiles cleanly against a stdlib that warns, the
  user sees no warnings.
- **4.2.2** When the user asks for every origin, the stdlib warnings are shown
  with their class and line.
- **4.2.3** When a dependency fails to compile, its error is shown, located.

## 5. Tightened warnings

### 5.1 Requirements

CAJETA_WARN_PLAIN_RETAIN_STORE warns that a plain store borrows a value that
may arrive owned. A plain store into a String field copies the value, so it
cannot dangle, and the warning must not fire there. 42 of the 54 stdlib sites
are `this.message = message` in exception classes.

CAJETA_WARN_LAST_USE_TRANSFER fires on every final-use lend of an owned local.
It must fire only where the callee can keep the value. A lend to a plain
formal the callee cannot keep, such as `Sha256.hash(w, n)`, is correct as
written.

Each warning keeps tests that show it firing where it should and silent where
it should not.

### 5.2 Use cases

- **5.2.1** When a String field receives a plain store of a parameter, no
  CAJETA_WARN_PLAIN_RETAIN_STORE is reported.
- **5.2.2** When a class field receives a plain store of a parameter that may
  arrive owned, the warning is still reported.
- **5.2.3** When an owned local is lent at its final use to a callee that
  cannot keep it, no CAJETA_WARN_LAST_USE_TRANSFER is reported.
- **5.2.4** When an owned local is lent at its final use to a callee that can
  keep it, the warning is still reported.

### 5.3 Measured 2026-10-04: the String-field premise is half right

5.1 says a plain store into a String field copies, so it cannot dangle. That
holds for a `#String` formal and fails for a plain one. A callee stored its
formal into a String field, the caller passed `#s`, then churned 2000
substrings and read the field (probes in cajeta-six tmp/u6):

| formal | store | result |
|---|---|---|
| `String p` | `this.v = p` | SIGSEGV |
| `#String p` | `this.v = p` | correct, and 400000 stores of 2 KB peak at 2.7 MB |
| `String p` | `this.v #= p` | correct |
| `#String p` | `this.v #= p` | correct |
| `Exception(#String)` | `this.message = message` | correct |

The 42 exception-class sites take `#String message`, so they are the safe
shape and the warning is noise there. A plain formal stored plainly into a
String field is a captured borrow. The same store into a class field is
already CAJETA_ERROR_CAPTURED_BORROW_PARAM, and the String field is the gap.
`cajeta.ifx.IfxInfo`'s private constructor has that shape today. Its only
caller passes literal names, so it does not crash yet.

Two more facts bear on 5.1. The warning fires on both formal kinds alike.
And a CLI compile turns the engine off for codegen (Compiler.cpp,
CodegenEngineOff), so this codegen-time warning reaches only JIT compiles
and the kernel session, never `cajeta build` or `--lint`.

5.2.1 is therefore under review (7.2).

## 6. A warning-free stdlib

### 6.1 Requirements

After 5, the stdlib sites that still warn are genuine and are fixed in the
stdlib sources. A test compiles the stdlib with every origin shown and fails
on any warning, naming the class and line.

### 6.2 Use cases

- **6.2.1** When a stdlib change introduces a warning, the gate fails and
  names the site.
- **6.2.2** When the stdlib is warning-free, the gate passes.

## 7. Open questions

- **7.1** How a call site decides that a callee "can keep" a lent value for
  5.1. A plain formal that a callee stores is already a
  CAJETA_ERROR_CAPTURED_BORROW_PARAM, so the candidate rule is "warn only for
  a `#` formal or an unresolved callee". Decided in plan Unit 3 against the
  ownership tests.
- **7.2** What replaces 5.2.1, given 5.3. The proposal: no warning when the
  stored value is a `#` formal, and a plain formal stored into a String field
  becomes CAJETA_ERROR_CAPTURED_BORROW_PARAM, as it is for a class field.
  `#` formals stored into class fields and array slots (DnsCache's resolver,
  QueryParams.append's keys) are not yet measured. Needs Julian's decision.
