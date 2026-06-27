# Benchmark Gap Sweep — Spec

## 1. Definition

### 1.1 Purpose
The profile benchmark site (`samples/profile/report/sample/site/`) must fairly
represent Cajeta's true optimized capability. The benchmark-fidelity campaign closed
the *fidelity* gaps (missing values, unfair competitor work). This campaign closes the
remaining *tractable performance* gaps — workloads where Cajeta loses to the best
competitor and a high-confidence, low-risk optimization exists.

### 1.2 Scope
Algorithmic / codegen wins that do not require an architectural rewrite. Each is a
self-contained benchmark or stdlib hot path. Ordered by tractability so visible wins
bank first.

### 1.3 Non-goals
- **Streams** (`stream-filter-map-reduce` 116×, `stream-parallel-reduce` 180×). The gap
  is the pull-based `Optional<T>`-per-element virtual-dispatch model; matching C++/Java
  needs escape-analysis devirtualization + chain inlining (or an internal-iteration
  rewrite). Deferred to its own spec/plan — too large and risky for this sweep.
- **task-spawn-await** (15× vs Go). Go's goroutines are a runtime-scheduler advantage;
  Cajeta already beats Rust/C++/Java/Python here.
- **md5** (portable-C, no OpenSSL asm — accepted), **blake3** (asm-tuned reference —
  accepted), **json-dom** (no SIMD parser — architectural).
- **time-*** (value-type in-place reassign limitation — deferred per fidelity spec).

## 2. Mandelbrot (clbg-mandelbrot)
Cajeta 28.78ms vs cpp 23.64ms (~1.22×); also behind go/rust/java. Scalar per-pixel
escape-time loop; the C++/Rust CLBG entries auto-vectorize.

### 2.1 Use case
As a benchmark reader, when I view clbg-mandelbrot, then Cajeta is at or ahead of the
compiled scalar field (cpp/go/rust/java ~23–26ms), by vectorizing 8 pixels/block in
`Vector<float64,8>` with a monotonic membership mask and an all-escaped early-out.

## 3. Atomic fetch-add (atomic-fetchadd)
Cajeta 5.28ms vs rust/cpp/go/java ~3.91ms (~1.35×). A single-threaded tight loop of
atomic fetch-add; the gap is per-iteration overhead around the atomic op.

### 3.1 Use case
As a benchmark reader, when I view atomic-fetchadd, then Cajeta is within ~10% of the
~3.91ms native-atomic field (the hardware `lock xadd` throughput floor), with no
extraneous per-iteration work (bounds checks, redundant loads) around the atomic.

## 4. ArrayList append (arraylist-append)
Cajeta 0.181ms vs rust/cpp 0.035ms (~5×); ahead of go/java/python. `ArrayList<int32>`
backed by a doubling `T[]`; the gap is per-`add` overhead (bounds check, capacity test,
growth strategy, or primitive boxing).

### 4.1 Use case
As a benchmark reader, when I view arraylist-append, then Cajeta is materially closer to
the Vec/vector floor (0.035ms) — root-cause the per-add overhead and remove it without
changing the one-unified-allocation model.

## 5. Sort sorted-input fast path (sort-int64 ascending / descending)
Cajeta 0.065ms asc / 0.075ms desc vs rust 0.011 / 0.013 (~6×); the run-detection
fast-path exists (vs 0.568ms random) but is slower than ipnsort/pdqsort's.

### 5.1 Use case
As a benchmark reader, when I view sort-int64 ascending/descending, then Cajeta's
already-sorted detection approaches the native fast-path (single linear scan, no
redundant work), narrowing the ~6× gap toward the pdqsort 0.020ms class.

## 6. Acceptance (whole campaign)
- Every optimized benchmark still passes its `checkResult()` oracle.
- The fidelity gate (`scripts/verify-fidelity.py`) stays green.
- Each win is measured (before/after min+median) and recorded.
- The site is regenerated and the standings re-read at the end.
