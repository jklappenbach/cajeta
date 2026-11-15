# Kernelized SPELA — spec

Status: **draft** 2026-11-14 (Phoenix, with Julian). Plan:
[`agents/spela-kernels-plan.md`](../agents/spela-kernels-plan.md).
Layer: `dev.cajeta.ml.kernels` and `dev.cajeta.ml.train`, with one runtime
change in `cajeta.xpu`. Research and decisions:
[`spela-kernels-research-findings.md`](spela-kernels-research-findings.md).

## 1. Definition

SPELA trains a network one layer at a time. Each layer computes a local loss
against fixed class vectors, updates itself, and hands its output to the next
layer detached, so no gradient crosses a layer boundary. A step is therefore
an inference pass with one weight-gradient product and one optimizer update
per layer.

Kernelized SPELA runs that step on the XPU substrate's own terms (CajetaXPU
§6): each layer's step is a declared route whose kernels choose their shapes
for the device they run on, with no host synchronization inside the step. On
CUDA the step then replays as one graph; on every backend it launches a
handful of kernels per layer where it launched dozens.

### 1.1 Problem statement

SPELA runs on the device today by composing tape ops (cajeta-ml-gpu 28.2.9).
It is correct on every backend and 2.3 to 6 times faster than torch on a
4090, but it does not adapt to a device it has not been measured on:

- The routes beneath it are constants measured on two parts: `Gemm.large`'s
  32 tiles, `splitsFor`'s 128 multiprocessors, the fused small Linear's
  threshold. A new device gets those numbers, not its own.
- Nothing in the step is a family, and nothing is timed.
- The step is host-bound. On the bench row (784-512-10, batch 64) it makes 51
  launches and about 107 us of device work in a 0.6 to 1.4 ms step.
- The global-norm clip reads one sum back per gradient tensor, so the step
  synchronizes with the host when the clip is on.

### 1.2 Scope

The first cut covers layers that are a Linear or a Linear followed by ReLU,
both losses (cosface with its margin, and cosine), AdamW and SGD, the
L2-SP anchor term, the global-norm clip, input normalization, and the online
surface, at f32. It serves two class regimes: a classifier's few classes, and
the thousands of codes of a per-level NCM head (8,192, or 40,960 with SPELA's
5x head trick), whose rows are sequences times positions. The conv stack is
the second cut.

### 1.3 Non-goals

- No silent fallback. A layer type with no kernel route is served by the
  tape-composed step only through a declared, lower-priority route, and the
  audit names it.
- No change to SPELA's arithmetic. The kernel step matches the
  tape-composed step within each kernel's stated tolerance.
- No epilogue fusion into the layer products without a measured win. Two
  such fusions cost more than they saved on the 4090 (28.2.8).
- No conv in the first cut.
- No bf16 in the first cut. Training at bf16, with f32 accumulation, master
  weights and optimizer state, is a later declared route once the large
  f16/bf16 tile families (cajeta-ml-gpu 19.2.2) are measured.
- The NCM experiment itself (adapting the heads of a frozen generator) is not
  part of this work; it needs the dev.cajeta.ncm engine. This work makes the
  heads trainable at that size.

## 2. Features

### 2.1 A step with no host synchronization

Every value the step needs stays on the device: the clip's norm and scale,
the per-layer loss slots, the detached handoff. Changing scalars (the
learning rate, the optimizer's bias corrections) travel as kernel arguments,
never as uploads. The host reads one loss row a batch.

Use cases:
1. A training loop with the clip on runs a batch with no stream sync and no
   transfer until the loss row is read.
2. On CUDA, the second batch of one shape builds a graph and every later
   batch replays it.
3. A changed learning rate takes effect on the next replay without a rebuild.

### 2.2 One optimizer launch per layer

A layer's parameters, their moments and their gradients live in one flat
arena per layer, so AdamW or SGD updates the whole layer in one launch, with
the anchor term and the clip's scale applied in the same pass. (Per-tensor
buffer arrays would also give one launch, but a launch that takes a buffer
array is never deferred, so it would break the graph.)

