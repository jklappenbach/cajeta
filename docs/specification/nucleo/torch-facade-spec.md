# Torch Façade — Specification

> Status: draft for review (2026-06-23). A **Layer-2 façade** — the recognizable PyTorch
> surface (`dev.cajeta.torch` + `.nn`, `.optim`, `.autograd`, `.utils.data`, `.amp`, `.io`)
> — a **thin skin over núcleo**, *not* an engine. The critical surface (`python-stack-analysis.md`
> §3.2: "the single most important surface to capture"). Companion design:
> `python-stack-analysis.md` §3.2, `target-experience.md` §2, `language-foundations.md` §3.
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions (the
> *how*) are deferred as `> **TBD (plan-time):**` markers and collected in §11, to be resolved
> when this spec is turned into a plan. Outline-numbered for addressability.
>
> **Scope boundary:** this spec is the **façade skin** — the recognizable `torch.*` names,
> signatures, argument order, and the deliberate corrections. The **engine internals it skins
> over are specified elsewhere and are NOT re-specified here**: the tensor substrate is stdlib
> `cajeta.math.Tensor`; the autodiff mechanism (the `Grad` combinator, VJP registry, eager
> tape) is `transform-intrinsics-spec.md` + `nucleo-autograd-spec.md`; the module/optimizer
> core is `nucleo-nn-optim-spec.md`; the columnar substrate is `nucleo-column-spec.md`;
> typed return bags use `records-spec.md`. This façade is a *recognizable* re-presentation of
> those, not a faithful re-implementation of PyTorch.

## 1. Definition

### 1.1 Purpose
`dev.cajeta.torch` exists to let a PyTorch developer **port a script with minimal edits**
while getting núcleo underneath: compiled, fused, typed autodiff. The façade owes the
developer their **muscle memory** — `torch.*` names, signatures, and argument order stay
familiar — **not bug-for-bug fidelity**. The port is the **on-ramp, not the product**
(`python-stack-analysis.md` §1.3): familiarity gets the developer in the door; type-level
shape/device/dtype, language-level fusion, and compiled autodiff are why they stay. Where an
upstream PyTorch API encodes a genuine mistake, this façade **does not inherit it** (§2.5 of
the analysis; the corrections are §3 here).

### 1.2 Scope
- A recognizable `torch.*` top-level surface (factory functions, math ops, `crossEntropy`,
  `save`/`load`) re-presented over `cajeta.math.Tensor`, preserving PyTorch's **names,
  signatures, and argument order** (named arguments make the order-sensitive call sites
  port cleanly).
- `torch.nn` — `Module`, `Parameter`, `Linear`/`Conv2d`/activations/loss modules, `operator()`
  call sugar — a **thin skin** over `nucleo-nn-optim-spec.md`.
- `torch.optim` — `SGD`/`Adam`/`AdamW` + LR schedulers — thin over `nucleo-nn-optim-spec.md`.
- `torch.autograd` — the **familiar `.backward()`/`.withGrads` shape** over the `Grad`
  combinator + eager tape (`transform-intrinsics-spec.md`, `nucleo-autograd-spec.md`).
- `torch.utils.data` — `Dataset`/`DataLoader`, **fiber-backed** prefetch/parallel loading.
- `torch.amp` — mixed precision via the `@Autocast` annotation (no global state).
- `torch.io` — `state_dict`/`load_state_dict` and **`.pt` serialization compatibility**.
- The **deliberate corrections** (§3) the façade makes to PyTorch's design mistakes.

### 1.3 Non-goals
- **Re-specifying engine internals.** The tensor, autograd mechanism, nn/optim core, and
  columnar buffer are owned by their own specs (§ scope boundary above). This spec defines
  only the *recognizable skin* and its corrections.
- **Bug-for-bug PyTorch fidelity.** Recognizable, not faithful (§1.1). The dropped mistakes
  (§3) are *intentional* divergences, not gaps.
- **The complete `torch.*` op tail.** v1 captures the load-bearing training surface; the long
  tail of ops is additive backlog behind the same names (the substrate already has most ops in
  `cajeta.math` — analysis §3.2 "already covered").
