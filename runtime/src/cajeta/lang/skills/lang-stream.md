---
id: lang-stream
applies-to: [cajeta/lang/stream/Stream]
title: Stream<T> — the pull-protocol base every combinator extends
description: The next()/Optional pull contract plus the unwrap/cloneChainOver/split hooks an operator subclass must override.
---

# Stream<T> — the base/entry-point class

`Stream<T>` is the base class of the whole `cajeta.lang.stream` pipeline AND the API
surface that carries every combinator. Two audiences read this page:

- **Consumers** hold a value typed `Stream<T>` / `#Stream<T>` and chain lazy
  intermediates + one eager terminal. For *which* method to call, the routing lives one
  level up — see the package skill `cajeta/lang/stream`. Do not enumerate the combinators
  here.
- **Operator authors** subclass `Stream` and override the small **protocol surface**
  below. That is what this skill details.

You almost never write `heap Stream<T>(...)` directly: the base `next()` returns an
**empty** Optional, so a bare `Stream` is a zero-element stream. You get a concrete
subclass (e.g. `ArrayStream`) or you *write* one.

## The pull contract: `next() -> Optional<T>`

```cajeta
public Optional<T> next() {            // base default — yields nothing
    return stack Optional<T>(false);
}
```

- Returns a **stack** `Optional<T>` (`Optional` lives in `cajeta.lang`). `isPresent()` =
  an element follows; `isEmpty()` = the stream is exhausted. Callers loop
  `o = s.next(); while (o.isPresent()) { ... o = s.next(); }`.
- **Ownership: `next()` moves the element out** — present results are built
  `stack Optional<T>(true, #v)`. The element leaves the producer. Roots that share a
  backing buffer (`ArrayStream`) still move the *element value* out per pull; ownership
  of the buffer itself stays with whoever owns it (package skill has the buffer rule).
- `Optional<T>.get()` **throws on empty** (raw `throw 1`, the None-unwrap error), so
  guard every `get()` with `isPresent()` / `isEmpty()` — there is no safe sentinel.

## What an operator subclass overrides

A lazy intermediate holds its upstream as a **`#Stream<T> source`** ctor arg (it **takes
ownership** of the source — the caller must stop using the stream it passed in) and pulls
through it in `next()`. The element type may change: type-changing wrappers extend
`Stream<R>`, not `Stream<T>`.

The remaining overrides exist only so the parallel driver can walk and rebuild the chain.
Their **base defaults declare "I am a non-splittable chain root,"** so a plain
sequential operator can skip them:

| method | base default | override when |
|---|---|---|
| `Stream<?> unwrap()` | `null` (= chain root) | a wrapper: return `this.source` |
| `#Stream<T> cloneChainOver(#Stream<?> newRoot)` | `(Stream<T>) newRoot` (root passthrough) | a wrapper: rebuild over `newRoot` (below) |
| `boolean isStatefulWrapper()` | `false` | the op depends on encounter order (`take`/`skip` return `true`) |
| `int64 splittableSize()` | `-1L` | a `Splittable` root: delegate to `estimateSize()` |
| `#Stream<?> trySplitRoot()` | `null` | a `Splittable` root: delegate to `trySplit()` |

`cloneChainOver` is recursive: rebuild the source chain over the split share, then wrap
the result in a fresh copy of self. **Inline the recursion — do not bind the inner result
to a local**, or that local's scope-exit drop frees the inner stream the new wrapper just
captured:

```cajeta
public #Stream<T> cloneChainOver(#Stream<?> newRoot) {
    return heap FilterStream<T>(this.source.cloneChainOver(#newRoot), this.pred);
}
```

A `take`/`skip`-style stateful wrapper returning `true` from `isStatefulWrapper()` causes
the driver to throw `CAJETA_ERROR_STREAM_PARALLEL_REJECT_STATEFUL` if it appears in a
parallel chain.

## Writing a minimal operator — worked example

```cajeta
import cajeta.lang.stream.Stream;
import cajeta.lang.stream.ArrayStream;

// A lazy "drop nulls / keep all" pass-through that also counts pulls.
public class TapStream<T> extends Stream<T> {
    Stream<T> source;
    (T) -> void tap;

    public TapStream(#Stream<T> source, (T) -> void tap) {
        this.source = source;          // takes ownership of the upstream
        this.tap = tap;
    }

    public Optional<T> next() {
        Optional<T> o = this.source.next();
        if (o.isPresent()) {
            this.tap(o.get());         // guarded get(); element forwarded unchanged
        }
        return o;
    }

    public Stream<?> unwrap() { return this.source; }   // wrapper, not a root

    public #Stream<T> cloneChainOver(#Stream<?> newRoot) {
        return heap TapStream<T>(this.source.cloneChainOver(#newRoot), this.tap);
    }
}

// drive it from a real source:
int32[] nums = { 1, 2, 3, 4 };
Stream<int32> s = heap TapStream<int32>(heap ArrayStream<int32>(nums, 4), (x) -> print(x));
int32 n = s.count();   // terminal pulls next() to exhaustion
```

## State, lifecycle, concurrency

- **Single-pass / consumable.** A stream has no rewind; once a terminal drains `next()`
  to empty it is spent. To re-stream, build a fresh source.
- **No `close()` / no dispose.** Streams drop on scope exit like any value.
- `parallel()` / `sequential()` just flip the `isParallel` field and return `this` (no
  allocation); wrappers copy the source's flag at construction. The **terminal** decides
  whether to actually fork — and only if the chain root is `Splittable`. The cross-cutting
  parallel/error contract (associativity, the type-changing-wrapper split-walk limitation,
  recovery-map semantics) is package-level — see `cajeta/lang/stream`, not here.
- `next()` is **not** thread-safe; the parallel path never shares one stream across
  workers — it splits into disjoint shares first (see `Splittable`, `ArrayStream`).

## What this class does NOT do / gotchas

- **No abstract-method enforcement.** Forgetting to override `next()` does not fail to
  compile — your stream silently yields zero elements. Always override `next()`.
- The cross-type combinators (`fold<R>`, `map<R>`, `collect<R>`, `flatMap<R>`,
  `mapOr*<R>`) are `final` / non-virtual — method-level templates cannot sit in a vtable.
  The only virtual surface a subclass customizes is `next()` plus the five chain-walk
  hooks above.
- No static factory: there is no `Stream.of(...)`. Start from a source root (`ArrayStream`,
  the `HashMap*Stream` family, or a collection's `.stream()`).
- Errors are `cajeta.error.Exception` (thrown by stateful ops / comb-less fold on a
  parallel stream); element terminals like `findFirst` report absence via an empty
  `Optional`, not an exception.
