# Transform Intrinsics — Specification

> Status: draft for review (2026-06-23). A **language feature** (trusted built-in transform
> intrinsics + the VJP rule registry), driven by núcleo; may graduate to
> `docs/specification/lang/` when implemented. Layer-1a foundation, Tier B (bounded IR).
> Companion design: `language-foundations.md` §1.6 / §1.6b.
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions (the
> *how*) are deferred as `> **TBD (plan-time):**` markers and collected in §11, to be resolved
> when this spec is turned into a plan. Outline-numbered for addressability.
>
> **Scope boundary:** this spec is the **mechanism** — the trusted combinators
> `Grad`/`Jit`/`Vmap`/`Pmap`, how the compiler recognizes and specializes them, and the VJP
> rule registry they consume. The **núcleo-facing autograd engine** (the eager tape, the
> tensor-op VJP rule set, `nn`/`optim` integration) lives in `nucleo-autograd-spec.md`; the
> **Tier-A source-synthesis facility** the backward may be delivered through lives in
> `source-synthesis-spec.md`. Records (used in typed return bags here) are in `records-spec.md`.

## 1. Definition

### 1.1 Purpose
The **transform intrinsics** are a small, trusted, core-owned set of **value-level combinators
over function values** — `Grad`, `Jit`, `Vmap`, `Pmap` — that take a function and return a
transformed function. `Grad(f)` differentiates `f`; `Jit(f)` fuses it; `Vmap(f)` batches it;
`Pmap(f)` parallelizes it across carriers. They compose by ordinary code —
`Jit(Vmap(Grad(f)))` — and are the autodiff/transformation hinge the entire ML spine (torch
façade, the splat flagship) stands on. The **VJP registry** is their rule source: each
differentiable primitive declares its vector–Jacobian-product rule, and `Grad` composes those
rules in reverse to synthesize a backward.

### 1.2 Scope
- The four built-in combinators `Grad`, `Jit`, `Vmap`, `Pmap` as **functions over function
  values** (`(T)->R` arguments), recognized by the compiler as **intrinsics** (precedent:
  `Math.*` name-recognition at `MethodCallExpression.cpp:2275+`).
- **Closure specialization** as the delivery path: a statically-known lambda/function passed to
  a combinator has its monomorphized body inlined (cajeta-ir Unit 4), so the transform sees
  `f`'s body at compile time.
- **Composition semantics** — stacking combinators, nesting (second-order `Grad(Grad(f))`),
  and the defined application order.
- The **VJP rule registry** — how a differentiable primitive declares its vector–Jacobian
  product, and how `Grad` consults it.
- The **`@Grad`/`@Jit`/`@Vmap`/`@Pmap` annotations** as **sugar** desugaring to the combinators.
- **Scope annotations** that modulate a transform: `@NoGrad`, `@Checkpoint`.
- The **delivery boundary** — when the backward is emitted as Tier-A source synthesis vs.
  Tier-B bounded IR.

### 1.3 Non-goals
- **The núcleo autograd engine and the tensor-op VJP rule set** — the actual rules for
  `matmul`/`conv`/`softmax`/etc., the eager runtime tape, and `nn`/`optim` wiring. Those live
  in `nucleo-autograd-spec.md`; this spec defines only the *mechanism* they plug into.
- **A library-pluggable transform system.** These intrinsics are **trusted and
  compiler-resident** (Tier B per `language-foundations.md` §1.6) — *never* library-authored.
  User code composes them; user code does not add new ones.
- **Forward-mode (JVP) as the primary surface.** Reverse-mode VJP is the v1 commitment; a JVP
  rule slot in the registry is noted but its combinator surface is deferred.
- **The Tier-A source-synthesis facility itself** — generalizing the `@Logged`/codec pattern
  into a shared helper + registry is specified in `source-synthesis-spec.md`. This spec only
  *consumes* that facility for backward delivery.