- **Python-runtime / TorchScript / `torch.compile` tracer interop.** Cajeta compiles ahead of
  time; `Jit` is a combinator (`transform-intrinsics-spec.md`), not a Python tracer. Loading
  PyTorch *weights* (`.pt` tensors / state-dicts) is in scope (§9); executing pickled Python
  *code* is not.
- **`functorch`/`torch.func` as a separate surface.** Functional grad **is** the first-class
  path here (via `Grad`), so a parallel `torch.func` namespace is redundant — noted, not built.

### 1.4 Relationship to existing constructs
- **Tensor reuses stdlib `cajeta.math.Tensor`** — `torch.Tensor` is not a new type; it is the
  stdlib tensor, with **device and dtype as type-level contracts** (`Tensor<float32>`, a
  device type parameter), monomorphized (analysis §3.1, "already covered"). This façade adds no
  tensor engine.
- **Autograd reuses the `Grad` combinator + eager tape** — `transform-intrinsics-spec.md`
  defines `Grad`/`@Grad` and the VJP registry; `nucleo-autograd-spec.md` owns the eager tape
  and the tensor-op rule set. The façade exposes the **familiar `.backward()`/`.withGrads`
  shape** over them; it adds no autodiff rules.
- **nn/optim are thin over `nucleo-nn-optim-spec.md`** — `nn.Module`/`Parameter`/`optim.Adam`
  re-present the núcleo module/optimizer core under PyTorch names.
- **Named arguments + defaults, operator overloading, monomorphized templates** (shipped — the
  Layer-0 enablers, `language-foundations.md` §3) are what let `torch.*` signatures stay
  recognizable while gaining types. **There are no global functions** in Cajeta, so `torch.zeros`,
  `torch.crossEntropy`, etc. are **static methods on a `torch`-named carrier** (module-object
  shape), not free functions — a mechanical re-presentation, invisible at most call sites.
- **DataLoader rides the fiber/carrier substrate** (the same substrate `Pmap` targets,
  `transform-intrinsics-spec.md` §6) for parallel/prefetched loading.

> **TBD (plan-time):** [T1] How `torch` (and `nn`/`optim`/`F`) are surfaced given no global
> functions — an importable module-object whose static methods carry the `torch.*` names
> (`import dev.cajeta.torch as torch; torch.zeros(...)`), vs. a thin set of carrier classes.
> Lean: module-object alias so `torch.zeros(3, 3)` reads identically to Python.

> **TBD (plan-time):** [T2] **Method-name casing policy.** This façade currently mixes
> camelCase Cajeta-idiomatic spellings (`crossEntropy`, `zeroGrad`) with PyTorch's exact
> snake_case identifiers where muscle-memory or `.pt` round-trip matters (`state_dict`,
> `load_state_dict`, `lr_scheduler`). Decide one principled rule and apply it uniformly:
> (a) **preserve PyTorch's exact identifiers** (snake_case included) to maximize minimal-edit
> porting — the recognizability mandate, at the cost of the Cajeta camelCase norm; or
> (b) **camelCase all surface methods** (`stateDict`, `loadStateDict`) — convention-consistent,
> at the cost of edit-on-port. Lean: (a) for serialization/`.pt`-coupled names where the string
> is load-bearing, (b) elsewhere — but state the rule explicitly so the surface stops reading as
> unprincipled. Resolve jointly with `keras-facade-spec.md` (which also names `state_dict`).

## 2. The recognizable surface — porting a script with minimal edits

The promise: a PyTorch training loop ports with **minimal edits**. Names, signatures, and
argument order stay; the edits are the deliberate corrections (§3) and Cajeta syntax
(`var`, types on tensors), not a rewrite.

