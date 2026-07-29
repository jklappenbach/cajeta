# lint-server — a warm lint process for the IDE

Status: draft (2026-07-13)
Related: `compiler-lint-mode-spec.md`, `lint-source-root-spec.md`,
`ide-symbol-index-spec.md` (§2.0.2 lint-channel xref rides this).

## 1. Definition

### 1.1 Purpose
Give the IDE plugin a long-lived lint process that primes the stdlib once and
answers per-edit lint requests warm, instead of paying the full compiler
bootstrap in a fresh subprocess on every edit.

### 1.2 The problem — measured
Per-edit lint spawns one `cajeta --lint` subprocess per (debounced) edit. On
2026-07-13, Debug build:

- A three-line file with no project: **14.1 s**. All of it is
  `ensureStdlibModule()` — ANTLR-parse the embedded stdlib, then
  `buildPendingPrototypes()` lays out every stdlib class into LLVM (vtables,
  RTTI, method prototypes), plus the forced `Class<?>` instantiation.
- The same target with `--source-root samples/tour/src` (91 files): **26.4 s**
  — `registerLintContext` prescans and signature-parses every sibling.

The plugin's subprocess timeout is 10 s. Per-edit lint cannot complete on a
tour-sized project. A Release binary shrinks the constant but not the shape:
both costs are per-process, and the process is per-edit.

### 1.3 The existing asset — in-process stdlib reuse
`StdlibReuseCache` (test/jit/JitTestHelper.cpp:226) already solves this shape
for the test shards: prime once per process, snapshot a baseline, and
`restoreBaseline()` between compiles (reuse-epoch bump, lazy-package reset,
stale method-template-instantiation removal, singleton re-pin). The compiler
carries the supporting discipline throughout (emit-module redirection so the
cached stdlib stays pristine). It is all process-local memory — nothing is on
disk — so it cannot help subprocesses, but it is exactly what a persistent
process reuses. Lint is an easier customer than the JIT tests: it stops before
codegen, so the prime needs no codegen passes and far less of the
pristine-stdlib surface is exercised.

### 1.4 Constraints
- 1.4.1 **Parity is the contract.** A warm lint's output must equal what a
  fresh one-shot `cajeta --lint` of the same input produces — diagnostics and
  xref records both. A diverging diagnostic from a stale cache is a wrong
  answer with confidence; wrong > missing governs here as in ide-symbol-index.
- 1.4.2 The one-shot path stays intact and unchanged. It is the fallback and
  the reference implementation the server is tested against.
- 1.4.3 One server per project root, one client (the IDE). No multi-project
  daemon, no shared socket.
- 1.4.4 The server must be safe to kill at any moment; all state is
  reconstructible by respawning.

### 1.5 Non-goals
- On-disk serialization of the parsed stdlib or LLVM state.
- Serving compiles, codegen, JIT execution, or the debugger.
- Concurrency within the server; requests are handled serially.

## 2. Server lifecycle and protocol

`cajeta --lint-server --source-root <root> --diag-format=json` reads NDJSON
requests on **stdin** and writes NDJSON responses on **stdout** (stderr is
uncontrolled: crash traces, logging). Responses for a request are contiguous
(serial server), bracketed by markers, and the payload lines between markers
are byte-identical to what the one-shot lint would emit — parity by
construction, testable by slice comparison.

- On start: `{"kind":"server","proto":{"major":1,"minor":0},"state":"ready"}`
  after the stdlib prime. The plugin refuses an unknown major wholesale (the
  xref version-handshake rule).
- Request: `{"kind":"lint","id":<n>,"file":"<staged-path>","shadow":"<original>",
  "emitXref":<bool>}`.
- Response: the one-shot lint's diagnostic/xref lines verbatim, then
  `{"kind":"done","id":<n>}`.
- Shutdown: stdin EOF (or `{"kind":"shutdown"}`); the server exits 0.

### Use cases
- 2.1 Project open: the plugin starts the server, which primes and reports
  ready. The prime cost is paid once per IDE project session.
- 2.2 Edit: the plugin sends a lint request for the staged buffer; diagnostics
  (and xref, when asked) return warm — target-file cost only.
- 2.3 Rapid edits: the plugin coalesces (it already debounces); the server
  processes serially in arrival order.
- 2.4 Malformed request line: the server answers
  `{"kind":"error","id":<n?>,...}` and keeps serving; it never crashes on
  input.
- 2.5 Server death: the plugin detects exit, falls back to one-shot lint
  immediately, and respawns with backoff.
- 2.6 Protocol major mismatch: the plugin never sends a request; it falls back
  to one-shot and surfaces one notification.

## 3. Warm stdlib between requests

- 3.1 Before each request the server restores the stdlib baseline
  (`restoreBaseline()` semantics: reuse epoch, lazy packages, template
  instantiations registered on stdlib classes, type/module singletons).
- 3.2 Parity (1.4.1) is the acceptance oracle: for a corpus of inputs —
  healthy, syntax-broken, semantically broken, lazy-stdlib-importing — request
  N's payload equals a fresh process's output for the same input, regardless
  of what requests 1..N-1 were.
- 3.3 The reuse machinery's correctness was proven under the JIT test suite's
  usage pattern. Lint reuse exercises it against prescan / registerLintContext
  / shadow-buffer state the tests never touch; that combination gets its own
  tests, not a free ride.

## 4. Sibling-context reuse

- 4.1 The first request against a root pays the full sibling sweep; the server
  keeps the external-module signatures warm.
- 4.2 Invalidation is per file by (mtime, size): a changed sibling is
  re-registered; an added file joins; a deleted file's signatures drop.
- 4.3 A sibling edit that changes a signature is visible to the next lint of a
  file that references it (no stale cross-file resolution).
- 4.4 When invalidation cannot be trusted (rename storms, root moved), the
  plugin's recourse is cheap: kill and respawn (1.4.4).

## 5. Plugin integration and fallback

- 5.1 `CajetacRunner` routes lint through the server when one is ready;
  otherwise (starting, dead, refused) it uses the one-shot subprocess path
  unchanged.
- 5.2 The xref stream (ide-symbol-index §2.0.2) rides the same responses;
  demux is unchanged.
- 5.3 Perf acceptance follows house rules: recorded before/after numbers in
  the plan, judged by review — no programmatic thresholds. The number that
  matters: warm per-edit lint on `samples/tour` vs the 10 s timeout.
