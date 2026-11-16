# Kernelized SPELA: research findings (2026-11-14)

Research ahead of a spec for running SPELA's per-layer step on the XPU
substrate's adaptation machinery (CajetaXPU §6) rather than on composed tape
ops. Paths are relative to the cajeta-ml checkout unless stated. Six questions
were open; each section answers one.

## 0. Where the step stands (4090, cajeta-ml c05b872)

spela-train-step (bench MLP 784-512-10, batch 64, cosface, AdamW) runs
0.61-1.36 ms against torch's 3.16-3.81. A cupti tally shows 51 launches and
about 107 us of device work a step, so the device is busy 10-20% of the step:
the step is host-bound. Device time splits into GEMMs (6 plain 16-tiles at
4.5 us, 2 fused small Linears) ~37 us, two axis reductions ~18, copies and
transposes ~23 (mostly launch count), the loss head ~17, Adam 7.

## 1. RouteTable (cajeta runtime/src/cajeta/xpu/Route*.cajeta)

- General by design: a row is one kernel variant, serving one format, in one
  regime (CajetaXPU-Routing.md §2, §6; route-table-spec §6 names training's
  forward/backward regimes). `shapeRefusal` is pure over declared facts and is
  what the audit walks; `readyRefusal` reads mutable state; both return the
  gate's own text. `pick(call)` once at bind, priority then registration
  order; no implicit fallback (a slow path is a lower-priority row, the
  terminal arm refuses by name).