```cajeta
import dev.cajeta.torch as torch;
import dev.cajeta.torch.nn;
import dev.cajeta.torch.optim;

var model = heap MLP();
var opt   = optim.AdamW(model.parameters(), lr: 3e-4);   // familiar name + arg order   ✅

@Grad
float32 step(Tensor<float32> x, Tensor<int64> y) {
    var logits = model(x);                                // nn.Module operator() call
    return torch.crossEntropy(logits, y);                // torch.* name preserved
}

for (var batch : loader) {
    var loss = step.withGrads(batch.x, batch.y);         // value + explicit grads
    opt.step(loss.grads);                                // explicit grads — no .grad soup
    opt.zeroGrad();                                      // kept for familiarity
}
```

**Use cases**
- **2.1** As a PyTorch developer, when I port a training loop, then the per-step shape
  (`model(x)` → loss → backward → `opt.step()`) reads the same, with only the deliberate
  corrections (§3) and Cajeta type annotations as edits — not a rewrite.
- **2.2** As a PyTorch developer, when I call a factory or op (`torch.zeros(3, 3)`,
  `torch.matmul(a, b)`, `torch.cat(parts, dim: 0)`, `x.relu()`, `a @ b`), then the **name,
  signature, and argument order match PyTorch**, with named arguments available for the
  order-sensitive call sites (`dim:`, `keepdim:`).
- **2.3** As a PyTorch developer, when I use an argument PyTorch spells `requires_grad`/
  `dtype`/`device`, then the recognizable name still parses but its *meaning* is the corrected
  one (dtype/device are type-level — §3.1; grad is explicit via `@Grad`/`Grad` — §3.2): the
  surface is familiar, the footgun is gone.
- **2.4** As a PyTorch developer, when an op exists in `cajeta.math` but not yet under a
  `torch.*` alias, then the gap is **additive** (a missing alias, not a missing capability) —
  the substrate already carries the op (analysis §3.2).

> **TBD (plan-time):** [T2] The exact edit-delta a representative real script (e.g. an
> nanoGPT-class training loop) requires — enumerated as the acceptance corpus, so "minimal
> edits" is measured, not asserted. Couples to which `torch.*` aliases ship in v1 (§2.4).

## 3. The deliberate corrections (recognizable, not faithful)

The façade replicates PyTorch only as far as PyTorch is sound. The corrections below are the
"drop the mistakes" list from `python-stack-analysis.md` §3.2, applied via the three-tier rule
(§2.5: silently-correct / better-by-opt-in / refuse-to-port).

### 3.1 No mutable global state; dtype/device are type-level
**Use cases**
- **3.1.1** As a developer, when I want a default dtype or device, then I set it **per call /
  per type**, not through a global mutator — `torch.set_default_dtype` / `set_default_device`
  and the global grad toggles **are not ported** (refuse-to-port: mutable global state is the
  mistake). The familiar effect is achieved by the dtype/device living in the **type**.
- **3.1.2** As a developer, when a tensor's dtype and device are part of its **type**
  (`Tensor<float32>` on a device type parameter, monomorphized — analysis §3.2 "the better
  way"), then a mismatch is a **compile error**, not a runtime dtype/device exception.
- **3.1.3** As a developer, when two operands live on different devices (host vs. device) or
  have incompatible dtypes, then the mismatch is caught at **compile time** with a located
  diagnostic — never a 2 a.m. runtime stack trace (`target-experience.md` §2).

### 3.2 Explicit grads, not the global `.grad`/`zero_grad` accumulator
**Use cases**
- **3.2.1** As a developer, when I differentiate a function, then I get gradients as
  **explicit return values** — `step.withGrads(x, y) -> { value, grads }` (a typed record per
  `records-spec.md`) over the `Grad` combinator (`transform-intrinsics-spec.md` §5) — **not** a
  global `.grad` accumulator that silently sums across iterations.
- **3.2.2** As a developer, when I want the familiar **functional-grad-first** path, then it is
  the **first-class** path here (analysis §3.2 "functional grad as the first-class path"): the
  `@Grad`/`Grad(f)` transform is primary; the eager-accumulate style is the *familiar skin* over
  the same VJP registry (`nucleo-autograd-spec.md`), not a second engine.
- **3.2.3** As a developer porting code that calls `zero_grad()`, then `optim.zeroGrad()` **is
  kept for familiarity** (better-by-opt-in: the familiar call works), but it is *not required*
  by the functional path — the explicit-grads model has no accumulator to clear
  (`target-experience.md` §2: "kept for familiarity; functional path is first-class").
- **3.2.4** As a developer porting a `loss.backward()` call, then a **familiar `.backward()`
  shape** is provided over the eager tape (`nucleo-autograd-spec.md`) so define-by-run /
  dynamic-control-flow code ports — but it surfaces grads explicitly (no hidden global
  side-effect on parameter `.grad` fields by default; §3.2.1).

> **TBD (plan-time):** [T3] The exact familiar-`.backward()` surface and how explicit it makes
> the grad result — pure `.withGrads` (no `.backward()` at all), `.backward()` returning a grad
> bag, or `.backward()` populating an *opt-in, non-global* grad holder for the most literal port.
> Couples to `transform-intrinsics-spec.md` [F3] (companion shape) and `nucleo-autograd-spec.md`.

### 3.3 No `.data` escape hatch; no in-place / version-counter footguns
**Use cases**
- **3.3.1** As a developer, when I reach for PyTorch's `.data` (the autograd escape hatch that
  silently detaches), then it **is not ported** (refuse-to-port) — the disciplined replacement
  is the `@NoGrad` scope annotation (`transform-intrinsics-spec.md` §9), which is statically
  visible, not a silent detach.
