# RecoverableException

`cajeta.error.RecoverableException` — recoverable subtype of
[Exception](Exception.md): errors a caller might reasonably handle and proceed
past — a failed parse, a missing record, transient I/O. Throw one (or a domain
subclass of it) when the caller has a sensible fallback and the program can
continue. Contrast with [UnrecoverableException](UnrecoverableException.md):
the runtime's unrecoverable detection chain-walks a thrown instance's vtable,
so anything reaching `RecoverableException` stays catchable rather than
propagating to abort.

```cajeta
try {
    throw heap RecoverableException("record not found");
} catch (RecoverableException e) {
    String why = e.message;   // recover and keep going
}
```

## Methods

| Signature | |
|---|---|
| `RecoverableException(#String message)` ⚑ | Wrap a heap `#String` message into a throwable recoverable error; `cause` is left unset |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/error/RecoverableException.cajeta`](../../../runtime/src/cajeta/error/RecoverableException.cajeta)
- [Exception](Exception.md), [UnrecoverableException](UnrecoverableException.md)
