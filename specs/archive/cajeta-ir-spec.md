# Spec: Cajeta IR (CIR) — a semantics-preserving mid-level IR

## 1. Definition

### 1.1 Purpose
Introduce **Cajeta IR (CIR)** — a typed, SSA, control-flow-graph intermediate
representation that sits **between the AST and LLVM IR** and *preserves the language
semantics LLVM discards*: unmonomorphized generics, closures as first-class values,
ownership/borrow, explicit drop points, and the value-vs-heap storage class. CIR is the
platform on which Cajeta-aware optimizations — **monomorphization / closure
specialization** first, then inlining, drop-elision, and stream fusion — become clean,
deterministic passes instead of hacks against an optimizer that can't see what we mean.

The immediate, measurable goal is **zero-cost abstraction for closures in generics**:
a generic function invoked with a statically-known non-capturing closure (the natural-
order `Sort.sort` comparator, a `Stream.map` lambda, a `binarySearch` predicate) must
compile to a **direct, inlined** operation — the same machine code C++ templates and Rust
generics produce — instead of an indirect closure call per invocation.

### 1.2 Problem (what drove this decision)
Cajeta lowers **AST → LLVM IR directly**. By the time code reaches LLVM IR, every
Cajeta-specific fact is gone: generics are erased to a single shape, closures are opaque
`{fn, captures, drop}` records reached through a pointer, ownership/`#`/drop are ordinary
runtime calls, and value-vs-heap is just `ptr`. LLVM therefore *cannot* perform the
optimizations that make a closure-and-generics language fast — it has no notion of "this
indirect call always targets this tiny lambda."

The concrete symptom, measured on the profile suite (`sort-int64`, n=50000):

| pattern    | cajeta (closure) | C++ std::sort | gap |
|------------|------------------|---------------|-----|
| random     | 2.80ms           | 1.88ms        | 1.5× |
| ascending  | 0.78ms           | 0.23ms        | 3.4× |
| dups       | 1.47ms           | 0.50ms        | 2.9× |

Almost the *entire* gap is the comparator: every comparison is an **indirect closure
call** (`(T,T)->int32 cmp` loaded from a record and called), whereas C++/Rust **inline**
the comparator. We proved the ceiling empirically — forcing LLVM's function
specialization (`-force-specialization`) to devirtualize the comparator gave:

| pattern    | + devirt | result |
|------------|----------|--------|
| random     | 1.94ms   | ~matches std::sort |
| ascending  | **0.21ms** | **beats std::sort** |
| dups       | 0.83ms   | 1.7× |

But that LLVM path is unusable as a real solution: (a) it's **narrow** — the cost model
fired only for `sort`, not `binarySearch` or streams; (b) it leans on a **version-fragile
debug flag**; and (c) it **conflicts with ThinLTO** — the specialization happens in the
`ld.lld` backend cross-module where it doesn't fire, and forcing it pre-link **breaks the
link** (private-symbol/summary desync). The experiment was valuable — it *proved the
ceiling is real* — but it confirmed the fix must live above LLVM, in our own IR, where we
can do it **deterministically, totally, and backend-agnostically**.

### 1.3 Solution overview
Build CIR and the **monomorphization / closure-specialization** analysis on it. When a
generic callee invokes a function-typed parameter and the call site supplies a
statically-known non-capturing closure, CIR rewrites that invocation to a **direct call**
to the closure's function, which then inlines. This is pure SSA value-tracking
(`apply_closure(c, …)` where `c` traces to a constant `make_closure(fn, ∅)` → `call fn`)
— no LLVM cost-model roulette, works under any backend including ThinLTO.

This mirrors established practice: **Rust's MIR** (borrow checking, monomorphization,
const-eval) and **Swift's SIL** (ARC/ownership optimization, generic specialization,
devirtualization) exist for exactly this reason — a language with ownership, generics,
and closures needs a semantics-preserving mid-level IR to be fast and safe. Cajeta is in
the same design space and wants the same tool.