- **3.3.2** As a developer, when I write an in-place op that PyTorch's version counter would
  trip over at backward time, then the value-semantics / deterministic-memory model + the
  mid-level-IR autodiff make the **in-place/version-counter footgun structurally absent** —
  there is no runtime version counter to desync (analysis §3.2 "in-place-op/autograd
  version-counter footguns" dropped).
- **3.3.3** As a developer, when I want stop-gradient or detach behavior, then I use `@NoGrad`
  (a statically-visible region/value, `transform-intrinsics-spec.md` §9.1), the disciplined
  replacement for `torch.no_grad()` / `.detach()` / `.data`.

### 3.4 Typed shapes — mismatch is a compile error
**Use cases**
- **3.4.1** As a developer, when I write a matmul or contraction with mismatched fixed shapes
  (`Matrix<float32, 4, 4> @ vec3`), then it is a **compile error** (const-generic dimensions —
  `target-experience.md` §6c), not a runtime shape error.
- **3.4.2** As a developer, when shape is statically known, then the typed-shape diagnostic is
  **named and located** at the offending op (the `@Einsum`/transform diagnostic bar,
  `transform-intrinsics-spec.md` §10); when a dimension is dynamic, the op still compiles and
  shape is checked at the dynamic boundary (no false compile error on genuinely dynamic shapes).

> **TBD (plan-time):** [T4] How much of the `torch.*` surface ships **fixed-shape-typed**
> (const-generic dims, full compile-time shape checking) vs. **rank/dtype-typed with dynamic
> dims** in v1 — the moat is strongest with fixed shapes, but most ported scripts use dynamic
> batch dims. Lean: dtype+device typed everywhere; fixed-shape opt-in where the developer
> declares dims. Couples to `cajeta.math` shape typing and `transform-intrinsics-spec.md` §10.

## 4. torch.nn — Module / Parameter (thin over núcleo)

`nn.Module` and `nn.Parameter` are the **recognizable PyTorch skin** over the module/parameter
core in `nucleo-nn-optim-spec.md`. The façade adds the PyTorch *shape* (subclass `nn.Module`,
declare sub-modules as fields, write `forward`, call via `operator()`); the engine is núcleo's.

**Use cases**
- **4.1** As a developer, when I define a model by extending `nn.Module`, declaring sub-modules
  as fields and a `forward` method, then it reads like PyTorch
  (`target-experience.md` §2: `class MLP : nn.Module { nn.Linear fc1 = ...; ... }`), with
  `operator()` call sugar so `model(x)` and `fc1(x)` work.
- **4.2** As a developer, when I call `model.parameters()`, then I get the model's trainable
  parameters (the núcleo `Parameter` set) to hand to an optimizer — the familiar `optim.Adam(
  model.parameters(), ...)` shape.
- **4.3** As a developer, when I use a built-in layer (`nn.Linear`, `nn.Conv2d`, `nn.ReLU`,
  `nn.CrossEntropyLoss`), then it carries PyTorch's name, constructor signature, and argument
  order, re-presenting the núcleo layer.
- **4.4** As a developer, when I nest modules (a module field that is itself a module), then
  parameter enumeration and `state_dict` recurse through the tree as in PyTorch.
- **4.5** As a developer, when a `Parameter` participates in `Grad`, then it is an explicit
  differentiated input (§3.2) — there is no per-parameter mutable global `.grad` field carrying
  hidden state across steps (the corrected model).

> **TBD (plan-time):** [T5] How `nn.Module` registers its sub-modules and parameters for
> enumeration/`state_dict` — reflection over module-typed fields (records/`Class<T>` field
> enumeration) vs. an explicit register call. Lean: reflection over typed fields (no
> `register_parameter` ceremony), resolved with `nucleo-nn-optim-spec.md`.

## 5. torch.optim — optimizers + schedulers (thin over núcleo)

`optim.SGD`/`Adam`/`AdamW` and the LR schedulers are the PyTorch skin over the núcleo optimizer
core (`nucleo-nn-optim-spec.md`). `optim.step` consumes the **explicit grads** the `Grad`
transform returns (§3.2), not a global accumulator.

**Use cases**
- **5.1** As a developer, when I construct `optim.AdamW(model.parameters(), lr: 3e-4)`, then the
  name, hyperparameter names, and defaults match PyTorch (named args carry `lr:`, `betas:`,
  `weight_decay:`).
- **5.2** As a developer, when I call `opt.step(loss.grads)`, then the optimizer applies the
  **explicitly provided** gradients to its parameters (the corrected, accumulator-free model —
  §3.2.1); the PyTorch `opt.step()`-with-implicit-`.grad` form is the familiar skin that reads
  the same grad bag, not a hidden global.
- **5.3** As a developer, when I attach an LR scheduler (`optim.lr_scheduler.CosineAnnealingLR`,
  etc.), then it wraps the optimizer with the recognizable PyTorch name/shape over the núcleo
  scheduler.
- **5.4** As a developer, when I call `opt.zeroGrad()`, then it is **available for familiarity**
  (§3.2.3) but is a no-op-of-necessity under the functional path (no accumulator to clear).

## 6. torch.autograd — the familiar grad shape over the combinator

`torch.autograd` is the **recognizable `.backward()`/`.withGrads` skin** over the `Grad`
combinator and the eager tape. **The autodiff mechanism is NOT specified here** — it is
`transform-intrinsics-spec.md` (the `Grad`/`@Grad` combinator, VJP registry, composition) and
`nucleo-autograd-spec.md` (the eager tape + tensor-op rule set). This section defines only how
the *PyTorch-shaped* surface maps onto them.

**Use cases**
- **6.1** As a developer, when I annotate a loss function `@Grad` (or wrap it `Grad(loss)`),
  then I get the **compiled, fused** backward (`transform-intrinsics-spec.md` §5) reached as
  `loss.withGrads(args) -> { value, grads }` — the primary, functional-first path (§3.2.2).
- **6.2** As a developer porting define-by-run code with dynamic control flow, then the
  **familiar eager `.backward()` feel** is available over the eager tape
  (`nucleo-autograd-spec.md`), replaying the same VJP rules at runtime (one rule-set, two
  drivers — analysis §4.4) — so dynamic models port, while static graphs get the fused path.
- **6.3** As a developer, when I differentiate twice (`Grad(Grad(f))`), then second-order grad
  works per `transform-intrinsics-spec.md` §5.1 — the façade exposes it under a recognizable
  shape, adding no rules.
- **6.4** As a developer, when a primitive in my function has no registered VJP rule, then I get
  a **hard, named compile error** (`transform-intrinsics-spec.md` §5.3) — never a silent wrong
  gradient (a correctness guarantee PyTorch's runtime cannot make statically).

## 7. torch.utils.data — fiber-backed DataLoader

`Dataset`/`DataLoader` carry PyTorch's shape; the **parallelism/prefetch is fiber-backed** (the
carrier substrate, the same one `Pmap` targets — `transform-intrinsics-spec.md` §6), not
PyTorch's process-fork `num_workers` model.

**Use cases**
- **7.1** As a developer, when I define a `Dataset` (`__len__`/`__getitem__`-shaped: a size and
  an indexed item accessor) and wrap it in `DataLoader(ds, batch_size: 64, shuffle: true)`, then
  it iterates batches with PyTorch's recognizable constructor arguments.
- **7.2** As a developer, when I set parallel loading (`num_workers:`-shaped), then prefetch and
  parallel item production run on **fibers/carriers** — no process fork, no pickling, no
  start-method footgun (the fiber substrate is the corrected mechanism).
- **7.3** As a developer, when I iterate the loader in a training loop (`for (var batch : loader)`),
  then batches arrive prefetched and collated, overlapping load with compute, under the familiar
  iteration shape.
- **7.4** As a developer, when a batch is consumed, then the borrow/move discipline of the
  fiber-backed buffers is sound (no double-free / use-after-move across the prefetch handoff) —
  the loader honors Cajeta's deterministic-memory model at the carrier boundary.

> **TBD (plan-time):** [T6] DataLoader's exact carrier mapping and collation — how prefetch
> depth, batch collation, and shuffling map onto the fiber/carrier model and its known
> constraints (single-carrier limits, bare-call `spawn` requirements). Couples to the fiber
> substrate and `Pmap` (`transform-intrinsics-spec.md` §6, [F8]).

## 8. torch.amp — mixed precision via @Autocast

Mixed precision (autocast) is delivered via an **`@Autocast` annotation**, not PyTorch's
context-manager + global `GradScaler` state. It is a transform/policy annotation in the same
family as `@Grad`/`@NoGrad` (`transform-intrinsics-spec.md` §9, `language-foundations.md` §1).

**Use cases**
- **8.1** As a developer, when I annotate a forward function `@Autocast`, then eligible ops run
  in the reduced precision (e.g. `float16`/`bfloat16`) and precision-sensitive ops stay in
  `float32`, decided by the autocast policy — replacing PyTorch's `with autocast():` context
  manager with a statically-visible annotation (no global state).
- **8.2** As a developer, when I train mixed-precision, then **loss/gradient scaling is handled
  without a stateful global `GradScaler`** — the scaling rides the explicit-grads path (§3.2),
  fitting the no-global-state correction (§3.1.1).
- **8.3** As a developer, when `@Autocast` composes with `@Grad` (`@Autocast @Grad step`), then
  the composition order is defined (nearest-the-declaration-first,
  `transform-intrinsics-spec.md` §4): the gradient is computed of the autocast-precision forward,
  consistently.

> **TBD (plan-time):** [T7] `@Autocast`'s policy surface — the precision the annotation selects
> (fixed `float16` vs. argument-selectable `bfloat16`/`float16`), the op allow/deny lists, and
> the gradient-scaling mechanism on the explicit-grads path (whether scaling is automatic or an
> explicit argument). Couples to `transform-intrinsics-spec.md` §9 (scope annotations) and
> `nucleo-autograd-spec.md`.

## 9. torch.io — state_dict and .pt compatibility

The façade reads and writes **PyTorch-compatible `state_dict`** and supports **`.pt`
serialization compatibility** so weights cross the boundary. **Loading PyTorch *weights*
(tensors / state-dicts) is in scope; executing pickled Python *code* is not** (§1.3).

**Use cases**
- **9.1** As a developer, when I call `model.state_dict()`, then I get a name→tensor mapping
  whose keys match PyTorch's parameter naming convention for the equivalent module tree (so the
  dict round-trips with a PyTorch model of the same architecture).
