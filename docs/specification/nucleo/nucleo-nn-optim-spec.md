# Núcleo NN & Optim — Specification

> Status: draft for review (2026-06-23). The **module/parameter system + optimizers** — the
> neural-net core (`dev.cajeta.nucleo.nn`, `dev.cajeta.nucleo.optim`) shared by the torch and
> keras façades and by toffee. Layer-1b núcleo core. Companion design:
> `python-stack-analysis.md` §3.2/§3.3, `target-experience.md` §2,
> `language-foundations.md` §3.
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions (the
> *how*) are deferred as `> **TBD (plan-time):**` markers and collected in §11, to be resolved
> when this spec is turned into a plan. Outline-numbered for addressability.
>
> **Scope boundary:** this spec owns the **backend-neutral neural-net core** — the
> `Module`/`Layer` abstraction, parameter collection, the optimizer protocol, LR schedulers,
> loss functions, and train/eval mode. It consumes **explicit gradients** produced by the
> autograd engine (`nucleo-autograd-spec.md`) and the transform combinators
> (`transform-intrinsics-spec.md`); it does **not** define differentiation rules. Module state
> serialization (`state_dict`/`.pt`) is deferred to `torch-facade-spec.md`. The façades
> (`torch-facade-spec.md`, `keras-facade-spec.md`) are thin skins over this; toffee
> (SPELA-based) sits directly on it. Tensors come from stdlib `cajeta.math.Tensor`; records
> (typed return bags) from `records-spec.md`.

## 1. Definition

### 1.1 Purpose
The **nn/optim core** is the backend-neutral neural-net spine: a composable **Module** that
owns parameters and sub-modules and defines a forward pass; **parameter collection** that
enumerates a module's owned tensors so an optimizer can update them; and the **optimizer
protocol** (SGD/Adam/AdamW + LR schedulers) that consumes the **explicit gradients returned by
the autograd engine** and writes updated parameter values back. It is the one nn layer the
torch façade, keras, and toffee all stand on — the façades add recognizable names and call
shapes, never a second engine.

### 1.2 Scope
- A composable **`Module`/`Layer`** abstraction: a module holds **parameters** and
  **sub-modules** and defines a **`forward`**.
- **Parameter collection** — enumerate a module's (and its sub-modules') parameters as an
  explicit collection the optimizer operates on.
- The **optimizer protocol** — `SGD`, `Adam`, `AdamW`, operating on **explicit grads** handed
  in per step (not a global `.grad` accumulator).
- **LR schedulers** — step/exponential/cosine schedules that adjust an optimizer's learning
  rate explicitly.
- **Loss functions** — backend-neutral differentiable losses (cross-entropy, MSE, …) usable as
  the scalar a `Grad`-transformed step differentiates.
- **Train/eval mode** — selecting train- vs. inference-time layer behaviour (dropout, batchnorm
  statistics) **without** a global mutable flag.

### 1.3 Non-goals
- **Differentiation itself.** The VJP rules, the `Grad` combinator, and the eager tape live in
  `transform-intrinsics-spec.md` and `nucleo-autograd-spec.md`. This spec *consumes* the grads
  they produce; it defines no autodiff.
- **Module state serialization format** (`state_dict`, `.pt` load/save). Deferred to
  `torch-facade-spec.md` (§7 cross-ref); this spec only requires that parameters be
  **enumerable in a stable, named order** so a serializer can be built over them.
- **The façade surfaces themselves.** `torch.nn.Linear`-shaped names/signatures and keras'
  `Model.fit` live in the façade specs; this core supplies the neutral primitives they wrap.
- **A global default dtype/device or global grad toggles.** Explicitly *rejected* (§4) — the
  upstream mistake this core exists to correct.
- **The concrete layer zoo** (every conv/attention/norm variant). The *abstraction* and a
  small foundational set are in scope; the full catalogue is a plan-time backlog behind the
  abstraction.

### 1.4 Relationship to existing constructs
- Builds on stdlib **`cajeta.math.Tensor`** (numpy, done) for parameter storage and forward
  compute, and on **`cajeta.gpu`** for the device model — dtype/device are **type-level**
  (`Tensor<float32>`), not runtime-global state.