- **`@Einsum`** and the dataframe's typed column accessors — Tier-A body/member synthesis,
  out of scope here (`source-synthesis-spec.md`).

### 1.4 Relationship to existing constructs
- Cajeta already has **first-class function types** `(T) -> R`, lambdas/closures, and method
  refs (grammar + `Lambdas.md`) — the combinators are ordinary functions over these.
- **Closure specialization (cajeta-ir Unit 4, shipped)** inlines a statically-known lambda's
  body at the call site (`ClosureSpecializationTests.cpp`, `BoundClosureField.h`). This is the
  exact machinery `Grad(f)` needs: the differentiator sees `f`'s monomorphized body.
- **Intrinsic-function dispatch (shipped)** already recognizes names+signatures (`Math.sqrt`,
  `tryAs<T>`) and emits custom IR instead of a call (`MethodCallExpression.cpp:2275+`). This is
  the proven precedent for trusting `Grad`/`Jit`/`Vmap`/`Pmap` as built-ins.
- **Monomorphization** means a transform sees concrete types (tensor dtype, statically-known
  shape where present), enabling typed diagnostics.
- The **annotation handler registry** (`language-foundations.md` §1.6, Tier B) is where the
  `@Grad`/`@Jit`/`@Vmap`/`@Pmap` sugar is recognized and desugared to the combinator form.

> **TBD (plan-time):** [F6] Do the combinators live as declared intrinsic signatures in a
> núcleo source surface (`Grad<T>((T)->T f) -> (T)->{value,grads}`) recognized by name like
> `Math.*`, or as pure compiler-internal forms with no source declaration? Lean: declared
> núcleo signatures + name-recognition (matches the shipped intrinsic-dispatch precedent and
> gives callers something to import and type against).

## 2. The combinator surface

The four combinators are functions: each takes a function value and returns a transformed
function value.
```cajeta
// Differentiate a scalar-valued function: f returns the transformed function.
var dfdx = Grad(f);                 // (T)->{value, grads}  (companion shape — see §5)
var fast = Jit(f);                  // same signature as f, fused
var batched = Vmap(f);             // f lifted over a leading batch axis
var spread = Pmap(f);              // f distributed across carriers
```

**Use cases**
- **2.1** As a developer, when I write `Grad(f)` for a function value `f` whose body is
  statically known, then closure specialization inlines `f`'s monomorphized body, the compiler
  recognizes `Grad` as an intrinsic, and I get back a transformed function (no library pass,
  no runtime tracer).
- **2.2** As a developer, when I pass a function whose body is *not* statically known (a
  function value resolved only at runtime, behind an indirect call), then the compiler raises a
  clear error (the transform requires a specializable target) rather than silently degrading.
- **2.3** As a developer, when I apply a combinator to a function with the wrong shape for that
  transform (e.g. `Grad` of a function returning a non-differentiable type), then I get a
  compile error naming the offending signature.
- **2.4** As a developer, when I store a transformed function in a variable and call it later
  (`var g = Grad(f); ... g(x)`), then it behaves as an ordinary first-class function value of
  the companion type.

> **TBD (plan-time):** [F1] Exact combinator signatures and template-parameter shape —
> particularly `Grad`'s argument arity (single-arg vs. multi-arg `f`, which argument(s) it
> differentiates with respect to, JAX's `argnums` analog) and whether that is a template
> parameter, a defaulted argument, or a separate combinator.

## 3. Recognition and specialization (the mechanism)

**Use cases**
- **3.1** As a compiler author, when a call to `Grad`/`Jit`/`Vmap`/`Pmap` is encountered, then
  it is matched by **name + signature recognition** (the `Math.*` precedent), not resolved to
  an ordinary user method — these are trusted intrinsics.
- **3.2** As a compiler author, when the function argument is a statically-known
  lambda/function ref, then **closure specialization** (cajeta-ir Unit 4) provides the
  transform with `f`'s **monomorphized body IR**, so the transform operates on real, typed,
  post-resolution IR.
