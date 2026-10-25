# benchmark-reference — the benchmark reference, shaped like the stdlib reference (spec)

> Status: **active**, written 2026-10-25 at Julian's request and approved by him
> the same day; every §12 question is decided on its recommendation. Follows `td-project-workflow.md`.
> Plan: `agents/benchmark-reference-plan.md`. Supersedes `profile-spec.md`
> (draft since 2026-06, plan 9 of 12 units done) and the standing of `bench/`
> as the site's Benchmarks tab; builds on the archived `benchmark-fidelity`,
> `benchmark-gap-sweep` and `home-and-benchmarks` work.

---

## 1. Definition

### 1.1 Purpose
Cajeta publishes **one benchmark reference**: a catalog shaped exactly like the
stdlib reference (`docs/stdlib/`), one page per package and one page per class
or capability that can be measured, each page carrying the benchmarks for that
class, the competitor it is measured against, the chart of the last published
run, and the sentence that says where Cajeta stands. The same catalog continues
past the stdlib into the ecosystem libraries that compete with named external
systems: `dev.cajeta.ml` against scikit-learn and PyTorch, `dev.cajeta.llm`
against llama.cpp, `cajeta-http` against the mainstream HTTP stacks, and
primavera against Spring Boot and its compile-time-DI peers.

The reference is **structure first, numbers second**. Until the XPU kernel and
LLM work now in flight closes, the box is not quiet enough to measure, so this
spec delivers the catalog, the harness, the competitor programs, the pages and
the site rendering with every chart in its "not yet measured" state. The
measurement is then one announced run on a quiet box, and publishing it is one
commit of CSV files plus a site deploy.

### 1.2 The problem it solves
Measured 2026-10-25 on `main` (v0.46.3):

1. **The published Benchmarks tab is the wrong document.** The site ingests
   `bench/**/*.md`, and `bench/README.md` is the 2026-05 JSON-codec note
   (Cajeta vs Jackson/Gson) plus the element-access micro-bench. The home page's
   "Wondering about speed?" link lands there.
2. **The real suite is invisible.** `samples/profile/` holds 50 Cajeta
   benchmarks over 10 areas, competitor programs in five languages, a CSV
   schema, a fidelity gate and a Python-generated minisite, and none of it is on
   cajeta.dev. Its committed sample report was recorded on **cajeta 0.7.1**; the
   compiler is at 0.46.3.
3. **Three harnesses, three shapes.** `samples/profile` (`Benchmark`/`Harness`/
   `Csv`), `bench/src/bench/*.cajeta` (ad-hoc mains, plus a dozen JSON scratch
   files that were never benchmarks), and per-repo instruments
   (`cajeta-llm/bench/SchedThroughput`, `cajeta-codec/bench/CompressBench`,
   `~/code/ml/xgbbench`) with no shared result format.
4. **Whole areas have no benchmark**: loops, exceptions, allocation and
   ownership, dispatch, `cajeta.io`, `cajeta.math` tensors, `cajeta.nucleo`,
   `cajeta.wire`, `cajeta.search`, toolchain speed (compile, start-up, binary
   size), and every ecosystem library except the LLM engine's own parity
   instruments.
5. **The kernel catalog tables on the site wrap.** `docs/kernels/catalog-llm.md`
   and `catalog-xgboost.md` render five columns inside the prose width and the
   `role` column line-wraps (Julian, 2026-10-25).

### 1.3 Scope
1. **A catalog layout** under `bench/` mirroring `docs/stdlib/` (§2).
2. **One harness** published as a library, `dev.cajeta.bench`, used by the
   compiler repo and every ecosystem repo (§3).
3. **A results model**: committed CSV per machine per run, rendered by the
   Astro site into per-benchmark charts, with a machine selector and an
   explicit unmeasured state (§4, §5).
4. **The language and stdlib catalog**: every stdlib package with a measurable
   surface, plus the language-level pages (§6).
5. **The ecosystem catalog**: ml, llm, http, primavera, with the codec and
   xgboost instruments folded in (§7).
6. **Competitors** per area, pinned, with the methodology each one is run under
   (§8).
7. **The run and publish procedure** for a quiet box (§9).
8. **Site work**: the Benchmarks tab, the home page link, the kernel-catalog
   table fix (§10).

### 1.4 Non-goals
- Optimizing anything the benchmarks reveal. Findings become their own specs,
  as `benchmark-gap-sweep` did.
