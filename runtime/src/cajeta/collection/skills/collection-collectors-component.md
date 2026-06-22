---
id: collection-collectors-component
applies-to: [cajeta/collection/Collector, cajeta/collection/Collectors]
title: Collector / Collectors — the Stream.collect reduction seam
description: How Collector bundles supplier/accumulator/combiner for Stream.collect<R>, and the Collectors factories (toList) that build them.
---

# Collector + Collectors — reducing a Stream into one value

To turn a `Stream<T>` into a single result `R`, you pass a `Collector<T, R>` to
`Stream.collect<R>(c)`. A `Collector` is just a struct of three callables; `Collectors`
is the factory class of ready-made ones. **For the common "drain into a list" case do
not hand-roll — call `Collectors.toList<T>()`.** Build a `Collector` directly only for a
custom reduction (sum, count, into your own type).

## Members and roles

- **`Collector<T, R>`** (`Collector.cajeta`) — value/support type. Three public
  function-typed fields, set once by the constructor:
  - `supplier : () -> #R` — makes a fresh, *owned* accumulator.
  - `accumulator : (R, T) -> #R` — folds one element in; returns the accumulator to use
    next (idiomatically the same instance).
  - `combiner : (R, R) -> #R` — merges two partials into one (used only under
    `.parallel()`, but **always required**).
- **`Collectors`** (`Collectors.cajeta`) — factory class. Each factory is
  method-level-templated (`static <T> ...`) so it monomorphizes per element type;
  `Collectors.toList<int32>()` and `Collectors.toList<String>()` are distinct symbols.
  Current surface: **`toList<T>()` only.**
- **`ArrayList<T>`** (`ArrayList.cajeta`) — the default accumulator behind `toList`; its
  `appendAll` is the combiner. See `collection/ArrayList`.

## How they cooperate (object graph + call sequence)

`Stream.collect<R>(c)` (`cajeta/lang/stream/Stream`) drives it:

- **Sequential** — `this.fold(c.supplier(), c.accumulator)`: one `supplier()` call seeds
  the fold, then `accumulator(acc, element)` runs per element. The combiner is never
  called.
- **Parallel** (`.parallel()`) — each worker fiber calls `c.supplier()` for its *own* R
  partial (so a mutable R like `ArrayList` never aliases across workers and races on
  `add`), then `ParallelDriver.collectParallelChain` merges partials pairwise via
  `c.combiner(left, right)` in worker-slot order.

This is why **both supplier and combiner are mandatory** even for sequential-only use:
a caller can add `.parallel()` later without the collector being re-engineered.

## Ownership across the boundary

- `supplier` returns `#R` (owned). Each call mints a *fresh* accumulator — never share
  one seed instance.
- `accumulator` and `combiner` return `#R`: ownership of the accumulator flows through
  the fold and out of `collect`. `collect`'s result is therefore **owned by the caller**
  (`#ArrayList<T>` from `toList`) — store it in a `#`-typed slot and you are responsible
  for its drop.
- For `toList`, the combiner uses `ArrayList.appendAll`, which copies elements in and
  does **not** consume `right` — but under collect the orchestrator owns both partials,
  so you never see the leftover.

## When to use which

- `Collectors.toList<T>()` — accumulate every element into an owned `ArrayList<T>`,
  in stream order. The accumulator returns the same list each step (no per-step copy).
- Hand-rolled `Collector<T, R>` — scalar reductions (sum, count) or folding into a
  non-list R. Make `R` a value type (e.g. `int32`) and the combiner an associative
  merge (`(a, b) -> a + b`).

## What this seam does NOT do

- `Collectors` has **no** `toSet` / `toMap` / `joining` / `groupingBy` yet — `toList` is
  the only factory. For anything else, construct a `Collector` directly.
- A `Collector` carries no state and is not a finisher: there is no transform applied
  after the fold (no Java-style `finisher`). The accumulator's running value *is* the
  result.
- `collect` does not bounds-check or deduplicate; `ArrayList` itself has no bounds checks
  in v1.

## Worked example

```cajeta
import cajeta.lang.stream.ArrayStream;
import cajeta.collection.ArrayList;
import cajeta.collection.Collector;
import cajeta.collection.Collectors;

public final class D {
    public static int32 run() {
        int32[] xs = { 1, 2, 3, 4, 5 };
        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 5);

        // Built-in: drain into an owned ArrayList (caller owns `out`).
        Collector<int32, ArrayList<int32>> c = Collectors.toList<int32>();
        ArrayList<int32> out = s.collect(c);          // #ArrayList<int32>
        return out.count();                            // 5
    }
}
```

Custom scalar reduction — sum, no list involved:

```cajeta
import cajeta.lang.stream.ArrayStream;
import cajeta.collection.Collector;

ArrayStream<int32> s = heap ArrayStream<int32>(xs, 5);
Collector<int32, int32> sum = heap Collector<int32, int32>(
    () -> 0,                                  // supplier: identity
    (int32 acc, int32 x) -> acc + x,          // accumulator
    (int32 a, int32 b) -> a + b);             // combiner (parallel merge)
int32 total = s.collect(sum);                 // 15
```

See `cajeta/lang/stream/Stream` for the `collect<R>` terminal and `collection/ArrayList`
for the accumulator type.