- **3.3** As a compiler author, when the transform runs, then it runs **after type resolution +
  monomorphization, before LLVM lowering** — over the typed mid-level IR where contractions are
  still contractions and shapes/index variance are still legible (the §1.3 mid-level-IR
  constraint from the analysis).
- **3.4** As a developer, when the same function is transformed in two places
  (`Grad(f)` here, `Jit(f)` there), then each transform specializes independently against `f`'s
  body; the intrinsics carry no global mutable state (purity for reproducible builds).

## 4. Composition order

Stacking combinators must have a defined, predictable order. **Rule: nearest the declaration
applies first.** Written as value-level composition, `Jit(Vmap(Grad(f)))` means: `Grad`
differentiates the raw `f`, `Vmap` batches the differentiated form, `Jit` fuses the batched
form. The annotation stack `@Jit @Vmap @Grad func` desugars to exactly this nesting (innermost
annotation = nearest the declaration = applied first), matching JAX's nesting semantics so the
mental model transfers (`language-foundations.md` §1.5).

**Use cases**
- **4.1** As a developer, when I compose `Jit(Vmap(Grad(f)))`, then `Grad` is applied first,
  `Vmap` second, `Jit` last, and the result is one fused, batched, differentiated function.
- **4.2** As a developer, when I write the annotation stack `@Jit @Vmap @Grad`, then it
  desugars to `Jit(Vmap(Grad(f)))` — the annotation nearest the declaration (`@Grad`) is
  applied first, identical to the explicit combinator form.
- **4.3** As a developer, when I reorder the stack (`Vmap(Grad(f))` vs. `Grad(Vmap(f))`), then
  the two produce different transforms (per-example grad vs. grad of the batched function) and
  both compile — order is meaningful, not normalized away.
- **4.4** As a developer, when I write the annotation sugar and the explicit combinator form
  for the same composition, then they are observably equivalent (the sugar is *only* sugar).

## 5. Grad — differentiating a function

`Grad` consumes the VJP registry (§7): it walks `f`'s mid-level IR, looks up each differentiable
primitive's vector–Jacobian-product rule, and composes them in reverse to synthesize a
**backward companion**. The forward `f` stays intact. The companion is reached through a
generated accessor returning a typed bag of value + gradients.

**Use cases**
- **5.1** As a developer, when I differentiate a scalar-valued function
  `Grad(loss)` where `loss : (Tensor<f32>) -> f32`, then I get a companion I can call as
  `loss.withGrads(x) -> { value, grads }` (a typed return record per `records-spec.md`),
  yielding the loss value and the gradient w.r.t. the inputs.
- **5.2** As a developer, when I use the returned grads, then they are **explicit return
  values** — no global `.grad` accumulator, no `requires_grad` bool, no `zero_grad` footgun
  (the deliberate correction from `target-experience.md` §2).
- **5.3** As a developer, when `f` contains a primitive that has **no registered VJP rule**,
  then `Grad` fails loud at compile time, naming the primitive that lacks a rule — never
  silently emits a zero or wrong gradient.
- **5.4** As a developer, when `f` is differentiable but its backward fuses cleanly with the
  forward, then the backward is emitted as ordinary IR that **fuses / DCEs / remats** with the
  rest of the program (the whole reason for mid-level placement).
- **5.5** As a núcleo author, when both the compile-time `Grad` and the runtime eager tape need
  a rule, then both consume the **same VJP registry** (one rule-set, two drivers — the eager
  tape itself is specified in `nucleo-autograd-spec.md`).

> **TBD (plan-time):** [F3] The companion shape and name — `f.withGrads(args) -> {value, grads}`
> vs. `Grad(f)` returning a plain function `(args) -> {value, grads}` vs. both. The combinator
> returns a function value; whether a `.withGrads` accessor is *also* synthesized on the
> original is open (`language-foundations.md` §1.9 F3 left this for jointly resolving here).

