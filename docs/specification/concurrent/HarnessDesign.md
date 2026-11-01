# HarnessDesign

This page describes a stress-test harness for measuring cajeta's threading model and
buffer-pipeline performance, plus the methodology for comparing those
numbers against other languages' concurrency primitives.

> **Status: design / planning.** The `cajeta_harness` binary, the
> `BufferChain` type, and the comparison harnesses under `harness/`
> are not built yet. This is the plan for them. The pieces they lean
> on are at different stages. The multi-carrier work-stealing scheduler
> is shipped (see below). `Fiber.sleep(Duration)` parks the calling fiber
> on the runtime timer wheel. Treat the dials,
> CLI shape, and methodology here as the target. They do not describe current behavior.

## What we're measuring

The goal is to characterize cajeta's fiber scheduler and buffer pipeline under
realistic server-shaped load, well enough to defend or refute the
architecture choices against mainstream alternatives.

The primary axes:

| Dial | Range | What it stresses |
|------|-------|------------------|
| Concurrent connections | 1 → 100K | scheduler / context-switch / TLS / wake-list |
| Per-request work (sleep) | 0.1 ms → 1 s | parking / timer-wheel / wakeup latency |
| Sleep jitter | 0% → 50% (uniform) | timer-wheel granularity, wake batching |
| Buffer chunk size | 256 B → 64 KB | allocator + cache behavior |
| Fiber stack size | 4 KB → 1 MB | per-fiber memory cost |

Outputs:

- **Throughput.** Requests/second sustained over a measurement window.
- **Latency.** p50, p90, p99, and p99.9 of the wall-clock time from request-arrival
  to response-sent.
- **Memory.** Peak RSS and the allocator high-water mark.
- **Carrier CPU%.** The wall-time fraction spent in user mode vs. blocked.

These are the same numbers a typical server benchmark (TechEmpower's
"plaintext", Tatoo's "ping", Vert.x's `req/s`) reports, so the comparison
plots have a known shape.

## Workload shape

Each simulated "connection" is a fiber that loops:

1. Wait `T_inter_arrival` ms, Poisson-distributed around the test's target
   arrival rate, which gives a realistic request stream rather than a closed loop.
2. Grab a buffer from the inbound `BufferChain`.
3. Decode a request payload out of the buffer.
4. Simulate work with `Fiber.sleep(Duration.ofMillis(work_ms + jitter))`.
5. Compute a response (a constant transform on the request).
6. Grab a buffer from the outbound `BufferChain`, encode the response.
7. Record `now - request_start` into a per-fiber latency histogram.

The sleep step is what makes this a concurrency benchmark rather than a
CPU benchmark. With cooperative `Fiber.sleep`, parking should yield the
carrier to other fibers. With a blocking `Thread.sleep`, the carrier
serializes and throughput collapses to `1 / work_time`. The contrast is
meaningful.

The buffer step exercises the design you sketched, with pre-allocated
`int8[MAX_SIZE]` chunks in a per-direction linked list.

## Carrier configuration

Cajeta's scheduler is a multi-carrier pool of N OS carrier threads, each
with its own Chase–Lev work-stealing deque (`cajeta_carrier_deque` in
`runtime/native/cajeta_runtime.c`, R8.2). The carrier count is read once at
first spawn from `CAJETA_CARRIERS`, defaulting to `min(nproc, 4)`. The
harness should still expose the carrier count as a CLI flag mapping to
`CAJETA_CARRIERS`, so runs against Java virtual threads or Go goroutines can
pin matching core counts, and so a single-carrier baseline (`--carriers 1`)
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

`fiber-stack-kb` and `carriers` become runtime arguments to the binary rather
than compile-time settings. This matches your "stacksize should be an argument to the
executable for running" directive.

## What we're comparing against

The same workload runs in six languages. Each implementation does the same
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
- Idiomatic for its language, with no "C ported to Rust" code.
- Tuned to comparable defaults. Do not sandbag tokio with a single worker if
  cajeta also has one carrier, but do compare equivalent core counts.
- Audited for what is in the hot path, with no JSON serialization, no logging, and
  no syscalls beyond what the model intrinsically requires.

Each implementation lives in `harness/<lang>/` with a Makefile and a short
README that captures the version and flags used. The runner script invokes each
with matching dials, parses the JSON output, and produces a comparison
table.

## Methodology guards

Avoid the obvious benchmarking traps:

- **Warmup phase before measurement.** The first 5 seconds are discarded. This captures
  JIT warmup (Java/JS), allocator pre-touching, and page faults.
- **Closed-loop vs open-loop.** The harness is open-loop, with Poisson arrivals and a fixed target
  RPS. Coordinated omission does not arise, because new fibers do not wait
  for in-flight ones. Reported latency reflects real queueing.
- **Histograms over means.** p99 latency is the interesting number, and the
  average hides it. Use HdrHistogram or an equivalent so tail buckets are not summarized.
- **Pin the run.** `taskset` to specific cores, and use `--cpu-quota` if running in
  a cgroup. This stops the kernel scheduler from confounding the comparison.
- **Repeat runs.** Each (lang, dial-setting) combination runs N=5 times, and the
  report gives the median and the min/max envelope. One-shot numbers are noise.

The `work-ms` dial is apples-to-apples across implementations.

