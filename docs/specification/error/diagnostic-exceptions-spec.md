# Diagnostic Exceptions — Specification (SRD)

> Status: DRAFT (2026-07-03), awaiting approval. The companions are `ErrorModel.md`,
> which holds the current hierarchy and machinery, and `ExceptionReview-plan.md`, whose
> §4/§5 items this supersedes as implementations of this model. This spec governs a
> refactor of Cajeta's exceptions from a Java-shaped, string-centric, human-only model
> into structured, agent-consumable diagnostics.

---

## 1. Definition

### 1.1 Purpose
Every exception becomes a structured `Diagnostic` in the same schema the compiler already
emits under `--diag-format=json`. It is rendered as readable text for developers by
default, and it is machine-readable for tools and agents by construction. One diagnostic
model spans compile time and run time.

### 1.2 Problem
Today's exceptions are Java-shaped and carry Java's weaknesses:
- **Stringly-typed.** All structure is smashed into a single `message` string, so callers
  and agents must regex prose to recover `path`, `errno` and the rest.
- **Human-only.** There is no stable id and no machine schema, which is the opposite of the
  compiler's structured `--diag-format=json` diagnostics.
- **Non-actionable traces.** Stack traces are raw return addresses symbolicated to
  `module(function+offset)`. There is no `file:line` and there are no Cajeta names, in JIT
  or in `--emit=exe`.
- **Async-blind.** In-fiber throws capture 0 frames, and a flat physical stack misrepresents
  the logical `await` chain.
- **No propagation context.** There is only a flat `Caused by:` wrap, and no way to attach
  "while loading config" as an error travels up.

### 1.3 Solution (scope)
1. A unified diagnostic schema (§2), extending `--diag-format=json`, that both the
   compiler and a runtime `Throwable` render to.
2. A structured exception model (§3) with reflection-serialized typed context fields,
   stable ids, category and affordances, and remediation.
3. Propagation context (§4), which is attach-as-you-go notes, distinct from the cause chain.
4. Semantic stack traces (§5), a `StackFrame` value record carrying
   `type/method/file:line/role`, resolved through a Cajeta-emitted line table, with
   `file:line` available in JIT and `--emit=exe`.
5. An async await-chain (§6) modeled at the diagnostic level and captured at `await`
   points, without unwinding fiber stacks.
6. Three access surfaces (§7), which are the canonical typed object, a lazy NDJSON string
   projection, and a v1 zero-copy C-ABI binary binding for embedding hosts, plus mode and
   toggle integration (§8).

### 1.4 Constraints
- **Cost is mode-gated.** Line-info emission and trace capture default on for
  diagnosability, and both are toggleable. A release build that opts out pays zero runtime
  or binary-size cost for what it disables.
- **Reuse, don't reinvent.** The schema extends the existing `--diag-format=json` shape and
  reuses the compiler's `DiagnosticEngine` emitter.
- **Dogfood.** `StackFrame` is a `record`, a value type, which exercises that feature.
- **Substrate is unchanged.** `throw`/`try`/`catch`/`finally` unwinding, the `Throwable`
  hierarchy, and the capture side-table are extended. None of them is rebuilt.

### 1.5 Non-goals (named future extensions)
- **1.5.1 Provenance.** This means tracing a failing value back to its origin via the
  ownership/borrow model ("this null came from `config.get("x")`"). It is differentiating,
  but it requires codegen-level value-origin tracking, so it is a future phase.
- **1.5.2 Per-frame value capture.** This means capturing typed locals and args at each
  frame in debug mode. It is a future phase, and v1 frames are semantic but value-free.
- **1.5.3 Fiber physical unwinding.** This means walking a `makecontext` fiber stack with
  `backtrace(3)`, which SIGSEGVs today. v1 delivers the logical await-chain via await-point
  capture instead, and physical in-fiber frames remain empty until a fiber-safe unwinder
  lands.
- **1.5.4 Windows capture.** `backtrace` is a no-op stub on mingw, and the platform DbgHelp
  path stays an ExceptionReview §7 item. This spec requires only that Windows and
  empty-trace cases degrade to a precise "unavailable: <reason>" and never to a silent
  blank.

---

## 2. The unified diagnostic schema

### 2.1 Requirements
- **2.1.1** Define one diagnostic object shape emitted by both producers (compiler,
  runtime), a superset of today's `{severity, code, message, file, line, column}`:
  `code`, `category`, `message`, `source` (SourceSpan), `fields` (structured map),
  `contextChain`, `causeChain`, `frames` (`StackFrame[]`), `awaitChain`, `task`,
  `remediation`.
