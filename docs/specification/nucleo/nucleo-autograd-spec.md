# Núcleo Autograd Engine — Specification

> Status: draft for review (2026-06-23). The **differentiable-tensor experience**
> (`dev.cajeta.nucleo.autograd`) — núcleo's autodiff core. Layer-1b. Companion analysis:
> `python-stack-analysis.md` §4.4 (autodiff placement — decided MIR-pass primary, tape skin);
> siblings `transform-intrinsics-spec.md` (the `Grad`/`Jit`/`Vmap` **mechanism** + the VJP rule
> registry this engine supplies rules to), `nucleo-expr-spec.md` (the fused expression a backward
> differentiates), `nucleo-column-spec.md` (the buffers grads flow into), `records-spec.md` (the
> typed `{value, grads}` return bag). Façade consumer: `dev.cajeta.torch`.
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions (the
> *how*) are deferred as `> **TBD (plan-time):**` markers and collected in §11, to be resolved
> when this spec is turned into a plan. Outline-numbered for addressability.

## 1. Definition

### 1.1 Purpose
The **autograd engine** is the differentiable-tensor experience built on top of the transform
mechanism. Where `transform-intrinsics-spec.md` owns *how* `Grad(f)` walks IR and consults a
registry, this spec owns *what núcleo puts in that registry and how a user differentiates a
loss*: the **VJP rules for the tensor primitives** of `cajeta.math`/núcleo, the engine's
**forward/backward contract**, the **activation store-vs-rematerialize** policy reached through
`@Checkpoint`, the **`@NoGrad`** stop-gradient scope, and the **runtime eager tape** that gives
the torch façade its define-by-run feel. It is the autodiff spine the entire deep-learning
lineage (torch/keras façades, the splat flagship) stands on.

### 1.2 Scope
- The **tensor-op VJP rule set** — the vector–Jacobian-product rule for each differentiable
  núcleo/`cajeta.math` primitive (`matmul`, `conv`, `softmax`, elementwise, reductions, …),
  registered into the shared VJP registry (`transform-intrinsics-spec.md` §7, F2).
- The **forward/backward contract** — the forward computation stays intact; a backward companion
  is synthesized that, as ordinary mid-level IR, **fuses / DCEs / remats** with the rest of the
  program (the decided MIR-pass placement, analysis §4.4).
- **Differentiability as a type distinction where useful** — `Diff<T>` vs. plain `T` instead of
  torch's runtime `requires_grad` bool.
