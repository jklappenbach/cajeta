# COMPLETED — template / Tensor capture work

Newest on top. Items popped off `STACK.md` once green + committed.

- bare `Tensor<? extends Floating>` variable/field over a mutable primitive
  container (numeric plan 5a/5b/5c/6) — the bare bounded-wildcard var form now
  compiles, assigns, and round-trips via capture. Design: a bounded-wildcard
  instantiation (`Holder<? extends Floating>`, `Tensor<? extends Floating>`) is an
  ABSTRACT pointer handle — `Method::generateCode` emits a TRAP STUB for its
  methods instead of the real body (no concrete primitive element layout to lower;
  the own-`T` internal `this.store.set(v)` would trip PECS). New predicate
  `CajetaClass::isBoundedWildcardInstantiation()` (any `? extends`/`? super` arg;
  EXCLUDES unbounded `?` so `Class<?>` reflection still gets real bodies).
  Operations go through capturing back to a concrete instantiation (item 3):
  `(Tensor<float32>) w` then call. **4.3 (capture-aware PECS exemption) proved
  unnecessary** — the trap stub sidesteps the internal-write site, and genuine
  EXTERNAL mutator writes through a `? extends` handle stay PECS-rejected at the
  call site (verified). Tests: `NumericBoundsTests.bareWildcard*` (var-form,
  operate-via-capture, external-write-rejected) + `bareWildcardRealTensorOperate
  ViaCapture` on the real `cajeta.math.Tensor`. Regression: 29/29 NumericBounds +
  ReifiedCapture green; `Method::generateCode` change is a no-op for all
  non-bounded-wildcard methods (and stdlib has zero bounded-wildcard
  instantiations). Pre-push full sweep still pending. Flips numeric-bounds-plan
  5a/5b/5c/6c from `[~]` to done.
- class-bounded-wildcard capture (capture plan 5b) — `(Box<? extends Animal>) w`
  and `w instanceof Box<? extends Animal>` succeed for `Box<Dog>`, fail/throw for
  `Box<Cat>`. Added runtime `__cajeta_instanceof_bounded(obj, baseName, argIndex,
  boundName)`: base-name match + recover the reified element type NAME from the
  container's templateArgs → `__cajeta_rtti_for_name` → `__cajeta_is_subtype` vs
  the bound (element==bound or element <: bound; fail-safe to 0 on an unresolved
  type). Codegen: `boundedWildcardTarget()` introspects `Base<? extends Bound>`
  (single bounded-EXTENDS arg) and wires both the instanceof path
  (`InstanceOfExpression`) and the throwing capture cast (`CastExpression` via a
  refactored shared throw-branch). Bound match uses the vtable parent-chain walk
  (correct for CLASS bounds; interface bounds + multi-arg/mixed targets are
  future work). NOTE: the bound class must be RTTI-registered to resolve at
  runtime — always true in JIT/Full; under Lean DCE a bound referenced ONLY in a
  wildcard could be stripped and the capture fails safe. End-to-end test of the
  name→RTTI registry on real JIT RTTI. All 18 `ReifiedCaptureTests` green.
- name→RTTI registry (class-bounded-wildcard precursor) — added
  `__cajeta_rtti_for_name(const char*)` to the runtime: resolves a type's
  canonical name → its `CajetaRtti*` over the existing REFL-8 name→#ClassObject
  table (rtti at offset 8). The capture-site lowering (item 3) will call it with
  the container's stored element-name string, then bound-check via the existing
  `__cajeta_is_subtype`. Unit-tested at the contract level against the PROCESS
  registry (host C++ shares the statically-linked runtime copy); the real-RTTI
  data path runs inside the JIT (which embeds its OWN runtime copy — host C++
  can't see JIT-local statics) and is covered end-to-end by item 3.
- nested + supertype capture targets (capture plan 5a) — `Box<Box<int32>>` and
  `List<int32> extends Container<int32>` capture targets, exact + supertype +
  nested, with discriminating negative arms. **No production change needed** —
  already satisfied by the existing design: nested falls out of the recursive
  `toCanonical()` string compare in `__cajeta_instanceof_named`, and supertype
  works because `TemplateInstantiator` records the *instantiated* super
  (`Container<int32>`) in the RTTI `parentNames`, which the runtime walks. Added
  5 regression tests to `ReifiedCaptureTests` to lock the behavior.
- `tryAs<T>()` → `Optional<T>` (capture plan 4b/4d) — non-throwing capture:
  present on match (`.get()` is the same object), empty on mismatch and on null.
  Built on `CajetaClass::heapConstruct` + `__cajeta_instanceof_named`.
  Landed on `main` as `6e773c4d`.
- throwing capture cast + `ClassCastException` — `(T) w` capture form that throws
  on mismatch. Landed on `main` as `8ca30390`.
- reusable class-construction helpers — extracted `heapConstruct` / construction
  helpers used by the capture casts. Landed on `main` as `c2e89379`.