The "work" is `Fiber.sleep(Duration.ofMillis(work_ms))`, a parked wait. Every
implementation parks the work unit cooperatively or blocks the underlying
thread, and the comparison is fair because the scheduler is doing all the
heavy lifting. A busy-loop version with CPU-bound work is a separate sweep.
It is useful, but it measures different machinery.

## What the numbers will tell us

For each language and each dial setting, we will learn:

1. **Throughput ceiling at fixed connection count.** When do we max out?
   For a single carrier this is roughly `connections / (work_ms + overhead)`.
   Cajeta's overhead is what we want to characterize.
2. **Tail-latency degradation under load.** As `target_rps` approaches the
   throughput ceiling, p99 grows. The shape of that curve tells us about
   scheduler fairness, whether it is FIFO, LIFO, or something with starvation.
3. **Memory per fiber.** Spawn N fibers and measure RSS. Cajeta's
   fixed-64 KB-stack default is generous, and we expect to land between Go
   (≈2 KB initial, growable) and Java virtual threads (≈few KB).
4. **Where the time goes.** With `perf record` on the carrier thread we
   can see which runtime functions dominate. Hot spots:
   - `__cajeta_fiber_park` / `swapcontext`, the context-switch cost.
   - Timer-wheel insertion / poll, the reactor cost.
   - The allocator, when buffer-count is low and we recycle aggressively.

If any of these are pathological, say `swapcontext` showing up at 30% of
samples, that is a signpost for the next round of work.

## View-overlay micro-benchmark

Separate from the server harness, a focused buffer test isolates
the `view`-over-byte-buffer pattern (see `Views.md`):

- N buffers, each 64 KB, linked.
- Walk the chain, decoding `view Record { int64 key; int64 timestamp; int64[6] payload; }` instances over each buffer's 64-byte sub-regions.
- Sum a field across all records.
- Measure: records-per-second decoded.

Compare with:
- **Java** uses `ByteBuffer` + `getLong`, with no overlay and per-field byte access.
- **Rust** uses `bytemuck::cast_slice::<u8, Record>(&buf)`, which is zero-copy too.
- **C** uses `(Record*) buf`, the bare-metal baseline.
- **cajeta with `@HostEndian`** should match C's numbers within noise, since the view's per-access codegen is a plain load+GEP.
- **cajeta with `@BigEndian` on a little-endian host** measures the cost of `bswap` intrinsics. The expected cost is ~5–10% over the host-endian case, and if it is worse, the codegen is doing too much.

This tells us whether cajeta's view overlay actually delivers
zero-copy performance or whether the compiler is inserting hidden
copies or bounds checks we did not intend. A secondary measurement
runs the same loop with `@Align(natural)` to quantify the
unaligned-access cost on ARM, which is negligible on x86.

## Sequence

Following docs/specification/'s implementation order:

1. **Externalize stdlib + parse-once refactor.** This unblocks everything else.
2. **`Fiber.sleep` + timer reactor + `nanoTime` intrinsic.** This is the
   long-pole work. docs/specification/concurrent/Concurrency.md grows a "Reactor" section, and the carrier
   loop gains a `next_deadline = timer_wheel.peek(); poll_or_sleep_until(next_deadline)`
   shape. This is where most of the engineering effort lives.
3. **Stack-size CLI flag on the cajeta binary.** It is passed through to
   `__cajeta_task_run`'s stack allocation. This is small.
4. **Buffer / BufferChain types in cajeta.io.** These land in stdlib.
5. **The harness itself, in cajeta.** This is a single `.cajeta` source that
   reads CLI args, spawns N fibers, runs the workload, and prints results.
   It is sub-second to write once the primitives exist.
6. **Comparison implementations in `harness/{java,go,rust,node,cpp}/`.**
   These run the same workload, each idiomatic.
7. **Runner script + result analysis.** A Python script drives the
   matrix of (lang, dial-setting), parses JSON output, and emits tables and plots.

Step 2 dominates the calendar, because it is where the design decisions land for
how cajeta's concurrency model actually works under load. The rest is
plumbing.

## Open questions for review

1. **Reactor design.** The open points are the timer wheel granularity (1 ms? 10 ms?), how the
   carrier blocks when no fiber is ready (epoll? blocking-sleep on next
   deadline?), and how spurious wakeups are handled. The answers go in docs/specification/concurrent/Concurrency.md
   as that work lands.
2. **Closed-loop benchmark fallback.** The open-loop model is the right
   one but harder to interpret. Do we also run a classic closed-loop
   sweep ("N connections, each does requests as fast as it can") for an
   easier-to-read throughput number? The recommendation is both.
3. **Sandboxing.** When the harness pushes 100K fibers we will see OS
   limits bite, such as the fd ceiling and the ulimit stack. Document the required
   `ulimit -n` / `ulimit -s` settings up front so reproduction is reliable.
4. **Comparison fairness on "no I/O." ** Our harness simulates work with
   `sleep`. A real server has real I/O, such as network round-trips and
   disk reads. The comparison generalizes to scheduler-bound workloads
   and does not generalize to I/O-bound ones. Document this caveat in the results
   write-up.
5. **What ships first.** If we hit time pressure, the minimum viable
   harness is cajeta-only, single carrier, with two dials (connections,
   work-ms). Even that tells us a lot about whether the scheduler holds
   up. The recommendation is to ship that first, then add comparators.
