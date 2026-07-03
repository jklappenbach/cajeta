# Collectors

`cajeta.collection.Collectors` — standard `Collector` factories. Each method
returns a `Collector` that `Stream`'s `collect` terminal can consume. The
factories are method-level templated, so each call site monomorphizes per `T`,
and every collector ships a combiner alongside the (seed, accumulator) pair so
`.parallel().collect(...)` can merge per-worker partials.

```cajeta
// Stream -> collect -> owned list:
int32[] data = { 3, 1, 2 };
ArrayList<int32> xs = (heap ArrayStream<int32>(data, 3))
    .collect(Collectors.toList<int32>());
```

## Methods

| Signature | |
|---|---|
| `static #Collector<T, ArrayList<T>> toList<T>()` ⚑ | Collect every element into a fresh `ArrayList<T>`; the combiner appends right onto left via `appendAll` |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/collection/Collectors.cajeta`](../../../runtime/src/cajeta/collection/Collectors.cajeta)
- [Stream](../lang/stream/Stream.md) — the `collect` terminal
- [ArrayStream](../lang/stream/ArrayStream.md) — the usual stream source
- [ArrayList](ArrayList.md) — `toList`'s accumulator