- **2.1.2** Every field except `code`, `category`, `message` is optional, so a minimal
  diagnostic is valid.
- **2.1.3** The NDJSON form is one object per line on stderr, backward-compatible with
  existing `--diag-format=json` consumers, since the new fields are additive.

### 2.2 Use cases
- **2.2.1** As an IDE/agent already consuming compiler NDJSON, when a program throws under
  `--diag-format=json`, then I receive the same-shaped object and parse it with no new
  code path.
- **2.2.2** As a tool, when I read a diagnostic, then I can rely on `code`/`category`/`message`
  always being present and every richer field being optional-but-typed.

---

## 3. Structured exception model

### 3.1 Requirements
- **3.1.1 Typed fields.** An exception's public fields are serialized into the
  diagnostic's `fields` map by a generic reflection-based serializer that walks `Class<T>`,
  so there is no per-type serialization code.
- **3.1.2 Stable id.** Every `Throwable` exposes `code()`, defaulting to its canonical type
  name and settable to a stable code via `@DiagnosticCode("CAJETA_ERR_…")`. The stdlib's own
  leaf exceptions carry explicit codes.
- **3.1.3 Category/affordances.** Generalize Recoverable/Unrecoverable into overridable
  properties, at minimum `retryable`, `transient` and `userActionable`, so callers and
  agents branch programmatically without catch-by-type.
- **3.1.4 Remediation.** Remediation is an optional `hint` and `docUrl`, debug-rich and
  strippable.
- **3.1.5** `getMessage()`/`getCause()`, which are already shipped, remain the message and
  cause accessors. The message becomes the human rendering of the structure and is no
  longer its sole carrier.

### 3.2 Use cases
- **3.2.1** As a developer catching `IoException`, when I inspect it, then `path`, `errno`
  and `bytesWritten` are typed fields. I never have to pull them out of `message` as
  substrings.
- **3.2.2** As an agent, when I receive a diagnostic with `code = CAJETA_ERR_IO_DISK_FULL`,
  then I key remediation on the stable id instead of on English prose.
- **3.2.3** As a caller, when I get an exception with `retryable = true`, then I retry
  without catching a specific type or parsing text.
- **3.2.4** As an author of a new exception type with public fields, when it is thrown and
  serialized, then its fields appear in `fields` automatically with no extra code.

---

## 4. Propagation context (the context chain)

### 4.1 Requirements
- **4.1.1** A `Throwable` accumulates an ordered `contextChain` of notes attached as it
  propagates, via a `.context("…")` API. The context chain is distinct from `causeChain`,
  which holds wrapped errors.
- **4.1.2** Attaching context does not allocate a new exception type nor lose the original
  cause/trace.
