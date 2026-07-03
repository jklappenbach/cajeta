# 20 — Error handling

Java-style `try` / `catch` / `throw`, with one deliberate change: `throws`
clauses document, they don't enforce.

Tour demo: [ErrorsDemo](../../samples/tour/src/main/cajeta/tour/error/ErrorsDemo.cajeta).

## The hierarchy

Four roots live in `cajeta.error`:

- `Throwable` — anything throwable; carries `message`.
- `Exception` — adds `cause` for chain-of-causality.
- `RecoverableException` — normal failure the caller may handle.
- `UnrecoverableException` — the alarm; the program shouldn't continue.

Domain exceptions extend one of these from their owning packages
(`cajeta.io.file.IoException`, `cajeta.io.net.TimedOutException`,
`cajeta.codec.Base64Exception`, ...). Your own exceptions do the same.

## throw, try, catch, finally

```cajeta
import cajeta.error.RecoverableException;

public class Loader {
    public int32 load(int32 port) throws RecoverableException {
        if (port < 0) {
            throw heap RecoverableException("invalid port: " + port);
        }
        return port;
    }
}
```

Catch by type; arms dispatch in source order, most-specific match first.
`finally` runs on every exit edge — fall-through, `return`, and unwind:

```cajeta
import cajeta.error.RecoverableException;

public class Robust {
    public int32 tryLoad(Loader l) {
        try {
            return l.load(-1);
        } catch (RecoverableException e) {
            System.stdout.println("recovered: " + e.message);
            return -1;
        } finally {
            System.stdout.println("attempt finished");
        }
    }
}
```

The drop chain unwinds owned locals on the throw path exactly as on the
return path, so owned resources release without a `finally`. Reach for
`finally` when the cleanup isn't tied to one owned value.

## Checked, in the advisory sense

A `throws` clause lists the `RecoverableException` subtypes that can flow
out. Call sites that don't catch or declare them get a compile **warning**
(`uncaught-throws`) — never an error, so there is no Java-style refactor
cascade. Accept propagation explicitly with:

```cajeta
import cajeta.error.RecoverableException;

public class PassThrough {
    @SuppressLint("uncaught-throws")
    public int32 forward(Loader l) {
        return l.load(8080);
    }
}
```

`UnrecoverableException` subtypes are fully unchecked: they never appear in
`throws` clauses. Anything can throw them.

## Recoverable vs unrecoverable at the top

The runtime wraps `main()` and every fiber in a default catch. An uncaught
`RecoverableException` logs and exits with code 1. An uncaught
`UnrecoverableException` logs and calls `abort()` — SIGABRT, core dump,
debugger stop — because the alarm going unanswered is itself fatal. You *can*
catch `UnrecoverableException`; the convention is don't. Throw sites capture
a stack trace by default (`--stack-trace-capture=off` disables it).

On fibers, a recoverable throw is stored on the `Task` and re-raised at
`await`; a `scope` cancels siblings with the first failure and re-raises it.

## No try-with-resources

Cajeta has no `try (R r = ...) { ... }` form. It was briefly in the grammar
and removed as redundant: destructors already run at the closing `}` of the
declaring block, in LIFO order, on return and on unwind. Declare the resource
as an owned local and cleanup is guaranteed. A Lombok-style `@Cleanup` for
close-before-drop cases is planned but not implemented.

Full model: [the error specification](../specification/error/ErrorModel.md).

Next: [Reflection](21-reflection.md).
