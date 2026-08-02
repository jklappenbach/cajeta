# cajeta-ml v3 — the neural framework (torch-role) in one ML library, with SPELA as a first-class trainer

## 1. Definition

`dev.cajeta.ml` becomes cajeta's single ML library: the classical surface
(v1/v2) stays as-is at the root, and v3 adds the **neural framework** —
modules, tensor autograd, optimizers, two training paradigms
(**backprop** and **SPELA** forward training), checkpoints, LoRA, and
data loading. The stdlib **sheds** its framework fragment (`nucleo.nn`,
`nucleo.optim` move into the library) and keeps only foundations: math
(Tensor/linalg/stats), data (frame/column/sparse), and the existing
compiler-integrated differentiation (`nucleo.autograd` scalar tape +
`Grad` expr) — "core, foundational libraries in stdlib, keeping it
light" (decision 2026-07-31; the caramelo library name is retired).

Doctrine continuity: torch is the **oracle and API muse, never a port**
(the sklearn/xgboost discipline). Numerics pin against a pinned torch
(per-op gradient fixtures, optimizer trajectories, checkpoint bytes) and
against Julian's SPELA reference implementation.

Phasing (decision): **training-core first, GPU seam designed in from day
one** — every op routes through one dispatch layer that honors Tensor
device residency; v3 ships the CPU path complete, `cajeta.xpu` kernels
arrive in a later phase without API change. Distributed, compile-style
fusion, AMP, and quantization are named later phases (§12).

## 2. Feature: consolidation — one ML home

- 2.1 `ml.nn` absorbs stdlib `nucleo.nn` (Module, Parameter, Linear,
  Dropout, Losses, Modes) verbatim-then-evolves; `ml.optim` absorbs
  `nucleo.optim` (SGD, Adam, AdamW, LrSchedule/Schedules, Updates).
- 2.2 The stdlib packages are REMOVED in the next toolchain release
  (v0.14.0): they are young (sole consumer: the compiler tour's
  training-loop demo, which retires; tour-quality coverage note updated).
  A loud release-note entry names the new home.
- 2.3 Classical API, protocol, skills, tour, docs remain; the protocol
  (`Estimator`/`Predictor`/`Transformer`) is unchanged and neural models
  join it (§9).

## 3. Feature: `ml.grad` — tensor autograd with the device seam

A define-by-run reverse-mode engine over `Tensor<float64>`/
`Tensor<float32>` (the stdlib scalar tape is unsuited to networks and
stays untouched). Library-resident on purpose: the op set churns with
every new layer, and library iteration must not require toolchain
releases.

- 3.1 `GradTensor` (value + grad + node) recorded on a `GradTape`;
  `backward(loss)` accumulates into every reachable parameter;
  `zeroGrad`; `noGrad { … }` scope; `detach()`.
- 3.2 Op set (each op = forward kernel + backward rule, both pinned):
  matmul, add/sub/mul/div with **broadcast-aware gradient reduction**,
  relu/gelu/tanh/sigmoid/softmax/logSoftmax, exp/log/pow/sqrt,
  sum/mean/max (axis-aware), reshape/transpose/slice/concat/embedding
  -gather, conv2d, maxPool2d/avgPool2d/adaptiveAvgPool2d, batchNorm2d,
  layerNorm, dropout (train/eval aware), crossEntropy/mse/cosine losses.
- 3.3 **Device seam (day-one design):** every op body calls a single
  `Ops.dispatch` layer keyed by the input tensors' residency (`isOnGpu`).
  v3 implements the CPU kernels; the GPU column is a registry the later
  xpu phase fills in. Mixed-residency inputs are a loud `MlException`
  (no silent transfers). The seam is pinned by tests that assert the
  dispatch layer is the ONLY kernel entry (no direct kernel calls from
  modules).
- 3.4 Fixtures: per-op forward + gradient checks against pinned torch
  (`torch.autograd.grad` on deterministic inputs), plus composite checks
  (an MLP forward/backward matching torch end-to-end to documented
  tolerance).

## 4. Feature: `ml.nn` — the module zoo

- 4.1 `Module` tree with **named parameters** producing torch-compatible
  names (`encoder.layers.0.attn.wq.weight`) — the checkpoint contract.
  `parameters()`, `namedParameters()`, `train()/eval()`, `requiresGrad`
  per parameter.
- 4.2 Layers: Linear, Dropout (absorbed) + Conv2d, MaxPool2d/AvgPool2d/
  AdaptiveAvgPool2d, BatchNorm2d, LayerNorm, Embedding, Multihead
  attention block, Sequential, Flatten, activation modules (ReLU, GELU,
  Tanh, Sigmoid, Softmax).