Use cases:
1. A two-Linear layer stack takes two optimizer launches a step.
2. An anchored layer adds `lambda (W - W0)^2`'s gradient in the update, with
   no launch of its own.
3. A frozen layer launches nothing.

### 2.3 The loss head, in two class regimes

The loss head takes a layer's output and produces its loss slot and the
output's gradient: the row norm, the product with the class vectors, the
cosface scale and margin or the cosine term, the mean cross-entropy, and its
gradient. A row mask excludes rows from the loss and the gradient. It is two
routes:

- **Few classes** (a classifier, tens of classes): one fused kernel, the
  class vectors held in shared memory.
- **Many classes** (an NCM head, thousands to tens of thousands of codes):
  the class product is a real GEMM through a family, and the cross-entropy is
  computed in column blocks with a running maximum and sum, so the full
  rows-by-classes logits are never stored. The gradient is formed block by
  block from the saved row statistics.

The class count decides the route at bind, by the row's shape gate, not by a
constant restated in the trainer.

Use cases:
1. cosface with margin 0.35 and scale 30, and cosine, each match the
   tape-composed loss and gradient on the bench shapes and the test shapes.
2. A masked row contributes nothing to the loss, the gradient or the mean's
   divisor.
3. An NCM-sized head (8,192 classes, 4,096 rows of width 512) trains with
   device memory for one column block of logits, not the whole matrix, and
   matches the tape-composed head at a size the tape can hold.
4. A head at 40,960 classes binds the many-class route with no code change.

### 2.4 The layer products choose their shapes

A layer's forward product and its weight-gradient product go through tile
families, so each device times its own candidates once, keeps the answer in
the tune store, and reuses it. The step prewarms its families when a layer is
sized.

Use cases:
1. On a device with no store, the untimed defaults are deterministic.
2. Under a store, the first batch times the members and later processes read
   the answer.
3. A member that spills or does not fit is pruned with its rule.

### 2.5 Declared routes

Training steps get regimes of their own in `cajeta.xpu.Regime`. Each layer
type SPELA serves is a RouteTable row: the kernel step for Linear and for
Linear+ReLU, and the tape-composed step as the declared lower-priority row
for any other module. The trainer picks a row per layer when it sizes the
layer, and the audit names every layer type a kernel row does not serve.

Use cases:
1. A Linear layer binds the kernel row; a conv layer binds the tape row, and
   the audit reports it.
2. A refusal names the row that came furthest and its clause.

### 2.6 The online surface

Structure stays on the host and data moves to the device, as training
frameworks and LLM servers split it: freezing, update cadence and which
layers step are host decisions that choose the kernel sequence; confidence
gating and pseudo-labels run on the device and reach the step as its row
mask; observe and flush stage a batch with one upload.

Use cases:
1. Self-distillation over a stream accepts and rejects examples with no
   readback per example.
2. Freezing the backbone removes those layers' launches from the step.
3. A flush of 100 buffered rows stages them in one upload.

### 2.7 The tune store

SpelaConfig takes a tune directory; when it is unset the library reads
`CAJETA_ML_TUNE_DIR`; with neither, there is no store and the defaults are
untimed.

## 3. Verification

The oracle is today's tape-composed SpelaTrainer: loss curves and final
weights on every backend, within each kernel's tolerance. The torch bench
twin stays the speed reference. Every new kernel is value-checked in the
kernels census on every backend. The Python reference is not on disk and is
not used.

## 4. Measurement

The spela-train-step bench row (784-512-10, batch 64) measures the few-class
regime. A second row, spela-head-step, measures the many-class regime: one
Linear head of 8,192 classes over 4,096 rows of width 512 from a fixed random
input, with a torch twin of the same local loss. Both are read against
torch, interleaved, by medians.

## 5. The second cut: conv

Conv layers (Conv2d, ReLU, MaxPool2d blocks) need families of their own:
today's conv kernels are fixed tiles with host-constant routing. The cut
starts with a SPELA conv bench row and its torch twin, so the conv step is
measured before it is designed.
