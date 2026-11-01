# Núcleo — Language Foundations (design sketch)

> Status: design sketch (2026-06-23). The moat reduces to two foundational language
> pieces, and they compose. This sketches both at design level and is not yet a formal spec.
> They are language-level features driven by núcleo, and they may graduate to
> `docs/specification/lang/` when formally spec'd. Companions: `python-stack-analysis.md`,
> `target-experience.md`.
>
> The reduction came from einsum-as-`@Einsum`, an annotation that synthesizes a body, which
> removed the separate "compile-time string DSL" item. What remains:
> 1. **The annotation-synthesis mechanism** generalizes `@Logged`/`@Kernel` into an
>    extensible, IR-level handler. It carries `@Grad`, `@Jit`, `@Vmap`, `@Einsum`, `@Kernel`.
> 2. **Records** are typed value-aggregates. They carry typed schemas (`Table<Tick>`), typed
>    return bags, and dataframe rows.
>
> They compose. The typed dataframe needs records for the schema and the synthesis
> mechanism to generate `Table<Tick>`'s typed column accessors. The synthesis mechanism
> is the deeper investment, and records sit beside it.

---

## 1. The annotation-synthesis mechanism

### 1.1 What exists, and what we generalize
Cajeta already has two special-cased compile-time annotation behaviours:
- **`@Logged`** synthesizes a member (a `static Logger` field) via source-fragment parse
  and AST injection.
- **`@Kernel`** lowers a function to device IR (SPIR-V/NVPTX) at the mid-level stage.

Both are hardcoded. The generalization is a registry of handlers, each keyed to an
annotation, each able to inspect and synthesize or transform the typed mid-level IR of the
annotated declaration. `@Grad`, `@Jit`, `@Vmap` and `@Einsum` become handlers alongside
`@Kernel`, and `@Logged` is re-expressed as one.

### 1.2 The three powers a handler may exercise
These form a spectrum, from least to most invasive. We name them so each annotation declares
which it uses:
- **(a) Member synthesis** injects new declarations into the enclosing type. *(`@Logged`)*
- **(b) Body synthesis** provides the body of a declared-but-bodyless method from the
  annotation's arguments. It works like a parameterized intrinsic. *(`@Einsum`)*
- **(c) IR transform** rewrites or augments a method's IR, optionally producing a companion
  function. *(`@Grad`, `@Jit`, `@Vmap`, `@Kernel`)*

### 1.3 Where handlers run — the mid-level IR, and why it's non-negotiable
Handlers run after type resolution and monomorphization, and before LLVM lowering. They run
over the typed mid-level IR, where contractions are still contractions and shapes and
index-variance are still legible. This is the same constraint the autodiff-placement decision
imposed (§4.4 of the analysis). The structure an `@Grad` or `@Einsum` handler needs is
destroyed by LLVM-level lowering. So the mechanism is "a registered pass over the typed
mid-level IR, keyed by annotation." This is also why núcleo cannot be a pure `.cja` library.
The handlers are compiler-resident.

### 1.4 The handler interface (conceptual)
A handler is a pure function `(declaration, annotationArgs) -> emitted IR`. It is given:
- the annotated declaration's typed IR, meaning the signature (parameter types including
  tensor dtype and any statically-known shape, and the return type) and, for power (c), the
  body IR;
- the annotation arguments, already parsed to typed literals (string / int / type-ref /
  list, the forms annotations already capture);