- Regression tracking in CI. The catalog is a published reference measured on
  announced runs; a CI performance gate is a later spec.
- Cloud or multi-node measurement (`cajeta-cloud`, nucleo distributed frame).
- Languages beyond the five: Kotlin, Swift, C#, Zig and Julia are out of v1.
  ASP.NET Core appears only as a primavera competitor (§8.6).
- vLLM, TensorRT-LLM, MLX, Ollama as LLM competitors. llama.cpp is the one the
  engine is built against and measured with; the others are a later column.

### 1.5 Constraints
1. **Nothing runs until the box is quiet.** This spec's units build and
   smoke-test; they do not take the published measurement. The run is one
   announced event (§9) after the xpu/llm work closes. The smoke tests run each
   benchmark for a handful of iterations to prove it builds, checks and emits a
   row; those numbers are never published.
2. **Every number carries its provenance**: compiler version and flags,
   competitor language, library and version, machine, backend, driver, dataset
   checksum, correctness check. A row that cannot record a version is a failed
   row, not an unlabeled number (profile §1.4.3, kept).
3. **Apples to apples**: same logical work, same input size, the competitor
   uses the API the page names, results are consumed so the optimizer cannot
   elide the work (benchmark-fidelity §1.4, kept). The fidelity gate
   (`verify-fidelity.py`) stays green over the whole catalog.
4. **Two machines, both published.** Phoenix (Ryzen 9 3950X, RTX 4090, WSL2,
   94 GB) and Proton (Ryzen AI MAX+ 395, Radeon 8060S gfx1151, Ubuntu 26.04,
   64 GB). CPU and language areas are measured on both; CUDA and Vulkan on
   Phoenix; HIP and Vulkan on Proton. A page states its machine; the site shows
   one machine at a time.
5. **Honest copy.** Pages state what was measured and stop. A loss is a loss
   with the reason linked; a skip is a row with its reason, never a blank.
6. **Model lanes** (Julian, 2026-10-25): harness, competitor programs, site
   rigging and the benchmark code are Opus 5.5 work; spec, plan, page copy,
   chart and page design are Fable work. The plan tags every unit.

---

## 2. Layout — `bench/` mirrors `docs/stdlib/`

### 2.1 Directory shape
```
bench/
├── README.md                 ← Overview: standings, machines, how to read a page
├── METHODOLOGY.md            ← the rules every number obeys (§8.1)
├── lang/                     ← language-level + cajeta.lang   (§6.1)
├── collection/  concurrent/  time/  hash/  codec/  io/  math/  nucleo/
├── wire/  search/  process/  reflect/  xpu/                   (§6.2–6.13)
├── toolchain/                ← compile, start-up, binary size, notebook kernel (§6.14)
├── ml/  llm/  http/  primavera/                                (§7)
├── results/                  ← committed runs (§4)
│   └── <machine>/<run-id>/{results.csv,env.csv}
├── src/main/cajeta/bench/    ← the harness project for the language + stdlib catalog
│   └── <package>/<Class>Bench.cajeta
├── competitors/{cpp,rust,go,java,python}/<package>/...
├── datasets/                 ← manifest.tsv + fetch.sh (content-addressed)
├── cajeta.json  bench.sh  test/*_smoke.sh
```
`samples/profile/` moves here whole (git history preserved); `samples/profile`
is retired and `report/report.py` deleted, the Astro site taking its role
(§5). The current `bench/src/bench/*.cajeta` scratch files are triaged:
`JsonBench` and `ElementAccessBench` become catalog benchmarks; the JSON
prototype files (`Emit*`, `Stage1`, `Fw/Jw/Sax*`, `Idx*`, `Skip*`, `TokCount`,
`BindProto`, `JsonApiTest`) are deleted. `bench/kernel-startup/` becomes
`bench/toolchain/NotebookKernel.md` with its script under `toolchain/`.

### 2.2 Page shape
Every class or capability page has the same sections, in this order, so a
reader who knows one knows all:

1. **Title and lede** — `cajeta.collection.HashMap`, one paragraph on what is
   measured and why that workload.
2. **Benchmarks** — a table, one row per benchmark: id, workload and size,
   competitor libraries, correctness check.
3. **Results** — one chart per benchmark (languages as bars, two metrics:
   time and allocation), rendered from `results/` for the selected machine.
   Unmeasured: the chart's place says "not yet measured on this machine" and
   nothing else.
