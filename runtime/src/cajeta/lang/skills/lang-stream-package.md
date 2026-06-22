---
id: lang-stream-package
applies-to: [cajeta/lang/stream]
title: cajeta.lang.stream — the lazy pull-based Stream pipeline
description: Neighborhood map of Stream, its source roots and lazy/terminal combinators, and how a chain is built and consumed.
---

# cajeta.lang.stream — neighborhood map

This package is the lazy, pull-protocol iteration pipeline (Java-Streams-shaped). The
whole package is built on **one base class, `Stream<T>`**: every combinator is a
`Stream` subclass that overrides a single primitive, `next()`, returning
`Optional<T>` (present = an element, empty = exhausted). You almost never name the
subclasses — you get a source root, chain lazy combinators (methods on `Stream`), and
finish with one eager terminal.

## Decide fast

- Want to **transform/filter/limit** elements? Call a method on `Stream` (`map`,
  `filter`, `take`, …) — it returns a new `#Stream`, nothing executes yet.
- Want a **result** (count, sum, list, first match)? Call a **terminal** (`count`,
  `fold`, `reduce`, `collect`, `forEach`, `findFirst`, `anyMatch`/`allMatch`/`noneMatch`).
  The terminal is what drives `next()` to exhaustion.
- Want **parallelism**? Call `.parallel()` on the chain before the terminal; the
  terminal forks if (and only if) the chain root is `Splittable` and big enough,
  otherwise it silently runs sequentially.
- This package does **not** provide eager collections, indexed access, re-iteration, or
  a `Stream.of(...)` static factory. A `Stream` is single-pass: once a terminal drains
  it, it is spent. To re-stream, build a fresh source.

## Inventory

**Base / entry surface**
- `Stream<T>` — the base class AND the API surface. Holds the combinator methods
  (lazy intermediates + eager terminals) and the parallel/chain-walk protocol
  (`next`, `unwrap`, `cloneChainOver`, `isStatefulWrapper`, `splittableSize`,
  `trySplitRoot`, `parallel`/`sequential`). You typically hold a value typed
  `Stream<T>` or `#Stream<T>` regardless of the concrete subclass.

**Source roots** (you construct these, or get them from a collection):
- `ArrayStream<T>` — over a contiguous `T[]` and a `limit`; `Splittable` (halves the
  index range, no copy). The canonical parallel-friendly source.
- `HashMapKeyStream<K,V>` / `HashMapValueStream<K,V>` / `HashMapEntryStream<K,V>` —
  snapshot walks of a `HashMap`'s slot arrays; all `Splittable`. Entry stream yields a
  fresh `heap Pair<K,V>` per occupied slot. Obtain via the map's `keys()` / `values()`
  / `entries()` rather than constructing by hand.

**Lazy intermediate wrappers** (produced by `Stream` methods; rarely named directly):
- `MapStream<T,R>` (`map`), `FilterStream<T>` (`filter`), `PeekStream<T>` (`peek`),
  `FlatMapStream<T,R>` (`flatMap`).
- `TakeStream<T>` (`take`), `SkipStream<T>` (`skip`) — **stateful** (encounter-order
  dependent).
- Recovery maps: `MapOrSkipStream<T,R>` (`mapOrSkip`), `MapOrFallbackStream<T,R>`
  (`mapOrFallback`), `MapOrLogStream<T,R>` (`mapOrLog`) — catch `RecoverableException`
  per element; `UnrecoverableException` always propagates.

**Support / infrastructure**
- `Splittable<T>` — marker interface (`trySplit()`, `estimateSize()`) for sources the
  parallel driver may fork. Non-splittable parallel chains fall back to sequential.
- `ParallelDriver` — internal fork/join engine the terminals dispatch into; not called
  directly.
- `Optional<T>` lives in `cajeta.lang`, not here, but is the element envelope every
  `next()` returns.

## How a chain is wired

