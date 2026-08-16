# Streams

The pull-protocol iteration substrate for the cajeta stdlib. Every
class that wants to expose "walk my elements" semantics multiple-
inherits `cajeta.lang.stream.Stream<T>` — there is no separate
`Iterator` type. Cajeta uses multiple inheritance precisely so a
container can BE a stream rather than expose one through a
factory method.

Status: the protocol root, the intermediate combinators, the terminals,
and the `.parallel()` fork/join path are all in place as real methods
on `Stream<T>` (see `runtime/src/cajeta/lang/stream/Stream.cajeta`).
Fluent chaining works (`xs.stream().filter(p).count()`); the historical
two-step workaround — binding each wrapper stream to a local before the
next op — is no longer required. Collections still expose a `.stream()`
factory (or `keys()`/`values()`/`entries()` on HashMap) rather than
multiple-inheriting `Stream` directly; that conversion is still pending.

## Overview

```
cajeta.lang.stream.Stream<T>              ← protocol root: next() + ops + terminals
├─ ArrayStream<T>          implements Splittable<T>  ← T[].stream() / ArrayList.stream()
├─ TakeStream<T>           ← .take(n) wrapper (stateful)
├─ SkipStream<T>           ← .skip(n) wrapper (stateful)
├─ FilterStream<T>         ← .filter(pred) wrapper
├─ MapStream<T,R>          ← .map<R>(fn) wrapper (Stream<R>)
├─ PeekStream<T>           ← .peek(fn) wrapper
├─ FlatMapStream<T,R>      ← .flatMap<R>(fn) wrapper (Stream<R>)
├─ MapOrSkipStream<T,R>    ← .mapOrSkip<R>(fn) wrapper (Stream<R>)
├─ MapOrFallbackStream<T,R>← .mapOrFallback<R>(fn, fallback) wrapper (Stream<R>)
├─ MapOrLogStream<T,R>     ← .mapOrLog<R>(fn, logger) wrapper (Stream<R>)
├─ HashMapKeyStream<K,V>   implements Splittable<K>          ← HashMap.keys()
├─ HashMapValueStream<K,V> implements Splittable<V>          ← HashMap.values()
└─ HashMapEntryStream<K,V> implements Splittable<Pair<K,V>>  ← HashMap.entries()
```

`Splittable<T>` (a sub-interface of `Stream<T>`) marks sources that can
be cheaply halved for the parallel driver — see `StreamParallelism.md`.

The collection types expose streams through factory methods today
(`list.stream()`, `map.keys()/values()/entries()`). The longer-term
design is for each container to multiple-inherit `Stream` of its element
type so `list.count()` / `map.filter(p).forEach(...)` dispatch directly;
that conversion has not landed yet.

## Pull primitive: `next(): Optional<T>`

Every stream class overrides `next()`. The base default returns an
empty `Optional<T>` so a never-overridden subclass behaves like an
empty stream — never null-defaults, never crashes.

```cajeta
public class Stream<T> {
    public Optional<T> next() {
        return stack Optional<T>(false, null);
    }
    // ... terminals inherited by every subclass ...
}
```

The pull primitive returns by value (sret + NRVO per
`docs/specification/lang/ValueReturns.md`); every wrapper override
(`FilterStream`, `MapStream`, `PeekStream`, etc.) follows suit so the
vtable slot ABI lines up. A subclass override whose body never says
`return stack X(...)` (e.g. `PeekStream.next` is `return o;` only)
still inherits the sret shape from `Stream.next` —
`Method::returnsStackValue()` walks the superclass chain so the
override matches the base method's signature without source-level
annotation.

## Terminal combinators

Walked-to-exhaustion (or short-circuiting) consumers. All inherited
from `Stream<T>`; subclasses don't override them.