4. **Standing** — the sentences written after the run. Absent before it.
5. **Reproduce** — the `bench.sh` line, the competitor build line, the
   dataset.
6. **See also** — the stdlib page (`docs/stdlib/<pkg>/<Class>.md`), the source
   under `runtime/src/`, the benchmark source.

Package `README.md` pages mirror `docs/stdlib/README.md`: a table of pages with
one-line descriptions, plus the package's standing paragraph after the run.

### 2.3 Use cases
- **2.3.1** As a reader on `/stdlib/collection/hashmap/`, when I want to know
  how it performs, then one link takes me to `/benchmarks/collection/hashmap/`
  and back.
- **2.3.2** As a reader of the Benchmarks tab, when I open the landing page,
  then I see the standings across packages for one machine and can switch
  machine.
- **2.3.3** As a contributor, when I add a stdlib class, then the
  `check-bench-coverage` test tells me whether it has a benchmark page or a
  recorded exemption (§6.0).

---

## 3. The harness — `dev.cajeta.bench`

### 3.1 Definition
The `samples/profile` harness (`Benchmark`, `Harness`, `Result`, `Csv`,
`Memory`) becomes a published library, `dev.cajeta.bench`, in a new repo
`cajeta-bench`, the way `dev.cajeta.unit` is the test library. The compiler
repo's `bench/` project and each ecosystem repo's `bench/` project declare it
as a dependency. One `Benchmark` contract, one CSV schema, one `--list` /
`--run` / `--area` / `--bench` CLI shape everywhere.

### 3.2 Contract (kept from profile, extended)
- `name()`, `area()` (now the catalog path, `collection/hashmap`), `backend()`,
  `datasetInfo()`, `warmupIters()`, `measuredIters()`, `baselineNanos()`,
  `inputSize()`, `available()`, `variants()`/`prepare`/`runVariant`,
  `supportsVariant`/`skipReason`, `setup`/`run`/`teardown`/`checkResult`.
- New: `machine()` read from `CAJETA_BENCH_MACHINE` or the hostname;
  `competitorNote()` for the row's `notes`; `unit()` naming the throughput
  unit so the chart labels itself.
- Schema v2 = v1's 31 columns plus `machine`, `os` (name and version, and
  `WSL2` when under it), `driver` (CUDA/ROCm/Vulkan driver version, empty on
  CPU), `model` (the LLM or ML model identity with its quantization, empty
  elsewhere), `platform` (the engine or library that produced the row:
  `cajeta-llm`, `llama.cpp`, `torch`, `axum`; for language rows the language
  name). `schema_version` is 2; `verify-fidelity.py` reads both. These are row
  columns, not `env.csv` facts, so a page can lay rows from different machines,
  operating systems and drivers side by side (§7.1).

### 3.3 Use cases
- **3.3.1** As cajeta-http, when I write `bench/` with a `cajeta.json` that
  depends on `dev.cajeta.bench`, then `cajeta bench` builds it and writes a
  `results.csv` the site can render with no translation.
- **3.3.2** As the harness, when a benchmark throws, then I record a `failed`
  row with the exception's message and continue with the next benchmark.
- **3.3.3** As the harness, when `CAJETA_BENCH_SMOKE=1`, then warmup and trials
  collapse to 1 and 3 and the CSV is tagged `smoke` so it can never be
  published (§9.3).

---

## 4. Results — committed, per machine, per run

### 4.1 Model
- `bench/results/<machine>/<run-id>/results.csv` and `env.csv`; `run-id` is
  `<ISO-8601-basic-UTC>-<cajeta-version>` (`20261101T0300Z-0.46.3`).
- `bench/results/<machine>/LATEST` names the run the site renders by default;
  older runs stay committed and selectable.
- Ecosystem repos write the same layout under their own `bench/results/`; the
  publish step (§9.4) copies their rows into the compiler repo's
  `bench/results/<machine>/<run-id>/` with the `area` prefixed (`llm/decode`).
- `env.csv` carries cpu, cores, kernel, OS, governor, memory, cajeta version,
  every competitor toolchain version, GPU name and driver, and the quiet-box
  evidence (load average, GPU utilization at start).

### 4.2 Use cases
- **4.2.1** As the site build, when `results/phoenix/LATEST` names a run, then
  every chart reads that run and no other unless the viewer selects one.
- **4.2.2** As a reviewer of the publish PR, when I read the diff, then the
  whole measurement is two CSV files per machine and the standings copy.

---

