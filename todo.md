# Template Wildcards (`<?>`) — Staged Implementation

See `cajeta-docs/TemplateWildcard.md` for cost-benefit analysis and
rationale.

Stage the work to deliver value incrementally rather than blocking the
parallel chain walk on a full implementation.

## 1. Type-checker plumbing

Land `<?>` parsing and basic assignability rules at
`src/cajeta/type/CajetaType.cpp:454` (the current
`"wildcard type arguments not supported in v1"` throw site).

- Parse `<?>` in type-argument position.
- Basic assignability: `Stream<T>` is assignable to `Stream<?>` for any
  concrete `T`.
- No variance yet (`? extends T` / `? super T` deferred to Step 6).
- Used nowhere in stdlib or tests yet — this is foundation only.
- Lands behind a feature flag with a backout path.

## 2. Drop-chain ABI

Decide and implement how wildcard-typed values are dropped.

- Option A: virtual destructor on `Stream` (and any other class that
  participates in wildcard-typed locals/fields).
- Option B: fat-pointer convention — wildcard-typed locals carry an
  inline destructor function pointer.
- Validate against the existing test suite — no regressions in
  non-wildcard code paths.

## 3. TemplateInstantiator wildcard cache bucket — DONE (audit only)

The Step 1 wildcard short-circuit in
`CajetaClass::instantiate` already satisfies the three acceptance
criteria. Audit + test coverage added; no implementation needed.

- Wildcards do not collide with concrete instantiations — canonical
  keys differ (`Box<?>` vs `Box<cajeta.int32>`). Verified by
  `TemplateWildcardP1Tests.wildcardCacheBucketDistinctFromConcrete`.
- One erased instantiation per generic-template / args tuple —
  `module->getStructures()[canonical]` lookup in the short-circuit
  returns the cached proxy on repeat calls. Verified by
  `TemplateWildcardP1Tests.wildcardProxyIsCachedAcrossInstantiateCalls`.
  Partial-wildcard forms (`Pair<?,int32>` vs `Pair<int32,?>` vs
  `Pair<?,?>`) get distinct buckets, verified by
  `TemplateWildcardP1Tests.partialWildcardCacheBucketsDistinct`.
- `visitClassDeclaration` register path (9b5f434) registers the
  template itself under canonical + short name; the wildcard
  short-circuit registers `Template<?>` under a suffix-bearing
  canonical. No key overlap.

## 4. Lint pass — wildcard-in-hot-loop detector — DEFERRED (specced)

Four rules added to `cajeta-docs/LintRules.md` under "Future rules":
- `wildcard-materialize-in-loop`
- `wildcard-crosses-hot-boundary`
- `wildcard-field-in-small-class`
- `discarded-wildcard-next`

Implementation lands as a separate effort following the v1 lint
infrastructure pattern (the `uncaught-throws` rule already in the
catalog) — compiler-internal, `@SuppressLint("rule-id")` suppression,
warning-only. Not a prerequisite for Step 6 or for merging the
template-wildcard branch back to main; user-facing wildcard
semantics work without these diagnostics, and the rules describe
performance footguns rather than correctness bugs.

## 5. Migrate parallel chain walk to use `<?>` — DONE (Steps 5a + 5b)

The immediate payoff that motivated this work.

**Step 5a (commit a1fc2d8):** removed the Step 1/2 erased-proxy
short-circuit; wildcard args now flow through full template
instantiation with T → wildcardSentinel substitution. Wildcard
proxies get fully-populated methods, fields, vtables. Method calls
on wildcard locals (`bw.tag()` on a Box<?>) work end-to-end via
template-relative vtable hashing — `buildVirtualTable` publishes
alias entries under the templateOrigin canonical alongside the
per-instantiation entry; the invokeMethod wildcard-receiver branch
hashes on the same template-relative canonical.

**Step 5b (this commit):**
- Flag default flipped ON; CAJETA_WILDCARDS=0 becomes the backout.
- `Stream<T>` surface: `unwrap()` returns `Stream<?>`,
  `cloneChainOver(Stream<?> newRoot)` takes wildcard,
  `trySplitRoot()` returns `Stream<?>`.
- Non-type-changing wrappers (Filter/Peek/Take/Skip) updated to
  match the new signatures.
- Type-changing wrappers (Map/FlatMap/MapOr{Skip,Fallback,Log})
  ship new `unwrap()` + `cloneChainOver()` overrides; the chain
  walker now steps past element-type changes.
- Splittable roots (ArrayStream / HashMap{Key,Value,Entry}Stream)
  retype `trySplitRoot()` to `Stream<?>`.
- 8 chain-walk sites in ParallelDriver.cajeta retyped from
  `Stream<T>` to `Stream<?>`.
- Vtable alias walk in `buildVirtualTable` extended to walk super
  templateOrigins (MapStream<int32,int32>::unwrap aliases under both
  MapStream::unwrap and Stream::unwrap so dispatch from a
  Stream<?>-typed cursor lands correctly).
- Tests: `.map().parallel().reduce()`, `.parallel().map().reduce()`,
  `.filter().map().parallel().reduce()`, `.map().parallel().count()`
  all pass.

Total change ~150 LOC including stdlib edits + new tests. Original
50-LOC estimate didn't anticipate the Step 5a method-resolution work
or the vtable-alias super-walk needed for Step 5b dispatch.

## 6. Bounded wildcards (`? extends T` / `? super T`) — DONE (minimum-viable)

Parse + classify + assignability check shipped. Capture conversion
(Java's `capture#N` synthetic types) deferred — no stdlib site needs
it today and the read/write polarity it enables would be ergonomic
sugar rather than load-bearing infrastructure.

- Parser site (CajetaType.cpp:454) recognizes `?`, `? extends T`,
  `? super T` distinctly. Bounded forms route through
  `wildcardSentinelExtends(bound)` / `wildcardSentinelSuper(bound)`.
- Per-(kind, bound) sentinels cached in canonicalMap under canonicals
  `?`, `? extends <bound-canonical>`, `? super <bound-canonical>`.
- Wildcard kind/bound queryable via `wildcardKind()` and
  `wildcardBound()`. `isWildcard()` returns true for all forms.
- `CajetaClass::isAssignableToWildcard` enforces per-arg-position
  bounds: `Box<Dog>` ⊆ `Box<? extends Animal>`, `Box<Animal>` ⊆
  `Box<? super Dog>`, etc.

Test coverage in `test/parser/TemplateWildcardP6Tests.cpp` (6 tests):
sentinel caching + classification, parser routing, assignability for
extends/super in both directions including the negative cases.

**Future work** (not a blocker for merging template-wildcard to main):
- Capture conversion proper (`Stream<? extends Number>` produces
  `Number` at read sites, etc.). Would extend method resolution to
  carry capture identities through the dispatch.
- Stdlib producer/consumer signature migration to use bounded
  wildcards where they'd express PECS variance more clearly than
  the current concrete instantiations.
