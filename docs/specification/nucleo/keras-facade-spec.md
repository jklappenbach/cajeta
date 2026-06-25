# Keras Façade — Specification

> Status: draft for review (2026-06-23). A **Layer-2 façade** — the high-level
> `Model`/`Layer`/`compile`/`fit`/`evaluate`/`predict`/callbacks/metrics **contract**
> (`dev.cajeta.keras`) over the **one núcleo core**. The *high-level front door* for the
> familiarity-seeking, torch-shaped world. Companion design:
> `python-stack-analysis.md` §3.3, `target-experience.md` §2, `language-foundations.md` §3.
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions (the
> *how*) are deferred as `> **TBD (plan-time):**` markers and collected in §8, to be resolved
> when this spec is turned into a plan. Outline-numbered for addressability.
>
> **Scope boundary:** this spec is the **high-level contract skin** — `Model`/`Layer`,
> `compile`/`fit`/`evaluate`/`predict`, callbacks, metrics. **The engine it skins over is NOT
> re-specified here:** modules/parameters and optimizers are `nucleo-nn-optim-spec.md`; the
> autodiff that drives `fit`'s training step is `transform-intrinsics-spec.md` +
> `nucleo-autograd-spec.md`; the tensor substrate is stdlib `cajeta.math.Tensor` /
> `nucleo-column-spec.md`; typed return bags (metrics/history) use `records-spec.md`. Keras
> here is, by design, a **high-level contract** (Keras 3 owns no tensor or autograd —
> analysis §3.3), so it sits *over* núcleo's nn/optim + autograd core directly — there is one
> core, not a choice of backends. (A future native framework, **caramelo**, is deferred and
> would be a *separate* front over núcleo, not a keras backend.)

## 1. Definition

### 1.1 Purpose
`dev.cajeta.keras` exists to be the **high-level front door** for the familiarity-seeking,
torch-shaped world (`python-stack-analysis.md` §1.4, §3.3). Keras 3 is *explicitly* a
high-level contract (`Model`/`Layer`/`fit`/`compile` over TF/JAX/PyTorch — it owns no tensor or
autograd), so porting it as a **high-level contract over the núcleo core** is the cheapest
high-value win once the core exists (analysis §3.3 "strategic value"). The developer gets a
familiar `build → compile → fit → evaluate` workflow; underneath, it resolves to the **one
núcleo nn/optim + autograd core** — the same core the torch façade skins. As with all façades:
**recognizable, not faithful** — where Keras encodes a genuine mistake, this contract does not
inherit it (§3).

### 1.2 Scope
- A high-level **`Model`** with the recognizable `compile` / `fit` / `evaluate` / `predict`
  lifecycle, preserving Keras's argument names and order (named arguments carry
  `optimizer:`/`loss:`/`metrics:`/`epochs:`/`batchSize:`/`validationData:`).
- A **`Layer`** abstraction and the common built-in layers (`Dense`, `Conv2D`, activations,
  normalization, dropout), re-presented over `nucleo-nn-optim-spec.md`.
- **Callbacks** (`EarlyStopping`, `ModelCheckpoint`, LR schedulers, a logging callback) and
  **metrics** (accuracy, loss aggregation, etc.) under recognizable Keras names.
- **One coherent model API** that collapses Keras's Sequential / Functional / subclassing churn
  (§3.1).
- **Explicit, early shape inference** — shapes resolved up front via the type system, not via a
  lazy `build()` on first call (§3.2).
- **No global configuration state** — precision/policy is per-model and explicit, not a
  process-wide setting (§3.3).

### 1.3 Non-goals
- **Re-specifying engine internals.** Modules/parameters/optimizers (`nucleo-nn-optim-spec.md`),
  autodiff (`transform-intrinsics-spec.md` + `nucleo-autograd-spec.md`), and the tensor/column
  substrate are owned by their own specs. This spec defines only the *high-level contract skin*.
- **The Keras v1/v2/v3 API lineage / churn.** We port **one** coherent surface (§3.1), not the
  historical accretion of three model-construction styles and two backend eras.
- **Keras's global backend configuration.** No `keras.backend.set_*`, no env-var backend switch,
  no global float-policy state (§3.3) — there is one núcleo core; precision/policy is per-model.
- **Bug-for-bug Keras fidelity.** Recognizable, not faithful (§1.1); the dropped mistakes (§3)
  are intentional.