## 5. Site rendering

### 5.1 Definition
The Astro site already treats `bench/` as the **Benchmarks** tab with the
reference-browser layout. It gains: charts rendered at build time from
`results/`, a machine selector, package index pages like the stdlib's, and the
landing standings. `report.py` is retired.

### 5.2 Mechanism
A fenced block in the page markdown names the benchmark:
````markdown
```bench
id: collection/hashmap-int-insert
metric: time | throughput | memory
```
````
A rehype plugin replaces it with a chart island (the existing `CorpusCharts`
bar-chart style, dataviz rules: one measure one hue, direct end labels, table
fallback) fed from the manifest the build writes by reading every
`results/<machine>/<run>/results.csv`. Languages are the bars, Cajeta first,
sorted by value; a skipped competitor is a labeled empty bar with its reason on
hover; a benchmark with no row on the selected machine renders the unmeasured
state. The machine selector is site-wide and remembered per viewer.

### 5.3 Landing standings
`bench/README.md` carries one `bench-standings` block: per package, the
geometric mean of Cajeta's time ratio to the best competitor over the
package's benchmarks, with the count of wins, losses and skips. The copy
beside it is written after the run.

### 5.4 Use cases
- **5.4.1** As a reader, when I switch machine from Phoenix to Proton, then
  every chart on the page re-reads without a page load and the URL carries the
  choice.
- **5.4.2** As a reader with no JavaScript, then I see the data table under
  each chart.
- **5.4.3** As the site build before any run, then every chart renders its
  unmeasured state and `npm run build` exits 0.

---

## 6. The language and stdlib catalog

### 6.0 Coverage rule
Every `docs/stdlib/<pkg>/<Class>.md` has either a `bench/<pkg>/<Class>.md` or
a line in `bench/EXEMPTIONS.md` naming why it is not measurable (pure type
with no hot path, e.g. `ZoneOffset`; exception classes; `BackendRegistry`).
`test/coverage_smoke.sh` fails when a stdlib page has neither. The current
stdlib has 124 class pages across 19 packages; the plan's package units each
end with the package fully covered or exempted.

Benchmarks already in `samples/profile` (50) keep their ids and move to their
pages; the list below marks them `[have]`. Everything else is new.

### 6.1 `lang/` — the language, and `cajeta.lang`
- **Loops.md** — counted loop with bounds checks on and off, nested loops,
  `foreach` over `T[]` and `ArrayList`, range iteration, loop-carried
  reduction, early exit. Competitors: C++, Rust, Go, Java, Python.
- **Exceptions.md** — `try` with no throw (zero-cost check), throw-and-catch
  one frame, throw through 10 and 100 frames, `RecoverableException` vs
  `Unrecoverable`, exception in a fiber. Competitors: C++ throw, Rust
  `Result` propagation, Go `error` return, Java throw with and without
  stack trace, Python raise.
- **Allocation.md** — small-object `heap` alloc and scope drop, object churn
  at 1 and 10 live, deep ownership transfer (`#`) through calls, live-set
  tracking on and off (`--live-set`), array allocation, struct value
  allocation. Competitors: C++ `new`/`delete` and `make_unique`, Rust `Box`,
  Go (GC), Java (TLAB), Python.
- **Dispatch.md** — direct call, virtual call, interface call, template
  monomorphized call vs Java erased generic, lambda invocation, method
  reference, `instanceof`.
- **String.md** `[have 4]` — build-concat, search, replace, uppercase, plus
  UTF-8 codepoint walk, `byteAt` scan, split, compare.
- **StringBuilder.md** — append N, append mixed types, `toString`.
- **Optional.md**, **Pair.md**, **Slice.md** — construct, map, unwrap in a
  loop; slice window creation and iteration.
- **Math.md** — scalar transcendental loop, integer division, `Math` intrinsics
  on host; `Vector<T,N>` SIMD saxpy and dot `[have 2 under math]`.
- **Guid.md** — generate, parse, format.
- **stream/Stream.md** `[have 2]` — filter→map→reduce, parallel reduce,
  plus `Collectors.toList`, `groupBy`.

### 6.2 `collection/`
ArrayList `[have]`, LinkedList `[have]`, HashMap int and string `[have 2]`,
HashSet `[have]`, Heap `[have]`, RedBlackTree `[have]`, Sort `[have 4]`,
BPlusTree (insert, point and range lookup), Cache (hit, miss, eviction,
TTL expiry), ImmutableList/Map/Set (build, lookup), Collectors (fold into
Stream page), `ltm/LtmBPlusTree` (insert and lookup past memory, vs RocksDB
and LMDB through their Rust and Java bindings). Competitors: C++ std and
`unordered_dense`, Rust std and `hashbrown`, Go, Java, Python.

