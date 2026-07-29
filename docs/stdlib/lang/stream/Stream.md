# Stream\<T\>

`cajeta.lang.stream.Stream` — the pull-protocol base for everything
iteration-related in the stdlib. Implementers override `next()` to yield
elements; the combinator methods on `Stream` itself consume `next()` and
inherit to every subclass. A stream comes from a splittable root such as
`ArrayStream` (or an array's `.stream()`); chain lazy intermediates (`filter`,
`map`, `take`, ...) and finish with an eager terminal (`count`, `reduce`,
`collect`, ...). `.parallel()` marks the stream parallel-eligible; terminals
consult the flag to decide whether to fork, merging per-worker partials
through a combiner.

```cajeta
int32[] xs = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
int32 evens = xs.stream()
    .filter((x) -> x % 2 == 0)
    .count();                                // 5
int32 squares = xs.stream()
    .map<int32>((x) -> x * x)
    .reduce(0, (a, b) -> a + b);             // 385
int64 sum = xs.stream()
    .fold<int64>(0L, (acc, e) -> acc + e);   // 55
```

## Methods

| Signature | |
|---|---|
| `Optional<T> next()` ⚑ | Pull the next element; implementers override, the default returns an empty Optional |
| `Stream<T> parallel()` | Mark this stream as parallel-eligible |
| `Stream<T> sequential()` | Clear the parallel-eligibility flag |
| `int32 count()` | Count elements by walking `next()` to exhaustion |
| `void forEach((T) -> void fn)` | Apply `fn` to every remaining element |
| `boolean anyMatch((T) -> boolean pred)` | True if any remaining element matches `pred` |
| `boolean allMatch((T) -> boolean pred)` | True iff every remaining element matches `pred` |
| `boolean noneMatch((T) -> boolean pred)` | True iff no remaining element matches `pred` |
| `Optional<T> findFirst((T) -> boolean pred)` | First matching element, or an empty Optional |
| `T reduce(T seed, (T, T) -> #T fn)` | Left-fold with an explicit seed |
| `final R fold<R>(R seed, (R, T) -> #R fn)` | Cross-type left-fold |
| `final R fold<R>(R seed, (R, T) -> #R fn, (R, R) -> #R combiner)` | Cross-type left-fold with explicit `combiner` for the parallel path |
| `final R collect<R>(Collector<T, R> c)` | Reduce via a `Collector`'s (supplier, accumulator, combiner) triple |
| `#Stream<T> filter((T) -> boolean pred)` | Lazy intermediate: keep only elements matching `pred` |
| `#Stream<T> take(int32 n)` | Stateful intermediate: cap the stream at the first `n` elements |
| `#Stream<T> skip(int32 n)` | Stateful intermediate: drop the first `n` elements |
| `#Stream<T> peek((T) -> void fn)` | Lazy intermediate: invoke `fn` for its side effect and pass the element through |
| `final #Stream<R> map<R>((T) -> #R fn)` | Lazy intermediate: apply `fn` to each element, yielding `Stream<R>` |
| `final #Stream<R> flatMap<R>((T) -> #Stream<R> fn)` | Map each element to a stream and drain each inner stream in turn |
| `final #Stream<R> mapOrSkip<R>((T) -> #R fn)` | Like `map`, but elements whose `fn` throws `RecoverableException` are dropped |
| `final #Stream<R> mapOrFallback<R>((T) -> #R fn, R fallback)` | Like `map`, but a `RecoverableException` substitutes `fallback` |
| `final #Stream<R> mapOrLog<R>((T) -> #R fn, (T, Exception) -> void logger)` | Like `map`, but a failing element is passed to `logger` and dropped |
| `Stream<?> unwrap()` | Wrapper-chain unwind protocol (parallel infrastructure) |
| `boolean isStatefulWrapper()` | Stateful intermediate marker (parallel infrastructure) |
| `#Stream<T> cloneChainOver(#Stream<?> newRoot)` | Recursive chain re-wrap (parallel infrastructure) |
| `int64 splittableSize()` | Splittable-shape bridge (parallel infrastructure) |
| `#Stream<?> trySplitRoot()` | Wildcard-typed split entry (parallel infrastructure) |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/lang/stream/Stream.cajeta`](../../../../runtime/src/cajeta/lang/stream/Stream.cajeta)
- [ArrayStream](ArrayStream.md) — the canonical splittable source
- [Collectors](../../collection/Collectors.md) — built-in `Collector` factories for `collect`
- [Optional](../Optional.md) — `next()` / `findFirst`'s result type