- A **`Module`** is an ordinary Cajeta class with `heap` lifetime; parameters and sub-modules
  are its **fields**. Parameter collection reuses **reflection** (`Class<T>`, field enumeration)
  to walk those fields — the same facility records lean on (`records-spec.md` §6).
- Optimizers consume the **explicit gradient bag** returned by `Grad(f)` /
  `f.withGrads(args) -> {value, grads}` (`transform-intrinsics-spec.md` §5) — a typed record
  (`records-spec.md`), not a mutated `.grad` field.
- **Named arguments + defaults** (shipped) give the optimizer/scheduler constructors their
  recognizable, Pythonic call shape (`optim.AdamW(params, lr: 3e-4)`).

> **TBD (plan-time):** [N1] Is `Module` a plain class implementing a `Module` interface, an
> abstract base class, or a `@Module`-annotated class whose parameter-collection plumbing is
> synthesized (Tier-A source synthesis, `source-synthesis-spec.md`)? Lean: an interface/base
> with reflection-driven collection; synthesis only if reflection-walk ergonomics fall short.

## 2. The Module / Layer abstraction

A module owns parameters and sub-modules as fields and defines a forward:
```cajeta
public class MLP : nn.Module {
    nn.Linear fc1 = nn.Linear(784, 256);   // sub-module field
    nn.Linear fc2 = nn.Linear(256, 10);    // sub-module field

    public Tensor<float32> forward(Tensor<float32> x) {
        return fc2(fc1(x).relu());
    }
}
```

**Use cases**
- **2.1** As a developer, when I declare a class extending `nn.Module` with `nn.Linear` fields
  and a `forward(x)`, then it is a usable module whose forward I invoke as `model(x)`
  (call-operator sugar) or `model.forward(x)`.
- **2.2** As a developer, when a module field is itself a `Module`, then it is a **sub-module**:
  the parent composes it, the parent's forward calls the child's forward, and the child's
  parameters are reachable through the parent (§3).
- **2.3** As a developer, when a module field is a `Parameter` (an owned, optimizable tensor —
  e.g. a weight matrix), then it is a **parameter** of that module, distinct from an ordinary
  non-optimizable tensor buffer (a fixed lookup table, a running statistic).
- **2.4** As a developer, when I define a custom layer (e.g. a fused attention block), then I
  extend `Module`, declare its parameters/sub-modules as fields, and implement `forward` — the
  same abstraction at every level (there is **one** model API, not Sequential/Functional/
  subclassing three ways — the keras collapse, `python-stack-analysis.md` §3.3).
- **2.5** As a developer, when I write a forward that uses tensor ops (`@`, `.relu()`, fused
  expressions), then those are ordinary `cajeta.math` operations over `Tensor<T>` — the module
  adds no compute; it only **owns parameters and composes** other modules' forwards.
- **2.6** As a developer, when a sub-module's `forward` has a different tensor dtype/shape than
  its caller expects, then it is a **compile error** (dtype/device/shape are type-level), not a
  runtime mismatch.

> **TBD (plan-time):** [N2] Whether `Layer` and `Module` are distinct types (keras separates
> `Layer` from `Model`) or one abstraction with `Layer` as an alias/role. Lean: one `Module`
> abstraction; `Layer` is a façade-level alias where recognizability wants it.

## 3. Parameter ownership and collection

The optimizer needs an explicit, enumerable collection of the parameters it updates. A module
**owns** its parameters; collection walks the module tree.

**Use cases**
- **3.1** As a developer, when I call `model.parameters()`, then I get an explicit collection of
  every `Parameter` owned by the module **and** by all of its (transitively reachable)
  sub-modules, in a **stable order** (so optimizer state and serialization line up run-to-run).