### 6.3 `concurrent/`
AtomicInt32/64 `[have atomic-fetchadd]`, Tasks `[have task-spawn-await]`,
Mutex (uncontended lock, 4 and 16 contending fibers), RwLock (read-heavy
90/10), Semaphore, Channel (spsc, mpsc, mpmc at N = 5M, T = 4, the crossbeam
harness), Fiber (sleep wake latency), FiberLocal (get/set), 1M-task
footprint. Competitors: C++ std::thread and atomics, Rust tokio and crossbeam,
Go goroutines and channels, Java virtual threads, Python asyncio.

### 6.4 `time/`
Instant `[have]`, LocalDate `[have]`, Duration arithmetic, LocalDateTime,
ZonedDateTime with a region zone, DateTimeFormatter format and parse ISO-8601,
Clock.now. Exempt: ZoneOffset, Period, ZoneId. Competitors: C++ chrono, Rust
`jiff` and `chrono`, Go time, Java java.time, Python datetime and `whenever`.

### 6.5 `hash/`
XXHash3 64 and 128 `[have 4]`, SipHash `[have]`, DefaultHasher over String
and int keys, `Hash` protocol over a struct. Competitors: the reference C
implementations, Rust `xxhash-rust` and `siphasher`, Go, Java, Python
`xxhash`.

### 6.6 `codec/`
json: tokenize, bind-skip, dom, serialize, roundtrip, conformance `[have 6]`;
csv: parse and write a 100 MB file; Base64 `[have 2]`, Base32; deflate and
inflate streaming and one-shot (from `cajeta-codec/bench/CompressBench`, vs
zlib, zlib-ng, Rust `flate2`, Go, Java Deflater). Competitors for JSON stay
nativejson-benchmark's: simdjson, yyjson, serde_json, Go encoding/json,
Jackson, orjson.

### 6.7 `io/`
Buffer (fill, read primitives, slice), file read and write sequential at
1 GB, random reads, directory walk, `net` TCP echo round-trip latency and
bulk throughput over loopback, UDP send rate, the reactor at 1k idle
connections. Competitors: C++ POSIX, Rust std and tokio, Go, Java NIO,
Python.

### 6.8 `math/`
Tensor: element access `[have ElementAccessBench]`, elementwise ops,
broadcast, reduce, transpose, matmul CPU `[have matmul]`, dtype cast;
linalg: solve, inverse, QR, SVD at 512 and 2048; fft: 1-D complex at 2^16 and
2^20, 2-D; random: uniform and normal at 1e8 draws; npio: `.npy` load and save
at 2^23 `[have Npy.decodeI32]`; poly: evaluate and fit. Competitors: NumPy and
SciPy (the reference a data scientist knows), Eigen, Rust `ndarray` and
`nalgebra`, Go `gonum`, Java `ojAlgo` or EJML; FFTW and pocketfft for fft.

### 6.9 `nucleo/`
Column (construct, filter, arithmetic), Table (groupby-sum at 1e6 and 1e7
rows, join, sort, filter, the h2oai db-benchmark groupby and join questions
q1–q5), Tape (append and replay), Fuse (fused pipeline vs unfused),
Transforms (standardize, one-hot). Competitors: pandas, polars, DuckDB; Apache
Arrow compute optional.

### 6.10 `wire/`
Encoder and SchemaEncoder encode and decode of a 100-field record at 1e6
records, zero-copy struct view read, CompressStream round-trip. Competitors:
protobuf (C++ and Java), FlatBuffers, Cap'n Proto, Rust `bincode`.

### 6.11 `search/`
distance (Levenshtein, Jaro-Winkler at 1e6 pairs), fuzzy (top-k over 1e5
strings), ngram (index build and query). Competitors: Rust `strsim` and
`nucleo-matcher`, Python `rapidfuzz`, Java `commons-text`.

### 6.12 `process/`, `reflect/`, `aot/`, `session/`, `ifx/`, `gfx/`, `error/`
process: spawn and wait a trivial child, pipe 100 MB through a child.
reflect: `Class` lookup and field enumeration. aot: `Scope` and component
resolution cost (DI graph of 100 components at start-up). Exempt by default:
session (package install), ifx and gfx (windowed; measured under xpu textures
if at all), error (measured under `lang/Exceptions`).

