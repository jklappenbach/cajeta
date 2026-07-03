# UnrecoverableException

`cajeta.error.UnrecoverableException` — fatal subtype of
[Exception](Exception.md) for conditions a program cannot sanely continue
past: invariant violations, programmer error, out-of-memory. Unlike
[RecoverableException](RecoverableException.md), it is not meant to be caught
and worked around — codegen registers this class's vtable with the runtime,
and when a thrown instance's vtable chain reaches it the throw bypasses user
`catch` handlers and propagates straight to abort.

```cajeta
int32 slot = 0;
if (slot == 0) {
    throw heap UnrecoverableException("allocator returned null slot");
}
```

## Methods

| Signature | |
|---|---|
| `UnrecoverableException(#String message)` ⚑ | Build a fatal exception carrying `message` (ownership transfers in); `cause` is left unset (`0`) |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/error/UnrecoverableException.cajeta`](../../../runtime/src/cajeta/error/UnrecoverableException.cajeta)
- [Exception](Exception.md), [RecoverableException](RecoverableException.md)
