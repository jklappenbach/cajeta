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
