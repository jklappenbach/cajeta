# benchmark-fidelity — spec

## 1. Definition

### 1.1 Purpose
The `samples/profile` cross-language benchmark suite and its generated report site
exist to show, honestly and completely, how Cajeta compares to Rust / C++ / Go /
Java / Python. Today the site **misrepresents Cajeta** in two ways: (a) it shows
*missing values* and *unfair comparisons* that make Cajeta look worse (or blank)
than it is, and (b) for several workloads the benchmark exercises a *slower path
than Cajeta is actually capable of*. This feature makes the published site
faithfully and completely represent Cajeta's true, optimized capability for the
in-scope benchmarks.

### 1.2 Scope
Two bodies of work:
- **Fidelity** — eliminate every missing-value and unfair-comparison defect so the
  site is honest: the Memory metric, equal-workload + spec-conformant competitors,
  benchmarks that currently produce no Cajeta result, and data-integrity outliers.
- **Quick-win optimization** — make the benchmark exercise Cajeta's real capability
  where the faster path already exists or is a direct SIMD transcription: the
  matmul CPU kernel-pool path, and vectorization of dot-product / saxpy /
  spectral-norm.

The deliverable of record is a **regenerated `report/sample`** produced in one clean
session where every category-(a) defect is resolved and every in-scope optimization
is reflected.

### 1.3 Non-goals (explicitly deferred to separate plans)
- Deep Stream-abstraction optimization (operator fusion, primitive specialization,
  removing per-element Optional boxing). Only the *fairness* of the stream benches
  is in scope here.
- Async task-spawn scheduler work (`task-spawn-await` vs Go/Java).
- Value-type / loop auto-vectorization codegen for the `time-*` benchmarks. Only a
  *fairness verification* of those benches is in scope.
- General collection performance (`arraylist-append`, `hashmap-*`, `sort-stable`).
- `blake3` (intrinsics-tuned reference ceiling) and `md5` (interop-only) — already
  honest, left as-is.

### 1.4 Definitions
- **Fair comparison** — every language performs the same logical workload (same
  element count / iteration count / dataset), the competitor uses the API the spec
  names (not a hand-substituted faster primitive), and each side's result is
  consumed so the optimizer cannot delete or constant-fold the measured work.
- **Missing value** — a CSV/site cell that should hold a comparable Cajeta number
  but is blank, `-1`, or `skipped` for a reason that is fixable.

---

## 2. Memory-metric completeness

### 2.1 Background
The report's **Memory** tab plots `alloc_bytes` (bytes requested from the allocator
in one execution) — the runtime-neutral metric every competitor emits (counting
allocator / Go MemStats / tracemalloc). The Cajeta harness emits `alloc_bytes = -1`,
so Cajeta is filtered out of **every** Memory chart on **every** page.

### 2.2 Use cases
- **2.2.1** As a reader, when I open the Memory tab of any benchmark, then I see a
  Cajeta bar with a real allocated-bytes value alongside the competitors.
- **2.2.2** As the harness, when I measure a benchmark, then I record the bytes the
  benchmark allocated during its measured execution (cumulative delta across the
  run), comparable in meaning to the competitors' allocation counters.
- **2.2.3** As a reader of a zero-allocation Cajeta benchmark (e.g. a reused-buffer
  hash), when I open the Memory tab, then Cajeta shows `0` (the leanest, tallest
  bar), not a blank.

---

## 3. Benchmark fairness (equal workload + conformant competitors)

### 3.1 Equal workload
- **3.1.1** As a reader of `string-build-concat`, when I compare languages, then
  every language builds the **same number of bytes**. (Today Cajeta runs 4000
  append iterations / 32 KB while the competitors run 500 / 4 KB, so Cajeta is
  charged ~8× the work and looks ~55× slower.)
- **3.1.2** As the suite, when any benchmark reports `input_size`, then the measured
  workload matches that size across all languages.

### 3.2 Spec-conformant competitors + anti-elision
- **3.2.1** As a reader of `stream-filter-map-reduce`, when I read the C++ column,
  then it uses the idiomatic streaming API the spec names (C++ `std::views`
  filter/transform), not a hand-fused scalar loop, and its result is sunk so it
  cannot be dead-code-eliminated.
