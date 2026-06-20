# profile — cross-language benchmark & diagnostic suite (spec)

> Status: **draft, pending approval**. Authored with the **design** skill.
> Sibling spec to the language **tour**; this is the *why* and the *what*.
> The actionable *how* lives in [`agents/cajeta/profile/profile-plan.md`](../../agents/cajeta/profile/profile-plan.md).
> Competitor libraries, datasets, and methodology in §4–§15 are pinned from two
> verified deep-research passes (25/25 then 24/25 claims confirmed 3-0); see §18 for the
> suites we mirror and §17 for the resolved decisions.

---

## 1. Definition

### 1.1 Purpose
`profile` is a **cross-language benchmark, comparison, and diagnostic suite** that
measures how Cajeta's shipped capabilities perform against the most popular
solutions in **Rust, C++, Java, Python, and Go** on widely-recognized workloads. It
is the performance counterpart to the language **tour**: where the tour demonstrates
that a feature *works*, `profile` demonstrates how that feature *competes* — and
emits everything a third party needs to reproduce each number.

### 1.2 The problem it solves
Today Cajeta's only cross-language measurement is the ad-hoc JSON comparison under
[`bench/`](../../bench/README.md) (Cajeta vs Jackson/Gson/serde_json). There is no
general, repeatable, self-documenting way to (a) quantify Cajeta's standing across
its breadth of capabilities, (b) catch performance regressions, or (c) hand a third
party everything they need to reproduce a result. `profile` generalizes and
formalizes the `bench/` approach into a standing suite grounded in the benchmark
suites the community actually uses (§18).

### 1.3 Scope
1. A sibling sample at **`samples/profile/`**, built and run with the **cajeta build
   tool** exactly like `samples/tour/` (manifest + `build.sh`/`run.sh`, no
   hand-rolled compile scripts).
2. A **benchmark harness** (timing + memory measurement, warmup, repeated trials,
   baseline subtraction, structured report emission).
3. A **catalog of ~77 benchmarks** (§5–§15) grouped by capability area, each with a
   Cajeta implementation and **several** competitor implementations per applicable
   language (the brief: see where Cajeta lands against the full spread).
4. A **reproducibility record** (§3) emitted with every result: test + version,
   language + version, library + version, hardware specs, per-trial timing, memory
   (peak RSS, allocated bytes/count, working-set delta), correctness cross-check, and
   the exact flags used.