- The **runtime eager tape** — the second driver over the same registry: define-by-run,
  dynamic-control-flow differentiation (the torch façade's `.backward()` feel).
- **`@NoGrad`** scopes (stop-gradient) and **`@Checkpoint`** (activation rematerialize) as
  consumed by *this* engine (the annotations themselves are defined in
  `transform-intrinsics-spec.md` §9).
- **Higher-order grad** over the rule set, and differentiating a **fused expression**
  (`nucleo-expr-spec.md`).

### 1.3 Non-goals
- **The transform mechanism itself** — `Grad`/`Jit`/`Vmap`/`Pmap` recognition, closure
  specialization, composition order, the Tier-A/Tier-B delivery boundary, and the registry's
  *declaration form* all live in `transform-intrinsics-spec.md`. This spec *consumes* the
  mechanism and *populates* the registry; it does not redefine them.
- **`nn`/`optim`** — the module/parameter system and optimizers (`nucleo.nn`, `nucleo.optim`)
  are separate specs; this engine is what they call into, not what they contain.
- **Forward-mode (JVP) as a primary surface** — reverse-mode VJP is the v1 commitment (matching
  the mechanism spec); a JVP rule slot is noted, its surface deferred.
- **The expression engine and the column layout** — owned by `nucleo-expr-spec.md` /
  `nucleo-column-spec.md`; this spec differentiates *over* them.
- **Bug-for-bug torch autograd compatibility** — núcleo deliberately drops the global `.grad`
  accumulator, `requires_grad` bool, `zero_grad` ceremony, `.data` escape hatch, and in-place
  version-counter footguns (analysis §3.2, `target-experience.md` §2). Recognizable, not faithful.

### 1.4 Relationship to existing constructs
- **The transform intrinsics (`transform-intrinsics-spec.md`).** `Grad(f)` is the entry point;
  this engine supplies the VJP rules `Grad` composes and is the home of the eager-tape driver
  that `Grad`'s §5.5 "two drivers, one registry" refers to.
- **`cajeta.math` (numpy, done).** The differentiable primitives are its tensor ops — `Tensor<T>`,
  the elementwise/reduction/contraction/linalg surface. Their VJP rules are this spec's payload.
- **`nucleo.expr` (the fusion engine).** A differentiated function's backward is ordinary IR that
  the same fusion/DCE/remat passes optimize — a fused forward expression differentiates into a
  fused backward (§7).
- **Records (`records-spec.md`).** The `{value, grads}` companion result is a typed record bag,
  not a positional tuple — full field typing on the gradients.
- **`@NoGrad`/`@Checkpoint`** are annotation **sugar over the mechanism**
  (`transform-intrinsics-spec.md` §9); this spec defines their *effect on differentiation*.

> **TBD (plan-time):** [F2] Whether the tensor-op VJP rules are authored as a compiler-internal
> table for the trusted built-in primitives, as `@Vjp(of: op)`-annotated rule functions, or both
> (the openable surface). Shared with `transform-intrinsics-spec.md` F2 — that spec owns the
> *declaration form*; this spec owns the rule *content*.

## 2. The VJP rule set for tensor primitives

This is the load-bearing payload: one vector–Jacobian-product rule per differentiable núcleo
primitive, registered into the shared registry so any function built from those primitives
becomes differentiable.

**Use cases**
- **2.1** As a núcleo primitive author, when I add a differentiable tensor op (`matmul`, `conv`,
  `softmax`, an elementwise op, a reduction), then I declare its VJP rule — its contribution to
  the backward given an upstream cotangent — and every function using the op differentiates
  through it.
- **2.2** As a developer, when I differentiate a function built only from primitives that have
  registered VJP rules, then `Grad` composes those rules in reverse to a correct backward with no
  per-op authoring on my part.
- **2.3** As a developer, when my function uses a tensor primitive that has **no registered VJP
  rule**, then differentiation fails loud at compile time, naming the primitive that lacks a rule
  — never a silently-zero or wrong gradient (mechanism §5.3).
- **2.4** As a maintainer, when I look for the differentiation rule of an op, then it lives in the
  **one** registry shared with the eager tape — there is no second, divergent rule set (one
  rule-set, two drivers; mechanism §7.4).
- **2.5** As a núcleo primitive author, when an op's VJP is itself expressed in registered
  differentiable primitives, then it composes for higher-order grad for free (§6); an op whose VJP
  is not so expressible must declare a higher-order rule (or higher-order grad through it fails
  loud — mechanism §5.1, F7).

> **TBD (plan-time):** The v1 set of differentiable primitives that must carry VJP rules — the
> minimal cut to render the torch façade and the splat flagship differentiable (matmul/conv/
> normalization/softmax/elementwise/reductions at least) vs. the full `cajeta.math` op tail.

## 3. The forward/backward contract

**Use cases**
- **3.1** As a developer, when I differentiate `loss : (Tensor<f32>) -> f32` with `Grad(loss)`,
  then the **forward `loss` stays intact** (unchanged behavior, same result) and a **backward
  companion** is synthesized alongside it — never a rewrite of the forward.
- **3.2** As a developer, when I call the differentiated form, then I get back an explicit
  `{value, grads}` record (`records-spec.md`) — the loss value and the gradient(s) w.r.t. the
  differentiated input(s) — with **no global `.grad` accumulator** and **no `zero_grad` step**
  (the deliberate correction, `target-experience.md` §2).
- **3.3** As a developer, when the backward is emitted, then it is **ordinary mid-level IR** that
  the fusion/DCE/remat passes optimize like any other code (the whole reason for MIR-level
  placement, analysis §4.4) — not an opaque tape replay.
- **3.4** As a compiler author, when the backward fuses across the forward/backward boundary,
  then it sits at the mid-level IR where contractions are still contractions and shapes/index
  variance are still legible (mechanism §3.3) — so the fused backward is efficient by structure,
  not by post-hoc loop recovery.
- **3.5** As a developer, when grads flow back to a column-backed input (a dataframe/tensor column
  — the splat case, analysis §4.6), then the gradient lands on the **same bytes** the forward
  read (column == tensor-buffer invariant, `nucleo-column-spec.md`), no marshalling.

## 4. Differentiability as a type distinction — `Diff<T>` vs. `T`

Instead of torch's runtime `requires_grad` bool carried on every tensor, núcleo can express
"this value participates in differentiation" **in the type**: a `Diff<T>` wraps a value that
carries a cotangent; a plain `T` does not.

**Use cases**
- **4.1** As a developer, when I declare an input as `Diff<Tensor<f32>>`, then it is statically a
  differentiated input and grads are returned for it; a plain `Tensor<f32>` input is a constant
  w.r.t. the differentiation (the typed analog of torch's per-tensor `requires_grad`, decided at
  compile time, zero runtime flag).
- **4.2** As a developer, when I mix `Diff<T>` and `T` inputs to a differentiated function, then
  the `{value, grads}` bag carries grads only for the `Diff<T>` inputs — which inputs are
  differentiated is visible in the signature, not discovered at runtime.
- **4.3** As a developer, when I read a plain `T` inside a differentiated function, then it
  contributes its value but a zero/absent cotangent, **statically** (no runtime version counter,
  no `.detach()` footgun — the `@NoGrad` discipline, §8, applies at scope granularity, `Diff<T>`
  at value granularity).
- **4.4** As a developer, when `Diff<T>` propagates through an op, then the op's result is
  `Diff<R>` exactly when (any) differentiated input flows into it — taint-propagation through the
  type, so a value's differentiability is never silently lost or silently gained.

> **TBD (plan-time):** [F1] How far `Diff<T>` is the *primary* surface vs. an inference detail.
> Two ends: (a) authors annotate inputs `Diff<T>` and the type propagates explicitly through ops;
> (b) `Grad`'s `argnums`-analog (mechanism F1) picks differentiated inputs and `Diff` is inferred
> internally, the user never writing it. Lean: `Diff<T>` available as the explicit-and-checkable
> surface, with `Grad`-argument selection as the ergonomic default — resolve jointly with
> mechanism F1 and the exact propagation rule through each op kind.

## 5. The runtime eager tape — define-by-run

The second driver over the same VJP registry: a runtime tape that records ops as they execute and
replays the rules in reverse on `.backward()`. This is the torch façade's familiar feel and the
escape hatch for **dynamic control flow** the compile-time `Grad` cannot statically specialize.

**Use cases**
- **5.1** As a torch-façade user, when I run a forward eagerly and call `.backward()` on the
  result, then the tape replays each recorded op's VJP rule in reverse and produces the gradients
  — the define-by-run experience, over the **same registry** the compiled path uses (no second
  rule set).
- **5.2** As a developer with **data-dependent control flow** (loop count / branch chosen at
  runtime), when the compile-time `Grad` cannot specialize the function, then the eager tape
  differentiates it by recording the path actually taken — the eager path is the dynamic-model
  answer, the compiled path the static-graph answer.
- **5.3** As a developer debugging a gradient, when I run on the eager tape, then I get a
  step-by-step, inspectable backward (the debug skin) — and the **same registry rules** guarantee
  the eager and compiled grads agree where both apply.
- **5.4** As a developer optimizing a hot static graph (the splat flagship, millions of splats —
  analysis §4.4/§4.6), when per-op eager dispatch would dominate cost, then I use the compiled
  `Grad` path (fused backward) and the eager tape is *not* on the hot path — eager is the
  compatibility/dynamic skin, compiled is the performance engine.

> **TBD (plan-time):** [F3] The eager tape's **scope and lifetime** — what bounds a tape (a
> lexical region, a fiber/`FiberLocal` context, an explicit tape handle), when it is recorded vs.
> discarded, whether a tape is reusable across `.backward()` calls (retain-graph analog), and how
> it interacts with the carrier/fiber substrate. Couples to the torch-façade `.backward()` surface.

