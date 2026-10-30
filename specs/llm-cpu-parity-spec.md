# cajeta-llm on the cpu backend at llama.cpp's speed

Status: **approved** 2026-10-28 (Julian). Origin: cajeta-llm plan 15.2.14.3 and 15.2.14.4.
Direction chosen by Julian 2026-10-28: threaded host int8 kernels on q8_K
activations, as llama.cpp's CPU backend does.

## 1. Definition

### 1.1 Problem

cajeta-llm must match llama.cpp on every backend it ships (15.1.5). On the cpu
backend it does not. 8B Q4_K_M on proton (Ryzen AI MAX+ 395, 16 cores), llm
96ca2a7, same hour as llama.cpp 5306f4b with `-dev none`:

| shape | ours | llama.cpp | ratio |
|---|---|---|---|
| pp512 | 22.1 tok/s | 221.1 | 0.10 |
| tg128 | 3.85 tok/s | 22.82 | 0.17 |

A decode step is almost all mat-vecs: a token reads about 4.4 GB of weights.
Per core, the cpu backend's wave mat-vec moves about 0.6 GB/s, our
single-threaded host f32 path 2.4, and llama.cpp about 6.5. llama.cpp
quantizes each activation row to q8_K once and runs int8 dot products against
the packed weight, with rows split across a persistent thread pool.

### 1.2 What exists

- Host mat-vecs in `Quant` for every format, f32 activations, one thread.
  Q2_K to Q6_K are 16-lane sub-block forms.
- `Quant.q4kMatVecIntoQ8`: a host Q4_K kernel on q8_K activations using
  `dotAccum`, never routed. Its last timing predates `dotAccum` reaching
  `vpdpbusd`.
- The q8_K activation pack (`q8kPack`) the GPU wave routes already use.
- The cpu XPU backend's native worker pool (`cajeta_xpu_dispatch.c`), which
  runs a kernel's blocks across threads.
- `spawn` and carriers in `cajeta.concurrent`.

### 1.3 Scope

The mat-vec and batched mat-mul work of a cpu-backend decode step and prefill
chunk:

- int8 host kernels per format on q8_K activations (§2);
- the same kernels over several rows at once, for prefill and batched decode
  (§3);
- splitting the work across threads (§4);
- which path the cpu backend takes (§5);
- accuracy against our f32 host path (§6);
- the bar (§7).

### 1.4 Non-goals

- GPU backends. Their routes do not change.
- Attention, norms and the other non-mat-vec ops, unless §7's measurement shows
  they keep the bar out of reach.
- A faster cpu-backend lowering in the compiler (not chosen 2026-10-28).
- Formats outside the 8B quant matrix (Q2_K, Q3_K_M, Q4_K_M, Q5_K_M, Q6_K,
  Q8_0) and the tensor types those files carry.
- Windows and other CPUs. proton's Zen 5 is the measured target. Other x86-64
  CPUs must still run correctly.

## 2. Int8 decode mat-vec

- **2.1** When a cpu-backend decode step runs a projection whose format has an
  int8 host kernel, the activation row is packed to q8_K once and every
  projection reading it uses the pack.
- **2.2** When a Q4_K or Q6_K projection runs, its int8 kernel moves at least
  5.5 GB/s of weight per core on proton.
- **2.3** When the int8 kernel is built for x86-64 with VNNI, the inner dot
  product is a `vpdpbusd`. Where VNNI is absent it falls back to a slower form
  and stays correct.
- **2.4** When a format in the 8B matrix has no int8 kernel yet, the existing
  f32 host path runs, and the route record names it.
- **2.5** When a projection accumulates into the residual, the kernel adds in
  place, as the GPU wave routes do.

## 3. Batched rows

- **3.1** When a prefill chunk or a batched decode step has several activation
  rows, each weight block is read once for all of them.