### 6.13 `xpu/`
Device: BabelStream copy, mul, add, triad, dot; device saxpy; wave reduce;
scan; GEMM through `CooperativeMatrix` at f32, f16 and bf16 `[have 2 gpu]`;
mandelbrot device; mixbench roofline sweep; launch overhead; host↔device
transfer; KernelBuffer allocate and free; KernelStream ordering (two streams
overlap); KernelTimer resolution; `mesh` tiling; textures sample rate
(`gfx/Texture2D`). The cross-backend showcase: the same `@Kernel` on cuda,
hip, vulkan and cpu with a consistency check. Competitors: CUDA C++ and HIP
C++ (BabelStream and mixbench reference builds), cuBLAS and rocBLAS for GEMM,
CUB and rocPRIM for reduce and scan, PyTorch (`torch.cuda`) as the
Python-user line.

### 6.14 `toolchain/`
Compile time of the tour and of a 10k-line synthetic program (cold, warm
cache, `--lto`), hello-world start-up (exe, `.cja` through the JIT cold and
warm), binary size, build-tool `cajeta build` no-op time, notebook kernel
start-up (from `bench/kernel-startup`), debugger attach. Competitors: javac
and the JVM, rustc, go build, g++, CPython start-up.

---

## 7. The ecosystem catalog

Each library's benchmark code lives in **its own repo** under `bench/`,
depends on `dev.cajeta.bench`, and runs with `cajeta bench`. Its **pages**
live in the compiler repo under `bench/<library>/` so the site renders them,
and its results are copied in at publish time (§4.1).

### 7.1 `llm/` — `dev.cajeta.llm` against llama.cpp
The doctrine in `docs/llm/measuring.md` is the method: interleaved rounds in
the same minute, the median read, the greedy-agreement gate before any
throughput is believed, batched tokens read beside the single-sequence walk.

**The two measures are prefill tokens/s and decode tokens/s, and every row
is keyed by four things** (Julian, 2026-10-25): the **operating system**
(Ubuntu 26.04 on Proton; Ubuntu 24.04 under WSL2 and Windows 11 native on
Phoenix), the **driver** (the NVIDIA driver and CUDA version, the ROCm
version, the Vulkan driver: RADV or the vendor ICD, each as reported at run
time), the **model** (name, parameter count, quantization, file checksum),
and the **platform** (`cajeta-llm` at its version and backend, or `llama.cpp`
at its commit and backend). A page row that lacks any of the four is a failed
row.

Pages:
- **README.md** — the matrix: one block per model, rows OS × driver ×
  backend, two column groups (prefill, decode), two bars in each (cajeta-llm,
  llama.cpp), tokens/s with the ratio printed. This is the page a reader
  lands on; the pages below are its cuts.
- **Decode.md** — tokens/s at 1 and 4 sequences, Llama-3.1-8B at Q2_K
  through Q8_0 and Qwen3-Coder-30B-A3B, per OS × driver × platform.
- **Prefill.md** — tokens/s at pp512 and pp4096, the same keys.
- **Load.md** — safetensors and GGUF load time, time to first token.
- **Memory.md** — resident set at 4096 context.
- **LongContext.md** — decode at 8k and 32k.
- **Kernels.md** — the parity legs from `census/parity-<backend>.tsv`, one GEMM
  shape per row.

Keys on Phoenix: WSL2 with the NVIDIA 610 driver on CUDA and on Vulkan; Windows
11 native on CUDA and Vulkan when the Windows build of both engines is green.
Keys on Proton: Ubuntu 26.04 with ROCm on HIP and with RADV on Vulkan. Each
key is one interleaved session of both platforms on the same model file; the
llama.cpp commit and build flags, and the cajeta-llm version and compiler
flags, are in the row. The existing `bench/SchedThroughput` and
`tools/parity/*` are wrapped to emit schema-v2 rows rather than rewritten;
`llama-bench` and `llama-batched-bench` output is parsed into the same rows.