- **The complete Keras layer/callback/metric tail.** v1 captures the load-bearing high-level
  training surface; the tail is additive backlog over the same núcleo core.

### 1.4 Relationship to existing constructs
- **Sits over `nucleo-nn-optim-spec.md`** — `Layer`/`Model` wrap the núcleo module/parameter
  core; `compile`'s `optimizer:` resolves to a núcleo optimizer. This façade adds the
  *high-level lifecycle*, not the module engine.
- **`fit`'s training step rides the `Grad` combinator** (`transform-intrinsics-spec.md` §5) over
  the VJP registry (`nucleo-autograd-spec.md`) — the compiled, fused, explicit-grads path the
  torch façade also uses (`torch-facade-spec.md` §3.2). Keras adds no autodiff.
- **One core, not a backend choice** — because Keras 3 owns no tensor/autograd (analysis §3.3),
  the `Model` contract resolves straight onto núcleo's nn/optim + autograd core — the same core
  the torch façade skins. keras and torch are two *fronts* over one engine, not two backends.
- **Named arguments + defaults, operator overloading, monomorphized templates** (shipped Layer-0
  enablers, `language-foundations.md` §3) keep `compile`/`fit` signatures recognizable while
  gaining types. **No global functions** — `keras.Model`, `layers.Dense`, etc. are static
  members on importable carriers, not free functions (a mechanical re-presentation, like the
  torch façade — `torch-facade-spec.md` §1.4 [T1]).
- **A future native framework is not a backend** — `caramelo` (deferred) would, like keras and
  torch, sit directly over núcleo as a *separate* front, so `dev.cajeta.keras` bakes in
  no assumption that it skins a particular framework.

## 2. Build → compile → fit → evaluate → predict (the lifecycle)

The promise: the familiar Keras workflow, over the one núcleo core, typed.

```cajeta
import dev.cajeta.keras;
import dev.cajeta.keras.layers;

var model = keras.Model([                                  // ONE model API (§3.1)
    layers.Dense(256, activation: "relu", inputShape: [784]),  // shape explicit/early (§3.2)
    layers.Dense(10),
]);

model.compile(
    optimizer: "adamw",                                    // familiar arg names + order
    loss:      "sparse_categorical_crossentropy",
    metrics:   ["accuracy"],
);

var history = model.fit(xTrain, yTrain, epochs: 10, batchSize: 64,
                        validationData: (xVal, yVal));     // fit drives Grad-based training
var score   = model.evaluate(xTest, yTest);               // typed metric record
var preds   = model.predict(xNew);
```

**Use cases**
- **2.1** As a Keras developer, when I build a model, compile it with an optimizer/loss/metrics,
  and call `fit(x, y, epochs:, batchSize:, validationData:)`, then the workflow and argument
  names/order read like Keras, and `fit` returns a **typed history** (per-epoch loss/metrics, a
  record per `records-spec.md` — not an untyped dict).
- **2.2** As a Keras developer, when I call `evaluate(x, y)`, then I get the model's loss + the
  compiled metrics as a **typed record** (named fields, a typo is a compile error — not a
  positional list or string-keyed dict).
- **2.3** As a Keras developer, when I call `predict(x)`, then I get model outputs as tensors,
  under the familiar Keras shape.
- **2.4** As a Keras developer, when I `compile` with string-named optimizers/losses/metrics
  (`"adamw"`, `"sparse_categorical_crossentropy"`, `"accuracy"`), then those familiar string
  names resolve to the núcleo optimizer / loss / metric — *and* a typed form
  (`optimizer: optim.AdamW(...)`) is accepted equivalently (better-by-opt-in: the string is
  familiar, the typed form is checked).
- **2.5** As a Keras developer, when `fit` runs a training step, then it uses the **compiled,
  fused, explicit-grads** path (`Grad` over the VJP registry — §1.4), not a hand-written
  backward — so the high-level loop inherits the engine's correctness/perf, invisibly.

> **TBD (plan-time):** [K1] The exact `fit`/`evaluate`/`predict` data-input surface — raw
> `Tensor` arguments vs. a `Dataset`/`DataLoader` (`torch-facade-spec.md` §7) vs. both; and how
> validation/batching/shuffling arguments map onto the fiber-backed loader. Lean: accept both
> raw tensors and the fiber-backed loader, batching via the loader.