- **3.2** When pp512 runs on the 8B Q4_K_M, the batched kernels reach at least
  200 tok/s on proton.
- **3.3** When the row count is not a multiple of the kernel's row tile, the
  remaining rows are computed without reading past the activation buffer.

## 4. Threads

- **4.1** When a mat-vec runs, its output rows are split across a pool of
  threads that persists across calls. No thread is created per call.
- **4.2** When the pool hands out one mat-vec, the overhead per call is at most
  10 µs on proton, so the ~225 mat-vecs of a decode step cost under 2.5 ms.
- **4.3** When the thread count is not set, it is the number of physical
  cores (16 on proton), and pool workers spin before they sleep. It can be
  set at engine start, above the physical count too. Changed 2026-10-29
  (Julian, "Verify, then flip") from every hardware thread: end to end, 16
  spinning workers decode 8B Q4_K_M at 19.5 tok/s against 15.6 for 32
  sleeping ones, and spin with every hardware thread in the pool stalls woken
  workers (plan 3.4.1).
- **4.4** When the cpu XPU backend's own kernels run in the same step, the two
  pools do not oversubscribe the cores.
- **4.5** When one mat-vec's output is split, the result is the same bit for
  bit at every thread count.

## 5. Selection

- **5.1** When the engine runs on the cpu backend, decode and prefill
  mat-vecs take the int8 host kernels for every format that has one.
- **5.2** When the engine runs on a GPU backend, nothing in this spec is used.
- **5.3** When the int8 path is switched off at engine start, the cpu backend
  takes its current routes, for A/B measurement.

## 6. Accuracy

- **6.1** When the int8 path runs, its logits against our f32 host path have a
  mean KL divergence no worse than llama.cpp's CPU against its own f32
  reference, by the KL census (15.2.15.3.6.1's instrument) on the 8B Q4_K_M.
- **6.2** When each int8 kernel is tested, it matches a host reference that
  quantizes the activations the same way, within the rounding of its sum order.

## 7. The bar

- **7.1** When the 8B Q4_K_M is measured on proton in a quiet window against
  llama.cpp's CPU at the same hour, tg128 and pp512 are each at least 1.0 of
  llama.cpp's.
- **7.2** When the other quants of the 8B matrix are measured the same way,
  each is at least 0.9 of llama.cpp, and any one below 1.0 is filed as a gap.
- **7.3** When the 4-seq batched decode is measured, it is at least 1.0 of
  llama-batched-bench's CPU figure.
- **7.4** When peak host memory is measured at ctx 4096, it is at most 1.25 of
  llama.cpp's CPU peak.

## 8. Decisions

Settled with the approval, 2026-10-28:

- **Q4** (2026-10-28, from Unit 2's measurement) The default thread count is
  every hardware thread (4.3). Not chosen: keep physical cores and close the
  gap in the kernels first; 32 now with 16-thread tuning filed for later.
- **Q4b** (2026-10-29, from plan 3.4.1) The default is physical cores with
  worker spin on, reversing Q4. Not chosen: spin as an llm opt-in only;
  parking it for Unit 4.

- **Q1** The pool is chosen by measuring per-call overhead against 4.2: the cpu
  XPU backend's native worker pool through a host entry, or a new pool in
  cajeta-llm on `spawn`.
- **Q2** The q8_K pack keeps its layout unless a measurement shows a host-side
  layout pays.
- **Q3** The kernels live in cajeta-llm's `Quant` for now.

The questions as filed:

- **Q1** Pool: reuse the cpu XPU backend's native worker pool through a host
  entry, or a new pool in cajeta-llm on `spawn`? The plan decides by
  measuring per-call overhead against 4.2.
- **Q2** Whether the q8_K pack's layout (320-byte blocks with bsums) suits the
  host kernels as it is, or a host-side layout pays more.
- **Q3** Whether the kernels belong in cajeta-llm's `Quant` or in a stdlib
  module other cajeta programs can use.