- **4.1.3** Rendering interleaves context above the frames ("Error: X / while loading config
  / while parsing line 42").

### 4.2 Use cases
- **4.2.1** As a developer, when a low-level `disk full` bubbles through config loading, then
  I attach `"while loading config"` at that layer and the final diagnostic shows the full
  contextual path without me defining a `ConfigLoadException`.
- **4.2.2** As an agent, when I read `contextChain`, then I get the logical breadcrumb of
  where the failure occurred, separate from the causal `why`.

---

## 5. Semantic stack traces

### 5.1 Requirements
- **5.1.1 `StackFrame` is a value `record`:** `declaringType`, `method`, `file`, `line:int32`
  (0 = unknown), `role: FrameRole {User|Stdlib|Runtime}`, `nativeAddress:int64`.
- **5.1.2** `Throwable.getStackTrace() -> StackFrame[]`, throw-site first, resolved
  eagerly on call by walking captured addresses through the line table.
- **5.1.3 Cajeta-emitted line table.** Codegen emits a compact per-method
  `{address-range → type, method, file, line}` map, registered with the runtime. The
  resolver produces unmangled Cajeta identifiers and works identically in JIT and
  `--emit=exe`.
- **5.1.4** `file:line` precision is exact in debug and best-effort under optimization,
  where inlining and reordering intervene. The diagnostic never claims a line it cannot
  support, and 0 means unknown.
- **5.1.5 Role tagging** derives from the resolved package (`cajeta.*` → Stdlib, `__cajeta_*`
  → Runtime, else User) so consumers can trim to user frames.
- **5.1.6** Address capture stays cheap at throw time, and resolution cost is paid only when
  `getStackTrace()` or JSON serialization is invoked.

### 5.2 Use cases
- **5.2.1** As a developer, when I catch and print an exception from a `--emit=exe` debug
  build, then the top frame reads `test.App.main(App.cajeta:7)` with a real line number.
- **5.2.2** As a developer on a release build with line-info on (default), then I still
  get `file:line`, understanding it may be approximate where code was inlined.
- **5.2.3** As an agent, when I request user-relevant frames, then role tags let me drop
  stdlib/runtime noise without heuristics.
- **5.2.4** As a developer with line-info opted off in release, then frames carry
  `type/method/nativeAddress` with `line = 0`, and nothing crashes.

---

## 6. Async await-chain

### 6.1 Requirements
- **6.1.1** The async dimension is a diagnostic-level structure `awaitChain:
  AsyncSegment[]`, where each segment is `{ task (id/name), frames: StackFrame[], awaitSite:
  StackFrame }`. `StackFrame` itself carries no fiber field.
- **6.1.2** The chain is captured at `await` points, recorded in await codegen, and never by
  unwinding fiber stacks. It is therefore solid even while in-fiber physical capture is
  empty.
- **6.1.3 v1 affordance:** a thrown `Throwable` is tagged with its task id or name, one
  cheap field, even before full chain capture lands.
- **6.1.4** The full `awaitChain` shape is specified now, and its capture is staged as its
  own implementation phase, tied to ExceptionReview §6.

### 6.2 Use cases
- **6.2.1** As a developer whose async task throws across `await`, when I read the diagnostic,
  then I see the logical await chain ("threw in task `db-query` at `Query.cajeta:42`; awaited
  by `request-handler` at `Handler.cajeta:10`") instead of the executor's physical stack.
- **6.2.2** As an agent, when a task fails, then `task` identifies which task failed even
  before the full chain is available.

---

## 7. Access surfaces, serialization & rendering

### 7.0 Access surfaces (the object is canonical)
The structured in-memory `Diagnostic` object is the single source of truth. The string
and binary forms are lazy projections of it, and they are never built eagerly per throw.
There are three surfaces and one model:
- **7.0.1 Typed object (in-process Cajeta).** The `Throwable` is the binding, with typed
  `code()`, typed `fields`, `getStackTrace() -> StackFrame[]` and the chains. There is no
  serialization.
- **7.0.2 NDJSON string (cross-boundary).** For subprocess, tool, agent (MCP) and log
  consumers that cannot take a struct, `toJson()` emits the §2 schema via the compiler's
  `DiagnosticEngine` emitter. This is the lingua franca across a process boundary.
- **7.0.3 C-ABI binary binding (in-process host / agent runtime), v1.** A zero-copy
  accessor set, mirroring `__cajeta_get_trace`, lets a host embedding the runtime read the
  structured diagnostic without allocating and reparsing JSON, for example:
  `__cajeta_diag_code(void* thr) -> const char*`,
  `__cajeta_diag_field(void* thr, const char* key, CajetaValueABI* out) -> int`,
  `__cajeta_diag_frame_count(void* thr) -> int32`,
  `__cajeta_diag_frame(void* thr, int32 i, StackFrameABI* out)`,
  plus context/cause/await accessors. The ABI structs (`StackFrameABI`, `CajetaValueABI`)
  are stable, documented layouts.

### 7.1 Requirements
- **7.1.1** `Throwable.toDiagnostic()` produces the §2 object, and the object drives all
  three surfaces (7.0). Projections are lazy. JSON is produced only on `toJson()` or an
  uncaught throw under `--diag-format=json`, and the C-ABI accessors resolve frames on
  first access.
- **7.1.2** `printStackTrace()`, which is already in the MVP, renders readable text by
  default, made of the message, the context chain, the frames, and the `Caused by:`/await
  interleaving.
- **7.1.3** Under `--diag-format=json`, an uncaught throw emits the NDJSON diagnostic instead
  of free text, matching compiler behavior.
- **7.1.4** Empty or unavailable traces, whether capture is off, the platform is Windows or
  the throw is in-fiber, render a precise `"(stack trace unavailable: <reason>)"` and never
  a silent blank. This satisfies ExceptionReview §5.
- **7.1.5** The three surfaces are consistent, so the same object yields the same `code`,
  fields, and frames whether read as a typed object, NDJSON, or via the C-ABI.

### 7.2 Use cases
- **7.2.1** As a developer, when an exception is uncaught in a normal build, then I get a
  readable, cause-and-context-aware trace on stderr.
- **7.2.2** As a CI/agent harness running with `--diag-format=json`, when a program throws,
  then I get one NDJSON diagnostic line I parse with the compiler-diagnostic parser.
- **7.2.3** As a host (C/C++/Rust) or agent runtime embedding the Cajeta runtime, when I
  catch a diagnostic across the FFI, then I read `code`, `fields`, and `frames` through the
  C-ABI accessors with no JSON allocation or parse.

---

## 8. Mode & toggle integration

### 8.1 Requirements
- **8.1.1** A new compiler feature toggle `--line-info=on|off`, listed in `CompilerModes.md`
  alongside `--stack-trace-capture`, controls line-table emission. It defaults on in every
  flavor (debug, debug-release, release, fast, minimal), and size-sensitive builds can opt
  out.
- **8.1.2** Trace capture remains gated by `--stack-trace-capture`, and remediation and
  context richness follow the diagnostic-verbosity axis.
- **8.1.3** With every diagnostic feature off, a release binary pays zero runtime cost and
  no binary-size cost for the disabled pieces, and the line table is omitted from the
  image.

### 8.2 Use cases
- **8.2.1** As a developer shipping release, when I keep defaults, then my binary has
  actionable traces. When I set `--line-info=off`, then the line table is absent and traces
  degrade to `type/method/addr`.

---

## 9. Migration & compatibility

### 9.1 Requirements
- **9.1.1** Existing `Throwable{message}` / `Exception{cause}` and the shipped
  `getMessage()`/`getCause()`/`getStackTrace()`/`printStackTrace()` MVP remain source-
  compatible, and the MVP `StackFrame{nativeAddress}` is extended to the §5.1.1 record.

  **EXCEPTION, 2026-08-31. `getStackTrace()` is no longer source-compatible.**
  It is a producer. Both paths build a fresh `heap StackFrame[]` and hand it
  out, so a plain return gives the caller a title the signature never mentions.
  It is now `#StackFrame[]`, and a caller must bind with `#=`. The promise
  above was already hollow, because codegen rejects the plain-return shape
  outright (`CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER`). The compatibility it
  guaranteed was to a spelling that could not be compiled through every path,
  and the warning had been printing on every compile that touched `Throwable`
  long enough to be ignored. The break is guided, because the compiler names
  the fix at the offending line. Sixteen call sites in this repo's own tests
  were migrated mechanically. This exception supersedes 9.2.1 for this one
  method.
- **9.1.2** `ErrorModel.md` is updated to describe the diagnostic model, the schema, the
  `StackFrame` shape, the context chain, and per-platform availability.
- **9.1.3** Existing `--diag-format=json` consumers keep working, because the new fields are
  additive.

### 9.2 Use cases
- **9.2.1** As a developer with code using `getMessage()`/`getStackTrace()`, when this lands,
  then my code still compiles and runs, and new capabilities are additive.
  **Amended 2026-08-31**: `getStackTrace()` is exempt, as 9.1.1 explains. Binding
  it with a plain `=` is now a compile error naming the fix.

---

## 10. Definition of done (acceptance)
A reviewer signs off when all of these hold:
1. A thrown exception is readable through all three surfaces (§7.0), which are the typed
   object, the NDJSON string in the same shape as `--diag-format=json`, and the C-ABI
   binding, and each yields the same `code`, typed `fields` from reflection, `contextChain`,
   `causeChain` and `frames`.
2. On Linux and macOS, a debug trace in both JIT and `--emit=exe` shows `Package.Class.method
   (File.cajeta:NN)` with real line numbers. The top frame is the throw site, and roles tag
   user, stdlib and runtime.
3. `--line-info=off` yields address-only frames with `line=0` and no crash, and the line
   table is absent from the image.
4. The context-chain API attaches notes without new types or lost cause/trace.
5. The `awaitChain` schema is specified and a thrown exception carries its `task`, and full
   capture is staged as a tracked phase.
6. Windows/in-fiber/empty traces render a precise "unavailable: <reason>".
7. `ErrorModel.md` and `CompilerModes.md` (`--line-info`) are updated, and existing consumers
   are unbroken.

## 11. Priority / phasing (informs the plan's unit order)
1. **Schema + object + NDJSON serialization core** (§2, §7.0.1–7.0.2). This is the unifying
   foundation.
2. **Structured model** (§3), with reflection fields, `code`, category and remediation.
3. **Semantic traces** (§5), with the line table, the `StackFrame` record and roles. This is
   the largest build.
4. **Context chain** (§4).
5. **C-ABI binary binding** (§7.0.3), the accessor set and stable ABI structs, once the object
   fields and traces exist to expose.
6. **await-chain schema + task tag** (§6.1.3, schema only, with capture staged).
7. **Mode toggle + docs** (§8, §9).
The deferred items, the non-goals of §1.5, are provenance, per-frame values, fiber physical
unwind and Windows capture.
