# Streams

The pull-protocol iteration substrate for the cajeta stdlib. Every
class that wants to expose "walk my elements" semantics multiple-
inherits `cajeta.lang.stream.Stream<T>` — there is no separate
`Iterator` type. Cajeta uses multiple inheritance precisely so a
container can BE a stream rather than expose one through a
factory method.

Status: foundation + intermediate + terminal sets all in place; chain
syntax (`xs.stream().filter(p).map(f).count()`) blocked on P6.6.

## Overview

```
cajeta.lang.stream.Stream<T>          ← protocol root: next() + terminals
├─ cajeta.lang.stream.ArrayStream<T>  ← T[].stream() lowers to this
├─ cajeta.lang.stream.TakeStream<T>   ← .take(n) wrapper
├─ cajeta.lang.stream.SkipStream<T>   ← .skip(n) wrapper
├─ cajeta.lang.stream.FilterStream<T> ← .filter(pred) wrapper
├─ cajeta.lang.stream.MapStream<T,R>  ← .map(fn) wrapper (Stream<R>)
├─ cajeta.lang.stream.PeekStream<T>   ← .peek(fn) wrapper
└─ cajeta.lang.stream.FlatMapStream<T,R> ← .flatMap(fn) wrapper (Stream<R>)
```

Optional, Pair, the collection types (ArrayList, HashMap, future
HashSet / LinkedList) all multiple-inherit `Stream` of the right
element type, so the same combinators work uniformly: `list.count()`,
`map.filter(p).forEach(...)`, `opt.findFirst(...)` all dispatch
through the same vtable slots.

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
`docs/stdlib/ValueReturns.md`); every wrapper override
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
| `findFirst` | `((T) -> boolean) : Optional<T>` | First matching element, or empty Optional. |
| `reduce` | `(T seed, (T, T) -> T) : T` | Left-fold; result and seed share `T`. |

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
```

Pinned by `test/parser/StreamTerminalTests.cpp` (13 tests).

## Intermediate combinators (lazy wrappers)

Each returns a fresh `Stream<R>` whose `next()` pulls from the source
stream on demand. None evaluate eagerly — the pipeline runs only when
a terminal consumes the tail.

| Wrapper | Construction | Behavior |
| ------- | ------------ | -------- |
| `TakeStream<T>` | `heap TakeStream<T>(source, n)` | Yield at most the first n elements. |
| `SkipStream<T>` | `heap SkipStream<T>(source, n)` | Drop the first n elements, then pass through. |
| `FilterStream<T>` | `heap FilterStream<T>(source, pred)` | Yield only elements where pred returns true. |
| `MapStream<T,R>` | `heap MapStream<T,R>(source, fn)` | Apply fn to each element; produces `Stream<R>`. |
| `PeekStream<T>` | `heap PeekStream<T>(source, fn)` | Invoke fn for side effects; pass element through. |
| `FlatMapStream<T,R>` | `heap FlatMapStream<T,R>(source, fn)` | fn returns `Stream<R>`; flatten by draining each. |

### Examples

```cajeta
import cajeta.lang.stream.ArrayStream;
import cajeta.lang.stream.Stream;
import cajeta.lang.stream.TakeStream;
import cajeta.lang.stream.FilterStream;
import cajeta.lang.stream.MapStream;

int32[] xs = {1, 2, 3, 4, 5, 6};

// take(3) → {1, 2, 3}
TakeStream<int32> t = heap TakeStream<int32>(xs.stream(), 3);
int32 takeSum = t.reduce(0, add);                                  // 6

// skip(2) → {3, 4, 5, 6}
SkipStream<int32> s = heap SkipStream<int32>(xs.stream(), 2);
int32 skipSum = s.reduce(0, add);                                  // 18

// filter(isEven) → {2, 4, 6}
(int32) -> boolean isEven = (int32 v) -> { return (v % 2) == 0; };
FilterStream<int32> f = heap FilterStream<int32>(xs.stream(), isEven);
int32 evenCount = f.count();                                       // 3

// map(double) → {2, 4, 6, 8, 10, 12}
(int32) -> int32 dbl = (int32 v) -> { return v * 2; };
MapStream<int32, int32> m = heap MapStream<int32, int32>(xs.stream(), dbl);
int32 doubledSum = m.reduce(0, add);                               // 42
```

Pinned by `test/parser/StreamIntermediateTests.cpp` (19 tests covering
all six wrappers).

### Chained construction (P6.6, not yet shipped)

The shape users want — `xs.stream().filter(p).map(f).count()` —
doesn't parse today. Each combinator currently needs its own ctor +
intermediate local. Tracked as P6.6 in `ToDo.md`. Until then, the
wrapper-stream pattern above is the working syntax.

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
public class ArrayStream<T> extends Stream<T> {
    T[] data;
    int32 idx;
    int32 limit;
    public ArrayStream(T[] data, int32 limit) { ... }
    public Optional<T> next() { ... }
}
```

The compiler intrinsic `arr.stream()` lowers to
`heap ArrayStream<T>(arr, arr.size())` — see
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
> verified end-to-end on the codegen path. Tracked in Features.md.

## Constructing streams from collections

The convention for every collection in `cajeta.collection`:

| Source | Stream constructor | Yields |
| ------ | ------------------ | ------ |
| `T[]` | `arr.stream()` intrinsic | `Stream<T>` over array elements |
| `ArrayList<T>` | `list.stream()` | `Stream<T>` over live elements |
| `HashMap<K, V>` | `map` IS a `Stream<Pair<K,V>>` (via multi-inheritance) | entries in unspecified order |
| `HashSet<T>` (future) | `set` IS a `Stream<T>` | elements in unspecified order |

The multi-inheritance design eliminates the `.entries()` / `.keys()` /
`.values()` Java boilerplate: the map IS the entry-stream, and the
caller picks how to consume it (`for-each`, `.count()`, `.filter(...)`,
etc.).

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
| HashMap (does not yet expose Stream) | `test/collections/HashMapTests.cpp`, `test/collections/PrimitiveHashMapTests.cpp` |

## Open items

Tracked in root `Features.md`:

- Multiple-inheritance for `Optional<T>` to `extends Stream<T>` —
  ergonomic but blocked on end-to-end multi-inheritance codegen.
- Multiple-inheritance for collection types (`ArrayList<T>`,
  `HashMap<K,V>`, etc.) to BE streams rather than have a `.stream()`
  factory.
- Chained-form parsing — `xs.stream().filter(p).map(f).count()`
  (P6.6).
- `collect(Collector<T,R>)` terminal — needs `Collector<T,R>` from
  collections (P2.1).
- `fold<R>(R seed, (R, T) -> R fn)` method-level-templated terminal —
  method-level type parameters are now supported on `final` instance
  methods (see `docs/stdlib/MethodLevelTemplate.md`). The
  stdlib uptake (rewriting `Stream<T>.reduce` as a wrapper around
  `fold<R>`, and adding `fold<R>` itself) is the next follow-up.
- More intermediate combinators not yet implemented:
  `takeWhile`/`dropWhile`/`distinct`/`enumerate`/`zip`/`chain`/`sorted`/`windowed`.