## 6. Higher-order differentiation

**Use cases**
- **6.1** As a developer, when I write `Grad(Grad(loss))`, then the inner `Grad`'s backward is
  itself ordinary registry-composed IR, so the outer `Grad` differentiates it — yielding
  second-order derivatives (Hessian-vector products, etc.) with no separately authored
  second-order driver (mechanism §5.1.1).
- **6.2** As a developer, when every primitive on the path has a VJP expressed in differentiable
  primitives (§2.5), then higher-order grad works without a separately authored higher-order rule;
  a primitive whose VJP is *not* so expressible must declare one, or higher-order grad through it
  fails loud (mechanism §5.1.2, F7).
- **6.3** As a developer, when I take higher-order grad on the eager tape, then the same
  composition holds — the backward's recorded ops are themselves differentiable registry ops.

## 7. Differentiating a fused expression

The engine and `nucleo.expr` meet: a fused tensor expression is differentiated into a fused
backward — one of the architecture's load-bearing wins over a temporary-materializing eager stack.

**Use cases**
- **7.1** As a developer, when I differentiate a fused expression
  `(t - t.mean()) / t.std()` (`nucleo-expr-spec.md` — one fused forward kernel, no temporaries),
  then its backward is synthesized as IR that **fuses with the forward**, so the differentiated
  form is also temporary-free — the language-level answer no eager autodiff stack gives.
