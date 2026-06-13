# Scope: eliminate per-test stdlib recompile in the release sweep

_Drafted 2026-06-13. Status: scoping (no code yet)._

## TL;DR

The release sweep is slow because **each test is run in its own process**, and
each process **re-parses + re-codegens the entire cajeta stdlib** before running
a tiny user program. The amortizing cache for exactly this already exists
(`test/jit/JitTestHelper.cpp` `StdlibCache`) — it primes the stdlib once per
**process** and reuses it across tests in that process. The sweep defeats it by
spawning one process per test. **Fix = change the harness process granularity so
the existing cache amortizes; no compiler change required.**

Expected win (prime cost is the dominant per-test cost; the user program is
tiny): Windows ~113 min → ~15–20 min (~6–7×), macOS ~5×, Linux ~3×.

## Root cause (grounded)

- `cajeta_tests.sh` parallel path: round-robins tests into N shard buckets; each
  shard **worker loops over its bucket launching `TEST_BIN --gtest_filter=$t`
  per single test `t`** under `timeout --kill-after=10 $TEST_TIMEOUT` (script
  ~line 330). One process = one test.
- `JitTestHelper.cpp` `StdlibCache` (lines ~216–360) primes the stdlib (parse +
  IR + codegen + static inits) **once per process** into a shared LLVMContext,
  captures a post-stdlib baseline, and `restoreBaseline()`s between tests so the
  next test reuses the primed module. Comment: "Collapses the ~14s/test stdlib
  parse+codegen to a one-time cost." It only helps when **multiple tests share a
  process** (e.g. `cajeta_test --gtest_filter='Suite.*'`).
- Net: per-test processes ⇒ prime paid 474× instead of ~(#processes)×.

## Why per-test processes were chosen (the constraint to preserve)

Documented in the script: (1) a hung test costs at most `TEST_TIMEOUT` and the
worker continues; (2) a crash (SIGSEGV/abort) doesn't take down the rest of the
bucket; (3) clean per-test crash/timeout attribution via synthetic markers.
Batching tests into one process trades these away unless mitigated.

## Options

- **A — whole-bucket-per-process.** Each shard runs its entire bucket as ONE
  `--gtest_filter=A:B:C:...` process. Max amortization (prime = #shards times,
  e.g. 4). Loses isolation: a crash/hang loses the rest of that bucket's results
  + attribution.
- **B — fixed-chunk batches.** K tests per process (e.g. K=25). Prime ≈
  ceil(474/K) times. Smaller blast radius per crash; less amortization.
- **C — batch-with-fallback (RECOMMENDED).** Run the whole bucket in one process
  (max amortization). If it exits non-zero (crash) or the batch deadline trips
  (hang), parse which tests already reported `[ OK ]/[ FAILED ]`, attribute the
  fault to the last `[ RUN ]` with no terminal line, then **re-run the
  not-yet-reported tests individually** (the existing per-test path) to recover
  isolation + attribution. Common case (all pass) = full speedup; failure case
  = automatic, localized fallback.
- **D — reuse-compat grouping (additive).** `StdlibCache::stdlibReusable(opts)`
  only reuses when a test's stdlib-affecting flags match the prime defaults;
  flag-divergent tests re-prime. Order buckets so divergent tests cluster (or
  run last) to avoid mid-batch re-primes. Minor; layer on top of C.

Bigger, NOT recommended now (only if 4× prime is still too slow or batching
proves unsafe):
- **Cross-process persisted prime:** serialize the primed module to disk, load
  per process (skips parse+IR, still pays codegen/link per process). Complex.
- **AOT stdlib → native dylib the JIT links:** skips codegen too, but blocked by
  monomorphization — templated stdlib (`Stream<T>`, `Optional<T>`,
  `ParallelDriver<T>`) can't be pre-instantiated for user types. Only the
  concrete (non-template) stdlib could be AOT'd; partial win, large effort.

## Recommended plan (Option C)

1. Add a `BATCH=1` mode to `cajeta_tests.sh` (default it on in
   `release_tests.sh`): each shard worker runs its bucket as a single
   `--gtest_filter=<bucket joined by ':'>` process with `--gtest_brief=1`.
2. Batch deadline = `min(bucket_size * PER_TEST_BUDGET, CAP)`; on `timeout`/crash
   exit, run the fallback.
3. Fallback: diff discovered-bucket vs reported `[ OK ]/[ FAILED ]`; re-run the
   remainder via the current per-test loop; emit the same synthetic CRASH/
   TIMEOUT markers + the last-running test name. Preserves KEEP_LOGS shard files
   and the flake-retry gate semantics.
4. Keep per-test mode as the fallback path AND as `BATCH=0` for debugging.

No changes to the compiler or `JitTestHelper` are required — the cache is built.

## Risks

- **R1 (key) — cross-test reuse correctness.** Per-test isolation may have been
  chosen partly because batched reuse can leak state (a test passing/failing
  differently than in isolation). Mitigation: gate the rollout on a one-time
  **batched-vs-per-test result diff** over the full suite (must be identical),
  and wire `CAJETA_STDLIB_VERIFY=1` (the existing `verifyPristine` leak detector)
  into a CI canary. If divergence exists, fix the leak or fall back to B.
- **R2 — hung-test detection in a batch.** gtest won't kill a hung test inside a
  process; the batch deadline + fallback finds it. Tune the deadline.
- **R3 — memory growth.** The cache accumulates template instantiations across a
  bucket ("bounded for built-in type args"); per-shard buckets bound it. Watch
  RSS on the largest bucket.
- **R4 — attribution on crash.** Parse the last unterminated `[ RUN ]`; the
  fallback re-run confirms which test actually crashes.

## Effort / sequencing

- Harness change (Option C) + result parsing + fallback: ~1–1.5 days.
- Correctness validation (R1 diff + verify canary): ~0.5 day.
- No compiler work. Land behind `BATCH` flag, validate on all 4 legs, flip
  `release_tests.sh` default once the diff is clean.

## Decisions needed from the user

1. Granularity: whole-bucket+fallback (C, recommended) vs fixed-chunk (B)?
2. Trust the in-process reuse machinery for release gating, contingent on a clean
   batched-vs-isolated diff? (R1.)
3. Default-on for release immediately after validation, or ship `BATCH=1`
   opt-in first?
