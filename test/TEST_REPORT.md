# Cajeta Test Suite — Quality & Reliability Report

_Linux baseline, generated from two full isolate-mode runs (D + E)._
_Branch: `main` @ `5bda23b` (post `cajeta-net` merge). Host: 32 cores._

> **Note (runner retired).** The dedicated Python runner `scripts/test-tui.py`
> has been removed. Its live per-shard progress display now lives in
> `cajeta_tests.sh` itself (an interactive run draws an overall bar + one row per
> shard). The process-per-test `--isolate` mode and the `--results` JSON
> aggregation are no longer available; the `scripts/test-tui.py …` commands below
> are retained only as a record of how this report was produced. To run a single
> test in isolation today, use `PARALLEL=0 ./cajeta_tests.sh <Suite.test>`.

---

## TL;DR

| Metric | Count | of 3322 |
|---|---:|---:|
| **Passing** | **3278** | 98.7% |
| **Broken here (deterministic)** | **44** | 1.3% |
| **Flaky (timing-dependent)** | **5** | 0.2% |
| Not run (coverage gap) | **0** | 0% |

> **Update 2026-06-06:** 15 of the 44 deterministic failures resolved.
> - `CompilerTests` ×2 — stale constant (test bug).
> - `Base64Tests` ×9 — harness violated the ownership-transfer rule (test bug).
> - `UriResolveTests` ×2 — **one real product bug fixed** (`Uri.resolve` threw
>   on an empty/same-document reference, RFC 3986 §5.2) + the vectors test
>   rewritten to compile once (320s → ~8s).
> - `WsFrameCodecTests` ×2 — **not hangs**, slow (62–63s of repeated stdlib
>   compiles); pass under the raised 120s per-test timeout.
>
> The four "pure-compute hangs" were the headline of the triage: **only
> `UriResolve` hid a real bug** — the 60s timeout had been masking it by killing
> the test before it reached the empty-reference vector. `Sha256` (46s) and
> `WsFrameCodec` (62–63s) are correct, just slow. Remaining deterministic:
> **29** (mostly net-stack crashes). See [Fixed](#fixed-this-pass).

- **The suite cannot complete at all under the legacy `cajeta_tests.sh` shard
  model**: a handful of net tests crash/hang *early* in their shard and, because
  gtest runs ~100 tests per shard process, take the rest of the shard down with
  them. First measurement lost **53% of tests to "notrun."** The fix is
  structural, not per-test (below).
- **Most breakage is the freshly-merged net stack**: 29 of the 44 deterministic
  failures are in `cajeta-net` (DNS/HTTP/WS/TLS/streaming).
- **Real non-net bugs exist too**: `Base64.decode` is completely broken (all 9
  decode tests fail, including empty input, while every encode test passes),
  plus struct-view bounds and a SHA-256 hang.
- **The only genuine flakiness is in the fiber scheduler** (`async` / parallel
  streams / `spawn`) — these hang under parallel load.
- **2 "failures" are not product bugs** — a stale hard-coded constant
  (`STDLIB_STRUCTURE_COUNT`) the net merge forgot to bump.

---

## Methodology

### Why a new runner (`scripts/test-tui.py --isolate`)

The legacy `cajeta_tests.sh` splits the suite into 32 shards, each a single
`cajeta_test` process running ~100 tests. Three problems make it unable to
*measure* (let alone report) reliability:

1. **Crash/hang contagion.** One crashing test kills its whole shard process —
   the ~100 tests behind it become `notrun`. Ten `DnsCacheTests` scattered
   one-per-shard wiped out ~790 tests of visibility in a single run.
2. **Global-state leakage.** `CajetaModule::getStructureToModule()` is a
   `static` map never cleared between tests, so order-dependent assertions
   pollute across a shard.
3. **Masked flakiness.** The shard runner silently re-runs failed shards
   serially and counts only deterministic failures — green CI can hide a real
   intermittent bug (`cajeta_tests.sh:385`).

**Process-per-test isolation fixes all three at once, for free.** Measured on
this host:

| Mode | Time | Per test |
|---|---:|---:|
| Binary startup (`fork`/`exec`, no test) | 0.022 s | — |
| 1 JIT test, fresh process | 8.37 s | 8.37 s |
| 14 JIT tests in **one** process | 129.6 s | 9.25 s |

The dominant cost — the ~8 s stdlib JIT recompile — is paid **per test even
inside a shared process** (zero amortization; batching is actually *slower* per
test as the long-lived process bloats). A `fork`/`exec` is 22 ms. So one fresh
process per test costs the *same* wall-clock as sharding but contains every
crash/hang to a single test and gives each test a clean global state.

### Runs

- **Run D, Run E** — two full `--isolate` passes, 32 lanes, 60 s per-test
  timeout. ~19 min each, **0 notrun** (full coverage both times).
- **Deterministic** = broken in *both* D and E. **Flaky** = broken in exactly
  one. Raw data: `test/test-quality/runs/run{D,E}-isolate.json`.

---

## Broken here on Linux — 44 deterministic

Every test below failed identically in both runs.

### Net stack (`cajeta-net` merge) — 29

| Suite | Tests | Signal | Likely cause |
|---|---:|---|---|
| `DnsCacheTests` | 10 (crash) | SIGSEGV ~20–31 s in | DNS reactor / fiber resolve crashes (whole suite) |
| `StreamingBodyTests` | 5 crash, 1 fail | crash ~18–24 s | chunked/content-length request-body streaming |
| `WsErrorHierarchyTests` | 4 (crash) | crash ~16–20 s | WS exception-as-root paths bring a live connection up |
| `WsFrameCodecTests` | 2 ~~(hung)~~ **not a bug** | slow (62-63s) | `encode/decodeGoldenVectors` — ~8 vectors × stdlib compile; **pass**, just slow. Durable fix: compile once. |
| `GoldenHttp` | 2 (fail) | 0.1 s | byte-exact golden fixtures (CRLF framing / manifest bytes) |
| `UriResolveTests` | ~~1 hung, 1 fail~~ **FIXED** | real bug | `Uri.resolve(base, "")` threw `MalformedUriException` (RFC 3986 §5.2 empty/same-document reference is valid). Fixed in `Uri.cajeta`; vectors test rewritten to compile once. |
| `HttpsServerTests` | 1 (crash) | crash ~17 s | `httpsRequestEndToEnd` |
| `NetExceptionTests` | 1 (crash) | crash ~15 s | `detailMessagePassesThrough` |
| `BufferPoolTests` | 1 (crash) | crash ~17 s | `reuseStaysBounded` |

Two of these are **pure-compute and hang with no network** (`WsFrameCodec`
golden vectors) — strongly suggesting an infinite loop in the WS frame codec,
independent of sockets. Worth triaging first; it's likely a small logic bug.

### Core language / stdlib (not net) — 13

| Suite | Tests | Signal | Note |
|---|---:|---|---|
| `Base64Tests` | ~~9 (fail)~~ **FIXED** | test bug | **Not a product bug.** The `runDecode` harness returned an owned `#int8[]` local without `#` transfer, tripping `CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER` at codegen — so every decode test threw. `Base64.decode` is correct. Harness fixed (`#int8[] run()` + `return #raw;`). |
| `StructViewBoundsTests` | 2 (fail) | assertion | `exactSizeBufferConstructs`, `oneByteShortStillThrows` — view bounds checking |
| `Sha256Tests` | 1 ~~(hung)~~ **not a bug** | slow (46s) | `fipsVectorsOneShotAndIncremental` does 6 stdlib JIT compiles; passes given time. Durable fix: compile once. Now under the 120s timeout. |
| `ViewMethodsTests` | 1 (fail) | assertion | `readOnlyMethodReturnsPrimitive` |

### Not a product bug — 2

| Suite | Tests | Cause | Fix |
|---|---:|---|---|
| `CompilerTests` | 2 (fail) | `canParseOnValid{Long,Short}Package` assert the stdlib loads exactly `1 + STDLIB_STRUCTURE_COUNT` (123). The net merge added stdlib classes; the constant is stale. Fails even in a fresh process (so it's the constant, not the global-state leak). | Re-anchor `STDLIB_STRUCTURE_COUNT` (`test/compile/CompilerTests.cpp:66`) to the actual count. |

---

## Fixed this pass

Both turned out to be **test defects, not product bugs** — the suite was
emitting false failures.

| Tests | Fix | Result |
|---|---|---|
| `CompilerTests.canParseOnValid{Long,Short}Package` | Re-anchored `STDLIB_STRUCTURE_COUNT` 123 → 264 (net merge loaded ~141 new prelude structures; live count is 265). `test/compile/CompilerTests.cpp:66`. | both pass |
| `Base64Tests.decode*` (9) | `runDecode` harness now uses `#int8[] run()` + `return #raw;` so the owned decode result transfers correctly. `test/parser/Base64Tests.cpp`. | all 29 `Base64Tests` pass |
| `UriResolveTests` (2) | **Product fix:** `Uri.resolve` now synthesizes the empty reference instead of calling `Uri.parse("")` (which throws). **Test perf:** `rfc3986…Vectors` compiles once (one helper method per vector + a dispatcher) — 320s → ~8s. `runtime/src/cajeta/net/uri/Uri.cajeta`, `test/expression/UriResolveTests.cpp`. | all 13 `UriResolveTests` pass |
| `Sha256` / `WsFrameCodec` ×3 (false "hung") | Not bugs — slow multi-compile golden-vector tests (46–63s). Raised isolate per-test timeout 60s → 120s. Durable fix (compile-once) tracked as a follow-up. | no longer flagged hung |

> Lesson for "accuracy": the first two failures investigated were *both* the
> test's fault. Worth re-checking whether the remaining "fails" (e.g.
> `StructViewBounds`, `ViewMethods`, `GoldenHttp`) are product bugs or more
> harness/fixture issues before assuming the product is broken.

## Flaky — 5 (timing-dependent, under parallel load)

Passed in run D, hung (60 s timeout) in run E. **All in the fiber/concurrency
family** — consistent with the "fiber-park / scheduler bug" history in the merge.

| Test | D | E |
|---|---|---|
| `AsyncSyntaxTests.scopeBlockExecutesContents` | pass | hung |
| `ParallelStreamP1Tests.parallelAllMatchLargeSourceOneFailure` | pass | hung |
| `ParallelStreamP1Tests.parallelCollectViaSupplierAggregatesAll` | pass | hung |
| `ParallelStreamP1Tests.parallelForEachDispatchesThroughFilter` | pass | hung |
| `SpawnDropTests.awaitSpawnDropsOnce` | pass | hung |

These hang only under 32-lane pressure (they pass when the scheduler isn't
contended). Root cause is almost certainly a race in the fiber scheduler /
parallel-stream dispatch, not the tests themselves. Highest-value reliability
fix: these are the tests that will randomly redden CI.

---

## Windows comparison

You flagged known-broken tests on Windows. This report is the **Linux** column;
to get a directly comparable Windows column, run the same isolate pass there.

- **Linux broken-here: 44 deterministic + 5 flaky** (list above).
- Caveat: `scripts/test-tui.py` shells out to GNU `timeout(1)`, which isn't on
  native Windows. To run the isolate pass on Windows we need a portable per-test
  timeout (a small Python `subprocess` watchdog instead of `timeout`). That's a
  ~20-line change — see [Follow-ups](#follow-ups). Until then, compare against
  `cajeta_tests.cmd` output.
- Expected overlap: the net-stack crashes are likely platform-independent (logic
  bugs in the new code); the `WsFrameCodec`/`Sha256`/`UriResolve` hangs are pure
  compute and should reproduce on Windows; `GoldenHttp` CRLF fixtures may differ
  by platform line-endings and are worth checking first.

---

## Recommended fix order

Ranked by value-to-effort for "dependable, reliable, accurate":

1. **`STDLIB_STRUCTURE_COUNT`** — 1-line constant bump; clears 2 false failures.
2. **`Base64.decode`** — fully broken, pure logic, 9 tests; high signal, likely
   small fix in `runtime/src/cajeta/codec/Base64.cajeta`.
3. **Pure-compute hangs** (no concurrency, no network): `WsFrameCodec` golden
   vectors, `Sha256` FIPS incremental, `UriResolve` RFC-3986 — each a probable
   infinite loop, each isolated and easy to bisect.
4. **Fiber-scheduler flakiness** (the 5 flaky) — the real "tests are flaky"
   complaint; fixing the scheduler race makes CI trustworthy.
5. **Net-stack crashes** (DnsCache, StreamingBody, WsErrorHierarchy, Https,
   NetException, BufferPool) — larger surface; track as net-stabilization.
6. **`StructViewBounds` / `ViewMethods`** — small language correctness bugs.

---

## Follow-ups (test infrastructure)

- **Adopt `--isolate` as the default measurement mode.** It is the only way to
  get accurate, contagion-free, full-coverage results. Same wall-clock as
  sharding.
- **Portable per-test timeout** so the isolate runner works on Windows (replace
  the GNU `timeout` wrapper with an in-process watchdog).
- **Stop masking flakiness** in `cajeta_tests.sh` (the silent serial retry that
  counts only deterministic failures), or at least surface what it recovered.
- **Fix the global-state leak**: clear `CajetaModule::getStructureToModule()`
  between tests (a gtest fixture/`SetUp`), so count assertions are robust even
  outside isolate mode.
- **Compile-once for golden-vector tests.** Any test that loops N vectors with
  one `CajetaJit::compile` each pays N × ~8s. `UriResolve` was rewritten this
  way (one helper method per vector + a dispatcher, all in one module: 320s →
  ~8s). Apply the same to `WsFrameCodecTests.{encode,decode}GoldenVectors` and
  `Sha256Tests.fipsVectorsOneShotAndIncremental` (and audit other `*Vectors`
  tests). Caveat: folding all vectors into one *function body* (many owned
  locals/blocks) is fine, but keep each vector in its own helper method to
  match the patterns that JIT cleanly.

---

## Reproducing

```sh
# Whole suite, isolated, live per-test progress + JSON:
scripts/test-tui.py --isolate --results /tmp/run.json

# A single suite or test (easy subsets):
scripts/test-tui.py --isolate DnsCacheTests
scripts/test-tui.py --isolate Base64Tests.decodeFoobar
scripts/test-tui.py --isolate 'Net*' 'Tls*'

# Interactive curses per-lane view (drop --no-tui / a tty):
scripts/test-tui.py --isolate 'Ws*'
```

Per-test stream line format:
`HH:MM:SS  done/total  pct%  STATUS  Suite.test  duration  start HH:MM:SS`

Raw data for this report: `test/test-quality/runs/run{D,E}-isolate.json`
(+ `.log` for the full per-test streams).