A chain is a linked list of `Stream` wrappers ending at a source root. Each wrapper
holds its `source` and pulls through it lazily; nothing runs until a terminal calls
`next()`. Type-changing wrappers (`MapStream`, `FlatMapStream`, `MapOr*`) extend
`Stream<R>`, not `Stream<T>` — the element type changes mid-chain.

```cajeta
import cajeta.lang.stream.ArrayStream;
import cajeta.lang.stream.Stream;

int32[] nums = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

// lazy build, eager finish — element-type-changing pipeline:
int32 total = (heap ArrayStream<int32>(nums, 10))
    .filter((x) -> x > 3)         // #Stream<int32>, lazy
    .map<int32>((x) -> x * 2)     // #Stream<int32>, lazy
    .take(3)                      // #Stream<int32>, lazy, STATEFUL
    .reduce(0, (a, b) -> a + b);  // terminal: drives next() to exhaustion
```

Collections expose `.stream()` returning an `ArrayStream` over their backing buffer, so
the idiomatic form is `xs.stream().filter(...).map(...).count()`.

## Ownership & lifecycle (the boundary rules)

- **Wrappers take ownership of their source** (`#Stream<T> source` ctor arg). Building
  `b = a.filter(p)` consumes `a` into `b`; do not keep using `a`. Combinator methods
  return `#Stream<...>` — the caller owns the returned chain.
- **Elements: `next()` moves the element out** (`#v` into the returned `Optional`).
  `MapStream`/`MapOr*` move the freshly mapped result out (`#mapped`); `Filter`/`Peek`/
  `Take`/`Skip` forward the source's element unchanged. Source roots (`ArrayStream`,
  `HashMap*Stream`) **share, not copy**, the backing array — element ownership stays
  with whoever owns that buffer; the streams only present index ranges.
- **No close/dispose.** Streams are dropped on scope exit like any value; there is no
  `close()`. A stream is **single-pass and consumable** — after a terminal it is
  exhausted, not reset.
- **HashMap streams are snapshots**: they capture the keys/vals/state array references
  at construction. Mutating or resizing the map afterward is undefined behavior (same
  rule as Java non-snapshot iterators).

## Parallel & error invariants (package-specific)

- `.parallel()` sets a flag and returns the same stream (no allocation); the flag
  propagates as wrappers copy their source's flag at construction. The **terminal**
  decides to fork.
- **Stateful wrappers reject parallelism**: `take(n)`/`skip(n)` throw
  `cajeta.error.Exception` if called on a `.parallel()` stream — call `.sequential()`
  first. Likewise the 2-arg `fold(seed, fn)` throws on a parallel stream (no combiner to
  merge partials); use the 3-arg `fold(seed, fn, combiner)`.
- Parallel reductions require `fn`/`combiner` **associative** and `seed` their
  **identity**, else the answer is implementation-dependent. `findFirst` becomes
  `findAny` under `.parallel()` (encounter order is not preserved).
- **Type-changing wrappers currently break the split walk**: a chain whose `Splittable`
  root sits behind a `MapStream`/`FlatMap`/`MapOr*` falls back to sequential (the
  chain-walker stops at the type flip). Keep the splittable source closest to the
  terminal for actual parallelism today.
- Recovery maps catch only `RecoverableException` (from `cajeta.error`):
  `mapOrSkip` drops the element, `mapOrFallback` substitutes a fixed value (captured at
  construction, same length out), `mapOrLog` reports to a `(T, Exception) -> void`
  callback then drops. `UnrecoverableException` always propagates.

## Pointers

For class-level construction/method detail, see the class skills for `ArrayStream`,
`FlatMapStream`, and the `HashMap*Stream` family. For the fork/join contract and
per-terminal parallel rules, see `ParallelDriver` and `docs/specification/lang/stream/
StreamParallelism.md`. `Optional<T>` (the element envelope) and `Collector`/`Collectors`
(for `collect`) live outside this package.
