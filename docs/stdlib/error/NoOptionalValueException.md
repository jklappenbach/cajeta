# NoOptionalValueException

`cajeta.error.NoOptionalValueException` — thrown by
[`Optional.get()`](../lang/Optional.md) on an empty Optional. Extends
[`RecoverableException`](RecoverableException.md): an unwrap miss is
catchable and the program continues — `UnrecoverableException` is reserved
for panic. Prefer guarding with `isPresent()` or using `orElse()`; the type
exists so an unguarded unwrap fails loudly at the fault site with a stable
diagnostic code (the class FQN).

```cajeta
Optional<int32> miss = stack Optional<int32>(false, 0);
try {
    int32 v = miss.get();
} catch (NoOptionalValueException e) {
    // reached: unwrap of an empty Optional
}
```

## Methods

| Signature | |
|---|---|
| `NoOptionalValueException(#String message)` ⚑ | Wrap a message describing the unwrap site |

⚑ = `@EntryPoint`

Inherits the full [`Throwable`](Throwable.md) surface: `getMessage`,
`getCause`, `getStackTrace`, `printStackTrace`, `toJson`, category
affordances.

## See also

- [Optional](../lang/Optional.md) — the thrower
- Source: [`runtime/src/cajeta/error/NoOptionalValueException.cajeta`](../../../runtime/src/cajeta/error/NoOptionalValueException.cajeta)
