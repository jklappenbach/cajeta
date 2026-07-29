# Optional\<T\>

`cajeta.lang.Optional` — value-typed sum: present or empty. Stack-allocated by
default; `T` may be any type — primitive, class reference, struct, even
another Optional. The v1 surface is the minimal Java-style core: construction
via the constructor, inspection (`isPresent` / `isEmpty`), extraction (`get` /
`orElse`). `get()` on an empty Optional throws
[`NoOptionalValueException`](../error/NoOptionalValueException.md)
(recoverable — catchable by type); guard with `isPresent()` or use
`orElse()` when a fallback exists.

```cajeta
Optional<int32> hit = stack Optional<int32>(true, 42);
Optional<int32> miss = stack Optional<int32>(false, 0);
if (hit.isPresent()) {
    int32 v = hit.get();            // 42
}
int32 fallback = miss.orElse(-1);   // -1
```

## Methods

| Signature | |
|---|---|
| `Optional(boolean present, #T value)` ⚑ | Build an Optional directly: `true` with the held value, or `false` for empty (the value slot is still consumed — pass a zero/default) |
| `boolean isPresent()` | True when a value is held |
| `boolean isEmpty()` | True when empty |
| `T get()` | Extract the value; throws `NoOptionalValueException` when empty |
| `T orElse(T fallback)` | The held value when present, otherwise `fallback` |

⚑ = `@EntryPoint`

## See also

- Tour: [AsyncDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/AsyncDemo.cajeta),
  [ConcurrentDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/ConcurrentDemo.cajeta)
- Source: [`runtime/src/cajeta/lang/Optional.cajeta`](../../../runtime/src/cajeta/lang/Optional.cajeta)
- [Stream](stream/Stream.md) — `next()` and `findFirst` yield Optionals
