# Cajeta IR (CIR) — Phase B specification

> Status: DRAFT (authored with the design skill, scoping approved 2026-06-21:
> **B-lite** — extend the analysis-driven specialization; do NOT build the canonical
> AST→CIR→passes→LLVM rewrite yet). Sibling of `cajeta-ir-spec.md` (Phase A).

## 1. Definition

### 1.1 Purpose
Phase A delivered closure devirtualization as **analysis-driven codegen specialization**
— a generic method invoked with a directly-supplied non-capturing lambda is monomorphized
into `F<types>$fn` whose comparator calls go direct. It banked the headline (sort-int64
**0.888 → 0.204 ms, 4.35×** under ThinLTO, zero correctness regressions, negligible code
growth), and it did so **without** a CIR lowering path. Phase B closes the *remaining,
mostly-measured* gaps that the same mechanism can reach — and gates the one expensive,
unmeasured idea (a middle-end optimization pass) behind a probe.

### 1.2 Problem (what Phase A left, with evidence)
- **Forwarding chains (MEASURED gap).** `binary-search` did **not** improve (~same, 3.61ms
  before and after) because `binarySearch` *forwards* its comparator to `lowerBound`/
  `upperBound`. The comparator escapes the directly-invoking frame, so Phase A's
  escape-robust path keeps it indirect-on-constant rather than devirtualized.
- **Capturing lambdas (BLOCKED gap).** A lambda that captures state is not specialized; the
  Phase-A builder records the capture flag but the **frontend rejects captures entirely**
  (`Expression.h:589`), so the case is unreachable today.
- **Natural-order sorts (ADJACENT gap).** A sort/search invoked with no comparator (natural
  `<` order) goes through a comparator closure internally; even the 2-arg `Sort.sort` builds
  a lambda. A `<`-based specialization could devirtualize order comparisons directly.
- **Middle-end optimizations (UNMEASURED).** Drop-elision, move-optimization, and operation
  fusion — the original CIR promise — have **no probe** showing they win. Phase A's thesis
  was "measure before committing the middle-end rewrite"; that measurement has not been taken.

### 1.3 Solution overview (B-lite)
Extend the **existing** specialization machinery — `instantiateSpecializedClosure`,
`BoundClosureField`, the `invokeMethod` redirect — to reach the forwarding, capturing, and
natural-order cases. **No** canonical CIR lowering path is built in Phase B. The one
optimization-pass idea (drop-elision/move-opt) is scoped as a **measurement probe feeding a
second go/no-go**, not as an implementation. Every extension preserves Phase A's invariants:
deterministic + structural classification, parity (identical results; non-matching code
unchanged), survival under ThinLTO, and bounded code growth.

### 1.4 Scope
In scope (this spec): §2 forwarding-chain specialization (implement), §3 capturing-lambda
specialization (implement) **plus its enabling prerequisite — frontend closure-capture support
(§3.4–§3.5), now in scope as actionable work rather than an external blocker**, §4 natural-order
operator specialization (implement), §5 drop-elision/move-opt **probe + go/no-go** (measure, do
not build).

### 1.5 Constraints
- **Parity:** every specialization produces bit-identical results to the indirect path; any
  call that does not match a specialization pattern compiles byte-for-byte as today.
- **ThinLTO:** all wins must hold in the release/ThinLTO (AOT) build, not only JIT — the
  Phase-A regression (cross-module reference to an internal lambda symbol → ld.lld crash)
  proved AOT must be verified explicitly.
- **Determinism:** classification stays structural (closure knownness / call-graph shape),
  never a cost-model heuristic.
- **Bounded growth:** one specialized instance per genuinely distinct
  `(callee, type-args, fn-identity[, chain])` key; code size growth stays observable.

### 1.6 Non-goals
- The canonical AST→CIR→passes→LLVM lowering path (deferred; revisited only if §5's probe
  says the middle-end optimizations pay off).
- Runtime polymorphic inline caches / speculative devirtualization — Phase B stays fully
  compile-time and total.
- Specializing genuinely runtime-chosen closures (a function value selected at runtime stays
  indirect, by design).

## 2. Feature: forwarding-chain specialization

When a specialized instance `F$fn` passes its now-known closure to another generic callee
`G`, that forwarded call should specialize too, so the chain devirtualizes end-to-end.

- 2.1 As the compiler, when building `F<types>$fn` and a `BoundClosureField` parameter `P`
  is passed as the closure argument to a generic call `G<...>(…, P, …)` **inside F's body**,
  emit a forwarded specialization request `(G, types', P↦fn)` so `G` is specialized over the
  same known target.
