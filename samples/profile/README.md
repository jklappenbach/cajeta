# Cajeta profile — cross-language benchmark & diagnostic suite

A sibling of the [`tour`](../tour/README.md): where the tour shows a feature *works*,
`profile` shows how it *competes*. It measures Cajeta's shipped capabilities against the
most popular solutions in Rust, C++, Java, Python, and Go on community-recognized
workloads, and emits a fully reproducible record (versions, flags, hardware, timing,
memory, correctness cross-check) plus a **Cajeta-themed HTML report site**.

Spec: [`docs/specs/profile-spec.md`](../../docs/specs/profile-spec.md). Plan:
`agents/cajeta/profile/profile-plan.md`.

## Quick start

```sh
./bench.sh                 # fetch datasets, build --release, run all, write the report site
./bench.sh --area sort     # one capability area
./bench.sh --bench saxpy   # one benchmark
./bench.sh --list          # list registered benchmarks + areas (no run)
```

Each run writes `results/<timestamp>/` containing `results.csv` (the raw, reproducible
source of truth), `env.csv` (the host block), `report.md`, and **`site/index.html`** —
open it in a browser. A committed example lives at
[`report/sample/site/index.html`](report/sample/site/index.html).

The compiler is expected at `<repo>/build-cajeta/src/cajeta` (the fork-LLVM build — it
bundles the TLS shim and is the toolchain the GPU area needs). Override with `CAJETA=`.

## Status

**Cajeta-side (Phase A) complete across 9 areas; competitor languages (Phase B) and GPU
are pending.** Every benchmark carries a structural correctness cross-check and is
measured `--release`.

| area | benchmarks | notes |
|---|---|---|
| **codec** | json-tokenize, json-bind-skip, json-dom, json-serialize, json-roundtrip, json-conformance, base64 enc/dec | bind-skip ~4.5 GB/s (10× tokenize); DOM only on integer-only inputs |
| **collection** | ArrayList, HashMap (int/str), HashSet, LinkedList, Heap, RedBlackTree | string keys ~5× int per-op |
| **sort** | sort-int64 (pattern matrix), sort-f64, sort-stable, binary-search | |
| **string** | build-concat, search, replace, uppercase | concat is O(N²) — no StringBuilder |
| **hash** | xxhash3 (44 GB/s), siphash, sha256, md5 | |
| **stream** | filter→map→reduce, parallel-reduce | |
| **math** | saxpy, dot-product, matmul | scalar; `Vector<T,N>` SIMD = follow-on |
| **clbg** | mandelbrot, fannkuch-redux, spectral-norm | canonical checksums verified |
| **time** | instant-arith, localdate-arith | |
| **concurrent** | atomic-fetchadd, task-spawn-await | ~24 µs/spawn (stackful fiber) |

## Diagnostic findings (surfaced as data, not crashes)

The suite doubles as a diagnostic harness. Limitations are recorded as **skip rows with a
reason** rather than crashing:

- **JSON DOM doesn't parse floats** (v1) — `json-dom`/`serialize`/`roundtrip` run on the
  integer-only `citm_catalog`; twitter/canada skip.
- **`Sort.sort` quicksort overflows its 128-deep range stack** on adversarial patterns
  (no introsort) — `sort-int64` runs `random`, skips ascending/descending/dups.
- **Live-allocation tracking caps at 65536** — node-heavy benchmarks are sized under it;
  CLBG binary-trees and the 1M-live-task footprint are deferred.
- **No `StringBuilder`** — `string-build-concat` is O(N²) (churns ~1.8 GB to build 32 KB).
- **Value-type in-place reassignment in a loop is a no-op** — time benchmarks derive a
  fresh `Instant`/`LocalDate` each iteration.
- **Debug builds stack-overflow on >~256 KiB buffers** — benchmarks are measured `--release`.

## Layout

```
samples/profile/
├── bench.sh                 ← top-level driver (fetch → build → run → env → report)
├── cajeta.json              ← build-tool manifest
├── src/main/cajeta/profile/ ← the Cajeta harness + benchmarks (one subpackage per area)
│   ├── Profile.cajeta           entry: registers benchmarks, --run/--list/--area/--bench
│   ├── Benchmark.cajeta         base (setup/run/teardown/checkResult + variants + skips)
│   ├── Harness.cajeta           measurement loop (warmup/trials/stats/memory)
│   ├── report/                  Result, Report, Csv, Memory (CSV emission)
│   └── codec/ collection/ sort/ string/ hash/ stream/ math/ clbg/ time/ concurrent/
├── datasets/                ← manifest.tsv + fetch.sh (content-addressed, SHA-256)
├── competitors/             ← per-language runner scaffolds + vendor.sh (Phase B)
├── scripts/                 ← probe-toolchains.sh, env-capture.sh
├── report/                  ← report.py (CSV → Cajeta-themed site) + sample/
└── test/                    ← *_smoke.sh — one per area + infra (the task-scoped tests)
```

## Tests

Each area + infrastructure piece has a `test/*_smoke.sh` that builds, runs, and asserts
the cross-checks:

```sh
bash test/sort_smoke.sh        # one area
for t in test/*_smoke.sh; do bash "$t"; done   # all
```

## Adding a benchmark

1. Write a `Benchmark` subclass in the area subpackage (`src/main/cajeta/profile/<area>/`),
   overriding `name()`, `area()`, `run(iterations)` (or `runVariant`), and `checkResult()`.
2. Append one `benchmarks.add(heap MyBench());` line in `Profile.cajeta`.

The runner picks it up automatically. Dataset-backed benchmarks override `available()`;
per-input limitations use `supportsVariant()`/`skipReason()` to record skips as data.