- **3.2.2** As a reader of `stream-parallel-reduce`, when I read the C++ column,
  then it uses `std::reduce(std::execution::par, …)` (the spec-named parallel API),
  result sunk.
- **3.2.3** As a reader of `time-instant-arith` / `time-localdate-arith`, when I
  compare against C++, then the C++ loop performs the **same per-iteration time
  arithmetic** with the result consumed, so its number is not the product of
  constant-folding the loop to a closed form. (Verification + fix only if the
  current competitor elides the work; Cajeta-side time optimization is out of scope
  per 1.3.)

---

## 4. Result completeness — JSON DOM benchmarks

### 4.1 Background
`json-dom`, `json-serialize`, and `json-roundtrip` are `skipped` for Cajeta: the v1
JSON DOM parses integer-only JSON (it accepts only the `citm_catalog` dataset and
rejects `twitter` / `canada` because they contain floats).

### 4.2 Use cases
- **4.2.1** As the JSON DOM parser, when I encounter a JSON number with a fraction
  or exponent, then I parse it into a `float64` value (not error/skip).
- **4.2.2** As a reader, when I open `json-dom` / `json-serialize` /
  `json-roundtrip`, then Cajeta shows a real measured result on every configured
  dataset (no `skipped` rows for the float-parsing reason).
- **4.2.3** As the suite, when a JSON value round-trips (parse → serialize), then a
  float value reproduces correctly (the round-trip check passes).

---

## 5. Compute-kernel performance fidelity

### 5.1 matmul — CPU kernel-pool path
- **5.1.1** As a reader of `matmul`, when I compare languages, then the Cajeta
  number reflects its **multi-core `@Kernel` matmul** fanned across all cores by the
  XPU CPU dispatcher (the path that lands ~0.057 ms for n=200, under numpy/OpenBLAS
  0.072–0.079 ms), not the single-core SIMD loop (~0.448 ms).
- **5.1.2** As the benchmark, when run as an AOT release executable, then the
  kernel-pool fast dispatch is enabled and stable (the hot worker-spin path is
  AOT-safe), and the result still passes the existing correctness check.

### 5.2 Vectorized numeric kernels
- **5.2.1** As a reader of `dot-product`, when I read the Cajeta column, then it is
  computed with SIMD `Vector<float64,8>` accumulators + horizontal sum, not a scalar
  `sum += x[i]*y[i]` loop.
- **5.2.2** As a reader of `saxpy`, when I read the Cajeta column, then it is
  computed with SIMD vector load/FMA/store, not a scalar loop.
- **5.2.3** As a reader of `clbg-spectral-norm`, when I read the Cajeta column, then
  the inner matrix-vector products are vectorized (the x12 scalar gap is closed as
  far as a SIMD transcription allows).
- **5.2.4** For every kernel in 5.1–5.2, the existing numerical correctness check
  continues to pass (results are bit-/tolerance-equal to the scalar reference).

---

## 6. Data & measurement integrity

### 6.1 Use cases
- **6.1.1** As a reader, when I view `clbg-fannkuch-redux`, then the published Cajeta
  number is a representative steady-state measurement, not a one-off outlier. (The
  current sample shows 335 ms where an earlier clean run showed ~147 ms — fastest in
  field; this must be re-verified and corrected if an outlier.)
- **6.1.2** As the maintainer, when the plan completes, then `report/sample` is
  regenerated in **one clean session** (Cajeta + all competitors) so all numbers are
  mutually consistent, every category-(a) defect from §2–§4 is resolved, and the
  §5 optimizations are reflected.
- **6.1.3** As a reader, when a benchmark is legitimately skipped, then the skip
  carries an accurate, current reason (no stale or misleading skip notes).

---

## 7. Acceptance (feature-level)
- **7.1** The Memory tab shows a Cajeta value on every benchmark page.
- **7.2** No Cajeta benchmark is unfairly charged a heavier workload than its
  competitors, and no competitor column measures elided work.
- **7.3** `json-dom` / `json-serialize` / `json-roundtrip` produce real Cajeta
  results on all configured datasets.
- **7.4** `matmul`, `dot-product`, `saxpy`, `clbg-spectral-norm` reflect Cajeta's
  vectorized / kernel-pool capability, with correctness checks intact.
- **7.5** `report/sample` is regenerated cleanly and committed; every claim above is
  visible in the published site.