## 3. The deliberate corrections (recognizable, not faithful)

The "drop the mistakes" list from `python-stack-analysis.md` §3.3, applied via the three-tier
rule (analysis §2.5).

### 3.1 One coherent model API (collapse Sequential / Functional / subclassing)
**Use cases**
- **3.1.1** As a developer, when I build a model, then there is **one** model-construction API —
  the three-way Sequential / Functional / subclassing split (and the v1/v2/v3 churn) is
  **collapsed to a single coherent surface** (analysis §3.3 "collapse to one"). A linear stack,
  a branched graph, and a custom forward are the *same* `Model`, not three classes with
  different rules.
- **3.1.2** As a developer, when I build a simple linear stack, then I pass an ordered list of
  layers (the Sequential ergonomics) — the easy case stays easy, without being a *separate
  class* from the general case.
- **3.1.3** As a developer, when I build a branched / multi-input / multi-output graph, then the
  **same `Model` API** expresses it (the Functional-graph power) — by composing layers, no
  separate `Model(inputs, outputs)` ceremony with different semantics from the stack form.
- **3.1.4** As a developer, when I need a fully custom forward, then I express it within the same
  `Model` API (the subclassing power) — custom logic is a *capability* of the one API, not a
  *third* construction style with its own lifecycle quirks.

> **TBD (plan-time):** [K2] The exact shape of the one collapsed API — a `Model` that accepts
> *either* a layer list (stack), *or* a composed layer graph (functional), *or* an overridable
> forward (subclass-equivalent), unified so the three are *modes of one type*, not three types.
> The unifying mechanism (a single `Model` with a layer-graph value + optional custom-forward
> hook) is the key design decision. Couples to `nucleo-nn-optim-spec.md`'s module composition.

### 3.2 Explicit, early shape inference (no lazy build() surprise)
**Use cases**
- **3.2.1** As a developer, when I declare a model's input shape (`inputShape:` on the first
  layer, or a typed input), then **shapes are inferred early via the type system** — at
  construction / compile time — **not** lazily on the first `fit`/`call` via a hidden `build()`
  (analysis §3.3 "make shapes explicit/early via the type system").
- **3.2.2** As a developer, when a layer's declared input shape is incompatible with the previous
  layer's output, then I get a **compile/construction-time error** naming the mismatch — not a
  deferred runtime shape error surfacing on the first batch (the moat: shape errors are compile
  errors, `target-experience.md` §2; `transform-intrinsics-spec.md` §10).
- **3.2.3** As a developer, when I inspect a model before training (`model.summary()` /
  per-layer output shapes), then the shapes are **known and reported** because they were
  inferred eagerly — there is no "unbuilt" state where shapes are `None` until first use.
- **3.2.4** As a developer, when an input dimension is genuinely dynamic (e.g. batch size), then
  it is modeled as a dynamic dim and *not* forced to compile-time-fixed — early inference resolves
  the *static* structure and defers only the genuinely-dynamic axis (no false error on dynamic
  batch — mirrors `torch-facade-spec.md` §3.4.2).

