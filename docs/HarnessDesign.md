# HarnessDesign.md

A stress-test harness for measuring cajeta's threading model and
buffer-pipeline performance, plus the methodology for comparing those
numbers against other languages' concurrency primitives.

> **Status: design / planning.** The `cajeta_harness` binary, the
> `BufferChain` type, and the comparison harnesses under `harness/`
> are not built yet — this is the plan for them. The pieces they lean
> on are at different stages: the multi-carrier work-stealing scheduler
> is shipped (see below); `Fiber.sleep` lowers to a runtime call but the
> full fiber-side timer wheel is still landing (R9.x). Treat the dials,
> CLI shape, and methodology here as the target, not current behavior.

## What we're measuring

**Goal:** characterize cajeta's fiber scheduler + buffer pipeline under
realistic server-shaped load, well enough to defend (or refute) the
architecture choices vs. mainstream alternatives.

**Primary axes:**

| Dial | Range | What it stresses |
|------|-------|------------------|
| Concurrent connections | 1 → 100K | scheduler / context-switch / TLS / wake-list |
| Per-request work (sleep) | 0.1 ms → 1 s | parking / timer-wheel / wakeup latency |
| Sleep jitter | 0% → 50% (uniform) | timer-wheel granularity, wake batching |
| Buffer chunk size | 256 B → 64 KB | allocator + cache behavior |
| Fiber stack size | 4 KB → 1 MB | per-fiber memory cost |

**Outputs:**

- **Throughput:** requests/second sustained over a measurement window.
- **Latency:** p50, p90, p99, p99.9 — wall-clock time from request-arrival
  to response-sent.
- **Memory:** peak RSS, allocator high-water mark.
- **Carrier CPU%:** wall-time fraction spent in user mode vs. blocked.

These are the same numbers a typical server benchmark (TechEmpower's
"plaintext", Tatoo's "ping", Vert.x's `req/s`) reports, so the comparison
plots have known shape.

## Workload shape

Each simulated "connection" is a fiber that loops:

1. Wait `T_inter_arrival` ms (Poisson-distributed around the test's target
   arrival rate; gives a realistic request stream rather than a closed loop).
2. Grab a buffer from the inbound `BufferChain`.
3. Decode a request payload out of the buffer.
4. **Simulate work:** `Fiber.sleep(work_ms + jitter)`.
5. Compute a response (a constant transform on the request).
6. Grab a buffer from the outbound `BufferChain`, encode the response.
7. Record `now - request_start` into a per-fiber latency histogram.

The sleep step is what makes this a *concurrency* benchmark rather than a
CPU benchmark. With cooperative `Fiber.sleep`, parking should yield the
carrier to other fibers; with a blocking `Thread.sleep`, the carrier
serializes and throughput collapses to `1 / work_time`. The contrast is
meaningful.

The buffer step exercises the design you sketched: pre-allocated
`int8[MAX_SIZE]` chunks in a per-direction linked list.

## Carrier configuration

Cajeta's scheduler is a multi-carrier pool — N OS carrier threads, each
with its own Chase–Lev work-stealing deque (`cajeta_carrier_deque` in
`runtime/native/cajeta_runtime.c`, R8.2). The carrier count is read once at
first spawn from `CAJETA_CARRIERS`, defaulting to `min(nproc, 4)`. The
harness should still expose carrier count as a CLI flag (mapping to
`CAJETA_CARRIERS`) so vs-Java-virtual-threads / vs-Go-goroutines runs can
pin matching core counts — and so a single-carrier baseline (`--carriers 1`)
is reproducible for the simplest comparison.

CLI shape:

```
./cajeta_harness \
    --connections 10000 \
    --target-rps 50000 \
    --work-ms 5 \
    --jitter-pct 20 \
    --buffer-size 4096 \
    --buffer-count 256 \
    --fiber-stack-kb 16 \
    --carriers 1 \
    --warmup-secs 5 \
    --measure-secs 30 \
    --output results.json
```

