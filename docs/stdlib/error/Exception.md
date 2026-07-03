# Exception

`cajeta.error.Exception` — general-purpose thrown error and the base of the
catchable exception hierarchy. Extends `Throwable` (the hierarchy root, which
carries the `message` field) and is itself the parent of
[RecoverableException](RecoverableException.md) and
[UnrecoverableException](UnrecoverableException.md) — the runtime walks that
vtable chain on a throw to decide whether it propagates to abort. Beyond the
inherited `message`, `Exception` adds a public `cause` link so a wrapped
lower-level failure can be carried along; it is `0` (none) when the throw has
no underlying cause.

```cajeta
try {
    throw heap Exception("config file missing");
} catch (Exception e) {
    String why = e.message;
}
```

## Methods

| Signature | |
|---|---|
| `Exception(#String message)` ⚑ | Construct an exception carrying `message`; `cause` is initialized to none (`0`) |

⚑ = `@EntryPoint`

## See also

- Tour: [ErrorsDemo](../../../samples/tour/src/main/cajeta/tour/error/ErrorsDemo.cajeta)
- Source: [`runtime/src/cajeta/error/Exception.cajeta`](../../../runtime/src/cajeta/error/Exception.cajeta)
- [RecoverableException](RecoverableException.md) — for errors a caller can handle;
  [UnrecoverableException](UnrecoverableException.md) — for fatal conditions