- **9.2** As a developer, when I `load_state_dict(sd)`, then parameters are populated by matching
  names, with the familiar strict/non-strict behavior (missing/unexpected keys reported), over
  the núcleo `Parameter` set.
- **9.3** As a developer, when I load a `.pt` file produced by PyTorch (weights / a state-dict),
  then the tensor buffers and the parameter-name mapping are recovered — `.pt` **weight**
  compatibility (the tensor-payload + key path), so a model trained in PyTorch initializes a
  Cajeta model of the same architecture.
- **9.4** As a developer, when I attempt to load a `.pt` that requires executing arbitrary
  pickled Python objects/code, then it is **refused with a clear diagnostic** (the security /
  no-Python-runtime boundary), not silently or unsafely executed — only the tensor/state-dict
  payload path is supported.
- **9.5** As a developer, when I `torch.save(model.state_dict(), path)`, then I write a file a
  PyTorch process can `torch.load` and `load_state_dict` for a matching architecture
  (round-trip the other direction).

> **TBD (plan-time):** [T8] The `.pt` compatibility depth — which slice of the pickle/zip `.pt`
> container is parsed (the `data.pkl` + tensor storages for the weights-only path), whether
> `safetensors` is offered as the *preferred* (safe, code-free) interchange alongside `.pt`, and
> the exact parameter-name mapping that makes a PyTorch and a Cajeta module tree's keys agree.
> Couples to the codec lib and `nucleo-nn-optim-spec.md`'s parameter naming (§4 [T5]).