### 7.2 `ml/` — `dev.cajeta.ml` (and `dev.cajeta.xgboost`) against scikit-learn, PyTorch, XGBoost
Classical: LinearRegression, LogisticRegression, KMeans, KNN, PCA, StandardScaler
fit and predict on synthetic data at 1e5 × 100 and the sklearn digits and
California housing sets; metrics. Neural: MLP, small CNN and a transformer
block, training step and inference throughput at batch 64 on CPU and GPU,
autodiff overhead vs torch; checkpoint load. XGBoost: the `~/code/ml/xgbbench`
harness (hist, GPU fit, predict) against upstream CUDA, moved under
`cajeta-xgboost/bench`. Competitors: scikit-learn, PyTorch (CPU and CUDA),
XGBoost upstream, versions recorded.

### 7.3 `http/` — `cajeta-http` against the mainstream stacks
Server: plaintext and JSON (the TechEmpower shapes without the database),
HTTP/1.1 keep-alive and HTTP/2 multiplexed at 64 and 512 connections,
WebSocket echo throughput and permessage-deflate, SSE fan-out to 1k clients,
TLS handshake rate. Client: `HttpClient` GET throughput and connection reuse.
Start-up time and idle RSS. Load generator pinned (`oha`, with `wrk` as a
cross-check). Competitors: Go `net/http`, Rust `axum` on hyper, Java Netty,
Node.js `http`, Python `uvicorn`.

### 7.4 `primavera/` — primavera against Spring Boot and its peers
The `register-user` shape: a JSON REST endpoint through DI, request scope,
validation and an auth filter; a templated page through cajeta-jinja; cold
start to first response; idle and loaded RSS; build time; a 100-component DI
graph start-up. Competitors: Spring Boot (MVC, Java 25), Quarkus and
Micronaut (JVM, with native-image as a second row where it builds), ASP.NET
Core, FastAPI. Database-backed shapes wait on a primavera data layer (§12.5).

### 7.5 Use cases
- **7.5.1** As a reader of `/benchmarks/llm/`, when I open the page, then
  for each model I see prefill and decode tokens/s for cajeta-llm beside
  llama.cpp, one row per operating system, driver and backend, with the
  llama.cpp commit, the cajeta-llm version and the greedy-agreement rate on
  the row, and I can read the same model on WSL2, Windows and Ubuntu in one
  glance.
- **7.5.1a** As a reader comparing drivers, when a driver update lands and a
  new run is published, then the old driver's rows stay selectable and the
  page can show both.
- **7.5.2** As cajeta-primavera's maintainer, when I release, then
  `cajeta bench` still builds against the published `dev.cajeta.bench`.

---

## 8. Competitors and methodology

### 8.1 `METHODOLOGY.md` states, for every number
release build (`--release`, `--opt=O3`, `--cpu=native`) against each
competitor's equivalent (`-O3 -march=native`, `target-cpu=native`, JIT
warmed, CPython as shipped); warmup then trials; min, median, p95; allocated
bytes and peak RSS; the correctness check; the skip reasons; interleaved
rounds for device work and the clock caveat; the quiet-box evidence; and the
rule that a smoke row is never published.

