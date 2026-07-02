# Spec — Split oversized gtest suites across shards in `cajeta_tests.sh`

## 1. Definition

### 1.1 Purpose
`cajeta_tests.sh` runs the gtest battery in parallel across N shards. In its
default `BATCH=1` mode the **scheduling unit is a whole gtest suite**: each suite
runs as a single process so the expensive stdlib JIT prime (~5 s Linux / ~15 s
macOS / ~60 s Windows) is paid once and reused across that suite's tests.

The unit of scheduling being an entire suite means a suite far larger than the
others becomes a **single-shard long pole**: it cannot be spread across the idle
shards, so its tests run strictly serially in one process while the other shards
finish and sit idle. This capability lets the harness **split an oversized suite
into several batched chunks distributed across shards**, each chunk still priming
the stdlib once, so a large suite's tests run concurrently.

### 1.2 Problem being solved
On the Windows leg of `release.yml`, the "Run release tests" step takes ~95 min,
essentially all of it one suite — `ParallelStreamP1Tests` (49 tests):
- It is pinned to one shard as a single batched process while 31 shards idle.
- Its batch deadline `min(n·TEST_TIMEOUT, BATCH_CAP)` = `min(49·300, 1800)` = 1800 s;
  the process is killed at the cap (exit 124), ~30 min wasted.
- The kill triggers the per-test fallback, which re-pays the ~60 s Windows prime
  for **each** of the 49 tests → 49 × ~79 s ≈ 65 min.

So one suite ≈ 30 + 65 ≈ 95 min = the entire test phase. macOS survives the same
pathology only because its prime is ~15 s, not ~60 s.

### 1.3 Scope
- In scope: the shard-scheduling / batched-execution logic in `cajeta_tests.sh`
  (unit construction, bin-packing, the batched runner, and its fallback).
- In scope: a knob controlling the maximum tests per batched chunk, with a
  sensible default, overridable via environment variable.

### 1.4 Non-goals
- No change to which tests run (the release filter is untouched).
- No change to test source, the JIT, or the stdlib-prime mechanism itself.
- No change to `BATCH=0` (strict one-process-per-test) semantics.
- No attempt to make chunks of one suite *share* a single primed process — each
  chunk is an independent process that primes independently (that is the accepted
  cost of parallelism; see 3.2).

## 2. Chunked scheduling

### 2.1 Requirements
A "chunk" is the new scheduling unit in `BATCH=1`: an ordered, contiguous slice of
one suite's selected tests, carrying (a) a display name and (b) its explicit test
list. A suite with ≤ `BATCH_MAX` tests is exactly one chunk (unchanged behavior).
A suite with > `BATCH_MAX` tests is split into `ceil(n / BATCH_MAX)` chunks. Chunks
are the items bin-packed across shards.

Implementation must stay bash-3.2-compatible (macOS ships 3.2 — no `declare -A`,
no `mapfile`), consistent with the existing code's constraints.

### 2.2 Use cases
- **2.2.1** As the release CI, when a suite has more tests than `BATCH_MAX`, then
  its tests are divided into multiple chunks that are distributed across different
  shards and run concurrently, so the suite is no longer a single-shard long pole.
- **2.2.2** As a developer, when every selected suite is ≤ `BATCH_MAX`, then
  scheduling is identical to today (one chunk per suite) — no behavior change for
  the common case.
- **2.2.3** As the harness, when it bin-packs work, then it balances **chunks**
  (by test count) across shards using the existing longest-processing-time-first
  heuristic, so total per-shard test counts stay balanced.
- **2.2.4** As a developer, when I set `BATCH_MAX=<N>`, then that value overrides
  the default max-tests-per-chunk; when I set `BATCH=0`, then chunking does not
  apply (strict per-test isolation is preserved).

## 3. Prime reuse and cost balance

### 3.1 Requirements
Each chunk runs as one batched process, so the stdlib prime is still reused across
the tests **within that chunk** (the reuse win is preserved per-chunk, not lost).
Splitting a suite into k chunks pays the prime k times instead of once; the default
`BATCH_MAX` must be chosen so that the wall-clock win from parallelism dominates the
extra prime payments on the worst-prime platform (Windows).

### 3.2 Use cases
- **3.2.1** As the harness on Windows, when a 49-test suite is split into
  `ceil(49/16)=4` chunks of ≤16 tests across idle shards, then wall-clock for that
  suite drops from ~95 min to roughly `prime + BATCH_MAX·per-test-work` (single-chunk
  critical path, ~9 min), at the cost of ~4 primes instead of 1 — a net large win
  because the shards were otherwise idle.
- **3.2.2** As the harness, when it splits suites, then it does **not** increase the
  number of concurrently running shard processes (still ≤ `shards`), so the existing
  RAM-based shard cap continues to bound peak memory — splitting changes only *what*
  each shard runs, not how many run at once.

## 4. Correctness: crash/timeout attribution and counting

### 4.1 Requirements
Chunking must not regress the harness's reporting guarantees:
- The batched-chunk runner keeps the existing "trust output iff gtest ran to
  completion, else re-run this chunk's tests individually" fallback — so crash and
  timeout attribution by test name is preserved (now bounded to a chunk's tests).
- The end-of-run aggregation (pass/fail/timeout/crash tallies, the
  Discovered-reconciliation, the `Passed:/Failed:/…` summary line that
  `release_tests.sh` greps) must produce identical totals whether or not a suite was
  split.
- The per-chunk batch deadline scales with the chunk's test count
  (`n_chunk · TEST_TIMEOUT`, capped at `BATCH_CAP`), so a chunk is not killed for
  merely being legitimately long, and a killed chunk falls back over only its own
  ≤`BATCH_MAX` tests.

### 4.2 Use cases
- **4.2.1** As the release gate, when a split suite passes, then the aggregate
  `Passed/Failed/Timed out/Crashed` counts equal what an unsplit run would report
  (verified by comparing a split vs `BATCH_MAX=∞` run of the same filter).
- **4.2.2** As the release gate, when one test in a split suite crashes or times
  out, then it is reported by name (Suite.test) exactly as today, and only that
  chunk (not the whole suite) drops to per-test fallback.
- **4.2.3** As a developer, when a suite is split, then `--gtest_list_tests`-based
  drift guarding in `release_tests.sh` is unaffected (chunking happens after
  discovery and changes no suite/test names).
```