### 5.1 Second-order differentiation

**Use cases**
- **5.1.1** As a developer, when I write `Grad(Grad(f))`, then the inner `Grad` produces a
  differentiable backward (its IR is ordinary, registry-composed IR), and the outer `Grad`
  differentiates *that* — yielding a second-order derivative (Hessian-vector products, etc.).
- **5.1.2** As a developer, when a primitive's VJP rule is itself expressed in differentiable
  primitives, then second-order `Grad` works without a separately authored second-order rule
  (the rule's own IR is differentiable); a primitive whose VJP is *not* so expressible must
  declare a higher-order rule or `Grad(Grad(...))` fails loud.

> **TBD (plan-time):** [F7] Whether the registry must carry an explicit higher-order/JVP rule
> slot for primitives whose VJP is not itself composed of differentiable primitives, or whether
> v1 restricts second-order grad to the differentiably-expressed subset and errors otherwise.

## 6. Vmap, Jit, Pmap

**Use cases**
- **6.1 (Vmap)** As a developer, when I write `Vmap(f)` for `f : (T) -> R`, then I get a
  function lifted over a leading batch axis — `f` written for a single example runs over a
  batch with no hand-written loop, batching realized as an IR transform (not a runtime wrapper).
- **6.2 (Vmap)** As a developer, when I `Vmap` a function whose body uses an operation with no
  defined batching rule, then I get a compile error naming it (bounded IR — unsupported
  constructs are rejected, the `@Kernel`/`XPU-N01` precedent), never a silently wrong batch.
- **6.3 (Jit)** As a developer, when I write `Jit(f)`, then `f`'s body is **fused** (temporaries
  eliminated, kernels merged) and the call site has `f`'s same signature — `Jit(f)` is a
  drop-in faster `f`, the language-level answer to numexpr/XLA-tracing
  (`python-stack-analysis.md` §1.3).
- **6.4 (Jit)** As a developer, when I compose `Jit` over a transformed function
  (`Jit(Vmap(Grad(f)))`), then `Jit` fuses the already-batched, already-differentiated IR — one
  fused kernel over the whole composed form.
- **6.5 (Pmap)** As a developer, when I write `Pmap(f)`, then `f` is distributed across carriers
  (the fiber/carrier substrate), the parallel analog of `Vmap`'s batch axis.
- **6.6 (Pmap)** As a developer, when the carrier substrate has constraints (e.g. a transform
  that needs a bare class-method call, single-carrier limits), then `Pmap` reports them as clear
  compile errors rather than miscompiling.

> **TBD (plan-time):** [F8] `Pmap`'s exact semantics and carrier mapping — how it relates to the
> `spawn`/fiber model and existing carrier constraints, and whether v1 ships `Pmap` or defers it
> behind `Vmap`+`Jit` (the immediate ML spine needs `Grad`/`Jit`/`Vmap` first).

## 7. The VJP rule registry

Each differentiable primitive declares its **vector–Jacobian-product** rule. `Grad` walks the
target's IR, looks each primitive's rule up, and composes them in reverse to build the backward.
The registry is the single source of differentiation rules, shared by the compile-time `Grad`
transform and the runtime eager tape (`nucleo-autograd-spec.md`).

**Use cases**
- **7.1** As a núcleo primitive author, when I add a new differentiable op, then I declare its
  VJP rule (its contribution to the backward given an upstream cotangent) so any function using
  the op becomes differentiable through it.
- **7.2** As `Grad`, when I encounter a primitive call in the target IR, then I look up its VJP
  rule in the registry; a missing rule is a hard, named compile error (§5.3).
- **7.3** As a núcleo primitive author, when my op's VJP is itself composed of registered
  differentiable primitives, then it composes for higher-order grad for free (§5.1.2).