5. **Reference datasets and reference implementations** pinned to recognized sources
   (nativejson corpus, Computer Language Benchmarks Game, sort-research-rs, Martin
   Ankerl's hashmap suite, SMHasher, BabelStream, mixbench — §18), fetched on demand
   and checksum-verified.

### 1.4 Constraints
1. Only **shipped, tested** Cajeta capabilities are benchmarked. Regex, networking,
   and file-I/O-bound workloads are excluded until those stdlib areas ship (§1.5).
2. Cajeta is **native AOT** (`--release`); no JIT warmup. Competitor harnesses must
   apply each language's idiomatic warmup (JVM JIT, CPython, Go) so comparisons are
   steady-state and fair (§4).
3. Every competitor toolchain and library version is **pinned and recorded**; a run
   that cannot record a version must fail loudly rather than report an unlabeled number.
4. Benchmarks must be **apples-to-apples**: the same logical operation on the same
   input, with allocation/IO baselines subtracted where they would distort the
   measured operation, and structural correctness cross-checks confirming parity.
5. All comparisons run on the **reference machine** (AMD Ryzen AI Max+ 395, Zen 5
   16C/32T, 64 GiB, Ubuntu 26.04, kernel 7.0.0-22, gfx1151) and degrade gracefully
   (skip with a recorded reason) where a backend or competitor toolchain is absent.
   Vendor self-benchmarks are not trusted: every number is produced **on our hardware**.

### 1.5 Non-goals
1. **Not** a microbenchmark framework to ship in the stdlib (it's a sample/tool).
2. **Not** a claim of universal superiority — honest losses are first-class results,
   same as wins (mirrors `bench/`'s honesty about the Jackson gap).
3. **Not** benchmarking unshipped features (regex `Pattern`/`Matcher` — so CLBG
   regex-redux is excluded; sockets/HTTP; `Path`/`Directory` I/O; process management;
   arbitrary-precision `pidigits` is excluded unless a bignum path is in scope).
4. **Not** a CI gate in v1 — regression-tracking hooks are a documented follow-up,
   not a v1 deliverable (the JSON schema is designed to allow diffing later).
5. **Not** a distributed/multi-node benchmark — single-machine only.

---

## 2. Harness & project layout

### 2.1 Requirements
1. `samples/profile/` is a standard cajeta project: `cajeta.json` manifest, sources
   under `src/main/cajeta/profile/<area>/`, `build.sh`/`build.cmd`, `run.sh`/`run.cmd`,
   and a `README.md` — mirroring `samples/tour/`.
2. Each benchmark is a class extending a shared `Benchmark` base (cf. tour's
   `DemoClass`) exposing: `name()`, `area()`, `setup()`, `run(iterations)`,
   `teardown()`, `datasetInfo()`, and `checkResult()` (the correctness cross-check). A
   `Profile` entry point walks an `ArrayList<Benchmark>` exactly as `Tour.run()` walks
   `demos[]`.
3. Benchmarks are organized into topic subpackages mirroring the stdlib layout and
   §5–§15 (`profile.codec`, `profile.collection`, `profile.sort`, `profile.string`,
   `profile.hash`, `profile.stream`, `profile.math`, `profile.clbg`, `profile.time`,
   `profile.concurrent`, `profile.xpu`).
4. Competitor implementations live under `samples/profile/competitors/<lang>/` with a
   per-language build entry (`Cargo.toml`, `CMakeLists.txt`/`Makefile`, Maven/`javac`,
   `requirements.txt`/script, `go.mod`) and a thin per-benchmark runner emitting the
   **same report schema** (§3) as the Cajeta harness. The existing `bench/{java,rust}`
   runners are the seed for JSON.
5. A top-level driver (`run.sh` + a `profile.json`/CLI) can run a single benchmark,
   one area, one language, or the full matrix, and writes reports to
   `samples/profile/results/<timestamp>/`.
6. GPU/XPU benchmarks build and run through the runtime backend dispatcher (CUDA → HIP
   → Vulkan → CPU fallback), as in `samples/tour/xpu/`, with a `--xpu-backend` selector.

### 2.2 Use cases
1. **As a developer, when I run `./run.sh`, then** the suite builds the Cajeta harness
   and every available competitor, runs the full matrix, and writes one structured
   report per (benchmark × language × library) plus a combined comparison table.
2. **As a developer, when I run `./run.sh --area=sort`, then** only the sorting
   benchmarks run, across all languages/libraries that implement them.
3. **As a developer, when I run `./run.sh --bench=json-tokenize --lang=cajeta,rust`,
   then** only those implementations of that one benchmark run.
4. **As a developer, when a competitor toolchain is missing (e.g. no `nvcc`), then**
   the harness records `skipped: <reason>` and continues — it never aborts the matrix.
5. **As a maintainer, when I add a benchmark, then** I drop a `Benchmark` subclass into
   the right subpackage, add the competitor runners, append one line to the
   `benchmarks[]` initializer, and the driver picks it up automatically (tour ergonomics).
6. **As a contributor, when I read `samples/profile/README.md`, then** I find build/run
   instructions, dataset-fetch + checksum commands, reference-machine specs, and a link
   to the latest results — paralleling `bench/README.md`.

---

## 3. Reporting schema (the reproducibility record)

### 3.1 Requirements
1. Every result emits a record containing **all** of the following so a third party can
   reproduce it:
   - **Benchmark identity**: name, capability area, schema/benchmark revision, dataset
     name + version/URL + size + **SHA-256 checksum** (where a dataset exists), input
     size parameter (e.g. CLBG N, element count).
   - **Language identity**: language name + version (`rustc 1.x`, `clang/gcc x`,
     `openjdk 21`, `CPython 3.13`, `go1.23`, `cajeta 0.7.x`).
   - **Library identity**: name + version for each third-party lib (e.g. `serde_json
     1.0.x`, `simdjson 3.x`, `yyjson x`, `glaze x`, `jackson-core 2.18.2`, `orjson x`,
     `ankerl::unordered_dense x`, `absl x`).
   - **Build/run flags**: every compiler/runtime flag verbatim (`--release`, `-O2`/`-O3`,
     `-march=native`, `RUSTFLAGS=-C target-cpu=native`, `GOAMD64=v3`, JVM flags, the
     `--xpu-backend`).
   - **Hardware**: CPU model + microarch + core/thread count + base/boost clock, RAM,
     OS + kernel; for GPU benchmarks also GPU model + driver/runtime (ROCm/CUDA/Vulkan).
   - **Timing**: warmup count, measured-trial count, per-trial times, and the reported
     statistics — **min / median / mean / p95** — plus derived throughput (MB/s, ops/s,
     items/s, GB/s) where meaningful.
   - **Memory** (§3.2): peak RSS, allocated bytes + allocation count, working-set delta.
   - **Correctness cross-check** (§3.3): the structural invariant the run asserted.
2. **Raw results are emitted as CSV** — the canonical, machine-readable source of
   truth. Every runner (Cajeta harness + each competitor) appends rows to a CSV whose
   columns are exactly the fields in §3.1.1 (one row per trial or per
   benchmark×language×library, plus a separate environment CSV for the §3.1 hardware
   block, joined by a run id). CSV is the lingua franca every language can write with no
   dependencies and Python/spreadsheets can read directly.
3. **Reports are generated from the CSV by a Python layer** (§3.5) in two forms: a
   Markdown summary (`report.md`, the combined comparison paralleling
   `docs/specification/codec/json/Json.md`) **and a self-contained static HTML minisite**
   (an `index` with per-area pages, sortable/filterable comparison tables, and embedded
   timing/memory/scaling charts — browseable offline, no server). Python is the **interim**
   report generator; once Cajeta ships the dataframe/plotting capability, a Cajeta-native
   reporter replaces it and emits the same Markdown + minisite from the same CSV.
4. The schema is **versioned** (a `schema-version` column) so historical CSVs stay
   interpretable, and the column set is stable so old and new runs concatenate.

### 3.2 Memory measurement requirements
> Methodology is research-pinned (§18): peak via cgroups/`time -v`, workload isolated by
> overhead subtraction, allocation counted per-runtime, managed heaps pinned not defaulted.
1. **Peak memory (cross-language common denominator)** — preferred:
   whole-process-tree peak via Linux **cgroups** (`memory.peak`, the BenchExec method
   the Benchmarks Game uses), which correctly captures JVM/CPython runtime threads and
   child processes. Portable fallback when cgroups is unavailable: **`/usr/bin/time -v`
   Maximum resident set size**, applied **uniformly** to native and managed binaries
   (the Energy-Languages method). Every report discloses which was used and that managed
   runtimes include interpreter/JIT baseline RSS.
2. **Workload isolation (overhead subtraction)** — to make JVM/CPython comparable to
   native, measure an **empty-runtime baseline** (the harness with no
   library/operation) several times and report **(peak with workload) − (empty-runtime
   peak)**, forcing GC to reveal the true minimum (the Java-Matrix-Benchmark method).
   This supersedes a naive before/after RSS delta.
3. **Pinned managed-runtime configuration** — do **not** use default heaps. Pin and
   record a documented heap/GC config per managed runtime (e.g. smallest non-crashing
   JVM heap grown until performance plateaus; Go `GOGC` pinned), per LangBench (USENIX
   ATC 2022), and state native **C++ `-O3`** as the reference baseline.
4. **Allocated bytes + allocation count** — per-runtime counters scoped to the measured
   region: Cajeta allocator/drop-chain counters; Rust **`allocation-counter`**
   (`bytes_total`/`count_total`/`bytes_max`) + **`dhat`** for high-water assertions; C++
   **jemalloc** `prof.dump` (or tcmalloc stats) around the region; Java
   **`com.sun.management.ThreadMXBean.getThreadAllocatedBytes`** (cumulative, Java 21+);
   Go **`runtime.MemStats`** (`TotalAlloc`/`Mallocs`/`HeapAlloc`); Python **`tracemalloc`**.
   Unavailable ⇒ `n/a`. Record whether a counter is cumulative-allocated vs. live-heap.
5. **GPU memory** (XPU benchmarks) — device allocation high-water via the backend API
   (`hipMemGetInfo`/`cuMemGetInfo`/Vulkan budget), recorded alongside host peak.

### 3.3 Correctness cross-checks
Every benchmark asserts a structural invariant so a wrong-but-fast implementation is
caught: JSON token/field counts and roundtrip identity; sorted-order assertions; final
collection sizes; known hash digests; CLBG reference checksums/format outputs (§12);
GPU result vectors vs a CPU reference within epsilon. A failed cross-check marks the
result `invalid`, not a winning time.

### 3.4 Use cases
1. **As a reader, when I open a result CSV row, then** I can recreate the exact build
   (versions + flags + dataset checksum) and re-run to within measurement noise.
2. **As a reader, when I open the generated report, then** I see one row per (benchmark,
   language, library) with timing **and** memory columns and the Cajeta-relative ratio.
3. **As a maintainer, when a dataset checksum changes, then** the CSV row + report
   surface it so stale-dataset comparisons can't masquerade as regressions.

### 3.5 Reporting pipeline (CSV → Python → report; Cajeta-native later)
1. **Emit (CSV)**: each runner writes/append rows to `results/<timestamp>/results.csv`
   (one shared schema) and a one-row-per-run `results/<timestamp>/env.csv`. Runners need
   no plotting/serialization libraries — just CSV writing, which every language has in
   its stdlib.
2. **Report (Python, interim)**: a script under `samples/profile/report/` (stdlib `csv`
   + optional `pandas`/`matplotlib`) reads the CSV(s) and produces (a) `report.md`
   (grouped comparison tables: timing, memory, Cajeta-relative ratio) and (b) a
   **stylish, Cajeta-themed static HTML minisite** under `results/<timestamp>/site/` —
   `index.html` (hero overview + reference-machine env block + area navigation), one page
   per capability area with sortable/filterable tables (sort by language, library, time,
   memory, Cajeta-ratio) and embedded charts (throughput bars, sort pattern×prediction
   heatmaps, parallel-scaling curves, GPU roofline plots).
   - **Branding**: reuse the existing Cajeta brand from `cajeta-docs-site/` — the 🚀
     wordmark + favicon, and the palette **primary `#0161EF`**, **secondary `#0154CF`**,
     **accent `#6D28D9`/`#8D46E7`**, dark page **`#030621`** — with a light/dark theme
     toggle matching the docs site. Cajeta's own rows/series are visually highlighted (it
     is the subject of the comparison).
   - **Self-contained**: inlined/relative CSS + a small vanilla-JS table sorter + chart
     images (matplotlib PNG/SVG, or pure-CSS bars when matplotlib is absent), no external
     CDN — opens offline via `file://` and is also drop-in deployable as static files.
   The Python layer **only reads CSV** — it never runs benchmarks — so it is trivially
   swappable; the Cajeta-native reporter later emits the same themed site.
3. **Dogfood (future)**: when Cajeta gains the dataframe/plotting capability, a
   Cajeta-native reporter replaces the Python script and consumes the identical CSV. The
   swap is a reporting-layer change only; no runner or schema change. *(This is itself a
   future `profile` milestone — Cajeta reporting on Cajeta's own benchmark data.)*

---

## 4. Methodology & fairness

### 4.1 Requirements
1. **Warmup + steady-state**: idiomatic per language — native/Cajeta: a few warmup
   iters; Java: ≥100 JIT-warmup iters; Python: warmup + best-of-N; Go: warmup then
   measured (matching `bench/`: 20–100 warmup, 200+ measured).
2. **Trial count**: ≥200 measured trials for fast ops; fewer (documented) for
   long-running ones. Report the full stat set (§3.1 timing).
3. **Baseline subtraction**: for ops that include unavoidable allocation/copy/IO (e.g.
   fresh-buffer reads), measure that baseline separately and subtract, as
   `BindBench.cajeta` does today.
4. **Branch-predictor state** (sort, and where relevant): run both **predictable**
   (hot) and **unpredictable** (cold) comparison patterns, per sort-research-rs (§18).
5. **Access pattern** (JSON, and where relevant): run both **in-order** and
   **out-of-order/missing-key** field access — the research showed lazy parsers
   (simdjson On-Demand) collapse ~13× on reversed keys while hash-based parsers hold.
6. **Pinned environment**: governor = performance, turbo state recorded, machine
   otherwise idle — all part of the emitted record.
7. **No cherry-picking**: reported statistic and trial count are fixed by the schema,
   not chosen per result. Losses are reported.

### 4.2 Competitor library matrix (research-pinned)
Several competitors per language per area, so each table shows the full spread. `—` =
no idiomatic equivalent (recorded `n/a`, not a zero). SOTA-vs-legacy and
hypotheses-to-confirm are flagged.

| Area | Rust | C++ | Java | Python | Go |
|---|---|---|---|---|---|
| **JSON** | serde_json, simd-json, **sonic-rs** | **simdjson, yyjson, glaze**, RapidJSON, _reflect-cpp_ | Jackson, Gson, **DSL-JSON, fastjson2** | json, orjson, **msgspec**, ujson | encoding/json, **bytedance/sonic, goccy/go-json**, jsoniter |
| **Hash map** | std/hashbrown, +fxhash/ahash | std::unordered_map, **absl::flat_hash_map, boost::unordered_flat_map, ankerl::unordered_dense** | HashMap | dict | map (+ Go 1.24 swiss-table) |
| **Sort** | sort_unstable (driftsort), sort, ipnsort | std::sort/stable_sort, pdqsort, **ips4o, vqsort**, x86-simd-sort, boost::sort | Arrays.sort (dual-pivot/TimSort) | sorted/list.sort (Timsort) | slices.Sort (pdqsort), sort.Slice |
| **String** | std str, **memchr/memmem, bstr, StringZilla, simdutf8** | std::string, **StringZilla, simdutf** | java.lang.String | str | strings, **StringZilla-go** |
| **Hash fn** | xxhash-rust, ahash, **rapidhash**, sha2 | xxHash/XXH3, **rapidhash/wyhash**, OpenSSL | MessageDigest, +xxhash port | hashlib, xxhash | cespare/xxhash, crypto/* |
| **Stream/par** | iterators, **rayon**, std threads | ranges, std::execution::par, **TBB, Taskflow, OpenMP** | Stream, parallelStream | comprehension, (no par) | hand loop, goroutine fan-in |
| **SIMD/numeric** | std::simd (nightly), nalgebra, ndarray | **Google Highway**, std::experimental::simd, Eigen, OpenBLAS | scalar (`n/a` SIMD) | numpy | scalar (`n/a` SIMD) |
| **CLBG** | ✓ ref entries | ✓ ref entries | ✓ ref entries | ✓ ref entries | ✓ ref entries |
| **Time** | chrono, time, **jiff** | std::chrono, Hinnant date | java.time | datetime, **arrow, pendulum, whenever** | time |
| **Concurrency** | tokio, rayon, crossbeam-channel, **flume, kanal, async-channel** | std::thread/async, coroutines | virtual threads (Loom), BlockingQueue, SynchronousQueue, **LMAX Disruptor** | asyncio | goroutines, channels |
| **GPU** | cust, wgpu | CUDA, HIP, **BabelStream, mixbench, ParallelReductionsBenchmark** refs, CUB/rocPRIM, cuBLAS/rocBLAS, _chipStar/SCALE_ | — | cupy (opt) | — |

**Bold** = research-surfaced SOTA or off-radar challenger worth pitting head-to-head
(e.g. memchr ≈ StringZilla ≈ std on substring search — no automatic SIMD-library win, so
measure). _Italic_ = sourced but not 3-vote-verified; include and let results decide.

### 4.3 Use cases
1. **As a reviewer, when I compare Cajeta vs Java on JSON, then** the Java number is
   post-JIT-warmup steady state — so the comparison is meaningful.
2. **As a reviewer, when a benchmark has no idiomatic Go/Python equivalent (explicit
   SIMD), then** that cell reads `n/a` with a recorded reason, not a misleading scalar
   fallback presented as the language's SIMD result.
3. **As a reviewer, when I read a JSON result, then** I can see both in-order and
   out-of-order access numbers — so a lazy-parser collapse can't hide behind a best case.

---

## 5. Capability area — JSON & codec (`profile.codec`)  — 11 benchmarks
> Cajeta APIs: `Json.parse<T>`, `Json.toBytes<T>`, SIMD stage-1 tokenizer, `Base64`.
> Corpus: nativejson (`twitter.json` ~617 KB CJK/UTF-8, `citm_catalog.json` ~1.7 MB,
> `canada.json` ~2.2 MB real-number-heavy). Methodology mirrors nativejson-benchmark (§18).

### 5.1 Use cases (benchmarks)
1. **Tokenize (streaming, no DOM)** — pull every token to EOF, numbers lazy. Cross-check:
   token count per file. *Extends existing `bench/`.*
2. **DOM parse (full materialize)** — build the value tree. Cross-check: node/field tally.
3. **Typed bind** — `Json.parse<T>` into a typed record. Cross-check: field values.
4. **Serialize / stringify (condensed)** — typed record → compact bytes. Cross-check:
   byte length + re-parse identity. *(yyjson/glaze lead here; simdjson is parse-only.)*
5. **Prettify (formatted)** — serialize with indentation.
6. **Number-heavy parse** — `canada.json` with float conversion. Cross-check: summed
   magnitude within epsilon.
7. **Access-pattern study (in-order vs out-of-order/missing-key)** — read fields in
   document order vs reversed vs with absent keys. *The differentiator test: exposes
   lazy-parser collapse (simdjson 1200→89 MB/s) vs hash/eager parsers.*
8. **Conformance** — JSON_checker / JSONTestSuite accept/reject corpus. Cross-check:
   pass/fail vector matches the reference verdicts (correctness, not speed).
9. **Roundtrip integrity** — parse→stringify→parse over the corpus; assert structural
   equality (nativejson's roundtrip test).
10. **Base64 encode throughput** — large buffer. vs Rust `base64`, C++ **simdutf** (WHATWG
    base64, ~10 GB/s) + stdlib, Java `Base64`, Python `base64`, Go `encoding/base64`.
11. **Base64 decode throughput** — inverse; same competitors; cross-check: round-trip identity.

---

## 6. Capability area — collections (`profile.collection`)  — 11 benchmarks
> Cajeta APIs: `ArrayList`, `HashMap`, `HashSet`, `LinkedList`, `Heap`, `RedBlackTree`,
> `BPlusTree`, `Cache`. Methodology mirrors Martin Ankerl's hashmap suite + jacksonallan
> c_cpp_hash_tables_benchmark (§18): insert, lookup-hit, lookup-miss, iterate, erase,
> with peak-mem and reserved-vs-unreserved variants.

### 6.1 Use cases (benchmarks)
1. **ArrayList append** — N grow-on-demand pushes (reserved + unreserved).
2. **ArrayList iterate/sum** — linear scan over N (cache behavior).
3. **HashMap, integer keys** — insert N, M random lookups (hit + miss). vs hashbrown,
   absl/boost/ankerl, java HashMap, dict, Go map.
4. **HashMap, string keys** — same with String keys (hashing cost dominates).
5. **HashMap mixed churn** — interleaved insert/lookup/erase. Cross-check: final size.
6. **HashSet dedup** — insert N with duplicates; report distinct count.
7. **LinkedList insert/traverse** — N inserts + full traversal.
8. **Heap / priority queue (heapsort)** — push N, pop N in order. Cross-check: sorted.
9. **RedBlackTree ordered insert + range scan** — N ordered inserts, range query.
10. **BPlusTree ordered insert + point lookup** — N inserts, M lookups.
11. **LRU Cache hit/miss** — `Cache<K,V>` under a Zipfian pattern; report hit rate.

---

## 7. Capability area — sorting & search (`profile.sort`)  — 6 benchmarks
> Cajeta APIs: `Sort.sort` (unstable), `Sort.sortStable` (mergesort), `binarySearch`/
> `lowerBound`/`upperBound`, comparator overrides. Methodology mirrors **sort-research-rs**
> (§18): parameterize each run by `<type>-<pattern>-<prediction>-<size>` — data type,
> input pattern (random, ascending, descending, sawtooth, many-duplicates), branch-
> predictor state (hot=predictable / cold=unpredictable comparisons), and N.

### 7.1 Use cases (benchmarks)
1. **Sort u64/i64** over the pattern×prediction matrix. vs sort_unstable(driftsort),
   std::sort, pdqsort, ips4o, vqsort, Arrays.sort, sorted, slices.Sort. Cross-check: ordered.
2. **Sort f64** (NaN-free) over the same matrix.
3. **Sort String** (lexicographic).
4. **Stable sort by key** — sort records by key, assert stability. vs sort/stable_sort/
   Arrays.sort(Object[])/sorted/SliceStable.
5. **Low-cardinality / many-duplicates** — quicksort worst-case guard. Cross-check: ordered.
6. **binarySearch / lowerBound** — M searches over a sorted N-array.

---

## 8. Capability area — strings (`profile.string`)  — 6 benchmarks
> Cajeta APIs: `+` concat, `indexOf`, `contains`, `substring`, `replace`, `split`,
> `trim`, case folding, codepoint `count()`, SIMD scanners. (No regex — excluded, §1.5.)
> Standard corpus: StringZilla's **1 GB English text** (avg word length 6); search
> queries ~5-byte words. Off-radar SIMD competitors (research-surfaced): **StringZilla**
> (C/C++/Rust/Go/Python bindings), Rust **memchr `memmem::Finder`** and **bstr**,
> **simdutf**/**simdutf8** for UTF-8. Key finding: on substring search memchr ≈ std ≈
> StringZilla within ~2% — *no automatic SIMD-library win*, so measure head-to-head.

### 8.1 Use cases (benchmarks)
1. **String build** — N appends into a growing string/builder.
2. **Substring search** — count needle occurrences in the 1 GB corpus (indexOf loop).
   vs memchr::memmem, std, StringZilla. Cross-check: match positions vs libc/std.
3. **Split + join** — split a large delimited text, rejoin.
4. **Global replace** — replace-all over a large text. Cross-check: output checksum.
5. **UTF-8 codepoint iteration** — count codepoints over a multibyte corpus. Cross-check:
   codepoint total.
6. **UTF-8 validation + transcoding (SIMD)** — validate UTF-8 and transcode UTF-8↔UTF-16
   over Lemire's mixed ASCII/Chinese/Emoji/Arabic corpora; targets ~10 GB/s. vs
   **simdutf** (C++), **simdutf8** (Rust drop-in for `from_utf8`), `std::str`, Java/Go/
   Python stdlib. Cross-check: round-trip equality + reject-invalid.

---

## 9. Capability area — hashing & crypto (`profile.hash`)  — 6 benchmarks
> Cajeta APIs: `XXHash3`, `SipHash`, `MD5`, `SHA1`, `SHA256`, `@AutoHash`. Report
> **SMHasher's** canonical metrics (§18): bulk throughput (MiB/s @ 256 KiB keys),
> small-key speed (cycles/hash @ 1–31 B), hashmap-use cycles/op. Non-crypto SOTA
> reference class: rapidhash/xxh3/wyhash.

### 9.1 Use cases (benchmarks)
1. **XXHash3 bulk throughput** — large buffer. vs xxhash-rust/ahash, xxHash(C++),
   xxhash(Python), cespare/xxhash(Go); rapidhash/wyhash as the faster-class reference.
2. **SipHash bulk throughput** — DoS-resistant hash.
3. **SHA-256 bulk throughput** — vs sha2(Rust), OpenSSL(C++), MessageDigest(Java),
   hashlib(Python), crypto/sha256(Go). Cross-check: known digest.
4. **MD5 bulk throughput** — same matrix; cross-check: known digest.
5. **Small-key latency** — cycles/hash over 1–31 B keys (SMHasher small-key metric).
6. **@AutoHash struct hashing** — hash N synthesized records via the generated hash.

---

## 10. Capability area — streams & functional pipelines (`profile.stream`)  — 5 benchmarks
> Cajeta APIs: `filter`/`map`/`reduce`/`fold`/`collect`, `.parallel()` fork-join over
> Splittable sources. No single recognized cross-language suite (best-judgment, §17);
> parallel-reduce competitors include **rayon** + std threads (Rust), **std::execution::par,
> TBB, Taskflow, OpenMP** (C++), `parallelStream` (Java); methodology cross-references
> ParallelReductionsBenchmark (§18) for the reduce kernel + speedup-curve shape.

### 10.1 Use cases (benchmarks)
1. **filter → map → reduce (sequential)** — over N elements. vs Rust iterators, C++
   ranges, Java Stream, Python generator, Go loop.
2. **Parallel reduce** — `.parallel()` over N. vs rayon, C++ `std::reduce(par)`, Java
   `parallelStream`, (Python `n/a`), Go goroutine fan-in.
3. **collect to list** — pipeline terminal materializing a collection.
4. **flatMap pipeline** — nested expansion + reduce.
5. **Parallel scaling** — same parallel reduce at 1/2/4/8/16 workers; report speedup curve.

---

## 11. Capability area — math & SIMD (`profile.math`)  — 6 benchmarks
> Cajeta APIs: `Vector<T,N>` lane ops, `Matrix<T,R,C>`, intrinsics. Compares **scalar**
> vs **explicit-SIMD** Cajeta paths. Portable-SIMD references: **Google Highway** (C++,
> 27 targets), **Rust std::simd** (nightly portable-simd); BLAS references: Eigen,
> OpenBLAS, nalgebra/ndarray (§18).

### 11.1 Use cases (benchmarks)
1. **SAXPY (CPU)** — `y = a*x + y` over N floats; Cajeta scalar vs `Vector<f32,N>`. vs
   Highway, std::simd, Eigen, numpy; Java/Go scalar labeled `n/a`-SIMD.
2. **Dot product** — reduction over N; scalar vs SIMD.
3. **Matrix multiply** — NxN register-resident. vs Eigen, OpenBLAS, nalgebra/ndarray,
   numpy; Java/Go scalar.
4. **Vector lane-ops microbench** — add/mul/min/max/tableLookup throughput on SIMD lanes.
5. **mandelbrot (SIMD study)** — tight FP; Cajeta scalar vs `Vector<f64>` (canonical CLBG
   port lives in §12; here it's the SIMD-vs-scalar study).
6. **spectral-norm** — FP-heavy, vectorizable (CLBG; shared canonical port with §12).

---

## 12. Capability area — Computer Language Benchmarks Game classics (`profile.clbg`)  — 8 benchmarks
> Canonical, widely-cited workloads with reference implementations and **exact input
> sizes + checksum/format verification** (§18). Faithful ports per language; report
> wall-clock secs, peak mem, gz source size, and cpu secs (the Game's own columns).
> Verify each at the small format-check N against the reference output, then measure at
> the perf N.

### 12.1 Use cases (benchmarks)
1. **binary-trees** — stretch tree + long-lived tree + many bottom-up trees; verify the
   long-lived tree survives. Format-check N=10 vs the 1 KB reference; perf at N=21.
   Cross-check: checksum.
2. **n-body** — gravitational sim. Cross-check: energy after N steps within epsilon.
3. **mandelbrot** — escape-time fractal → PBM bitmap. Cross-check: PBM checksum.
4. **fannkuch-redux** — all n! permutations, reverse-first-k, count flips. Format-check
   N=7; perf at N=12. Cross-check: max-flips + even/odd-indexed checksum.
5. **fasta** — pseudo-random DNA generation (defined LCG). Cross-check: output checksum.
6. **k-nucleotide** — hashmap + string frequency counting. Cross-check: count tables.
7. **spectral-norm** — eigenvalue estimate (one canonical port, shared with §11.6).
8. **reverse-complement** — large-buffer byte transform. Cross-check: output checksum.
> Excluded: **regex-redux** (no regex, §1.5), **pidigits** (no bignum path).

---

## 13. Capability area — time & date (`profile.time`)  — 3 benchmarks
> Cajeta APIs: `Instant`, `Duration`, `LocalDate(Time)`, `ZonedDateTime`,
> `DateTimeFormatter`, system tz database. No recognized cross-language harness exists
> (ad-hoc, §17); include the rising libs the research surfaced — **jiff** (Rust),
> **whenever / arrow / pendulum** (Python) — alongside chrono/std::chrono/java.time/Go time.

### 13.1 Use cases (benchmarks)
1. **Instant/Duration arithmetic** — N add/subtract/compare. vs chrono/jiff, std::chrono,
   java.time, datetime/whenever/arrow/pendulum, Go time.
2. **Timezone resolution** — N `ZonedDateTime` constructions across zones (tz lookup).
3. **Format + parse round-trip** — `ofPattern` format then parse N timestamps (RFC 3339 +
   custom patterns). Cross-check: round-trip identity.

---

## 14. Capability area — concurrency & async (`profile.concurrent`)  — 5 benchmarks
> Cajeta APIs: `spawn`/`Task<T>` stackful fibers, `Lock.withLock`, channels, atomics,
> cooperative scheduler. Channel throughput mirrors the **crossbeam-channel** harness
> (§18); 1M-task footprint mirrors the pkolaczk / hez2010 async-runtimes comparisons
> (sourced, not 3-vote-verified).

### 14.1 Use cases (benchmarks)
1. **Task spawn/await latency** — spawn + await N trivial tasks (fiber switch cost). vs
   tokio tasks, C++ std::async/coroutines, Java virtual threads (Loom), asyncio, goroutines.
2. **1M-task memory footprint** — spawn 1,000,000 tasks that each sleep, measure peak
   memory (§3.2). *The canonical "how much memory do 1M tasks cost" comparison* —
   stackful fibers vs goroutines vs virtual threads vs tokio vs asyncio.
3. **Channel throughput** — the crossbeam-channel workloads **seq / spsc / mpsc / mpmc**
   at N=5,000,000 messages, T=4 threads. vs crossbeam-channel, **flume, kanal,
   async-channel**, Go channels, Java `LinkedBlockingQueue`/`SynchronousQueue`/**LMAX
   Disruptor**, asyncio queue. Cross-check: all messages received.
4. **Lock contention** — N workers contending `Lock.withLock` on a counter. Cross-check:
   final tally exact.
5. **Parallel-stream scaling** — cross-reference §10.5 under the concurrency lens.

---

## 15. Capability area — GPU / XPU (`profile.xpu`)  — 10 benchmarks
> Cajeta APIs: `@Kernel` + runtime backend dispatch (CUDA → HIP → Vulkan → CPU
> fallback), cooperative matrix, wave reduce. Built/run like `samples/tour/xpu/` with
> `--xpu-backend`. Methodology mirrors **BabelStream** (sustained device bandwidth,
> excludes PCIe) + **mixbench** (roofline operational-intensity sweep) (§18). Reduction/
> scan vs CUB/rocPRIM/Thrust; GEMM vs cuBLAS/rocBLAS. GPU memory via §3.2.4. Competitors:
> CUDA C++ (`nvcc`), HIP C++ (`hipcc`), Rust (`cust`/`wgpu`); Python `cupy` optional.
> Absent backend/toolchain on gfx1151 ⇒ `skipped: <reason>`. Note: the external way to
> run one source across backends is portability layers (**chipStar**, **SCALE** =
> HIP/CUDA-on-other-vendor); Cajeta's native CUDA/HIP/Vulkan/CPU emit from one `@Kernel`
> is the comparison this area showcases (§15.1.10).

### 15.1 Use cases (benchmarks)
1. **BabelStream Triad** — `a = b + scalar*c`; sustained GB/s, the canonical bandwidth number.
2. **BabelStream Copy/Mul/Add/Dot** — the remaining STREAM kernels + device dot.
3. **SAXPY (device)** — `y = a*x + y` on-device. Cross-check: result vs host ref.
4. **Reduction / sum** — wave-reduce sum of N. vs CUB/Thrust, rocPRIM; methodology
   cross-references **ParallelReductionsBenchmark** (§18, a portable CPU+GPU reduction
   harness). Cross-check: sum.
5. **Prefix sum / scan** — inclusive scan of N. Cross-check: last element + spot checks.
6. **Matrix multiply (cooperative matrix)** — tiled GEMM on-device. vs cuBLAS/rocBLAS
   single-call reference. Cross-check: vs CPU GEMM within epsilon.
7. **mandelbrot (device, compute-bound)** — per-pixel escape time. vs CUDA.
8. **mixbench roofline sweep** — vary operational intensity; locate the compute-vs-memory
   bound transition.
9. **Kernel launch overhead** — latency of N trivial dispatches (host↔device round-trip).
10. **Cross-backend portability showcase** — the *same* Cajeta `@Kernel` source run on
    CUDA, HIP, Vulkan, and CPU; report per-backend timing + a consistency check that all
    backends produce identical results. *The XPU differentiator: one source, every backend
    — and mixbench/BabelStream themselves have no Vulkan backend, so this is coverage they
    can't offer.*

---

## 16. Invocation, selection & output

### 16.1 Requirements
1. The driver supports selection by **benchmark**, **area**, and **language/library**,
   plus a **full-matrix** default, and a **`--list`** enumerating available benchmarks +
   detected toolchains.
2. It records the **environment block** (§3.1 hardware) once per run and links each
   result to it.
3. Output → `samples/profile/results/<timestamp>/` as `results.csv` + `env.csv` (the
   raw source of truth), then `report.md` and the **Cajeta-themed HTML minisite**
   (`site/`) generated from them by the Python layer (§3.5); a `--out` flag overrides the
   location, and `--no-report` emits CSV only (run benchmarks now, report later / elsewhere).
4. Missing toolchains/datasets produce **recorded skips** (a CSV row with
   `status=skipped` + reason), never silent omissions or hard aborts.

### 16.2 Use cases
1. **As a developer, when I run `./run.sh --list`, then** I see every benchmark, its
   area, the languages/libraries that implement it, and which toolchains are present.
2. **As a developer, when I run the full matrix, then** I get `results.csv` + `env.csv`,
   a Python-generated `report.md`, and a browseable Cajeta-themed `site/` (open
   `site/index.html`) with grouped, sortable comparison tables + charts (timing + memory +
   Cajeta-relative ratio).
3. **As a data-minded developer, when I want a custom view, then** I open `results.csv`
   in pandas/a spreadsheet directly — no bespoke format to parse.
4. **As CI (future), when results regress beyond a threshold, then** a follow-up hook
   (out of v1 scope) can diff CSV rows — the stable column schema is designed to allow it.

---

## 17. Decisions (resolved) & residual notes
All five review questions are **resolved** per developer direction; recorded here.
1. **Benchmark count** — **~77** benchmarks (§5: 11, §6: 11, §7: 6, §8: 6, §9: 6,
   §10: 5, §11: 6, §12: 8, §13: 3, §14: 5, §15: 10), many parameterized (sort
   pattern×prediction, JSON access-pattern, 3-file corpus, 1..N-core scaling) so the
   measured data-point count is several hundred. **Decision: comprehensive — keep all;
   size inputs so the full matrix is bounded in wall-clock, not "forever."**
2. **Output/reporting** — **raw CSV** from every runner; a **Python** layer generates
   `report.md` + charts; a **Cajeta-native reporter** replaces Python once the language
   has the dataframe/plotting capability (§3.5). Resolved.
3. **Memory methodology** — **resolved & sourced** (§3.2): cgroups/`time -v` peak with
   overhead subtraction, pinned managed heaps (C++ `-O3` baseline), per-runtime
   allocation counters. No longer an open question.
4. **Competitor breadth** — **full breadth + off-radar challengers** (§4.2). The second
   research pass added the non-obvious ones the developer asked for: memchr/StringZilla/
   simdutf (string), flume/kanal/Disruptor (channels), jiff/whenever (time),
   sonic-rs/fastjson2/goccy/msgspec (JSON), Taskflow/OpenMP (parallel). All vendored.
5. **Heavy vendoring** — **vendor everything in v1** (vqsort, ips4o, Highway, BabelStream,
   mixbench, StringZilla, simdutf, sort-research-rs, etc.) under `competitors/`.
6. **Plan granularity (my call)** — **~17 units**, one runnable slice each:
   (1) harness + `Benchmark` base, (2) CSV reporting schema + memory capture,
   (3) methodology + dataset fetch/checksum, (4) competitor build scaffolding +
   vendoring, then (5)…(15) one capability area each — **JSON first** (reuses `bench/`),
   **GPU last** (most env-dependent) — then (16) Python report layer, (17) top-level
   driver + README.

**Residual (genuinely ad-hoc — no recognized cross-language standard exists, flagged in
place):** stream/functional parallelism (§10), date/time (§13), and the 1M-task
*numbers* (§14.1.2) have sourced reference harnesses but no canonical suite; these areas
use our best-judgment workloads and say so in their reports.

---

## 18. Benchmark suites & sources we mirror
Pinned from two verified deep-research passes (25/25 then 24/25 claims confirmed 3-0).
These define the workloads, input sizes, correctness checks, competitor sets, and
measurement methodology above. `[v]` = 3-vote-verified; `[s]` = sourced this pass but not
3-vote-verified (treat winner-rankings skeptically, re-run locally).

- **JSON** — `[v]` nativejson-benchmark corpus + methodology (twitter/citm/canada; parse,
  DOM, stringify/prettify, JSON_checker conformance, roundtrip, peak-mem/alloc-count):
  `github.com/miloyip/nativejson-benchmark`. C++ SOTA contest + access-pattern collapse:
  `github.com/ibireme/yyjson`, `github.com/stephenberry/json_performance`. Per-language
  SOTA `[s]`: `github.com/fabienrenaud/java-json-benchmark` (fastjson2/DSL-JSON/Jackson),
  `github.com/cloudwego/sonic-rs`, `gist…/jcrist` (msgspec vs orjson).
- **Hash maps** — `[v]` `martin.ankerl.com/2022/08/27/hashmap-bench-01/`,
  `jacksonallan.github.io/c_cpp_hash_tables_benchmark/`, `github.com/martinus/unordered_dense`.
- **Sort** — `[v]` `github.com/Voultapher/sort-research-rs`; vqsort/ips4o SP&E 2022:
  `arxiv.org/pdf/2205.05982`.
- **String** — `[v]` StringZilla (1 GB corpus, 8-language SIMD bindings):
  `github.com/ashvardanian/stringzilla`; memchr-vs-StringZilla head-to-head (StringWars):
  `github.com/ashvardanian/memchr_vs_stringzilla`; UTF-8 validation/transcoding+base64:
  `simdutf.github.io/simdutf/`, `github.com/rusticstuff/simdutf8`.
- **Hashing** — `[v]` SMHasher (4 canonical metrics, fastest non-crypto class):
  `github.com/rurban/smhasher`.
- **Stream/parallel** — `[s]` portable reduction + speedup curves:
  `github.com/ashvardanian/ParallelReductionsBenchmark`.
- **Portable SIMD** — `[v]` Google Highway: `google.github.io/highway/`; Rust
  portable-simd: `github.com/rust-lang/portable-simd`.
- **CLBG** — `[v]` `benchmarksgame-team.pages.debian.net/benchmarksgame/` (per-problem
  description + performance pages give exact N and checksum/format verification).
- **Date/time** — `[s]` rising libs: `docs.rs/jiff` (comparison page),
  `whenever.readthedocs.io` (Python). No canonical cross-language harness.
- **Concurrency** — `[v]` channel throughput harness (seq/spsc/mpsc/mpmc, N=5M, T=4):
  `github.com/crossbeam-rs/crossbeam-channel/tree/master/benchmarks`; Kanal extension
  (flume/async-channel/Go): `github.com/fereidani/rust-channel-benchmarks`. 1M-task
  footprint `[s]`: `pkolaczk.github.io/memory-consumption-of-async/`,
  `hez2010.github.io/async-runtimes-benchmarks-2024/`.
- **GPU** — `[v]` BabelStream (device bandwidth, STREAM kernels, excludes PCIe):
  `github.com/UoB-HPC/BabelStream`; mixbench (roofline): `github.com/ekondis/mixbench`.
  Portable reduction `[s]`: ParallelReductionsBenchmark (above); cross-backend
  portability layers `[s]`: chipStar, SCALE (HIP/CUDA-on-other-vendor).
- **Memory & reproducibility** — `[v]` peak via cgroups/BenchExec:
  `benchmarksgame…/how-programs-are-measured.html`; uniform `/usr/bin/time -v`:
  `github.com/greensoftwarelab/Energy-Languages`; overhead subtraction:
  `Java-Matrix-Benchmark` DescriptionMemory; pinned-heap methodology + C++ `-O3` baseline:
  LangBench, USENIX ATC 2022 (`usenix.org/system/files/atc22-lion.pdf`); allocation
  counters: `github.com/fornwall/allocation-counter`, `docs.rs/dhat`,
  `com.sun.management.ThreadMXBean`, jemalloc heap profiling.
```