**We are building this analysis-only first.** CIR is introduced as a *side analysis* that
drives closure specialization into the existing AST→LLVM codegen — it does NOT replace the
codegen path in this first phase. This is the deliberate, de-risked rollout (chosen over a
big-bang middle-end rewrite): it delivers the comparator/monomorphization win immediately,
stays parity-checkable against today's output, and lets CIR earn its way into the codegen
path. Only in later phases does CIR become the canonical lowering path with its own passes.
See §1.4 for the phase breakdown.

### 1.4 Scope — phased, analysis-first
Rollout is **analysis-only first** (chosen to de-risk a middle-end addition):

- **Phase A (this spec's core): CIR as a side analysis that drives specialization.**
  CIR is built from the AST for a *closed slice* of functions (a generic callee, its
  call sites, and the closures passed to it). It does not yet replace codegen; instead it
  produces **specialization requests** — `(callee, param ↦ known-fn)` bindings — that the
  existing AST→LLVM codegen consumes to emit a specialized instance whose bound-parameter
  invocations are direct, inlinable calls. Lowest risk, parity-checkable, delivers the
  comparator win immediately.
- **Phase B+: CIR becomes a real lowering path + optimizing IR.** AST→CIR→(passes)→LLVM
  for a growing set of functions; passes graduate from "drive codegen" to "transform CIR":
  closure specialization, inlining, ownership-based drop-elision / move-opt, and stream
  fusion. End state: CIR is the canonical middle-end.

This spec defines CIR's form fully (so Phase A's analysis model and Phase B's lowering
agree) and specifies Phase A's compiler behavior concretely. Phase B passes are described
at the design level; their detailed behavior is deferred to follow-on specs.

### 1.5 Constraints
- **Parity:** every Phase-A change must be observably equivalent to today's output except
  for performance — same results, same correctness, same diagnostics. The full test suite
  and benchmark `check=true` gates are the guard.
- **Backend-agnostic:** the win must hold under both non-LTO and ThinLTO (the prior
  funcspec failure mode).
- **Determinism:** specialization decisions are driven by static structure (known
  non-capturing closures), not an opaque cost model — given the same input, the same
  specializations are produced.
- **Soundness around ownership:** CIR must represent `#`/borrow/drop faithfully so later
  ownership passes are correct; Phase A must not perturb drop semantics.
