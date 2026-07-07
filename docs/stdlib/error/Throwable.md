# Throwable

`cajeta.error.Throwable` — root of the cajeta exception hierarchy: every
throwable type derives from it, carrying a human-readable `message`.
[Exception](Exception.md) extends it with a `cause` chain and is in turn the
parent of [RecoverableException](RecoverableException.md) and
[UnrecoverableException](UnrecoverableException.md). Beyond the message, a
`Throwable` carries the diagnostic surface: a stable `code()` (the canonical
type name unless `@DiagnosticCode` overrides), overridable category flags
(`isRetryable` / `isTransient` / `isUserActionable`) and remediation hints
(`hint` / `docUrl`), the captured stack via `getStackTrace()` — semantic
`StackFrame`s (type, method, file, line, role) when line-info was on, raw
native addresses otherwise — and `toJson()`, which renders the whole thing as
one NDJSON diagnostic object.

```cajeta
try {
    throw heap Exception("config load failed", heap Exception("file missing"));
} catch (Exception e) {
    Throwable t = e;
    String msg = t.getMessage();
    StackFrame[] frames = t.getStackTrace();
    String json = t.toJson();
}
```

## Methods

| Signature | |
|---|---|
| `Throwable(#String message)` ⚑ | Wrap a message into a throwable |
| `String getMessage()` | The human-readable message this throwable was constructed with |
| `#String code()` | The stable diagnostic code; defaults to the canonical type name, `@DiagnosticCode` overrides |
| `Optional<Throwable> getCause()` | The underlying cause; empty when none — `Exception` overrides this to wrap its `cause` field |
| `boolean isRetryable()` | Whether retrying the failed operation may succeed (default `false`) |
| `boolean isTransient()` | Whether the failure is transient — environmental, likely self-clearing (default `false`) |
| `boolean isUserActionable()` | Whether a human/agent action is required to resolve this (default `false`) |
| `Optional<String> hint()` | A short human/agent-readable fix hint; empty when none |
| `Optional<String> docUrl()` | A documentation URL for this error; empty when none |
| `#String toJson()` ⚑ | Render as one NDJSON diagnostic object: `severity`/`code`/`message` plus `category`, `remediation`, `causeChain` (outer→inner), and `frames` |
| `StackFrame[] getStackTrace()` ⚑ | The captured stack frames, throw-site first; empty when capture was off |
| `void printStackTrace()` ⚑ | Print this throwable's stack trace to stderr |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/error/Throwable.cajeta`](../../../runtime/src/cajeta/error/Throwable.cajeta),
  [`StackFrame.cajeta`](../../../runtime/src/cajeta/error/StackFrame.cajeta),
  [`FrameRole.cajeta`](../../../runtime/src/cajeta/error/FrameRole.cajeta)
- [Exception](Exception.md) — adds the `cause` chain;
  [RecoverableException](RecoverableException.md) / [UnrecoverableException](UnrecoverableException.md) — the catchable/fatal split