`fiber-stack-kb` and `carriers` become runtime arguments to the binary, not
compile-time; matches your "stacksize should be an argument to the
executable for running" directive.

## What we're comparing against

**Same workload, six languages.** Each implementation does the same
sleep-and-respond loop with the same dials.

| Stack | Concurrency primitive | Notes |
|-------|----------------------|-------|
| **cajeta** | Stackful fibers, single carrier + reactor | The thing we're measuring |
| **Java (21+)** | Virtual threads (`Thread.ofVirtual`) | The closest analog — also stackful, also scheduler-parked |
| **Go** | Goroutines + scheduler | Industry baseline. M:N scheduler with work stealing. |
| **Rust + tokio** | Async/await on a multi-threaded executor | Stackless coroutines; very different shape |
| **Node.js** | Event loop, async callbacks/promises | Single-threaded reactor; the simplest comparison |
| **C++ (boost::fiber)** | Stackful fibers, similar model to cajeta | Sanity check that cajeta isn't off by a factor of N vs. a well-tuned C++ baseline |

Each implementation must be:
- Idiomatic for its language (no "C ported to Rust" code).
- Tuned to comparable defaults (don't sandbag tokio with a single worker if
  cajeta also has one carrier, but DO compare equivalent core counts).
- Audited for what's in the hot path — no JSON serialization, no logging,
  no syscalls beyond what the model intrinsically requires.

Each implementation lives in `harness/<lang>/` with a Makefile and a short
README that captures version + flags used. The runner script invokes each
with matching dials, parses the JSON output, and produces a comparison
table.

## Methodology guards

**Avoid the obvious benchmarking traps:**

- **Warmup phase before measurement.** First 5 seconds discarded. Captures
  JIT warmup (Java/JS), allocator pre-touching, page faults.
- **Closed-loop vs open-loop.** Open-loop (Poisson arrivals, fixed target
  RPS) — coordinated omission isn't a thing because new fibers don't wait
  for in-flight ones. Reported latency reflects real queueing.
- **Histograms, not means.** p99 latency is the interesting number, not
  average. Use HdrHistogram (or equivalent) so tail buckets aren't summarized.
- **Pin the run.** `taskset` to specific cores; `--cpu-quota` if running in
  a cgroup. Stops kernel scheduler from confounding the comparison.
- **Repeat runs.** Each (lang, dial-setting) combination runs N=5 times;
  report median + min/max envelope. One-shot numbers are noise.

**Apples-to-apples on `work-ms`:**

The "work" is `Fiber.sleep(work_ms)` — a parked wait, not a busy loop. Every
implementation parks the work unit cooperatively or blocks the underlying
thread; the comparison is fair because the *scheduler* is doing all the
heavy lifting. A busy-loop version (CPU-bound work) is a separate sweep —
useful but it measures different machinery.

## What the numbers will tell us

For each language and each dial setting, we'll learn:

1. **Throughput ceiling at fixed connection count.** When do we max out?
   For a single carrier this is roughly `connections / (work_ms + overhead)`.
   Cajeta's overhead is what we want to characterize.
2. **Tail-latency degradation under load.** As `target_rps` approaches the
   throughput ceiling, p99 grows. The shape of that curve tells us about
   scheduler fairness — is it FIFO, LIFO, or something with starvation?
3. **Memory per fiber.** Spawn N fibers, measure RSS. Cajeta's
   fixed-64 KB-stack default is generous; we expect to land between Go
   (≈2 KB initial, growable) and Java virtual threads (≈few KB).
4. **Where the time goes.** With `perf record` on the carrier thread we
   can see which runtime functions dominate. Hot spots:
   - `__cajeta_fiber_park` / `swapcontext` — context-switch cost.
   - Timer-wheel insertion / poll — reactor cost.
   - Allocator (when buffer-count is low and we recycle aggressively).