- 4.3 Reference architectures as executable proof (and tour material):
  an MLP, a small CNN (CIFAR-class), and a small transformer encoder
  block stack — each trainable by BOTH trainers where applicable.
- 4.4 Initialization: torch-default schemes (kaiming/xavier/normal) with
  explicit seeds — deterministic by construction.

## 5. Feature: `ml.train` — two first-class trainers

### 5.1 BackpropTrainer
Loss + optimizer + schedule over `ml.grad`: epoch/step loops, gradient
clipping, seeded shuffling, metric history, checkpoint-every-N. The
optimizer suite (absorbed SGD/Adam/AdamW + schedules) is re-pinned
against torch trajectories (same data order ⇒ same weights to
documented tolerance).

### 5.2 SpelaTrainer — SPELA(O), pinned to the reference
Port of `code/ml/spela-training`'s `SpelaTrainer` (the oracle: its
PyTorch implementation + 14 tests), including BOTH recipes:

- Paper-exact mode: per-layer SGD, pure `-cosine(h, e[y])` local loss,
  L2-normalized layer inputs (Algorithm 1).
- Modernized recipe (reference defaults): per-layer AdamW + weight
  decay, linear-warmup→cosine LR, `cosface` loss (cross-entropy over
  cosines to ALL class vectors, optional additive margin).
- Class embeddings: fixed unit-sphere vectors, `farthest` (greedy
  farthest-point) and `random` methods, seeded; lazily sized per layer
  from the first batch's activation dim.
- Structure: per-layer optimizer, immediate update after each layer's
  local loss, **detached handoff** to the next layer (single forward
  sweep, no stored activations, no global backward) — the local
  per-layer gradient comes from a layer-scoped `ml.grad` tape.
- MLP and CNN-block stacks (auto-flatten; blocks reduce to `(B, D)`);
  early-exit inference (`evaluate(fromLayer)`), per-layer loss/accuracy
  metric history.

### 5.3 SPELA production surface (the api-roadmap, in scope)
- `observe(x, y)` / `flush()` streaming updates with
  `onlineBufferSize` / `onlineFlushEvery`.
- Per-layer control: `setLayerTrainable`, `freezeBackbone()`/
  `unfreezeAll()`, per-layer LR multipliers, per-layer update cadence.
- Confidence gating: `observe(x)` with self-distillation
  (`selfDistill`, `confidenceThreshold`) — pseudo-label only above the
  cosine-confidence bar.
- Anchor regularization: `anchorToInit` + `anchorLambda` (L2-SP against
  the construction-time snapshot) — the forgetting guard.

Use cases: 5.a train the reference MLP on a deterministic MNIST-class
fixture in both SPELA recipes and match the reference's loss/accuracy
trajectory; 5.b freeze-backbone-adapt-head on a drifting stream (the
online-personalization scenario); 5.c the same network trained by
backprop and by SPELA through ONE `Trainer`-shaped seam.

## 6. Feature: `ml.io` — checkpoints

- 6.1 **safetensors** reader/writer (JSON header + raw LE tensors;
  F32/F16/BF16 widen to f32 on load); round-trip bit-stable for f32.
- 6.2 **`.pt` reader**: constrained unpickler (ZIP container, pickle
  protocol subset, `persistent_load` storage protocol) sufficient for
  `torch.save(state_dict)` files; loud rejection of arbitrary pickle
  opcodes (never an executor).
- 6.3 `loadStateDict(module, dict, strict)` mapping by torch-compatible
  names; `saveStateDict` (safetensors). Fixtures: checkpoints written by
  pinned torch load bit-correctly; a fine-tune resumed from a torch
  checkpoint matches torch's trajectory.

## 7. Feature: LoRA (`ml.nn.lora`)

Low-rank adapters on Linear (and attention projections): wrap, freeze
base, train adapters (either trainer), `merge()` back. Fixture: LoRA
fine-tune on the reference MLP matches a torch+peft-style baseline to
documented tolerance.

## 8. Feature: `ml.data` — loading & batching

Seeded batching/shuffling over Tensors, `Frames`/`Table<R>` sources, and
the SPELA streaming shapes; a deterministic MNIST-class fixture
generator lives in-repo (no dataset downloads in CI).

## 9. Feature: protocol integration

`NetRegressor`/`NetClassifier` wrap (module + trainer) as `Predictor` —
neural models ride `crossValScore`, `Pipeline`, `Frames.design`
unchanged. SPELA's early-exit exposes `predictFromLayer`.

## 10. Oracles & fixtures

- Pinned torch venv (torch ≥ 2.1, exact version recorded per fixture)
  alongside the sklearn 1.9.0 venv; generators in `tools/fixtures/`.
- SPELA: fixture parity with `spela-training`'s test suite (its 14
  PyTorch tests are the behavioral contract); trajectories pinned from
  its trainer on deterministic data.