- 2.2 As the compiler, the forwarded closure value is statically known (it is the bound `fn`,
  not a runtime value), so `G`'s specialization uses the same constant target — `binarySearch
  $fn` forwards to `lowerBound$fn` / `upperBound$fn`, each with direct comparator calls.
- 2.3 As the compiler, forwarding-chain specialization is **transitive but terminating**:
  follow forwarded-known-closure edges through the call graph, with a depth/visited guard so
  a cycle or a deep chain can't loop or explode code size.
- 2.4 As the compiler, when a forwarded call's closure operand is **not** statically known
  (a runtime value, a different closure), the forwarded call is left indirect — only the
  known-closure edges propagate.
- 2.5 As a developer, `binary-search` (and any forward-then-invoke generic) **measurably
  improves** over the Phase-A baseline, with identical results.

## 3. Feature: capturing-lambda specialization

A lambda that captures state can still be specialized: the target function is known; the
captures are threaded rather than reconstructed.

- 3.1 As the compiler, when a directly-supplied lambda **captures** (non-empty environment)
  and its target fn is statically known, specialize the callee over that fn while **passing
  the captures** to the direct call (the captures are runtime data; only the *dispatch* is
  removed).
- 3.2 As the compiler, the specialized direct call threads the closure record's captures
  pointer as the call's implicit environment argument — identical ABI to the indirect path,
  minus the indirect dispatch.
- 3.3 As the compiler, escape/ownership safety still gates: a capturing closure that escapes
  or participates in drop in a way specialization would perturb stays indirect (Phase A
  §3.5.5 carries over).
- 3.4 **Enabling prerequisite (in scope).** The frontend does not yet support captures at all
  (`Expression.h:589` — a lambda referencing outer-scope names is a codegen error). Rather
  than leave §3 blocked on an external dependency, Phase B **includes the work to unblock it**:
  building frontend closure-capture support (the "L2 closures" tier — capture analysis, a real
  captures environment in the `{fn, captures, drop}` record, by-value / by-borrow / `#`-owned
  capture classification, and borrow scope-bounding). §3.5 states that prerequisite's
  requirements; the specialization (§3.1–§3.3) builds on top.
- 3.5 **Frontend capture support (the unblocker).**
  - 3.5.1 As a developer, a lambda that references an outer local **compiles and runs** — by
    value (primitive copy), by borrow (heap reference, scope-bounded), or `#`-owned (moved in,
    dropped via the closure's drop slot).
  - 3.5.2 As the compiler, a borrow-capturing closure is **scope-bounded**: returning or
    storing it past the declaring scope is rejected (the `hasBorrowCaptures` escape check).
  - 3.5.3 As the compiler, an owned (`#`) capture is dropped via the closure record's drop
    slot at end of life — no double-free, no leak.
  - 3.5.4 As the compiler, the indirect closure path threads the captures environment as the
    implicit arg (the L2 ABI `emitClosureCall` already assumes); the non-capturing constant-
    global fast path is unchanged.
  - 3.5.5 If an L2-closures spec/plan already exists in the roadmap, **reconcile** with it
    rather than duplicate — this requirement may fold into that effort.

## 4. Feature: natural-order operator specialization

A sort/search invoked in natural order should devirtualize its order comparison without a
user comparator.

- 4.1 As a developer, when I call a natural-order overload (`Sort.sort(arr, n)` with no
  comparator), the order comparison specializes to the element type's `<` / `compareTo`
  directly, with no closure dispatch — the 2-arg path no longer pays for an internal lambda.
- 4.2 As the compiler, the natural-order specialization keys on `(callee, element type)` and
  emits direct primitive comparisons (or the type's comparison method) in place of an
  `apply.closure` — composing with the existing instance cache.
- 4.3 As the compiler, this is independent of the closure path: it applies even where no
  lambda is written, closing the gap for the common "just sort it" call.
- 4.4 As a developer, results are identical to the comparator path and the natural-order
  sort/search benchmarks improve.

## 5. Feature: drop-elision / move-opt — probe + go/no-go (measure, do not build)

The full-CIR optimizations are scoped here as a **measurement**, extending Phase A's
"measure before committing the middle-end rewrite" discipline to the one unmeasured idea.

- 5.1 As the project, **probe** representative workloads for the *potential* of drop-elision
  (redundant scope-exit drops that a CIR ownership analysis could remove) and move-opt
  (copies that could become moves) — e.g. instrument or hand-analyze hot paths to estimate
  the recoverable cost. This is analysis, not a pass.
- 5.2 As the project, **quantify** the estimated win (cycles / allocations / drop-calls
  avoided) against the **cost** of building the canonical AST→CIR→passes→LLVM path (the large
  middle-end rewrite the probe would justify).
- 5.3 As the project, **record a second go/no-go**: build the canonical CIR lowering path
  only if the probe shows the middle-end optimizations clear a materiality bar on real
  workloads; otherwise Phase B ends at the specialization extensions and CIR stays an
  analysis, as Phase A concluded.
- 5.4 Non-goal for this spec: implementing drop-elision/move-opt or the lowering path. Those
  are a *future* spec, authored only if 5.3 says GO.

## 6. Feature: cross-cutting requirements

- 6.1 **Parity gate (every feature).** A specialization is admissible only when it is
  provably equivalent to the indirect path; the test suite asserts identical results and the
  benchmark `check_ok` stays clean across all workloads.
- 6.2 **AOT/ThinLTO gate (every feature).** Each feature is verified in the release/ThinLTO
  build, not only JIT — including module-placement of any new specialized instances so no
  cross-module reference to an internal symbol reaches the ThinLTO summary.
- 6.3 **Code-growth visibility.** The plan reports the specialized-instance count per build;
  forwarding-chain transitivity must not multiply instances unboundedly (2.3's guard).
- 6.4 **Determinism.** Classification across all features stays structural and reproducible
  across builds and LTO modes.

## 7. Acceptance themes
- Forwarding chains devirtualize: `binary-search` measurably improves, results identical,
  holds under ThinLTO, instance count bounded.
- Natural-order sort/search devirtualizes without a written comparator; benchmarks improve.
- Capturing-lambda specialization is defined and ready, marked blocked until the frontend
  capture prerequisite lands.
- The drop-elision/move-opt probe produces a quantified second go/no-go; no middle-end
  rewrite is built without it.
- Zero correctness regressions; full closure/generic regression sweeps clean.