- **3.2** As an optimizer author, when I am constructed with `model.parameters()`, then I hold a
  reference to that parameter set and update its tensors in place on `step` — the parameters are
  **owned by the module**, **borrowed by the optimizer** (the optimizer never takes ownership;
  the module's lifetime governs the tensors).
- **3.3** As a developer, when I want only a subset of parameters (freeze a backbone, train a
  head), then I can collect parameters from a chosen sub-module (`model.head.parameters()`) and
  construct an optimizer over just that subset — selection is **explicit collection**, not a
  per-tensor mutable `requires_grad` toggle.
- **3.4** As a developer, when I enumerate parameters with names (`model.namedParameters()`),
  then each parameter carries a **stable dotted path** (`fc1.weight`, `fc2.bias`) — the basis a
  `state_dict` serializer keys on (serialization itself deferred to `torch-facade-spec.md`).
- **3.5** As a núcleo author, when parameter collection walks the module's fields, then it uses
  **reflection** over the module's typed fields to find `Parameter` and sub-`Module` fields — no
  hand-maintained registration list, and a field added to the module is collected automatically.
- **3.6** As a developer, when a tensor field is **not** a `Parameter` (a buffer, a running
  mean), then it is **excluded** from `parameters()` (the optimizer must not update it) but may
  be exposed via a separate `buffers()` enumeration for serialization.
- **3.7** As a núcleo author, when a collected `Parameter` reaches a `Grad` site (the training
  step), then it is the input the autograd engine differentiates with respect to — i.e. a
  `Parameter` *is* a `Diff<T>` input at that site (`nucleo-autograd-spec.md` §4). This is the
  reconciliation of the two type-level concepts: **which** tensors train is decided by *explicit
  collection* (§3.1–§3.3), and the autograd engine differentiates exactly those — there is **no**
  per-tensor mutable `requires_grad` flag; collection-membership replaces it.

> **TBD (plan-time):** [N3] The **parameter ownership/borrow model** — confirm "params are owned
> by the module, the optimizer borrows them for in-place update" against Cajeta's borrow checker
> (a long-lived borrow across many `step` calls; the optimizer outlives no parameter). And
> whether **grads** handed to `step` are **borrowed** (read-only, consumed within the call and
> not retained) — the leaning answer, avoiding any grad lifetime owned by the optimizer.
> Resolve jointly with `collections-dont-own-elements` (owning-collection drop semantics) so the
> parameter collection does not leak or double-free its tensors.

> **TBD (plan-time):** [N4] What `Parameter` *is* — a distinct wrapper type over `Tensor<T>` (so
> "this tensor is optimizable" is type-visible and collection is a type test), vs. an annotation
> on a tensor field, vs. a marker the module records. Lean: a `Parameter<T>` newtype over
> `Tensor<T>`, making collection a typed walk and keeping buffers/params distinct by type.

## 4. No global mutable state — the corrections (the why)

This core **exists to correct** the upstream mistakes catalogued in `python-stack-analysis.md`
§3.2 ("drop the mistakes"). The corrections are requirements, not preferences.

**Use cases**
- **4.1** As a developer, when I set a tensor's dtype/device, then it is a **type-level**
  property (`Tensor<float32>`, a device type parameter) — there is **no** `set_default_dtype` /
  `set_default_device` global to set, and therefore no order-dependent action-at-a-distance.
- **4.2** As a developer, when I want gradients, then I obtain them as the **explicit return** of
  a `Grad`-transformed function (`transform-intrinsics-spec.md` §5) — there is **no** global
  grad-enabled toggle, no `requires_grad` bool per tensor, and no `torch.no_grad()` global
  context. Stopping gradient is the lexical `@NoGrad` (that spec §9), not a runtime flag.
- **4.3** As a developer, when an optimizer updates parameters, then it consumes the **grads
  passed to `step`**; there is **no** mutable `.grad` accumulator hanging off each parameter and
  therefore **no `zero_grad` correctness footgun** (a forgotten `zero_grad` cannot silently
  accumulate stale grads — there is nothing to forget). The familiar `zeroGrad()` may exist as a
  no-op-or-thin-shim for muscle memory (`target-experience.md` §2) but is **not** load-bearing.
- **4.4** As a developer, when I read a parameter's value, then there is **no `.data` escape
  hatch** that detaches it from autograd behind the engine's back — the autograd boundary is the
  explicit `Grad` transform, not a per-tensor mutable attribute.
- **4.5** As a developer, when two threads/fibers train two models, then because dtype/device/
  grad-state are type-level or explicit-per-call (never process-global mutable), there is **no
  shared global an interleaving could corrupt** — the engine is reentrant by construction.

> **TBD (plan-time):** [N5] Exactly how thin the **familiar-but-non-load-bearing** shims are
> (`zeroGrad`, a `.data`-shaped accessor): kept as recognizable no-ops/aliases on the **façade**
> only, or absent from the neutral core entirely and reintroduced (if at all) in
> `torch-facade-spec.md`. Lean: absent from the core; façade decides.

## 5. The optimizer protocol

An optimizer holds a (borrowed) parameter collection and a hyper-parameter config; its `step`
consumes **explicit grads** and writes updated parameter values in place.
```cajeta
var opt = optim.AdamW(model.parameters(), lr: 3e-4);
var result = step.withGrads(batch.x, batch.y);   // {value, grads} from Grad (autograd spec)
opt.step(result.grads);                           // explicit grads in; params updated in place
```

**Use cases**
- **5.1** As a developer, when I construct `optim.SGD(params, lr: 0.1, momentum: 0.9)` /
  `optim.Adam(params, lr: 1e-3)` / `optim.AdamW(params, lr: 3e-4, weightDecay: 0.01)`, then I
  get an optimizer over that parameter set with those hyper-parameters (named args + defaults).
- **5.2** As a developer, when I call `opt.step(grads)`, then the optimizer matches each grad to
  its parameter **by the stable collection order/name** (§3.1), applies its update rule, and
  writes the new value into the owned parameter tensor in place — **grads come in explicitly**,
  nothing is read from a global `.grad`.
- **5.3** As a developer, when the `grads` passed to `step` do not correspond to the optimizer's
  parameter set (count/shape/name mismatch), then it is a **compile error where statically
  knowable**, else a fail-loud runtime error — never a silent partial update.
- **5.4** As an optimizer author, when my rule keeps per-parameter state (Adam's first/second
  moment estimates), then that state is **owned by the optimizer instance** (not bolted onto the
  parameter), initialized lazily on first `step` and kept in the same stable order as the params.
- **5.5** As a developer, when I share the **one VJP rule-set / two-driver** model, then the
  optimizer is **driver-agnostic**: the `grads` it consumes may come from the compiled `Grad`
  transform **or** the eager tape (`nucleo-autograd-spec.md`) — `step`'s contract is "explicit
  grads in," indifferent to which driver produced them.
- **5.6** As a developer, when I implement a custom optimizer, then I implement the **optimizer
  protocol** (a `step(grads)` + state-init contract) — the protocol is an interface other
  optimizers (and toffee's SPELA-style updates) implement, not a fixed closed set.

> **TBD (plan-time):** [N6] The optimizer protocol's exact shape — a single `step(grads)`, or a
> split `step()` + a separately supplied grad source; how per-parameter optimizer state is keyed
> (positional index vs. parameter identity/name) so it survives parameter-set reconstruction;
> and whether gradient transforms (clipping, accumulation across micro-batches) are optimizer
> options or composable wrappers over the grad bag.

## 6. Learning-rate schedulers

A scheduler adjusts an optimizer's learning rate over training, **explicitly** (no hidden
global step counter).

**Use cases**
- **6.1** As a developer, when I construct a scheduler over an optimizer
  (`optim.CosineSchedule(opt, tMax: 1000)`, `optim.StepSchedule(opt, stepSize: 30, gamma: 0.1)`,
  `optim.ExponentialSchedule(opt, gamma: 0.95)`), then it computes the LR for a given step and
  applies it to the optimizer.
- **6.2** As a developer, when I advance the schedule (`sched.step()` once per epoch/iteration),
  then the optimizer's LR is updated to the schedule's value for that step — the **step count is
  the scheduler's explicit state**, not a process-global.
- **6.3** As a developer, when I query the current LR (`sched.lr` / `opt.lr`), then I read the
  value actually in effect — observable, not buried.
- **6.4** As a developer, when I compose or chain schedules (warmup then cosine), then schedules
  are composable over the same optimizer (a warmup wrapper feeding a base schedule) rather than a
  single monolithic policy enum.

> **TBD (plan-time):** [N7] Whether the scheduler **borrows the optimizer and mutates its LR**
> (the torch shape) or is a **pure `lr(step) -> float` function** the training loop reads and
> applies (the functional shape, no scheduler→optimizer back-reference). Lean: a pure function
> with a thin mutating wrapper for façade familiarity — keeps the core free of another mutable
> back-edge.

## 7. Loss functions

A loss is a backend-neutral differentiable function producing the scalar a `Grad`-transformed
step differentiates.

**Use cases**
- **7.1** As a developer, when I call a loss (`nucleo.nn.crossEntropy(logits, targets)`,
  `nucleo.nn.mse(pred, target)`), then I get a scalar `Tensor` (or `float`) computed over the
  given tensors via ordinary `cajeta.math` ops — differentiable because its primitives carry VJP
  rules (`transform-intrinsics-spec.md` §7), not because of any special loss machinery.
- **7.2** As a developer, when I differentiate a step that ends in a loss
  (`Grad(step)` / `step.withGrads(x, y)`), then the returned `value` is the loss scalar and
  `grads` are the gradients w.r.t. the parameters the optimizer holds — the loss is just the
  function's scalar output (§5 of the transform spec).
- **7.3** As a developer, when I pick a reduction (`mean` / `sum` / `none`), then it is an
  **explicit named argument** with a sensible default — no global reduction state, and the
  return type reflects the choice (scalar for `mean`/`sum`, tensor for `none`).
- **7.4** As a developer, when I write a custom loss, then it is an ordinary function returning a
  scalar tensor over differentiable primitives — no base class, no registration; it is
  differentiable for free.

> **TBD (plan-time):** [N8] Whether losses are **free functions** (`nucleo.nn.crossEntropy(...)`,
> the leaning answer — they own no parameters) or also offered as **`Module`s** for the keras/
> torch `nn.CrossEntropyLoss()` recognizability (a parameterless module wrapping the function).
> A parameterized loss (focal-loss α, class weights) is a closure/partial over the free function.

## 8. Train / eval mode (without a global flag)

Some layers behave differently at train vs. inference time (dropout active vs. identity;
batchnorm using batch statistics vs. running statistics). The mode must be selectable **without
a global mutable flag** (`model.train()`/`model.eval()` as process-global state is the upstream
mistake — `python-stack-analysis.md` §3.2).

**Use cases**
- **8.1** As a developer, when I run a module's forward for training vs. inference, then the mode
  is **passed/scoped explicitly** — not toggled by a global `model.train()`/`eval()` that mutates
  hidden per-module state an interleaved evaluation could read in the wrong setting.
- **8.2** As a developer, when a mode-sensitive layer (dropout, batchnorm) runs, then it reads
  the **explicitly supplied mode** for that forward call — two concurrent forwards in different
  modes do not interfere (reentrant, like §4.5).
- **8.3** As a developer, when I evaluate inside a training loop (validation pass), then I select
  eval mode for that pass **without** disturbing the training pass's mode — no save/restore of a
  global flag, no ordering hazard.
- **8.4** As a developer, when a module has no mode-sensitive layers, then mode selection is a
  **no-op** it can ignore — the mechanism imposes no ceremony on mode-insensitive modules.

> **TBD (plan-time):** [N9] The **train/eval mode representation** — three candidates:
> (a) a `mode` parameter threaded through `forward(x, mode: Mode.Train)` (most explicit, most
> intrusive on signatures); (b) a **`FiberLocal<Mode>`** ambient set for a scope (`fiberlocal-state`
> — reentrant per fiber, no signature churn, the leaning answer); (c) an immutable mode captured
> when the module tree is *instantiated* for a phase (a `model.forTraining()` view returning a
> mode-bound module). Resolve for reentrancy + ergonomics; (b) matches the no-global-mutable
> requirement while keeping `forward` signatures clean.

## 9. Backend neutrality (the façades are thin skins)

This core is the **shared substrate**; the façades add only recognizable names and call shapes.

**Use cases**
- **9.1** As a façade author, when I build `dev.cajeta.torch.nn`, then `torch.nn.Linear`,
  `torch.optim.AdamW`, `torch.nn.functional.cross_entropy` are **thin wrappers** over the núcleo
  `Module`/optimizer/loss primitives — recognizable signatures, **one** engine underneath
  (`python-stack-analysis.md` §3.2, conservative-fidelity stance).
- **9.2** As a façade author, when I build `dev.cajeta.keras`, then `Model`/`Layer`/`compile`/
  `fit`/callbacks sit over the **same** `nucleo.nn`/`nucleo.optim` core
  (`python-stack-analysis.md` §3.3) — keras owns no nn/optim engine of its own.
- **9.3** As a toffee author, when I write a SPELA-based (forward-only, per-layer local-loss)
  trainer, then I use the **same `Module`/`Parameter`/optimizer-protocol** core directly — no
  façade — and my non-backprop update rule implements the **optimizer protocol** (§5.6),
  consuming the per-layer local grads SPELA computes rather than autograd's reverse-mode grads.
- **9.4** As a developer, when I move a model between the torch skin, keras, and toffee, then the
  `Module` and parameters are **the same objects** — the façade is a presentation layer, not a
  conversion boundary.

## 10. Acceptance criteria (spec-level)
- A `Module` can be declared with `Parameter` and sub-`Module` fields and a `forward`; it is
  invoked as `model(x)`/`model.forward(x)` and composes sub-modules' forwards.
- `model.parameters()` enumerates all owned + sub-module parameters in a stable order;
  `namedParameters()` gives stable dotted paths; non-parameter buffers are excluded.
- Parameters are **owned by the module**; the optimizer **borrows** them for in-place update and
  consumes **explicit grads** passed to `step` — no global `.grad`, no `zero_grad` correctness
  dependency.
- There is **no** global mutable dtype/device default and **no** global grad toggle; dtype/
  device are type-level and gradients are explicit returns of the `Grad` transform.
- `SGD`/`Adam`/`AdamW` implement a common optimizer protocol; per-optimizer state is owned by the
  optimizer; a custom optimizer (incl. SPELA-style) can implement the same protocol.
- LR schedulers adjust an optimizer's LR explicitly (step/exponential/cosine), with the step
  count as explicit scheduler state, not a global.
- Loss functions are differentiable scalars over `cajeta.math` primitives (cross-entropy, MSE),
  with explicit reduction.
- Train/eval mode is selectable **without a global mutable flag** and is reentrant across
  concurrent forwards.
- The core is backend-neutral: the torch and keras façades and toffee are thin skins over the
  same `Module`/optimizer/loss objects.

## 11. Open questions (resolve at plan time)
- **[N1]** `Module` as interface/base class vs. `@Module`-synthesized plumbing (§1.4).
- **[N2]** Whether `Layer` and `Module` are distinct or one abstraction (§2).
- **[N3]** **Parameter ownership/borrow model** — params owned by module, optimizer borrows for
  in-place update; grads borrowed (read-only) by `step`; reconcile with the borrow checker and
  owning-collection drop semantics (§3).
- **[N4]** What `Parameter` is — a `Parameter<T>` newtype over `Tensor<T>` vs. annotation vs.
  marker (§3).
- **[N5]** How thin / where the familiar-but-non-load-bearing shims live (`zeroGrad`, `.data`):
  façade-only vs. absent from the core (§4).
- **[N6]** The optimizer protocol's exact shape, optimizer-state keying, and where grad
  transforms (clip/accumulate) live (§5).
- **[N7]** Scheduler as optimizer-mutating wrapper vs. pure `lr(step)->float` function (§6).
- **[N8]** Losses as free functions vs. also `Module`s for façade recognizability (§7).
- **[N9]** **Train/eval mode representation** — threaded `mode` parameter vs. `FiberLocal<Mode>`
  ambient scope vs. phase-bound module view (§8).
- **Module state serialization** (`state_dict`/`.pt`) is **out of scope** — deferred to
  `torch-facade-spec.md`; this spec only guarantees stable, named parameter/buffer enumeration
  as its basis (§1.3, §3.4).