## 10. Acceptance criteria (spec-level)
- A representative PyTorch training loop ports with **minimal edits** — `torch.*` names,
  signatures, and argument order are recognizable; the edits are the §3 corrections + Cajeta
  type annotations, not a rewrite (measured against the §2 [T2] corpus).
- Tensor reuses stdlib `cajeta.math.Tensor`; **dtype and device are type-level contracts**, and a
  host/device or dtype mismatch is a **compile error** (§3.1, §3.4).
- Gradients are **explicit return values** via `Grad`/`@Grad`/`.withGrads` (no global `.grad`
  accumulator); `zeroGrad()` is kept for familiarity but unrequired by the functional path (§3.2).
- No `set_default_dtype`/`set_default_device`/global grad toggles; no `.data` escape hatch; no
  in-place/version-counter footgun (refuse-to-port — §3.1, §3.3).
- `nn.Module`/`Parameter`/`optim.*` are **thin skins** over `nucleo-nn-optim-spec.md`; autograd is
  a thin skin over `transform-intrinsics-spec.md` + `nucleo-autograd-spec.md` (no engine
  re-specified here).
- `DataLoader` is **fiber-backed** (no process-fork workers) and borrow-sound across prefetch.
- Mixed precision is delivered via **`@Autocast`** with no global `GradScaler` state.
- `state_dict`/`load_state_dict` round-trip with PyTorch naming; PyTorch `.pt` **weights** load
  (tensor/state-dict payload), and a request to execute pickled Python code is **refused**.

## 11. Open questions (resolve at plan time)
- **[T1]** How `torch`/`nn`/`optim`/`F` are surfaced given no global functions — importable
  module-object vs. carrier classes (§1.4).
- **[T2]** The measured edit-delta of a representative real script, as the acceptance corpus, and
  which `torch.*` aliases ship in v1 (§2).
- **[T3]** The exact familiar-`.backward()` surface and how explicit it makes the grad result
  (§3.2); couples to `transform-intrinsics-spec.md` [F3].
- **[T4]** Fixed-shape (const-generic) vs. rank/dtype-with-dynamic-dims typing across the v1
  `torch.*` surface (§3.4).
- **[T5]** `nn.Module` sub-module/parameter registration — reflection over typed fields vs.
  explicit register; jointly with `nucleo-nn-optim-spec.md` (§4).
- **[T6]** `DataLoader`'s carrier mapping, prefetch depth, and collation on the fiber model (§7);
  couples to `Pmap` (`transform-intrinsics-spec.md` [F8]).
- **[T7]** `@Autocast`'s precision/op-list/grad-scaling policy surface (§8).
- **[T8]** `.pt` compatibility depth and parameter-name mapping; whether `safetensors` is the
  preferred safe interchange (§9); couples to the codec lib.