### 8.2 Language competitors (lang, stdlib, toolchain)
C++ (g++ 13 or the fork's clang 23, pinned per machine), Rust stable, Go, Java
25, Python 3.12 with NumPy where idiomatic. Pinned libraries stay those of
`competitors/vendor/manifest.tsv` (simdjson, yyjson, pdqsort, unordered_dense,
xxhash, Jackson) plus the additions §6 names.

### 8.3 Device competitors (xpu)
CUDA 12.x on Phoenix, ROCm on Proton: BabelStream and mixbench reference
builds, cuBLAS/rocBLAS, CUB/rocPRIM, PyTorch CUDA/ROCm.

### 8.4 Data competitors (nucleo, math)
pandas, polars, DuckDB; NumPy, SciPy, FFTW.

### 8.5 Model competitors (ml, llm)
scikit-learn, PyTorch, XGBoost; llama.cpp.

### 8.6 Service competitors (http, primavera)
Go net/http, axum, Netty, Node, uvicorn; Spring Boot, Quarkus, Micronaut,
ASP.NET Core, FastAPI.

### 8.7 Provisioning
`competitors/setup.sh` installs or verifies every toolchain and pinned
library per machine and writes `competitors/toolchains-<machine>.tsv`;
`probe-toolchains.sh` (kept) fails the run when a pinned version is missing.
Measured 2026-10-25 on Phoenix: Java 25 and Go 1.22 present; **rustc, cargo,
dotnet absent**; clang 14 (too old; use g++ 13 or the fork's clang); Python
3.12 without polars or duckdb.

---

## 9. The run and the publish

### 9.1 Quiet-box check
`bench.sh` refuses to start a published run unless load average is under 1,
no CI runner job is active, and GPU utilization is 0 with no compute
processes (`feedback_check_gpu_free_before_runs`); it records the evidence in
`env.csv`.

### 9.2 Announcement
The run is announced per `td-project-workflow.md` § Runs: start time,
estimated end from the smoke timings times the trial multiplier, and the hard
cap. Each area is a leg; legs are reported against their estimates.

### 9.3 Order
language and stdlib areas on CPU; toolchain; device areas; nucleo and math
with their Python competitors; then each ecosystem repo's `cajeta bench`;
llm last (it owns the GPU for longest). Smoke rows are filtered by the
publish step by their `smoke` tag.

### 9.4 Publish
`bench.sh publish <machine> <run-id>` collects the compiler repo's rows and
each ecosystem repo's rows into `bench/results/<machine>/<run-id>/`, runs
`verify-fidelity.py`, updates `LATEST`, and regenerates the site. The
standings copy (§5.3, each page's Standing) is written by hand from the
rendered charts, committed with the CSV, and the site deployed
(`feedback_deploy_site_when_asked`).

---

## 10. Site work

### 10.1 Benchmarks tab
Package index pages, class pages, charts, machine selector, standings
(§5). The tab stays pinned last.

### 10.2 Home page
"Wondering about speed?" links to `/benchmarks/` and, after the run,
carries the one-sentence standing.

### 10.3 Kernel catalog tables (Julian, 2026-10-25)
`docs/kernels/catalog-llm.md` (258 kernels, five columns: kernel, source,
llama.cpp counterpart, `cuda / cpu / hip`, role) and `catalog-xgboost.md`
(kernel, stage, source, `@Wave(32)`, role) wrap their last column inside the
prose width. Use case: as a reader, when I open the catalog, then every row
reads on one line at the site's default width with the role legible.
Recommendation (§12.6): drop the `source` column and link the kernel name to
its source; move `role` out of the grid into a second line under the kernel
name so the grid keeps three narrow columns; the generator emits the new
shape so the next regeneration cannot reintroduce the wrap.

---

## 11. Acceptance (whole spec)
1. `bench/` mirrors `docs/stdlib/` and `coverage_smoke.sh` passes: every
   stdlib page has a benchmark page or an exemption.
2. `dev.cajeta.bench` is published on Olla and consumed by the compiler repo
   and by cajeta-llm, cajeta-ml, cajeta-xgboost, cajeta-http, cajeta-primavera,
   cajeta-codec.
3. Every benchmark in §6 and §7 builds, passes its smoke test (builds, checks,
   emits a row) on at least one machine, and has a competitor program per
   named competitor that passes the same smoke.
4. `npm run build` in `site/` renders every page with every chart in the
   unmeasured state before the run, and with bars after it.
5. The kernel catalog tables render without wrapping at the site's default
   width.
6. The published run exists for both machines, passes `verify-fidelity.py`,
   and the standings copy is on the site.

---

## 12. Open questions — all decided on the recommendation (Julian, 2026-10-25)
1. **Harness as a library.** `dev.cajeta.bench` in a new `cajeta-bench` repo
   (**recommended**: it is the `cajeta-unit` pattern and the only way six
   repos emit one schema) versus the harness living in the compiler repo and
   each ecosystem repo copying five files.
2. **Retire `samples/profile` and `report.py`** in favor of `bench/` and the
   Astro site (**recommended**), or keep the Python minisite as a second
   renderer.
3. **Default machine on the site.** **Proton** for CPU and language areas
   (native Linux, no WSL2 jitter) and Phoenix for CUDA; or Phoenix throughout.
4. **Competitor breadth for primavera.** Spring Boot, Quarkus, Micronaut,
   ASP.NET Core, FastAPI (**recommended**); or Spring Boot alone in v1.
5. **Database shapes** for http and primavera wait on a primavera data layer
   (**recommended**), or run against SQLite through `@Native` now.
6. **Kernel catalog columns.** Drop `source` and demote `role` to a
   description line (**recommended**); or widen the table container to the
   full page width and keep five columns.
7. **Device competitors in Python.** PyTorch as the one Python-user line for
   xpu and math (**recommended**); or add CuPy and JAX.
8. **Where xgboost pages live.** Under `bench/ml/` beside dev.cajeta.ml
   (**recommended**) or their own `bench/xgboost/`.