- **7.4** As a maintainer, when I read the registry, then it is the *one* place
  differentiation rules live — there is no second, divergent rule set for the eager tape (one
  rule-set, two drivers).

> **TBD (plan-time):** [F2] **Where/how VJP rules are declared** — three candidate forms:
> (a) a `@Vjp(of: someOp)` annotation on a rule function (Tier-A-friendly, discoverable);
> (b) an explicit registration API call at primitive-definition time;
> (c) a compiler-internal table for the trusted built-in primitives.
> Lean: (c) for the trusted built-in tensor ops (they are core-owned anyway), with (a) as the
> openable surface if/when third-party differentiable primitives are allowed. Resolve jointly
> with `nucleo-autograd-spec.md` (which owns the actual rule *content*).

## 8. Delivery boundary — Tier-A source synthesis vs. Tier-B IR

The combinators are **Tier B** (trusted, core-owned, bounded IR). But per
`language-foundations.md` §1.6b, `Grad`'s **backward can be delivered as Tier-A source
synthesis** — generating cajeta source that re-enters parse → type-check → borrow-check →
codegen (the `@Logged`/codec model, `source-synthesis-spec.md`) — wherever the backward can be
expressed in checked primitives, dropping to Tier-B raw IR **only** where fusion demands it.
Tier A is preferred because its output is re-checked exactly like hand-written code (safe by
construction); Tier B is reserved for the fusion-critical hot path.

**Use cases**
- **8.1** As a compiler author, when a primitive's backward is expressible as a call to existing
  checked primitives, then I synthesize it as **Tier-A source** (re-checked, cannot emit unsound
  IR), keeping autodiff in the safe tier.
- **8.2** As a compiler author, when fusion across the forward/backward boundary requires raw IR
  the source surface can't express, then I drop to **Tier-B bounded IR** for that span only, with
  the rest of the backward still source-synthesized.
- **8.3** As a developer, when the backward is delivered either way, then the observable result
  (value + grads) is identical — the tier is an implementation choice invisible at the call site.

> **TBD (plan-time):** [F4] **The precise source-synth-vs-IR boundary for the backward** — a
> decision rule for which spans go Tier-A vs. Tier-B (e.g. "everything Tier-A unless a fusion
> opportunity crosses a primitive boundary"), and how much shape/type info the mid-level IR must
> carry for the Tier-B spans to fuse and to produce good diagnostics. Couples to §10's
> diagnostic-quality bar and to `source-synthesis-spec.md`'s facility surface.

## 9. Scope annotations — @NoGrad, @Checkpoint, @Autocast

These annotations **modulate** a transform within a function body; they are sugar/policy over
the same mechanism, not separate combinators. They are Tier-B recognized (trusted, core-owned);
façades skin them (e.g. the torch façade surfaces `@Autocast` as `torch.amp` —
`torch-facade-spec.md` §8 — and re-specifies none of its mechanics).

**Use cases**
- **9.1 (@NoGrad)** As a developer, when I mark a region (or a called function) `@NoGrad`, then
  `Grad` treats it as a constant — no backward is synthesized through it, gradients do not flow
  in (the disciplined replacement for torch's `torch.no_grad()` / `.detach()`).
- **9.2 (@NoGrad)** As a developer, when I read a `@NoGrad` value in a differentiated function,
  then it contributes its value but a zero/absent cotangent, and this is statically visible (no
  runtime version-counter footgun).
- **9.3 (@Checkpoint)** As a developer, when I mark a function `@Checkpoint`, then `Grad`
  **rematerializes** its activations in the backward instead of storing them — trading compute
  for memory, decided per the function's policy (`language-foundations.md` §1.7).
- **9.4 (@Checkpoint)** As a developer, when I do *not* mark `@Checkpoint`, then the default
  store-vs-remat policy applies; the annotation is an opt-in override, not a required ceremony.
- **9.5 (@Autocast)** As a developer, when I mark a region `@Autocast`, then ops within it run at
  a reduced precision policy (mixed-precision) while gradients/accumulation stay at full
  precision — a Tier-B recognized scope annotation composing with `Grad` per §4. The torch
  façade's `torch.amp` (`torch-facade-spec.md` §8) is a thin skin over this; the mechanism lives
  here, not in the façade.

> **TBD (plan-time):** [F5] Whether `@NoGrad`/`@Checkpoint` apply at function granularity only,
> or also to lexical blocks / expression regions (a `noGrad { ... }` scope form), and the exact
> surface for each.

## 10. Diagnostics

The intrinsics' value is partly in **good compile errors** — the moat is "shape errors are
compile errors, not 2 a.m. stack traces" (`target-experience.md` §2, `python-stack-analysis.md`
§1.3).

**Use cases**
- **10.1** As a developer, when a transform can't proceed (no VJP rule, no batching rule,
  non-specializable target, wrong return type), then I get a **named, located** compile error
  pointing at the offending op/primitive/signature — not a generic failure.
