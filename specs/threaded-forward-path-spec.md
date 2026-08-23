# The host forward path runs on one core

**Filed 2026-08-22**, from the performance ranking that followed
`quant-scalar-decode-cost`. Every other item on that ranking was a
percentage of one kernel. This one is a multiple, and it is the only
remaining lever of that size.

## 1. Definition

`cajeta-llama`'s host decode path is entirely serial: there is not one
`spawn` in `src/main`. The packed mat-vecs it spends its time in are
embarrassingly parallel over output rows. This spec defines the work to
run them across the carrier pool the cajeta runtime already has.

**Scope.** The four packed mat-vec kernels
(`Quant.q4k/q5k/q6k/q8MatVecInto*`) and their single caller seam,
`Linear.matvecInto`. The runtime carrier cap that bounds how far this can
scale.

**Non-goals.** The GPU/XPU path (already has `KernelThread`). Prefill
batching to GEMM — a real win but structural, prefill-only, and
independent of this. Any change to numerics: this spec must not move a
single output bit.

## 2. Why — the measurement, not the intuition

- **2.1** A Q4_K projection at 4096x4096 reads **9.44 MB** of weights.
  At today's 11.28 ms (f32 kernel) that is **837 MB/s** against a machine
  that sustains **~39 GB/s** — 2% of available bandwidth.
- **2.2** The memory floor for that read is **0.242 ms**. The kernel is
  **47x above its own floor** (36x for the q8_K kernel at 8.77 ms). It is
  compute-bound with a very large margin, which is exactly the condition
  under which adding cores pays close to linearly.
- **2.3** This is also why `llama.cpp` gains only ~1.8x from `-t 32`
  while we should gain far more: it is already near the memory wall and
  we are nowhere near it. The comparison is not evidence that threading
  pays poorly — it is evidence that it pays poorly *once you are fast*.
- **2.4** `y[i] = dot(W[i], x)` — rows share only the read-only weight
  buffer and the read-only activation vector, and each writes one
  distinct `y[i]`. There is no reduction, no accumulator contention and
  no false sharing beyond the last cache line of `y`.

## 3. What the runtime already provides — and what it costs

Read from `runtime/native/cajeta_rt_concurrent_exec.c`, not assumed.

- **3.1** `spawn expr` yields `Task<R>` and runs on **real OS threads**:
  a fixed pool of pthread carriers with per-carrier work-stealing deques.
  This is genuine parallelism, not cooperative fibers on one core.
- **3.2** Carrier count is `$CAJETA_CARRIERS`, else
  `min(_SC_NPROCESSORS_ONLN, CAJETA_DEFAULT_CARRIERS_CAP=4)`, clamped to
  `[1, CAJETA_MAX_CARRIERS=16]`. On the 32-core development box the
  default is therefore **4**, and 16 is the ceiling without a runtime
  change. DECIDED 2026-08-22: lift the ceiling so the pool can reach
  `nproc`. §5.0 records why that ceiling turned out to be an artifact
  rather than a limit.
- **3.3** Fiber stacks are **1 MB** (ucontext). Task granularity must be
  **one task per carrier over a row range**, never one per row — 4096
  tasks would ask for gigabytes of stack.
- **3.4** THE TRAP: the first `spawn` calls
  `__cajeta_live_set_go_multithreaded()`, a **one-way, process-global**
  flip. After it, every `__cajeta_live_set_add` and every
  `__cajeta_live_set_claim` — so every heap allocation and every drop
  anywhere in the process, for the rest of its life — takes one global
  `pthread_mutex`. Nothing turns it back off.
- **3.5** The mat-vec kernels themselves are safe from §3.4: as of
  `headK4` every scratch array is hoisted out of the row loop, so the
  inner loops allocate nothing. The risk is to the REST of the engine —
  the tokenizer, tensor construction, the sampler — which allocate
  freely and would begin paying a global lock they do not today.

## 4. Splitting the mat-vec

- **4.1** When a packed mat-vec runs, its output rows are divided into as
  many contiguous ranges as there are carriers, and each range is
  computed independently.
- **4.2** When a kernel is asked for a row range, it computes exactly
  those rows and touches no others — so the existing whole-matrix entry
  point becomes the range `[0, rows)` and keeps its signature.
- **4.3** When the row count does not divide evenly by the carrier count,
  the remainder is spread one row per range rather than piled on the last
  one, so no carrier finishes late.
- **4.4** When a matrix is small enough that dispatch costs more than the
  work, it runs serially on the calling thread — the threshold is
  measured, not guessed.
- **4.5** When threading is disabled, the path is bit-identical to today
  and takes no locks it does not take today.

## 5. The carrier ceiling

The history was read (`git log -S`), not assumed, and it separates two
constants that look alike and are not.

