# ArrayStream\<T\>

`cajeta.lang.stream.ArrayStream` — a `Stream` backed by a contiguous `T[]`
buffer over the index range `[idx, limit)`. Because the backing store is a
flat array, `ArrayStream` implements `Splittable` cheaply: `trySplit()` halves
the index range without copying any elements, making it the canonical
parallel-friendly source for the streams pipeline. Elements are yielded one at
a time as `Optional` values — present while elements remain, empty once the
cursor reaches `limit`.

```cajeta
int32[] data = { 10, 20, 30, 40 };
ArrayStream<int32> s = heap ArrayStream<int32>(data, 4);

Optional<int32> o = s.next();
while (o.isPresent()) {
    o = s.next();
}
int64 remaining = s.estimateSize();   // 0 — drained
```

## Methods

| Signature | |
|---|---|
| `ArrayStream(T[] data, int32 limit)` ⚑ | Wrap an existing `T[]` buffer as a stream over `[0, limit)`; the buffer is shared, not copied |
| `Optional<T> next()` | Yield the next element wrapped in `Optional` and advance the cursor |
| `int32 count()` | Count terminal |
| `#Stream<T> trySplit()` | Halve the remaining index range |
| `int64 estimateSize()` | Exact remaining element count (`limit - idx`), O(1) |
| `int64 splittableSize()` | `Splittable` size hint; exact for an array-backed stream |
| `#Stream<?> trySplitRoot()` | Wildcard-typed split entry for the parallel chain walker |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/lang/stream/ArrayStream.cajeta`](../../../../runtime/src/cajeta/lang/stream/ArrayStream.cajeta)
- [Stream](Stream.md) — the combinators inherited by every stream
- [ArrayList](../../collection/ArrayList.md) — `stream()` hands back an `ArrayStream`
