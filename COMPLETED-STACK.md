# COMPLETED — template / Tensor capture work

Newest on top. Items popped off `STACK.md` once green + committed.

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