- Every backward rule individually pinned before composition (the
  autograd failure modes — broadcast reduction, in-place aliasing,
  non-differentiable points — are caught per-op, not end-to-end).

## 11. Lifecycle

- Toolchain: v0.14.0 removes `nucleo.nn`/`nucleo.optim` (breaking,
  release-noted); no other stdlib change is REQUIRED by v3.
- Library: lands as `dev.cajeta.ml` 0.4.x series (units release
  incrementally; the classical surface's semver promise holds — root
  API unchanged). Skills/docs/tour grow per unit (the v2 bar).

## 12. Non-goals (named later phases)

GPU kernels for the op registry (`cajeta.xpu` phase — the seam ships in
v3), distributed training, compile-style graph fusion, AMP/mixed
precision, quantization, sparse NN ops, dataset download tooling,
torchvision-style transforms beyond what the fixtures need.

## 13. Feature: transformer & transfer-learning completion

*Added 2026-07-31, verified 2026-08-01. The feed-forward and CNN surface is
already covered by §3–§5: forward/backprop, MSE and cross-entropy, the SGD
family, and the sigmoid/tanh/ReLU/softmax activations are all present in §3.2.
What the transformer and transfer-learning surface needs is below. No separate
spec: this is v3's scope.*

### 13.1 API doctrine — settled

The reference material for this surface is TensorFlow/Keras, but **cajeta's
neural API is torch-shaped** (decision 2026-07-31, Julian: "We do torch"). Rationale: the API
must match the oracle §10 already pins, tensor layout stays NCHW rather than
straddling NHWC, foreign-weight import (§13.6) is close to mechanical against a
torch-shaped module tree, and SPELA is already expressed in torch module and
parameter terms. `docs/specification/nucleo/keras-facade-spec.md` stays
**parked** — if it is ever built it is a thin convenience layer over this one
engine, never a second API with its own semantics.

Consequence, accepted knowingly: Keras reference notebooks do **not** port
line-by-line. Fixtures are produced by rebuilding the architecture and comparing
metrics, not by translating cells.

### 13.2 Positional encodings

- **13.2.1** When a transformer input is built, sinusoidal and learned
  positional encodings are both available and compose with `Embedding`.
- **13.2.2** When a sequence exceeds the encoding's configured maximum length,
  it fails loudly rather than silently truncating or wrapping.

### 13.3 Encoder-decoder attention

§4.2 supplies a multihead attention block and §4.3 targets an encoder only.

- **13.3.1** When a decoder layer is built, cross-attention over encoder
  outputs is available alongside self-attention.
- **13.3.2** When a causal mask is applied, position `i` attends only to
  positions `≤ i`, and the mask is verified by test rather than assumed.
- **13.3.3** When a padding mask is passed, padded positions contribute nothing
  to attention weights and produce no gradient.

### 13.4 Data augmentation (`ml.data`)

§8 is loading and batching only, and §12 defers "torchvision-style transforms
beyond what the fixtures need". Vision training is not credible without
augmentation, so a minimal transform set moves in scope.

- **13.4.1** When a transform pipeline is composed, random flip, crop,
  rotation, and normalization are available and seedable.
- **13.4.2** When augmentation is enabled, it applies on the training path only
  and is inert in eval mode — the same train/eval discipline `Dropout` already
  follows.

### 13.5 Transfer learning

Three case studies depend on it (Brain Tumor, COVID-19 chest X-ray, Food Image).

- **13.5.1** When a module subtree is freezed, its parameters report
  `requiresGrad = false`, receive no updates, and the optimizer does not
  silently carry state for them.
- **13.5.2** When for fine-tuning is unfreezed, training resumes on those
  parameters with optimizer state initialized correctly.
- **13.5.3** When a classification head is replaced, the backbone's loaded
  weights are preserved and only the new head is initialized.

### 13.6 Pretrained-weight import

§6 covers cajeta's own checkpoints. The BERT case study needs foreign weights.

- **13.6.1** When a torch `state_dict` is imported, parameters map onto the
  module tree by name, and any unmatched key on either side is reported — never
  silently dropped.
- **13.6.2** When a tensor's shape disagrees with its target parameter, the
  import fails naming both shapes.

### 13.7 Open questions

- **13.7.1** Contrastive learning is adjacent to this surface but has no consumer.
  Recommendation: out of scope for v3; revisit if a project needs it.
- **13.7.2** Graph neural networks are **not** in scope. They are a distinct
  architecture family with no consumer here; `ml-graph-analytics` covers
  classical graph analysis and does not imply GNN support.
- **13.7.3** Does foreign-weight import (§13.6) imply a safetensors/HF reader,
  or is a torch `state_dict` enough? Recommendation: `state_dict` only for v3.