- **10.2** As a developer, when shape/dtype information is statically known, then a transform
  uses it to sharpen the diagnostic (e.g. a rank/shape mismatch in a contraction the backward
  would build), matching the `@Einsum` diagnostic bar (`language-foundations.md` §1.4).

> **TBD (plan-time):** [F4] (shared with §8) How much shape/type info the mid-level IR must
> carry for these diagnostics — ties to how much shape is statically known and to the IR the
> Tier-B spans operate over.

## 11. Acceptance criteria (spec-level)
- `Grad`/`Jit`/`Vmap`/`Pmap` are usable as **value-level combinators over function values** and
  compose as `Jit(Vmap(Grad(f)))`, with composition order = nearest-the-declaration-first.
- `@Grad`/`@Jit`/`@Vmap`/`@Pmap` annotations desugar to the combinator form and are observably
  equivalent to it.
- A statically-known function passed to a combinator is specialized via closure specialization;
  a non-specializable target is a clear compile error.
- `Grad` of a scalar-valued function yields value + explicit grads (no global `.grad` state).
- Second-order `Grad(Grad(f))` works for differentiably-expressed primitives.
- Each differentiable primitive declares a VJP rule; a missing rule is a hard, named compile
  error (no silent wrong gradient).
- The same VJP registry drives both `Grad` and the eager tape (one rule-set, two drivers).
- The intrinsics are trusted/core-owned and never library-pluggable; the backward may be
  delivered via Tier-A source synthesis where fusion allows, Tier-B IR only where needed.
- `@NoGrad`/`@Checkpoint` modulate `Grad` (stop-gradient / rematerialize) as specified.
- Transform failures produce named, located diagnostics.

## 12. Open questions (resolve at plan time)
- **[F1]** Exact combinator signatures and `Grad`'s differentiation-argument selection (§2).
- **[F2]** Where VJP rules are declared — `@Vjp` annotation vs. registration API vs. internal
  table (§7); jointly with `nucleo-autograd-spec.md`.
- **[F3]** The companion shape/name returned by `Grad` — `withGrads` accessor vs. plain returned
  function vs. both (§5).
- **[F4]** The Tier-A-source-synth vs. Tier-B-IR boundary for the backward, and how much
  shape/type info the IR must carry for fusion + diagnostics (§8, §10).
- **[F5]** `@NoGrad`/`@Checkpoint` granularity — function-only vs. lexical-block scopes (§9).
- **[F6]** Whether combinators are declared núcleo intrinsic signatures (name-recognized) or
  pure compiler-internal forms (§1.4).
- **[F7]** Higher-order/JVP rule slot in the registry for non-differentiably-expressed
  primitives, vs. v1 restricting second-order grad to the expressible subset (§5.1).
- **[F8]** `Pmap` semantics + carrier mapping, and whether `Pmap` ships v1 or defers behind
  `Vmap`/`Jit` (§6).