- **No new public language surface** in Phase A (it's an internal optimization); CIR is a
  compiler-internal IR, not a user-facing artifact (though its textual dump is a
  documented internals tool).

### 1.6 Non-goals
- 1.6.1 Replacing the whole middle-end in one step (explicitly phased; Phase A is
  analysis-only).
- 1.6.2 A user-facing/serializable IR format with stability guarantees (the textual dump
  is a debugging/teaching aid, not an ABI).
- 1.6.3 Capturing-closure specialization in Phase A (only non-capturing closures, whose
  target is a known constant function, are specialized first; captured-state
  specialization is later work).
- 1.6.4 Replacing the borrow checker (CIR will eventually *host* borrow-based analyses,
  but the existing lint/debug-runtime borrow path is unchanged here).
- 1.6.5 GPU/@Kernel device lowering (CIR is host-side first; device paths keep their
  current lowering until a later phase).

## 2. Feature: the IR specification (CIR)

CIR is a typed SSA CFG. A program in CIR is a set of **functions**; each function is a CFG
of **basic blocks**; each block is a list of **instructions** ending in a **terminator**;
instructions produce **values**; values carry a **CajetaType** and an **ownership kind**.

### 2.1 Form and structure (use cases)
- 2.1.1 As a compiler author, a CIR **function** records: its name; its (possibly
  unbound) **generic type parameters**; its **formal parameters** (each with a CajetaType
  and ownership kind: `owned`/`borrowed`); a return type + return ownership; and its list
  of basic blocks. A generic function stays generic until a monomorphization pass
  instantiates it.
- 2.1.2 As a compiler author, a **basic block** has a label, an ordered list of **block
  parameters** (SSA values passed on entry — replacing phi nodes, SIL/MLIR-style), a list
  of non-terminator instructions, and exactly one **terminator**.
- 2.1.3 As a compiler author, a **value** is in SSA form (defined once): the result of an
  instruction, a block parameter, a function parameter, or a literal/constant. Every value
  exposes its CajetaType and ownership kind for analyses.
- 2.1.4 As a compiler author, the CFG is well-formed: every block ends in a terminator;
  every value is dominated by its definition; branch targets pass the right block-parameter
  arities/types.

### 2.2 Types and ownership (use cases)
- 2.2.1 CIR types ARE `CajetaType`s (not LLVM types) — primitives, value types
  (`@ValueType`), classes (heap reference), arrays (`T[]` heap, `T[N]` inline), interfaces
  (fat pointer), and **function types** `(T…)->R` as first-class.
- 2.2.2 Each value carries a **storage/ownership kind**: `value` (a value-type/primitive
  held by value), `owned` (a `#`-owned heap reference responsible for its drop),
  `borrowed` (a non-owning reference bounded by a scope). This is the information drop-
  elision and move-opt need and LLVM lacks.
- 2.2.3 **Generic type parameters** are represented symbolically (`T`), so a generic
  function is one CIR body the monomorphization pass clones per concrete type set.

### 2.3 Instruction set (use cases)
The opcode families (a Phase-A subset is lowered; the full set is the target):
- 2.3.1 **Constants:** `const.int`, `const.float`, `const.bool`, `const.str`,
  `const.null`.
- 2.3.2 **Arithmetic / logic / compare:** typed `add/sub/mul/div/rem`, `and/or/xor/shl/shr`,
  `icmp.<pred>` / `fcmp.<pred>` (carrying signedness + overflow mode, since CIR keeps the
  Cajeta type, not the erased LLVM type).
- 2.3.3 **Aggregates:** `field.addr` / `load.field` / `store.field` (struct/class field by
  declared name), `elem.addr` (array index — heap header GEP or `T[N]` inline GEP),
  `extract` / `insert` (value-aggregate element).
- 2.3.4 **Memory & ownership:** `alloc.stack` (value/`stack`), `alloc.heap` (`heap`/`new`),
  `load` / `store`, `move` (ownership transfer — the `#` operator, marks source moved),
  `drop` (an explicit scope-exit drop point for an owned value).
- 2.3.5 **Calls:**
  - `call <fn>(args…)` — a **direct** call to a known function (the monomorphization/
    devirt target).
  - `call.indirect <fnptr>(args…)` — through a raw function pointer.
  - `apply.closure <closure>(args…)` — invoke a closure value through the closure ABI.
  - `call.method <recv>, <selector>(args…)` — virtual dispatch (final-class/known-type
    devirt rewrites this to `call`).
  - `call.generic <fn><T…>(args…)` — a call to a generic function with type args, before
    monomorphization.
- 2.3.6 **Closures:** `make.closure <fn>, [captures…]` → a closure value. A **non-capturing**
  closure (`captures = ∅`) records its target `fn` as a **known constant** — the key fact
  the specialization pass keys on.
- 2.3.7 **Control / terminators:** `br <block>(args…)`, `cond_br <i1>, <t>(args…), <f>(args…)`,
  `switch <int>, [cases…], default`, `return <value?>`, `unreachable`.

### 2.4 Closure representation + the specialization-relevant invariant (use cases)
- 2.4.1 A `make.closure(fn, ∅)` value has a **statically known, unique target** `fn` (the
  synthesized lambda function or a method reference). CIR records target-knownness on the
  closure value.
- 2.4.2 An `apply.closure(c, args…)` whose closure operand `c` **traces** (through SSA
  defs, block params with a single known incoming value, and function-argument
  substitution at a specialized call) to a known `make.closure(fn, ∅)` is **specializable**:
  it can be rewritten to `call fn(args…)`.
- 2.4.3 A closure with captures, or whose target can't be traced to a single known `fn`,
  is **not specializable** in Phase A — it keeps the indirect `apply.closure` (existing
  closure path); behavior is unchanged, only the fast path is added.

### 2.5 Textual form (use cases)
- 2.5.1 As a compiler author, CIR has a **human-readable textual dump** (one function per
  block of text: signature, blocks, typed SSA instructions) so the IR and the
  specialization decisions are inspectable and testable. Example sketch:
  ```
  fn Sort.sort<T>(%a: T[] borrowed, %n: i32, %cmp: (T,T)->i32) -> () {
  bb0(%a, %n, %cmp):
      ...
      %c = apply.closure %cmp(%x, %y) : i32      ; specializable iff %cmp known
      ...
  }
  ```
- 2.5.2 As a compiler author, a flag (e.g. `--emit=cir` / `CAJETA_DUMP_CIR=<fn>`) prints
  the CIR for selected functions before and after the specialization pass, so a test can
  assert that a specializable `apply.closure` became a `call`.

## 3. Feature: compiler behavior (Phase A — analysis-driven specialization)

### 3.1 Pipeline placement (use cases)
- 3.1.1 As a compiler, for the closed slice (a generic function with a function-typed
  parameter, its callers, and the closures passed), **build CIR from the AST** after type
  resolution and before LLVM codegen of that function.
- 3.1.2 As a compiler, **CIR does not replace codegen in Phase A** — it produces
  *specialization requests* that codegen consumes. Functions not in the slice are
  unaffected (existing AST→LLVM path).

### 3.2 The specialization analysis + transform (use cases)
- 3.2.1 As a compiler, at a call to a function `F` that has a function-typed parameter
  `P`, when the matching argument is a **directly-supplied non-capturing closure** (a
  `LambdaExpression` / method reference whose CIR `make.closure` target is known), emit a
  **specialization request** `(F, P ↦ fn)`.
- 3.2.2 As a compiler, materialize a **specialized instance** `F$<fn>` of `F` in which:
  every `apply.closure(P, args…)` becomes `call fn(args…)`; `P` is dropped from the
  signature; and the now-direct call to the small `fn` is marked inlinable so the inliner
  folds it.
- 3.2.3 As a compiler, **redirect the call site** to `F$<fn>` (passing no closure).
- 3.2.4 As a compiler, specialization composes with **type monomorphization**: the
  instance is keyed on `(F, concrete type args, fn-identity)`, so `Sort.sort<int64>` with
  the natural-order lambda yields one cached `Sort.sort<int64>$natfn`.

### 3.3 Caching, determinism, fallback (use cases)
- 3.3.1 As a compiler, specialized instances are **cached** by their key; multiple call
  sites with the same `(types, fn)` share one instance (no code-size blowup beyond one
  copy per distinct specialization).
- 3.3.2 As a compiler, when an argument is **not** a known non-capturing closure (a stored
  closure value, a captured lambda, a runtime-chosen function), **no specialization
  request** is made and the call uses the existing indirect closure path — identical
  behavior to today.
- 3.3.3 As a compiler, the decision is **deterministic and structural** (closure
  knownness), independent of any optimizer cost model or LTO mode.

### 3.4 Correctness + non-regression (use cases)
- 3.4.1 As a developer, a specialized call produces **identical results** to the
  indirect-closure call for every input (the only difference is speed).
- 3.4.2 As a developer, ownership/drop semantics are unchanged — Phase A does not move or
  elide drops (that's Phase B).
- 3.4.3 As an existing program, code that doesn't hit the specialization pattern compiles
  byte-for-byte as before.

### 3.5 What the analysis probes for
Phase A is *analysis-only*: it does not transform CIR, it **interrogates** CIR to extract
a small set of facts per call site, then hands those facts to codegen. The probes:

- 3.5.1 **Closure-target knownness.** For each `apply.closure(c, args…)` reachable in the
  callee, trace the closure operand `c` backward through SSA — instruction defs, block
  parameters with a single known incoming value, and function-parameter substitution at a
  specialized call boundary — to decide whether `c` resolves to exactly one
  `make.closure(fn, …)`. Output: the unique target `fn`, or "unknown".
- 3.5.2 **Capture-freeness.** Whether that `make.closure`'s capture set is empty (target is
  a pure constant function with no runtime state). Only capture-free targets are Phase-A
  specializable; a captured closure carries data the direct call can't reconstruct.
- 3.5.3 **Call-site directness.** At the *caller*, whether the function-typed argument is a
  **directly-written** non-capturing lambda or method reference (vs a closure read from a
  field/local/array, or chosen at runtime). Directness is what makes the target statically
  determinable at the boundary.
- 3.5.4 **Parameter-invocation map.** Which formal parameter is the function-typed one, and
  the complete set of `apply.closure` sites inside the callee that invoke it — so the
  rewrite is *total* (every invocation goes direct, or the specialization is rejected).
- 3.5.5 **Escape & ownership safety.** Whether the closure (or the parameter) escapes or
  participates in drop in a way specialization would perturb — e.g. stored into a field,
  returned, or `#`-moved. Probed against CIR's ownership kinds so Phase A can guarantee
  drop semantics are untouched (§3.4.2).
- 3.5.6 **Target shape (informational).** The size/leaf-ness of `fn` — recorded to predict
  inlinability and, later, to bound code growth; in Phase A we specialize whenever 3.5.1–
  3.5.5 hold and let the inliner decide the fold.
- 3.5.7 **Specialization multiplicity.** The set of distinct `(callee, type-args,
  fn-identity)` keys a compilation actually needs — i.e. how many specialized instances
  the program demands — to keep code growth observable and bounded.

### 3.6 How the probe results drive decisions
The facts from §3.5 drive a deterministic decision per call site, and an aggregate
strategic decision for the project:

- 3.6.1 **Per-site classification.** If (3.5.1 known target) ∧ (3.5.2 capture-free) ∧
  (3.5.3 supplied directly) ∧ (3.5.4 a complete invocation set) ∧ (3.5.5 escape-safe) →
  classify the site **SPECIALIZE**; otherwise **LEAVE-INDIRECT** (existing closure path,
  unchanged behavior). The classification is binary and structural — no cost-model
  threshold, so it is reproducible across builds and LTO modes.
- 3.6.2 **Request generation.** A SPECIALIZE classification emits a *specialization request*
  `(callee F, concrete type-args, parameter P ↦ target fn, invocation-site set)` — the
  exact data codegen needs to materialize the fast instance.
- 3.6.3 **Instance materialization + caching.** Codegen produces `F$⟨types, fn⟩` once per
  distinct key (3.5.7), rewriting each invocation site (3.5.4) to a direct `call fn`,
  dropping `P` from the signature, and marking the direct call inlinable. Cache hits reuse
  the instance — code growth is one body per genuinely distinct specialization.
- 3.6.4 **Redirect / fallback.** The caller is rewritten to call the specialized instance
  with the closure argument removed. LEAVE-INDIRECT sites are emitted exactly as today.
- 3.6.5 **Strategic probe → Phase-B go/no-go.** Because Phase A is itself a deliberate
  *probe of the whole CIR thesis*, its aggregate results gate the bigger investment: we
  measure **coverage** (how many real call sites classify SPECIALIZE — sort, search,
  streams), the **realized speedups** (under ThinLTO, against the §7 targets), the **code-
  growth cost** (instance count from 3.5.7), and **parity** (zero behavioral change). Those
  numbers drive the decision to graduate CIR from an analysis to the **canonical lowering
  path** (Phase B: AST→CIR→passes→LLVM) — or to stop at analysis-only if the wins don't
  generalize beyond what Phase A already bought. This is the "measure before committing the
  middle-end rewrite" gate the analysis-first rollout exists to provide.

## 4. Feature: use cases (the wins)

### 4.1 Closure-in-generics specialization
- 4.1.1 As a developer, `Sort.sort<int64>(a, n)` (natural order) compiles the comparator
  as an **inlined `<` comparison** — sort-int64 approaches/beats C++ `std::sort`
  (ascending beats it; random matches), under ThinLTO.
- 4.1.2 As a developer, `Sort.sort(a, n, (x, y) -> myCompare(x, y))` with a directly-
  written non-capturing lambda is likewise specialized.
- 4.1.3 As a developer, `binarySearch` / `lowerBound` / `upperBound` with a natural-order
  or directly-supplied predicate inline the predicate (closing the gap the LLVM funcspec
  path missed entirely).
- 4.1.4 As a developer, a `Stream` pipeline `xs.map(f).filter(p).reduce(...)` with
  directly-supplied non-capturing lambdas devirtualizes each stage's call (the precursor
  to Phase-B stream fusion).