- No production user anywhere yet. SPELA would be the first registrant
  (cajeta-llm's adoption is open plan work, codebook-quants 9.2.9.x).
- `Regime` has only DecodeRow, PrefillBatch, Bind, FusedTail. Training needs
  either a new appended value (a runtime change that ships with a toolchain
  release) or the layer type and loss mode carried on a RouteQuery subclass
  and gated in `shapeRefusal`.
- Rows and tile families are separate layers: a row's `dispatch` is the
  launcher, and a family chooses the shape inside it.

## 2. Deferred streams (cajeta runtime/native/cajeta_xpu_defer.c)

- On CUDA, a recorded launch queue becomes an explicit CUDA graph: the first
  sequence goes direct, the second builds the graph, and later ones replay,
  patching nodes whose grid or argument bytes changed. Scalars (lr, Adam's
  bias corrections) and buffer addresses may change per step; only the kernel
  sequence must match.
- Every memcpy (either direction), cuMemFree, sync or event flushes all
  queues and splits the graph. One graph a step needs: no host reads or
  uploads mid-step, changing scalars as kernel arguments, `flush()` at the end.
- hip, vulkan and cpu: `setDeferred` is accepted and does nothing. Graph
  replay is a CUDA-only lever; elsewhere the host cost falls only by cutting
  launches. (Two stale comments say otherwise: KernelStream.cajeta:114-116,
  cajeta-llm QuantKernel.cajeta:69. The spec says a sequence is recorded on
  its third sighting; the code records on the second.)
- Limits: 8 deferred stream slots per process, queues under 3 launches never
  become graphs, 48 args / 384 bytes per launch, profilers turn it off.

## 3. Fused epilogues and prologues on tile families

- The plug-in mechanism exists: a template-parameter class whose `@Device
  static` the tile calls. `FusedGemm<Epi>` calls `Epi.apply(acc, row, col,
  aux0, aux1, n)` per element; `DecodedGemm<D>` generates the B operand on
  load through `D.decode(...)`. Both are fixed-shape, selftest-only, and not
  TileShape members of any family.
- The hook is per element and stateless. Nothing in the tree fuses a per-row
  reduction into a GEMM. A deterministic per-row partial needs a new hook
  (a row reduce over the staged tile, or a partials buffer and a join, the
  split-K pattern); float atomics would not be bit-repeatable.
- Measured caution (cajeta-ml 28.2.8, 2026-11-13/14): new epilogue parameters
  and stores in a hand tile cost more than they saved twice (the GELU forward
  store, the softmax-backward store slowing every launch of the batched tile).

## 4. Backend constraints for fused kernels (CajetaXPU §3.2, §3.6, §3.7; memory notes)

- cpu vectorizes a work-item loop only when the kernel calls a wave op; index
  lanes by `Group.width()`, declare `@Wave(32)` when the code assumes 32, keep
  blocks multiples of the wave, never size a declared kernel's grid from the
  host wave.
- No per-work-item accumulator across a barrier inside a non-uniform loop;
  reduce with a wave op, then a small shared step. No lane exchange before a
  tile verb (tile-only kernels are not wave kernels on cpu).
- A runtime mode switch builds every arm on every backend; a template chosen
  by a runtime `if` instantiates every arm (spirv). Large fused bodies risk
  the 600 s cpu vectorize deadline. Fused tile kernels need a cpu reference
  check (TcTile's cpu fault is unresolved).

## 5. What the step must cover (SpelaTrainer, SpelaConfig)

- A layer is one Module, possibly a Sequential: Linear, Sequential(Linear,
  ReLU), Sequential(Conv2d, ReLU, MaxPool2d) are what tests and the bench use;
  BatchNorm, Dropout, AvgPool are never used. Outputs over 2-D are flattened;
  there is no pooling of its own.
- Per layer: optional input rowNorm (2-D inputs only), forward, then COSFACE
  (rowNorm, product with scale x V^T, optional margin gather, cross-entropy) or
  COSINE (gather V[y], mean cosine), optional L2-SP anchor term, backward,
  optional global-norm clip, AdamW or SGD, the loss written to its slot, the
  flattened output copied as the next layer's detached input; one loss-row
  readback a batch.
- Host decisions that stay host: class vectors and tables built on the first
  batch, the trainable and cadence tests, the LR schedule (a scalar).
- The online surface is host-heavy: observe/flush assemble batches element by
  element with flatGet/flatSet; self-distillation runs two forwards and two
  readbacks per accepted example; freezing is a host flag.
- Defaults: AdamW, lr 1e-3, wd 1e-4, cosine decay with 5% warmup, cosface
  scale 30, margin 0, FARTHEST class vectors, inputs normalized, clip off.
- The Python reference repo (code/ml/spela-training) is not on disk; the only
  visible twin is the torch bench competitor, which uses RANDOM class vectors
  where cajeta defaults to FARTHEST.

## 6. Existing optimizer and conv kernels

- Optim.adam/sgd: one lane per element over one tensor, one launch per
  parameter, lr and bias corrections as scalars, betas and eps hard-coded. No
  multi-tensor path.
- The global-norm clip (BackpropTrainer.clipOnDevice) reads back one SUMSQ
  per gradient tensor, a host sync each, so with the clip on the step cannot
  be one graph.
- Conv is direct convolution, not im2col and GEMM: conv2dTiled (fixed 256
  lanes x 2 columns x 16 channels), convBackIn, convBackWeight3x3 (Wave32,
  36 accumulators), convBackWeightSplit+Sum (syncs when it allocates its
  partials). Routing is host constants; nothing is a family or tuned; bias is
  fused into the forward, ReLU is not.
- The tape already skips dX for a detached input, so SPELA never pays for
  input gradients.

## 7. What follows for the design

- At SPELA's sizes the lever is launch count and host syncs, not device
  traffic. On CUDA a sync-free step replays as one graph; on every other
  backend only fewer launches help.
- Order of value: (1) a sync-free step (device-side clip, device loss slots,
  no mid-step copies to the host); (2) a multi-tensor AdamW/SGD, one launch a
  layer; (3) a fused loss head (rowNorm, the small class product, margin,
  cross-entropy or cosine, and its gradient) replacing about ten launches;
  (4) the layer GEMMs and convs as families, so a new device chooses its own
  shapes. Epilogue fusion into the layer GEMM is last and only with a
  measured win, given 28.2.8's results.
- Open for the spec: Regime value vs RouteQuery subclass; whether conv joins
  in the first cut (it needs its own families and a SPELA conv bench row);
  the online surface; the tune store's home; the reference's absence.

## 8. Decisions (Julian, 2026-11-14)

- Training steps get regimes of their own, appended to `cajeta.xpu.Regime`;
  it rides the next toolchain release, so cajeta-ml's pin moves to use it.
- Linear first: cut 1 covers Linear and Linear+ReLU layers. The conv stack is
  the next cut, with families of its own and a SPELA conv bench row and torch
  twin first.
- The online surface splits structure from data, as training frameworks and
  LLM servers (and cajeta-llm's pipelined decode) do: freezing, cadence and
  which layers step stay host decisions that choose the kernel sequence;
  confidence gating and pseudo-labels run on the device as a row mask inside
  the step, with no readback; the L2-SP anchor term goes into the fused
  optimizer; observe/flush stage a batch with one upload.
- The tune store is a SpelaConfig option with an environment fallback; with
  neither set there is no store, and the untimed defaults are deterministic.
- The oracle is today's tape-composed SpelaTrainer (loss curves and weights,
  every backend); the torch bench twin stays the speed reference.