- **7.2** As a developer, when I compose `Jit(Grad(f))` (mechanism §6.4), then `Jit` fuses the
  already-differentiated IR into one kernel over the whole forward+backward — the backward is not
  a separate dispatched pass.
- **7.3** As a núcleo author, when an expression lowers to GPU (`cajeta.gpu`, `nucleo-expr-spec.md`),
  then its backward lowers to the same device — gradients computed where the forward ran, no
  host round-trip.

## 8. `@NoGrad` — stop-gradient scopes

`@NoGrad` is defined in `transform-intrinsics-spec.md` §9; this spec specifies its **effect on
differentiation** in the engine.

**Use cases**
- **8.1** As a developer, when I mark a region or a called function `@NoGrad`, then this engine
  treats it as a **constant**: no backward is synthesized through it and gradients do not flow in —
  the disciplined, statically-visible replacement for torch's `torch.no_grad()` / `.detach()`
  (mechanism §9.1).
- **8.2** As a developer, when I read a `@NoGrad` value inside a differentiated function, then it
  contributes its value but a zero/absent cotangent, visible at compile time — no runtime
  version-counter footgun (mechanism §9.2).
- **8.3** As a developer, when I use `@NoGrad` for an inference-only sub-computation inside a
  larger differentiated step, then its omitted backward also **reduces stored activations** (a
  no-backward region needs none) — the memory benefit is automatic.

## 9. `@Checkpoint` — activation store vs. rematerialize

`@Checkpoint` is defined in `transform-intrinsics-spec.md` §9 / `language-foundations.md` §1.7;
this spec specifies the **store-vs-rematerialize trade** the engine makes.

**Use cases**
- **9.1** As a developer, when I mark a function `@Checkpoint`, then the engine **does not store**
  that function's intermediate activations for the backward and instead **rematerializes** them by
  re-running the forward during the backward — trading compute for memory (gradient checkpointing),
  decided per the function's policy.