### 3.3 No global configuration state
**Use cases**
- **3.3.1** As a developer, when I set precision/policy on a model, then it is an
  **explicit/typed** choice — there is **no** `keras.backend.set_backend(...)`, no backend env
  var, no global float policy (analysis §3.3 "global backend state" dropped; consistent with the
  torch façade's no-global-state correction, `torch-facade-spec.md` §3.1).
- **3.3.2** As a developer, when two models or two threads/fibers run, then each carries its own
  precision/policy choice in its **type/configuration**, not a shared mutable global — no
  cross-model interference from a process-wide setting.

## 4. Layers — recognizable, over núcleo

`Layer` and the built-in layers are the Keras skin over `nucleo-nn-optim-spec.md`'s
module/parameter core (the same core the torch façade's `nn.*` skins — both are thin fronts over
one engine).

**Use cases**
- **4.1** As a developer, when I use a built-in layer (`layers.Dense`, `layers.Conv2D`,
  `layers.ReLU`, `layers.BatchNormalization`, `layers.Dropout`), then it carries Keras's name,
  constructor argument names, and defaults, re-presenting the núcleo layer.
- **4.2** As a developer, when I write a custom `Layer` (a `call`-shaped forward + declared
  trainable weights), then it composes into a `Model` like a built-in — over the núcleo
  parameter set, with the same early-shape-inference contract (§3.2).
- **4.3** As a developer, when a layer holds trainable weights, then they are núcleo
  `Parameter`s enumerated for the optimizer and for serialization (§5) — the same parameter model
  the torch façade exposes (`torch-facade-spec.md` §4), so the two fronts agree on the engine.

> **TBD (plan-time):** [K4] How custom `Layer` weights are declared/registered for enumeration
> and early shape inference — reflection over typed weight fields (records/`Class<T>`) vs. an
> explicit `addWeight` call; resolved jointly with `nucleo-nn-optim-spec.md` and the torch
> façade's parameter registration (`torch-facade-spec.md` §4 [T5]) so both fronts share one model.

## 5. Callbacks and metrics

Callbacks and metrics carry recognizable Keras names over the núcleo training loop.

**Use cases**
- **5.1** As a developer, when I pass callbacks to `fit` (`callbacks: [EarlyStopping(...),
  ModelCheckpoint(...)]`), then they fire at the recognizable lifecycle points (epoch/batch
  begin/end) and observe/affect training (early stop, checkpoint, LR schedule) under Keras's
  names and argument shapes.
- **5.2** As a developer, when I register a `ModelCheckpoint`, then it serializes model weights
  via the núcleo/torch serialization path (`torch-facade-spec.md` §9 `state_dict`/`.pt`) — the
  two fronts share one weight format.
- **5.3** As a developer, when I specify `metrics: ["accuracy", ...]` in `compile`, then those
  metrics are computed during `fit`/`evaluate` and reported in the **typed history/score record**
  (§2.1, §2.2), under recognizable Keras metric names.
- **5.4** As a developer, when I write a **custom callback**, then it implements the lifecycle-hook
  contract (epoch/batch hooks) and composes with built-ins — without a global registry or hidden
  state.

> **TBD (plan-time):** [K5] The callback lifecycle-hook surface and what state it may
> read/mutate — read-only training telemetry + a bounded set of controls (stop, LR, checkpoint)
> vs. broader access. Lean: a typed hook interface exposing telemetry + bounded controls (no
> global mutable training state), consistent with §3.3.

## 6. Acceptance criteria (spec-level)
- A model can be **built, compiled, fit, evaluated, and predicted** under recognizable Keras
  names/argument order; `fit` returns a **typed history** and `evaluate` a **typed score record**
  (`records-spec.md`).
- There is **one coherent model API** — Sequential/Functional/subclassing collapsed to a single
  `Model` (the easy stack stays easy; branched graphs and custom forwards are the same type — §3.1).
- **Shape inference is explicit/early** (type-system, at construction/compile) — an incompatible
  layer shape is a **compile/construction error**, not a lazy `build()` surprise; genuinely
  dynamic dims are deferred without false error (§3.2).
- There is **no global configuration state** — precision/policy is explicit/typed, per-model (§3.3).
- `Model`/`compile`/`fit` resolve straight onto the **one núcleo nn/optim + autograd core** — the
  same core the torch façade skins; there is no backend choice to make.
- `Layer`/`Model` are **thin skins** over `nucleo-nn-optim-spec.md`; `fit`'s training step rides
  the `Grad`/VJP path (`transform-intrinsics-spec.md` + `nucleo-autograd-spec.md`) — **no engine
  re-specified here**.
- Callbacks fire at recognizable lifecycle points; metrics report into the typed records;
  checkpointing reuses the shared weight-serialization path (`torch-facade-spec.md` §9).

## 7. Open questions (resolve at plan time)
- **[K1]** `fit`/`evaluate`/`predict` data-input surface — raw tensors vs. the fiber-backed
  `Dataset`/`DataLoader` vs. both, and the batching/validation mapping (§2).
- **[K2]** The exact shape of the one collapsed model API — how stack / functional-graph /
  custom-forward become *modes of one `Model`*, not three types (§3.1).
- **[K4]** Custom `Layer` weight declaration/registration — reflection over typed fields vs.
  explicit `addWeight`; shared with the torch façade's parameter model (§4; `torch-facade-spec.md`
  §4 [T5]).
- **[K5]** The callback lifecycle-hook surface and the bounded state it may read/mutate (§5).
- The v1 layer/callback/metric cut — which load-bearing set ships first vs. defers as additive
  backlog (§1.3).
