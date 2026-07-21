# fast-debug-launch — spec

## 1 Definition

### 1.1 Purpose
Make starting a debug session fast: seconds on a warm project, not minutes.
Today every `cajeta dap` launch recompiles the entire program and prelude
from source before the first breakpoint can hit.

### 1.2 Problem
`buildJit` (CajetaJitHost.cpp) constructs a fresh Compiler and compiles every
`.cajeta` under the source root into a throwaway archive root
(`/tmp/cajeta_jitrun_*`). It never sets a cache manifest — the incremental
machinery (`--cache-manifest`, clean/dirty designation, cache slots,
discriminator keying) is wired only to the `cajeta compile` CLI. Building the
project first therefore does not help the debugger at all; a JVM-style
"compiled output loads instantly" experience is structurally absent. Measured
on samples/tour 2026-07-20: ~40s to the first stop with the Release compiler,
several minutes with the Debug compiler. During the compile the IDE shows
nothing, which reads as a hang (observed live, twice).

### 1.3 Constraints
- The cache discriminator keys on compile flags, so debug-flavored compiles
  (Debug mode, safepoints, debugInfo) get their own cache slots. "Warm"
  means a prior debug launch or a debug-flavored build populated them — the
  first-ever debug launch may still pay full price.
- Cache reuse must not change debug behavior: safepoints, loc ids, and drop
  thunks must be identical to a from-scratch compile (the jit-drop-backfill
  regression tests must stay green).
- Correctness beats speed: on any cache doubt (discriminator mismatch,
  missing slot), fall back to compiling — never a stale class in the JIT.
- Perf acceptance is by review: commit before/after numbers, no programmatic
  thresholds.

### 1.4 Non-goals
- Debugging AOT-compiled binaries (the JIT session model stays).
- Per-function lazy JIT / interpreter tiers.
- Speeding up the first-ever cold compile of a project.
- Replacing the Debug compiler binary in developer setups (a settings
  concern, handled in run-config docs).

## 2 Cache-backed JIT build

### 2.1 Requirements
`buildJit` gains an incremental mode: given a persistent per-project debug
cache location, it loads clean classes from cache slots and compiles only
dirty sources, using the same manifest/discriminator machinery as the AOT
path. Every debug launch reads AND repopulates the cache, so launch N+1
pays only for what changed since launch N. The DAP `launch` request carries
the cache location (plugin-provided, derived from the project); absence
means today's full compile.

### 2.2 Use cases
- 2.2.1 As a developer who debugged tour five minutes ago, when I launch a
  new debug session with no edits, then no source is recompiled and the
  first breakpoint hits in seconds.
- 2.2.2 As a developer who edited one file, when I relaunch, then only that
  file (plus its dependents per the manifest) recompiles.
- 2.2.3 As a developer switching compiler binaries or flags, when I
  relaunch, then the discriminator mismatch invalidates the cache and the
  session compiles from scratch — never runs stale code.
- 2.2.4 As the jit-drop-backfill regression suite, when the cache serves
  classes, then drop thunks and safepoints are byte-for-byte equivalent to a
  cold compile.

## 3 Launch progress visibility

### 3.1 Requirements
While the server compiles, it emits DAP `output` events reporting progress
(at least: compile started, N/M sources, compile finished). The plugin
surfaces them in the debug console so a working launch never looks hung.

### 3.2 Use cases
- 3.2.1 As a developer starting a cold debug session, when the compile runs,
  then the console shows live progress instead of silence.

## 4 Regression coverage

### 4.1 Requirements
Headless tests in cajeta_debug_test cover: warm relaunch compiles zero
sources (cache-hit path), single-file edit recompiles only the dirty set,
discriminator mismatch falls back to full compile, and a cached launch still
hits breakpoints/steps identically to a cold one.

### 4.2 Use cases
- 4.2.1 As CI (the by-hand debug-tests run), when cache reuse regresses into
  staleness or a slow path, then a test fails before it reaches a live IDE.