| Name | Signature | Behavior |
| ---- | --------- | -------- |
| `count` | `() : int32` | Walk `next()` to exhaustion; return the count. |
| `forEach` | `((T) -> void)` | Apply fn to each remaining element. |
| `anyMatch` | `((T) -> boolean) : boolean` | True iff any remaining element matches. Short-circuits. |
| `allMatch` | `((T) -> boolean) : boolean` | True iff every remaining element matches. Empty → true. |
| `noneMatch` | `((T) -> boolean) : boolean` | True iff no remaining element matches. Empty → true. |
| `findFirst` | `((T) -> boolean) : Optional<T>` | First matching element, or empty Optional. Becomes `findAny` under `.parallel()`. |
| `reduce` | `(T seed, (T, T) -> #T) : T` | Left-fold; result and seed share `T`. Thin wrapper over `fold<T>`. |
| `fold<R>` | `(R seed, (R, T) -> #R) : R` | Cross-type left-fold; the accumulator type `R` is method-level, so it may differ from `T`. Sequential-only (rejects a parallel stream — use the 3-arg form). |
| `fold<R>` | `(R seed, (R, T) -> #R, (R, R) -> #R) : R` | Cross-type fold with an explicit `combiner` that merges per-worker partials on the parallel path. |
| `collect<R>` | `(Collector<T, R>) : R` | Reduce via a `Collector` (supplier / accumulator / combiner triple). See Collections.md § Collector. |