- **9.2** As a developer training a deep model that runs out of memory storing activations, when I
  `@Checkpoint` the expensive blocks, then peak activation memory drops to the checkpoint
  boundaries at the cost of one extra forward per block — the standard memory/compute lever, opt-in.
- **9.3** As a developer, when I do **not** mark `@Checkpoint`, then the engine's **default
  store-vs-remat policy** applies (some cheap activations may be rematerialized by the fusion pass
  regardless); the annotation is an override, not required ceremony (mechanism §9.4).
- **9.4** As a compiler author, when the backward is emitted as ordinary IR (§3.3), then remat is a
  **natural consequence of the IR being re-derivable and fusable** — `@Checkpoint` steers the
  store-vs-recompute decision the optimizer already makes, rather than bolting on a separate
  checkpointing runtime.

> **TBD (plan-time):** The **default checkpoint policy** when `@Checkpoint` is absent — store-all
> (memory-hungry, torch's default), a fusion-pass cost-model that rematerializes cheap-to-recompute
> activations automatically, or store-all with remat only under memory pressure. Lean: a cost-model
> default that rematerializes cheaply-recomputable activations, with `@Checkpoint` as the explicit
> force-remat override at block granularity. Couples to mechanism F5 (annotation granularity).

## 10. Mixing eager and compiled AD

**Use cases**
- **10.1** As a developer, when my model has a **static fused core** and a **dynamic outer loop**,
  then I differentiate the static core with compiled `Grad` (fused, fast) and the dynamic part with
  the eager tape — both consuming the same registry, their grads composing consistently.
- **10.2** As a developer, when I move a prototype from the eager tape to compiled `Grad`, then the
  gradients are the **same** (one rule-set) — the change is performance, not semantics (the
  define-by-run prototype and the fused production path agree).
- **10.3** As a torch-façade user, when I write define-by-run code with `.backward()`, then I get
  the eager feel by default, and opting a function into `@Grad` upgrades it to the compiled path
  with no rule change — the migration is additive.

## 11. Acceptance criteria (spec-level)
- Each differentiable núcleo/`cajeta.math` tensor primitive carries a VJP rule in the shared
  registry; a primitive with no rule is a hard, named compile error (no silent wrong gradient).
- Differentiating a scalar-valued loss yields an explicit `{value, grads}` record — no global
  `.grad`, no `requires_grad` bool, no `zero_grad` ceremony.
- The forward stays intact; the backward is ordinary mid-level IR that fuses / DCEs / remats.
- The compile-time `Grad` path and the runtime eager tape consume the **same** registry and
  produce agreeing gradients where both apply.
- `Diff<T>` expresses differentiability in the type (vs. a runtime flag), propagating through ops.
- `Grad(Grad(f))` works for differentiably-expressed primitives (higher-order grad).
- A fused forward expression differentiates into a fused backward (no materialized temporaries),
  and lowers its backward to the same device as the forward.
- `@NoGrad` stops gradient through a region/function statically; `@Checkpoint` rematerializes that
  function's activations in the backward instead of storing them.
- Eager and compiled AD compose; moving between them changes performance, not gradient semantics.

## 12. Open questions (resolve at plan time)
- **[F1]** How `Diff<T>` propagates through ops, and whether it is the primary author-facing
  surface or inferred behind `Grad`'s argument selection (§4) — jointly with mechanism F1.
- **[F2]** The VJP-rule **content/authoring** for the tensor primitives — internal table vs.
  `@Vjp` annotation vs. both (§2) — jointly with `transform-intrinsics-spec.md` F2 (which owns the
  declaration form).
- **[F3]** The eager tape's **scope and lifetime** — what bounds/retains/discards a tape, and its
  fiber/carrier interaction (§5).
- The **default checkpoint policy** when `@Checkpoint` is absent (§9).
- The **v1 differentiable-primitive cut** — the minimal op set that must carry VJP rules for the
  torch façade and splat flagship (§2).