- **5.0** `CAJETA_MAX_CARRIERS = 16` is **not a policy decision**. It
  entered with the R8.3 scaffold (`902bf080`, 2026-05-30) as the bound of
  a static array — `static struct cajeta_carrier
  __cajeta_carriers[CAJETA_MAX_CARRIERS]` — and no commit, comment or doc
  argues for the value. The commit that flipped the default to
  multi-carrier (`8ac21177`) anticipated this exact moment: *"Final value
  still passes through the [1, CAJETA_MAX_CARRIERS=16] clamp, so a future
  ceiling raise is one constant."*
- **5.0.1** `CAJETA_DEFAULT_CARRIERS_CAP = 4` IS reasoned, by the same
  commit: *"a 64-core box doesn't need 16 carriers on a program with no
  work for them, and the typical fan-out workloads in the test battery
  saturate around 2–4 carriers."* It stays. It answers what an
  incidental-fan-out program gets for free — a different question from
  what a compute kernel may ask for, and this spec's caller asks
  explicitly.
- **5.1** When a machine has more hardware threads than the built-in
  ceiling, a program that asks for them gets them — there is no
  compile-time bound on the pool size.
- **5.2** When the ceiling is lifted, programs that do not ask for
  carriers see no change: the default stays `min(nproc, 4)` and only the
  ceiling moves.
- **5.3** When a carrier count is requested that the machine cannot
  support, it is clamped rather than refused.
- **5.4** When the pool is sized, it reserves memory for the carriers it
  actually starts. Each carrier embeds a 2048-slot Chase–Lev deque
  (~16 KB), so the present static array reserves **256 KB of BSS
  unconditionally** — including under `CAJETA_CARRIERS=1`. Allocating the
  array at first spawn removes the ceiling entirely rather than moving
  it, and cuts the common case to one carrier's worth. That is the
  preferred shape: it is the same size of change as raising the constant
  and strictly better than it.

### 5.5 Documentation drift found alongside

- **5.5.1** `docs/specification/concurrent/Concurrency.md` states fiber
  stacks are "~64 KB initial". They have been **1 MB**
  (`CAJETA_FIBER_STACK_SIZE`) since the native-call-depth fix — 64 KB was
  too little for a fiber driving a real native library. The doc is
  corrected as part of this work.
- **5.5.2** The same file repeats the `[1, CAJETA_MAX_CARRIERS=16]` clamp
  in three places; all of them follow whatever §5.1 lands.

## 6. Not moving the numbers

- **6.1** When a threaded mat-vec finishes, its output is **bit-identical**
  to the serial one. Row independence makes this achievable exactly — no
  reassociation, no reduction order to preserve, so this is a hard
  equality and not a tolerance.
- **6.2** When the engine decodes a prompt threaded, it produces the same
  tokens as serial, which is the end-to-end form of 6.1.
- **6.3** Every existing K-quant fixture continues to pass unchanged.

## 7. Acceptance

- **7.1** The Q4_K mat-vec at 4096x4096 scales measurably with carrier
  count, reported as a table across 1/2/4/8/16/nproc carriers rather than
  a single number, so the scaling curve is visible and its knee is where
  the next question starts.
- **7.2** End-to-end decode improves against the 7.83 s/token last
  measured (which predates today's kernel work, so the serial baseline is
  re-measured in the same session rather than carried over).
- **7.3** Bit-identical output per §6, gated by test.
- **7.4** The §3.4 cost is measured and reported as its own number,
  separately from the threading gain, so a reader can see what going
  multithreaded cost and what parallelism bought. A net win that hides a
  large regression inside a larger gain is not an acceptable result.
- **7.5** Every timing is taken on a verified-idle machine per
  `quant-scalar-decode-cost` §2, with an untouched control measured in
  the same interleaved run. Today's Q6_K result — where the changed
  kernel and the untouched one moved the same 2% — is why the control is
  mandatory and not decorative.

## 8. Open questions

- **8.1** DECIDED 2026-08-22 — spike §3.4 FIRST. Before any threading is
  built, force the multithreaded flag on in an otherwise-serial engine
  run and measure end-to-end decode against the unflipped baseline. If
  the global allocation mutex costs more than threading can win, this
  spec's plan pivots to sharding the live-set and the threading work
  waits behind it. Cost of finding out: about an hour. Cost of not:
  building the whole thing against a moving floor.
- **8.2** What `CAJETA_DEFAULT_CARRIERS_CAP = 4` was protecting is not
  recorded beyond "empirically picked ... gives real parallelism for the
  typical channel/fan-out workloads in tests". Raising the MAX is decided
  (§3.2); whether the DEFAULT should also move is not, and should be
  answered by what the spike and §7.1's curve show rather than now.
- **8.3** Whether `Linear.matvecInto` is the right seam or whether the
  split belongs one level up, at the projection group (Q/K/V computed
  together), where there is more independent work per dispatch. Deferred
  until §7.1's curve shows whether dispatch overhead is material.