`fold<R>` / `map<R>` / `flatMap<R>` / `collect<R>` and the `mapOr*<R>`
ops carry a method-level type parameter (written `final` in source, so
they can't sit in a vtable — see `MethodLevelTemplate.md`); the inherited
`next()` is what subclasses customize.

### Examples

```cajeta
import cajeta.lang.stream.Stream;

int32[] xs = {1, 2, 3, 4, 5};

// count
int32 n = xs.stream().count();                                     // 5

// forEach (named-local lambda — see "Lambda binding" below)
(int32) -> void echo = (int32 v) -> { /* ... */ };
xs.stream().forEach(echo);

// anyMatch / allMatch / noneMatch
(int32) -> boolean isEven = (int32 v) -> { return (v % 2) == 0; };
boolean any = xs.stream().anyMatch(isEven);                        // true
boolean all = xs.stream().allMatch(isEven);                        // false
boolean none = xs.stream().noneMatch(isEven);                      // false

// findFirst
(int32) -> boolean over3 = (int32 v) -> { return v > 3; };
Optional<int32> hit = xs.stream().findFirst(over3);                // 4

// reduce (sum)
(int32, int32) -> int32 add = (int32 a, int32 b) -> { return a + b; };
int32 sum = xs.stream().reduce(0, add);                            // 15

// fold<R> into a wider accumulator (R = int64 differs from T = int32)
int64 wide = xs.stream().fold<int64>(0L,
    (int64 acc, int32 e) -> { return acc + (int64) e; });          // 15

// collect into an owned ArrayList via a Collector
ArrayList<int32> all = xs.stream().collect(Collectors.toList<int32>());
```

Pinned by `test/parser/StreamTerminalTests.cpp` (13 tests) and
`test/parser/StreamFoldTests.cpp` (4 tests for `fold<R>`).

## Intermediate combinators (lazy wrappers)

Each returns a fresh `#Stream<R>` whose `next()` pulls from the source
stream on demand. None evaluate eagerly — the pipeline runs only when
a terminal consumes the tail. Call them as methods on any `Stream<T>`
(each constructs and returns the corresponding wrapper); the wrapper
class is the implementation, not the call site.

| Method | Wrapper | Behavior |
| ------ | ------- | -------- |
| `take(int32 n)` | `TakeStream<T>` | Yield at most the first n elements. **Stateful** — rejected on a `.parallel()` stream. |
| `skip(int32 n)` | `SkipStream<T>` | Drop the first n elements, then pass through. **Stateful** — rejected on a `.parallel()` stream. |
| `filter((T) -> boolean pred)` | `FilterStream<T>` | Yield only elements where pred returns true. |
| `map<R>((T) -> #R fn)` | `MapStream<T,R>` | Apply fn to each element; produces `Stream<R>`. |
| `peek((T) -> void fn)` | `PeekStream<T>` | Invoke fn for side effects; pass element through. |
| `flatMap<R>((T) -> #Stream<R> fn)` | `FlatMapStream<T,R>` | fn returns `Stream<R>`; flatten by draining each. |
| `mapOrSkip<R>((T) -> #R fn)` | `MapOrSkipStream<T,R>` | Like `map`, but elements whose fn throws `RecoverableException` are dropped (stream shortens). |
| `mapOrFallback<R>((T) -> #R fn, R fallback)` | `MapOrFallbackStream<T,R>` | Like `map`, but a throwing element is substituted with `fallback` (length unchanged). |
| `mapOrLog<R>((T) -> #R fn, (T, Exception) -> void logger)` | `MapOrLogStream<T,R>` | Like `map`, but a throwing element is passed to `logger(elem, exc)` then dropped. |

The three `mapOr*` recovery ops catch only `RecoverableException`;
`UnrecoverableException` always propagates (the alarm contract — see
`StreamParallelism.ErrorHandling.md` § 3.3). They are type-changing
wrappers, so under `.parallel()` the chain falls back to sequential.

### Examples

```cajeta
import cajeta.lang.stream.ArrayStream;
import cajeta.lang.stream.Stream;

int32[] xs = {1, 2, 3, 4, 5, 6};

(int32) -> boolean isEven = (int32 v) -> { return (v % 2) == 0; };
(int32) -> int32 dbl = (int32 v) -> { return v * 2; };

// take(3) → {1, 2, 3}
int32 takeSum = xs.stream().take(3).reduce(0, add);                // 6

// skip(2) → {3, 4, 5, 6}
int32 skipSum = xs.stream().skip(2).reduce(0, add);               // 18

// filter(isEven) → {2, 4, 6}
int32 evenCount = xs.stream().filter(isEven).count();             // 3

// map<int32>(double) → {2, 4, 6, 8, 10, 12}
int32 doubledSum = xs.stream().map<int32>(dbl).reduce(0, add);    // 42
```

Each method is sugar for constructing the matching wrapper, so the
explicit form (`heap FilterStream<int32>(xs.stream(), isEven)`) still
works and is what the methods lower to.

Pinned by `test/parser/StreamIntermediateTests.cpp` (24 tests covering
all nine wrappers, including the three `mapOr*` recovery ops).

### Fluent chaining

`xs.stream().filter(p).map<R>(f).count()` works today — the intermediate
combinators are real methods on `Stream<T>` that return `#Stream<R>`, so
they chain directly. The historical P6.6 two-step workaround (binding
each wrapper stream to a named local) is no longer required;
`test/parser/ChainedFormTests.cpp` pins the chained terminal forms, and
the parallel suite pins fluent dispatch through filter chains.

## Lambda binding

Lambdas are usually bound to a named local before being passed in,
both because the parser today rejects some in-line lambda forms inside
call sites and because named lambdas survive multiple uses without
re-allocation:

```cajeta
(int32) -> boolean isEven = (int32 v) -> { return (v % 2) == 0; };
xs.stream().anyMatch(isEven);
xs.stream().allMatch(isEven);
```

When a wrapper stream stores a lambda in a field (`(T) -> R fn` on
`MapStream<T, R>`), invocation through `this.fn(args)` is now wired
end-to-end through MethodCallExpression's function-typed-field path
(see `src/cajeta/asn/expression/MethodCallExpression.cpp` — function-
field-invocation block). Before that landed, the call silently became
`i1 false` for boolean lambdas / garbage for others.

## `ArrayStream<T>` and the `T[].stream()` intrinsic

```cajeta
public class ArrayStream<T> extends Stream<T> implements Splittable<T> {
    T[] data;
    int32 idx;
    int32 limit;
    public ArrayStream(T[] data, int32 limit) { ... }
    public Optional<T> next() { ... }
    // Splittable surface for the parallel driver:
    public #Stream<T> trySplit() { ... }   // halves the index range, O(1)
    public int64 estimateSize() { ... }    // limit - idx, exact, O(1)
}
```

`ArrayStream` is the canonical `Splittable<T>` root: `trySplit()`
halves the index range without copying, making it the parallel-friendly
source (see `StreamParallelism.md`). It also overrides `count()` with an
O(1) `limit - idx` fast path under `.parallel()`.

The compiler intrinsic `arr.stream()` lowers to
`heap ArrayStream<T>(arr, arr.count())` — see
`src/cajeta/asn/expression/MethodCallExpression.cpp` (the `.stream()`
intrinsic block, scoped to a `CajetaArray` receiver). The instantiated
ArrayStream<T>'s ctor is dispatched via the template's instantiation
cache, identical to any user `heap T<...>(...)` call.

Pinned by `test/parser/StreamTests.cpp` (9 tests) plus indirect
exercise across StreamTerminalTests / StreamIntermediateTests.

## `Optional<T>` as a single-shot stream

`Optional<T>` multiple-inherits `Stream<T>` so the present-value case
behaves like a one-element stream and the empty case like a zero-
element stream. `next()` returns the contained value on the first
call (present case) and empty thereafter.

> Implementation status: today `Optional<T>` does NOT yet multiple-
> inherit `Stream<T>` (see `runtime/src/cajeta/lang/Optional.cajeta` —
> no `extends Stream<T>` clause). Lands once multiple inheritance is
> verified end-to-end on the codegen path. Tracked in specs/Features.md.

## Constructing streams from collections

The convention for every collection in `cajeta.collection`:

| Source | Stream factory | Yields |
| ------ | -------------- | ------ |
| `T[]` | `arr.stream()` intrinsic | `Stream<T>` over array elements |
| `ArrayList<T>` | `list.stream()` | `Stream<T>` over live elements |
| `ImmutableList<T>` / `ImmutableSet<T>` | `.stream()` | `Stream<T>` over elements |
| `HashMap<K, V>` | `map.keys()` / `map.values()` / `map.entries()` | `Stream<K>` / `Stream<V>` / `Stream<Pair<K,V>>`, slot-walk order |

`HashMap`'s three views are `Splittable` snapshots taken at the call
(`keys()` → `HashMapKeyStream`, etc.); a concurrent `put`/`remove`/resize
during traversal is undefined behavior, same as Java's non-snapshot
iterators. The longer-term plan is for containers to multiple-inherit
`Stream` so the factory call drops away — not landed yet.

## Tests

Complete features have pinned test files:

| Feature | Tests |
| ------- | ----- |
| Stream base + terminals | `test/parser/StreamTerminalTests.cpp` (13) |
| ArrayStream + `.stream()` intrinsic | `test/parser/StreamTests.cpp` (9) |
| TakeStream, SkipStream, FilterStream, MapStream, PeekStream, FlatMapStream | `test/parser/StreamIntermediateTests.cpp` (19) |
| Optional construction + extraction | `test/parser/OptionalTests.cpp` (6), `test/parser/OptionalAndAllocateTests.cpp` (6) |
| Pair construction + accessors | `test/parser/PairTests.cpp` (3) |
| ArrayList including `.stream()` | `test/parser/ArrayListTests.cpp` (7) |
| HashMap core ops | `test/collections/HashMapTests.cpp`, `test/collections/PrimitiveHashMapTests.cpp` |
| HashMap stream views (keys/values/entries) | `test/collections/HashMapStreamTests.cpp` |
| HashMap parallel stream terminals | `test/collections/HashMapStreamParallelTests.cpp` |
| `fold<R>` (2- and 3-arg) | `test/parser/StreamFoldTests.cpp` (4) |
| Parallel driver dispatch / chains | `test/parser/ParallelStreamP1Tests.cpp` |

## Open items

Tracked in `specs/Features.md`:

- Multiple-inheritance for `Optional<T>` to `extends Stream<T>` —
  ergonomic but blocked on end-to-end multi-inheritance codegen.
  (`Optional<T>` is a plain class today — it does NOT yet extend
  `Stream<T>`; see `runtime/src/cajeta/lang/Optional.cajeta`.)
- Multiple-inheritance for collection types (`ArrayList<T>`,
  `HashMap<K,V>`, etc.) to BE streams rather than expose a `.stream()`
  / `keys()` / `values()` / `entries()` factory.
- **Shipped since this doc's first draft:** `fold<R>` (both the
  sequential 2-arg form and the parallel 3-arg-with-combiner form),
  `collect<R>(Collector<T,R>)`, the `mapOr*` recovery ops, the
  `.parallel()` fork/join driver, HashMap `Splittable` stream views,
  and single-/multi-hop fluent chaining.
- More intermediate combinators not yet implemented:
  `takeWhile`/`dropWhile`/`distinct`/`enumerate`/`zip`/`chain`/`sorted`/`windowed`.
