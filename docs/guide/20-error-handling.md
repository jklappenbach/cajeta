# 20 — Error handling

Java-style `try` / `catch` / `throw`, with one deliberate change: `throws`
clauses document, they don't enforce.

Tour demo: [ErrorsDemo](../../samples/tour/src/main/cajeta/tour/error/ErrorsDemo.cajeta).

## The hierarchy

One root lives in `cajeta.error`: every throwable type derives from
[`Throwable`](../stdlib/error/Throwable.md).

- `Throwable` — the root; carries `message` and the whole diagnostics
  surface below.
- `Exception extends Throwable` — adds `cause` for chain-of-causality.
- `RecoverableException extends Exception` — normal failure the caller may
  handle.
- `UnrecoverableException extends Exception` — the alarm; the program
  shouldn't continue.

Domain exceptions extend one of these from their owning packages
(`cajeta.io.file.IoException`, `cajeta.io.net.TimedOutException`,
`cajeta.codec.Base64Exception`, ...). Your own exceptions do the same. One
mechanical note: `super(...)` isn't supported yet, so a subclass constructor
writes the inherited fields directly (`this.message = message;`).

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
            System.stdout.println("recovered: " + e.getMessage());
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

## Inspecting a throwable

`Throwable` carries the trace and diagnostics surface — every exception
inherits it:

```cajeta
import cajeta.error.StackFrame;
import cajeta.error.Throwable;

public class Reporter {
    public void report(Throwable t) {
        String msg = t.getMessage();
        Optional<Throwable> cause = t.getCause(); // empty when none
        StackFrame[] frames = t.getStackTrace();  // throw-site first
        t.printStackTrace();                      // human-readable, to stderr
        String json = t.toJson();                 // one NDJSON diagnostic object
    }
}
```

Each [`StackFrame`](../stdlib/error/Throwable.md) carries `declaringType`,
`method`, `file`, `line`, a role (User / Stdlib / Runtime), and the raw
`nativeAddress`. The semantic fields are resolved from the runtime's line-info
shadow stack — no debug info needed; when capture was off
(`--stack-trace-capture=off`) or unavailable, frames fall back to native
addresses only.

## Machine-readable diagnostics

Every throwable has a stable diagnostic `code()` — by default its canonical
type name (`cajeta.error.RecoverableException`). Pin a refactor-proof
identifier with `@DiagnosticCode`:

```cajeta
import cajeta.error.DiagnosticCode;
import cajeta.error.Exception;

@DiagnosticCode("APP_ERR_CONFIG_MISSING")
public class ConfigMissingException extends Exception {
    public ConfigMissingException(#String message) {
        this.message = message;
        this.cause = null;
    }
}
```

`toJson()` renders the throwable as one NDJSON object: `severity`, `code`,
`message`, a `category` object (`retryable` / `transient` / `userActionable`
— overridable predicates on `Throwable`), optional `remediation`
(`hint()` / `docUrl()`), the `causeChain` outer-to-inner, and `frames`.

The compiler flag `--diag-format=json` puts the whole toolchain on that
wire format: compile-time diagnostics come out as one NDJSON object per
line on stderr, and an *uncaught* throw at runtime is reported the same
way instead of the human-readable trace:

```json
{"severity":"error","code":"cajeta.error.RecoverableException","message":"nobody catches this","frames":[{"declaringType":"snip.Uncaught","method":"run","file":"Uncaught.cajeta","line":7,"role":"User"}]}
```

## No try-with-resources

Cajeta has no `try (R r = ...) { ... }` form. It was briefly in the grammar
and removed as redundant: destructors already run at the closing `}` of the
declaring block, in LIFO order, on return and on unwind. Declare the resource
as an owned local and cleanup is guaranteed. A Lombok-style `@Cleanup` for
close-before-drop cases is planned but not implemented.

Full model: [the error specification](../specification/error/ErrorModel.md).

Next: [Reflection](21-reflection.md).