- an emit builder, to add members, generate a body, or synthesize a companion;
- a diagnostics sink, to raise good compile errors (e.g. *"`@Einsum` spec
  `'bhqd,bhkd->bhqk'` expects rank-4 inputs; `query` is rank-3"*).

Purity is required for reproducible builds. There is no I/O, no global state, and the output
is deterministic.

### 1.5 Composition order (stacked annotations)
`@Jit @Vmap @Grad func` must have a defined order. The rule is that the annotation nearest the
declaration applies first. So `@Grad` differentiates the raw function, `@Vmap` batches the
differentiated form, and `@Jit` fuses the batched form. This reads as `Jit(Vmap(Grad(func)))`,
matching JAX's nesting semantics, so the mental model transfers.

### 1.6 Registration — the safe model (resolved by a compiler audit)
A read of all 43 special-cased annotations in the compiler settles the security question.
No existing annotation runs arbitrary logic. None is parameterized by user code, only by
literal args. There is no plugin system. Annotations are DATA, and handlers are
compiler-internal. The real work splits two ways, and both are safe:

- **Tier A — source synthesis (the `@Logged` / codec model).** The handler generates cajeta
  source that re-enters the normal parser → type-check → borrow-check → codegen pipeline.
  It is safe by construction. The output is re-checked exactly like hand-written code and can
  only call existing, checked primitives, so it cannot emit unsound IR. This is leverage of
  primitives to produce workflow, with no new code introduced. The precedent is `@Logged`,
  which synthesizes a `log` field via `synthesizeLoggerField`, and all codec annotations
  (`@JsonProperty`/`@CsvColumn`/… drive `synthesizeJsonMethodSource()`/
  `synthesizeCsvMethodSource()`, generating method bodies that re-parse). This tier carries
  `@Einsum`, the dataframe's typed column accessors, and essentially every DSL/boilerplate
  generator. Because the output is re-checked, this is the tier that could eventually open to
  libraries at low risk.
- **Tier B — bounded IR transform (the `@Kernel` model).** The handler emits or transforms IR
  by fixed, grammar-bounded rules, and unsupported constructs are rejected with errors (e.g.
  `XPU-N01`). It is powerful, and it is trusted and compiler-resident, never library-pluggable.
  The precedent is `@Kernel`/`@Device` (device lowering), `@Native` (forwarder), `@Inject`, and
  AOP advice. This tier carries the small trusted set of deep transforms, `Grad`/`Jit`/`Vmap`.

So the security concern dissolves without surrendering anything. The risky power (raw IR) is
a small core-owned set, and the extensible power (source synthesis) cannot produce unsound
code. We are not adding a plugin system. We are formalizing two patterns the compiler already
uses. Today they are ad-hoc, with an inline ANTLR-boilerplate in `@Logged`, an `if-else`
synthesizer chain at `MethodTemplateInstantiator.cpp:343` with no shared helper, and scattered
`if (findAnnotation(...))` checks. The work is a reusable source-synthesis facility plus a
registry replacing the scatter. It is engineering, with no new risk.

### 1.6b First-class function transforms are nearly shipped (the `Grad(f)` route)
The companion audit found the machinery for value-level transforms already exists:
- **First-class function types** `(T) -> R` (grammar + `Lambdas.md`), lambdas/closures, method refs.
- **Closure specialization (cajeta-ir Unit 4, shipped).** When a function template is called
  with a statically-known lambda, the compiler generates a specialized instance with the
  lambda's body inlined (`ClosureSpecializationTests.cpp`, `BoundClosureField.h`). This is
  exactly what `Grad(f)` needs. The differentiator sees f's monomorphized body at compile time.
- **Intrinsic-function dispatch (shipped).** The compiler already recognizes names and
  signatures (`Math.sqrt`, `tryAs<T>`, …) and emits custom IR instead of a call
  (`MethodCallExpression.cpp:2275+`). This is the proven precedent for a trusted built-in
  `Grad`/`Jit`/`Vmap`.

So `Grad(f)` is roughly three bounded steps. Declare `Grad<T>((T)->T f)` in núcleo, recognize
it as an intrinsic, and emit the backward. The backward can be delivered as Tier-A source
synthesis, reusing the Json-synthesizer hook pattern over the specialized body, which keeps
autodiff in the safe tier where feasible, with Tier-B IR only where fusion demands it. The
annotation `@Grad` becomes sugar that desugars to `Grad(f)`. Combinators are the primary
surface (per directive), composable as `Jit(Vmap(Grad(f)))` by ordinary code, with no
library-authored compiler passes.

### 1.7 Worked example A — `@Grad` (the autodiff hinge)
`@Grad` uses power (c), IR transform plus companion synthesis.
- **Inputs:** a function `f : (inputs...) -> scalar` (or, for Jacobians, `-> tensor`).
- **Rule source:** a VJP registry. Each differentiable primitive op in
  `cajeta.math`/núcleo declares its vector-Jacobian-product rule, plausibly via a
  small annotation, `@Vjp(of: someOp)`, or a registration call. This registry is the
  "one rule-set" from §4.4.
- **The transform:** walk `f`'s mid-level IR, look up the VJP of each primitive, and compose
  them in reverse to synthesize a backward companion. The forward `f` stays intact.
- **What's exposed:** a generated companion reached as `f.withGrads(args) -> {value, grads}`
  (name TBD). The backward is ordinary IR, so it fuses, DCEs and remats with everything else.
- **Activations:** store-vs-rematerialize is decided per the `@Checkpoint` policy on the function.
- **Two drivers, one registry:** the compile-time `@Grad` handler and the runtime eager
  tape both consume the same VJP registry. `@Grad` is the fast, fused path. The tape replays
  the rules at runtime for define-by-run and dynamic-control-flow cases, which is the torch
  façade's feel.

### 1.8 Worked example B — `@Einsum` (the DSL-as-annotation)
`@Einsum` uses power (b), body synthesis.
```cajeta
@Einsum("bhqd,bhkd->bhqk")
Tensor<float32> attentionScores(Tensor<float32> query, Tensor<float32> keys);
```
- The handler has the string and the declared signature. It parses the spec, then
  checks it against the types. Input label-groups must match parameter ranks, every contracted
  index must appear in at least two inputs, and output labels must agree with the declared
  return type. A mismatch is a compile error.
- It synthesizes a fused contraction kernel as the body (a parameterized intrinsic).
- The call site is clean and typed, `attentionScores(q, k)`, with no DSL string visible to
  callers.
- A runtime sibling stays for one-offs, `Tensor.einsum("ij,jk->ik", a, b)`, which is a
  lowerCamel method, runtime-parsed and numpy-familiar.

### 1.9 Open forks
- **F1. RESOLVED (audit).** Registration is the two-tier safe model (§1.6), Tier-A
  source-synthesis (safe, eventually openable) plus Tier-B bounded-IR intrinsics (trusted,
  core-owned). There is no plugin system.
- **F2.** Where VJP rules live, whether a `@Vjp`-style annotation on each op, an explicit
  registration API, or a compiler-internal table. *(Still open.)*
- **F3. RESOLVED (directive + audit).** The primary surface is value-level combinators
  `Grad(f)`, `Jit(Grad(f))`, viable now via shipped closure specialization and intrinsic
  dispatch (§1.6b). `@Grad` is sugar desugaring to `Grad(f)`. A `f.withGrads(...)` accessor may
  still exist as the companion shape the combinator returns.
- **F4.** The error-quality bar, meaning how much type/shape info the IR must carry for good
  diagnostics. It ties to how much shape is statically known (see §2.6). *(Still open.)*

---

## 2. Records

### 2.1 Motivation
`structure` / `record` are reserved but unimplemented. Records unlock three things at once:
- **Typed dataframe schemas** such as `Table<Tick>`, the distinctive, Python-impossible dataframe.
- **Typed multiple-return** such as `QrResult { q, r }` instead of scipy's positional tuple bags.
- **Row model** for a single dataframe row, and named aggregates generally.

### 2.2 The model
A record is a named, immutable value-type aggregate of typed fields:
```cajeta
record Tick { Instant ts; float64 price; float64 size; Symbol venue; }
```
- **Value semantics.** A record is copied rather than heap-referenced by default, which fits
  Cajeta's `@ValueType` story and the deterministic-memory model.
- **Immutable by default.** A record is construct-complete with no field mutation, and a
  "change" constructs a new one. This avoids the mutation footguns the dataframe design also
  rejects.
- **Construction** via the existing aggregate initializer, `Tick { ts: t, price: p, size: s,
  venue: v }` (named) and/or positional.

### 2.3 Named records + reflection, NOT anonymous structural types
A key simplification is that the typed-schema moat needs named records plus reflection over
their fields, and it does not need anonymous structural types. The earlier
`Table<{ts: Instant, price: f64}>` inline-structural form is not required. `record Tick {...}`
then `Table<Tick>` gives the same power with a name and is far more tractable, since there is
no structural typing and no anonymous-type unification. Cajeta already has reflection
(`Class<T>`, field enumeration) and monomorphizes, so `Table<Tick>` is a distinct type whose
field layout is statically known.

### 2.4 How records + synthesis produce the typed dataframe (they compose)
`Table<T>` wants compile-time typed column access, so that `ticks.price` autocompletes and a
typo is a compile error. That comes from generating an accessor per field of `T`. There are
two routes:
- **Reflection-only** gives dynamic field access and loses compile-time `.price`. It is
  insufficient for the moat.
- **Synthesis (recommended)** has `Table<T>` lean on the annotation-synthesis mechanism
  (§1) to generate a typed column accessor per field of `T`, reflected from the record. This
  is why the two foundational items compose. Records describe the schema, and the synthesis
  mechanism turns it into typed accessors. The column-expression form (`col.price`,
  Polars-style) is the same field set wearing an expression-builder hat.

### 2.5 Physical layout — record is the schema, storage is columnar
One distinction is important. `Table<Tick>` is struct-of-arrays, with one column buffer per
field, and it is never an array of `Tick` records. The record is the type-level schema
descriptor. The table transposes it to columns, and, per §2.3 of the analysis, each non-null
field-column is a tensor buffer. A single `Tick` instance (one row) is AoS, and the table is
SoA. Records model the row, and the engine owns the column transpose.

### 2.6 Destructuring (separable ➕ sugar)
Records alone already give typed return bags via field access:
```cajeta
var res = linalg.qr(a);   use res.q, res.r;     // works with records, no new syntax
```
Destructuring sugar, `var (q, r) = linalg.qr(a)` or `var {q, r} = ...`, is a separable
add-on in the ➕ tier. It is nice, and it is not required for the capability.

### 2.7 Open forks
- **F5.** Record mutability, strictly immutable (recommended) vs. allowing a `mut` opt-in.
- **F6.** Records vs. existing `@ValueType` classes. Is `record` distinct sugar, or does it
  lower to a `@ValueType` class? Lowering reuses machinery, and a distinct surface keeps intent
  clear.
- **F7.** Schema access ergonomics, synthesized member accessors (`ticks.price`) vs. a
  `col.price` expression builder vs. both.
- **F8.** Destructuring syntax, if and when added, `(q, r)` positional vs. `{q, r}` by-name.

---

## 3. The logical progression — foundation first, then verticals
This is grounded in what the code audit confirmed is shipped vs. needed. Each layer is a clean
dependency cut. Nothing in a layer starts before the layer beneath it lands.

**Layer 0 — Substrate (DONE).** `cajeta.math.Tensor` (numpy), `cajeta.xpu` (the multi-target
compute/kernel model for CPU/NVPTX/AMDGPU/SPIR-V) and codec IO, plus the shipped language
enablers this whole plan leans on. Those enablers are named args with defaults, operator
overloading, monomorphization, first-class function types, lambdas/closures, closure
specialization (a static function arg inlined, cajeta-ir Unit 4), method-level templates,
intrinsic-function dispatch (`Math.*` etc.), and reflection.

**Layer 1 — Foundation (language + núcleo core).**
- *L1a — language enablers* (small; leverage the shipped machinery above):
  - **Records** are the one genuinely new data-modeling primitive.
  - **Reusable source-synthesis facility** generalizes the `@Logged`/codec pattern into a shared
    helper plus a registry (Tier A). It carries `@Einsum`, dataframe accessors and boilerplate.
  - **Trusted transform intrinsics** `Grad`/`Jit`/`Vmap` are recognized like `Math.*`, specialized
    via closure specialization, and composed by the VJP registry (Tier B). `@Grad` is sugar over
    these.
  - **Cheap sugar (➕):** `@` matmul, bracket slicing + `...`/newaxis, unary `-`, destructuring,
    type aliases.
- *L1b — núcleo core* (on L1a + substrate):
  - `column` (Arrow layout + C Data Interface) + the Tensor Arrow retrofit (alignment + C-Data seam).
  - `expr` (fusion) · `autograd` (VJP registry + `Grad` + eager tape) · `nn` / `optim`.
  - `frame` (typed dataframe = records + source-synthesized accessors) · `sparse` · `linalg` · `index`.

**Layer 2 — Verticals (on the foundation).**
- Façades: torch → keras → scipy → pandas-skin.
- Flagship: differentiable splat rendering. It rides `Grad` and the column engine, and it is
  the integration proof.
- Lineages: `trees` (gradient boosting / XGBoost) · classical sklearn.

This supersedes the analysis-doc §5 sketch. Within L1a, records and the source-synthesis
facility come first, because they unblock typed returns, the schema type, and `@Einsum`. The
`Grad`/`Jit`/`Vmap` intrinsics follow, and they are bounded because the closure-specialization
and intrinsic-dispatch machinery is already shipped. The flagship is the first Layer-2 milestone
because it exercises the whole stack.
