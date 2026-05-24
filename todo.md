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

## 5. Migrate parallel chain walk to use `<?>`

The immediate payoff that motivated this work.

- Retype chain-walk cursor variables in
  `runtime/src/cajeta/lang/stream/ParallelDriver.cajeta` as
  `Stream<?>`.
- Add `unwrap()` and `cloneChainOver()` overrides on type-changing
  wrappers:
  - `runtime/src/cajeta/lang/stream/MapStream.cajeta`
  - `runtime/src/cajeta/lang/stream/FlatMapStream.cajeta`
  - `runtime/src/cajeta/lang/stream/MapOrSkipStream.cajeta`
  - `runtime/src/cajeta/lang/stream/MapOrFallbackStream.cajeta`
  - `runtime/src/cajeta/lang/stream/MapOrLogStream.cajeta`
- Downcast at the worker boundary so the hot loop stays specialized.
- ~50 LOC change once Steps 1-4 are in place.
- Tests for `.map().parallel()`, `.parallel().map()`,
  `.flatMap().parallel()`, `.filter().map().parallel()`, etc.

## 6. Bounded wildcards (`? extends T` / `? super T`)

Separate later step — long-term dividend, not a blocker for Step 5.

- Parse and type-check `? extends T` / `? super T`.
- Capture conversion rules: `Stream<? extends Number>` produces
  `Number` at read sites; `Collection<? super Cat>` accepts `Cat` at
  write sites.
- Update diagnostics renderer to handle capture identities cleanly.
- Migrate stdlib producer/consumer signatures opportunistically.