If any of these are pathological — say, `swapcontext` shows up at 30% of
samples — that's a signpost for the next round of work.

## View-overlay micro-benchmark

Separate from the server harness, a focused buffer test that isolates
the `view`-over-byte-buffer pattern (see `Views.md`):

- N buffers, each 64 KB, linked.
- Walk the chain, decoding `view Record { int64 key; int64 timestamp; int64[6] payload; }` instances over each buffer's 64-byte sub-regions.
- Sum a field across all records.
- Measure: records-per-second decoded.

Compare with:
- **Java** — `ByteBuffer` + `getLong` (no overlay; per-field byte access).
- **Rust** — `bytemuck::cast_slice::<u8, Record>(&buf)` (zero-copy too).
- **C** — `(Record*) buf` (the bare-metal baseline).
- **cajeta with `@HostEndian`** — should match C's numbers within noise; the view's per-access codegen is a plain load+GEP.
- **cajeta with `@BigEndian` on a little-endian host** — measures the cost of `bswap` intrinsics. Expected ~5–10% over the host-endian case; if it's worse, the codegen is doing too much.

This tells us whether cajeta's view overlay actually delivers
zero-copy performance or whether the compiler is inserting hidden
copies / bounds checks we didn't intend. A secondary measurement
runs the same loop with `@Align(natural)` to quantify the
unaligned-access cost on ARM (negligible on x86).

## Sequence

Following docs/stdlib/'s implementation order:

1. **Externalize stdlib + parse-once refactor.** Unblocks everything else.
2. **`Fiber.sleep` + timer reactor + `nanoTime` intrinsic.** The
   long-pole work. docs/stdlib/Concurrency.md grows a "Reactor" section; the carrier
   loop gains a `next_deadline = timer_wheel.peek(); poll_or_sleep_until(next_deadline)`
   shape. This is where most of the engineering effort lives.
3. **Stack-size CLI flag on the cajeta binary.** Passed through to
   `__cajeta_task_run`'s stack allocation. Small.
4. **Buffer / BufferChain types in cajeta.io.** Land in stdlib.
5. **The harness itself, in cajeta.** A single `.cajeta` source that
   reads CLI args, spawns N fibers, runs the workload, prints results.
   Sub-second to write once the primitives exist.
6. **Comparison implementations in `harness/{java,go,rust,node,cpp}/`.**
   Same workload, each idiomatic.
7. **Runner script + result analysis.** Python script that drives the
   matrix of (lang, dial-setting), parses JSON output, emits tables / plots.

Step 2 dominates the calendar — it's where the design decisions land for
how cajeta's concurrency model actually works under load. The rest is
plumbing.

## Open questions for review

1. **Reactor design.** Timer wheel granularity (1 ms? 10 ms?), how the
   carrier blocks when no fiber is ready (epoll? blocking-sleep on next
   deadline?), how spurious wakeups are handled. Goes in docs/stdlib/Concurrency.md
   as that work lands.
2. **Closed-loop benchmark fallback.** The open-loop model is the right
   one but harder to interpret. Do we also run a classic closed-loop
   sweep ("N connections, each does requests as fast as it can") for an
   easier-to-read throughput number? Recommended: both.
3. **Sandboxing.** When the harness pushes 100K fibers we'll see OS
   limits — fd ceiling, ulimit stack — bite. Document the required
   `ulimit -n` / `ulimit -s` settings up front so reproduction is reliable.
4. **Comparison fairness on "no I/O." ** Our harness simulates work with
   `sleep`. A real server has real I/O (network round-trips,
   disk reads). The comparison generalizes to scheduler-bound workloads
   but not to I/O-bound ones; document this caveat in the results
   write-up.
5. **What ships first.** If we hit time pressure, the *minimum viable
   harness* is: cajeta-only, single carrier, two dials (connections,
   work-ms). Even that tells us a lot about whether the scheduler holds
   up. Recommended: ship that first, then add comparators.