### 4.2 Foundation for later passes (described; specified later)
- 4.2.1 As a compiler author, CIR's ownership-annotated values enable **drop-elision /
  move-opt** (Phase B): eliminate redundant drop-chain bookkeeping where ownership is
  statically clear (cf. Swift's ARC optimizer).
- 4.2.2 As a compiler author, CIR's first-class closures + inlining enable **stream
  fusion** (Phase B): collapse a `map/filter/reduce` chain into a single loop with the
  lambdas inlined (Rust-iterator "zero-cost" parity).
- 4.2.3 As a compiler author, CIR's retained generics + method dispatch enable broader
  **devirtualization** (known dynamic type / final method) than the current final-class-
  only path.

## 5. Feature: documentation plan

- 5.1.1 **This spec** (`docs/specs/cajeta-ir-spec.md`) — the authoritative why/what.
- 5.1.2 **An internals/architecture doc** (`docs/specification/compiler/CajetaIR.md`) —
  the CIR reference: form, full instruction set with operand/result types, the textual
  grammar, the pipeline placement, and the specialization pass. Kept in step with the
  implementation; the source of truth for contributors.
- 5.1.3 **A Language Guide note** (user-facing, brief) — a "zero-cost abstractions" section
  stating the *guarantee*: passing a non-capturing lambda/method-ref to a generic compiles
  to a direct, inlined call (no per-call closure overhead), so writing `sort(a, n)` or a
  `map().filter().reduce()` pipeline is as fast as a hand-written loop. No CIR internals
  leak into user docs beyond the guarantee.
- 5.1.4 **The CIR textual dump as living documentation** — `--emit=cir` output (§2.5) is
  the canonical illustration; doc examples are generated/checked from real dumps so they
  never drift.
- 5.1.5 **Code-level docs** — the CIR data structures and the specialization pass carry
  header/doc comments tracing back to this spec's section numbers (project convention).
- 5.1.6 **Decision record** — the technical motivation (§1.2, the funcspec experiment +
  why-not-LLVM + the MIR/SIL precedent) is preserved here and summarized in a memory note
  so the rationale survives.

## 6. Feature: tour examples

The Tour (`samples/tour/`) gains examples that *demonstrate the zero-cost guarantee* — each
written to be obviously high-level yet provably fast, with a note pointing at the CIR dump:

- 6.1.1 **Custom-comparator sort** — sort records by a field with a directly-supplied
  lambda comparator; the Tour text notes the comparator inlines (no closure overhead), and
  the example doubles as the §4.1 benchmark shape.
- 6.1.2 **Natural-order sort + search** — `Sort.sort(a, n)` and `binarySearch(a, n, key)`,
  showing the natural-order path is fully specialized.
- 6.1.3 **Stream pipeline** — a `map().filter().reduce()` over a collection with
  non-capturing lambdas, framed as "this reads like three passes but compiles toward one"
  (the fusion teaser; the pipeline already benefits from per-stage devirt in Phase A).
- 6.1.4 **A "look under the hood" example** — a tiny program plus the command to dump its
  CIR (`--emit=cir`), showing an `apply.closure` before and a `call` after specialization,
  so readers can *see* the optimization. This is the teaching artifact that makes CIR
  tangible.
- 6.1.5 (Phase B) **Ownership/drop example** — once drop-elision lands, a Tour example
  showing CIR-level drop reasoning; deferred with that pass.

Each Tour example must compile + run in the Tour harness and, where it claims a perf
property, be backed by a profile-suite benchmark so the claim is verified, not asserted.

## 7. Acceptance themes

- 7.1 **Correctness/parity:** the full test suite stays green; specialized calls produce
  identical results to indirect ones; non-matching code is unchanged.
- 7.2 **Performance:** `sort-int64` (all patterns), `binary-search`, and a stream
  benchmark show the devirt win **under ThinLTO** (the prior funcspec failure mode), with
  `check=true`. Target: sort-int64 ascending ≤ ~0.25ms (beats std::sort), random ≤ ~2.0ms.
- 7.3 **Determinism:** specialization fires from static closure-knownness, not LLVM
  cost-model/LTO state — same input, same specializations (a CIR-dump test asserts the
  `apply.closure → call` rewrite).
- 7.4 **Documentation:** this spec + the CIR internals doc + the Language-Guide guarantee
  + at least the §6.1.1/§6.1.4 Tour examples land with the feature.
- 7.5 **Foundation:** the CIR form is defined fully enough (§2) that Phase-B passes
  (inlining, drop-elision, fusion) can build on it without redefining the IR.
