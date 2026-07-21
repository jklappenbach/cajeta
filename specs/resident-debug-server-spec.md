# resident-debug-server — spec

## 1 Definition

### 1.1 Purpose
Make the edit→relaunch debug loop take seconds by keeping the compiled world
resident between debug sessions. fast-debug-launch delivered 0.45s no-edit
relaunches; this spec covers the other half — a relaunch after an edit.

### 1.2 Problem
`cajeta dap` is spawned per session and dies with it, so every launch after
an edit rebuilds the world from a fresh process. Measured on samples/tour
(Release, quiet machine, 2026-07-21): 42.5s = user parse 8.9 + stdlib parse
6.6 + codegen 8.3 + merge 7.2 + LLJIT materialize 9.5 (+1.7 misc). Every one
of those costs is proportional to the whole program, not to the edit. Only a
process that keeps the parsed and compiled modules alive can charge an edit
for the edit alone.

### 1.3 Decisions (with Julian, 2026-07-21)
- **Residency = the DAP process itself.** The plugin keeps one `cajeta dap`
  alive per project and runs sequential debug sessions over the same stdio
  pipe. No separate daemon, no discovery.
- **Fresh LLJIT per session.** The parsed/compiled world is resident; the
  execution engine is not. Statics and clinits rerun each session; a session
  behaves like a fresh process (measured materialize-from-object cost:
  ~0.13s).
- **Staleness = handshake + self-exit.** Each session handshake carries the
  compiler identity the plugin expects; on mismatch the server finishes
  cleanly and exits, and the plugin respawns it fresh.

### 1.4 Constraints
- Debug behavior must be indistinguishable from a fresh-process session:
  safepoints, loc ids, drop thunks, breakpoints, stepping, Watch/Evaluate,
  environment overlays (`EnvironmentScope` restores fully between sessions).
- The in-process merge (`llvm::Linker::linkModules`) consumes donor modules;
  residency therefore requires the program to reach the LLJIT WITHOUT a
  destructive whole-program merge (per-module residency).
- Composes with the fast-debug-launch caches: a cold server start may serve
  from the whole-program slot; per-module objects extend the same
  `.cajeta/cache` tree and discriminator rules. Correctness beats speed —
  any doubt falls back to recompiling.
- Perf acceptance by review: committed before/after numbers, no thresholds.

### 1.5 Non-goals
- Sharing the resident world with the lint server (kept separate for now).
- Parallel debug sessions in one server (sequential only).
- Surviving IDE restarts (the plugin owns the process; IDE exit ends it).
- Hot code replace in a RUNNING session (edits apply at next relaunch).

## 2 Resident session lifecycle

### 2.1 Requirements
The plugin starts one `cajeta dap` per project on first debug and keeps it
across sessions. The DAP conversation supports repeated
launch→…→disconnect cycles on one connection; `disconnect` ends the session
but not the process. An idle server costs nothing but memory. The plugin
kills the server on project close and respawns it if it dies.

### 2.2 Use cases
- 2.2.1 As a developer, when I debug, stop, and debug again without edits,
  then the second session starts in under a second and no new process
  appears.
- 2.2.2 As a developer, when the server process dies (crash, kill), then the
  next debug click transparently spawns a fresh one and the session works.
- 2.2.3 As a developer closing the project, then the server exits with it —
  no orphan processes.

## 3 Incremental recompile on edit

### 3.1 Requirements
Between sessions the server keeps all parsed modules and their compiled
per-module objects. At the next launch it detects changed sources (digest
comparison), reprocesses ONLY those (parse, codegen, and whatever
dependent-module work correctness requires), and reuses everything else.
The whole-program destructive merge is replaced by per-module delivery to
the LLJIT, so unchanged modules' objects are reused as-is. Any doubt about
reuse soundness (dependency edges, template instantiations, drop-thunk
obligations) falls back to recompiling the doubtful set — never stale code.

### 3.2 Use cases
- 3.2.1 As a developer who edited one file, when I relaunch, then the session
  reaches my breakpoint in seconds, not tens of seconds.
- 3.2.2 As a developer who edited a widely-imported file, when I relaunch,
  then dependents recompile as needed and behavior matches a cold build
  exactly.
- 3.2.3 As the jit-drop-backfill and stepping regression suites, when run
  against an incrementally rebuilt session, then results are identical to a
  fresh-process session.

## 4 Session isolation

### 4.1 Requirements
Each launch builds a fresh LLJIT from the resident modules/objects. Program
statics, clinits, fiber registries, safepoint counters, exception handlers,
and environment overlays start clean every session; nothing observable leaks
from session N into session N+1. Server-side per-session state
(DebugController, loc-table view, breakpoint arming) resets on `disconnect`.

### 4.2 Use cases
- 4.2.1 As a developer whose program mutates a static and exits, when I
  relaunch, then the static has its initial value again.
- 4.2.2 As a developer using launch env overlays, when session 1 sets
  `FOO=1` and session 2 doesn't, then session 2's debuggee sees no `FOO`.
- 4.2.3 As a developer whose debuggee exits abnormally or is
  force-terminated, when I relaunch, then the new session is unaffected.
  NOTE (2026-07-21, found in Unit 1 TDD): an UNCAUGHT throw terminates the
  process — the runtime's uncaught handler exits, and in-process that is the
  server. Recovery is the plugin respawn (§2.2.2) at the cost of residency;
  armed break-on-throw parks instead and is unaffected. Making the uncaught
  path session-scoped is future work, out of scope here.

## 5 Staleness and failure

### 5.1 Requirements
The session handshake carries the compiler identity (version + git hash +
binary digest) the plugin expects. On mismatch — or when the server detects
its own binary changed — the server completes/refuses the session cleanly
and exits; the plugin respawns. Cache-tree interactions keep the
fast-debug-launch discriminator rules: a compiler change invalidates
everything, silently, at the cost of one cold start.

### 5.2 Use cases
- 5.2.1 As a developer who rebuilt the compiler, when I next debug, then the
  first session pays a cold start on the new compiler and nothing serves
  stale artifacts.
- 5.2.2 As a developer with two projects open, then each has its own server
  and neither sees the other's modules.

## 6 Regression coverage

### 6.1 Requirements
Headless tests drive one server process through multiple sessions over DAP
stdio: no-edit relaunch (fast, identical behavior), one-edit relaunch (only
the edit reprocessed; behavior matches cold), static/env isolation between
sessions, compiler-identity mismatch (clean self-exit), and debuggee
crash/kill followed by a working session.

### 6.2 Use cases
- 6.2.1 As CI (the by-hand debug-tests run), when residency regresses into
  staleness, state bleed, or a dead server, then a test fails before it
  reaches a live IDE.
