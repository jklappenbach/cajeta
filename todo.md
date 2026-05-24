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

## 4. Lint pass — wildcard-in-hot-loop detector

Add a compiler diagnostic (not external linter) that flags the known
performance footguns:

- `Stream<?>` materializing `T` inside a loop ("likely boxing").
- Wildcard return crossing a hot-path boundary.
- Wildcard field in a small frequently-allocated class
  ("drop becomes virtual").
- Discarded wildcard `next()` result
  ("the box allocates even though the value is unused").

Diagnostics live in the compiler so users see them inline, not only
under `cajeta-lint`.

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

## 6. Bounded wildcards (`? extends T` / `? super T`)

Separate later step — long-term dividend, not a blocker for Step 5.

- Parse and type-check `? extends T` / `? super T`.
- Capture conversion rules: `Stream<? extends Number>` produces
  `Number` at read sites; `Collection<? super Cat>` accepts `Cat` at
  write sites.
- Update diagnostics renderer to handle capture identities cleanly.
- Migrate stdlib producer/consumer signatures opportunistically.
