# 15 — Errors & Stack Traces

This chapter defines the error model — the throwable hierarchy and its two tiers, `throw` and handler selection, `finally` on the unwind path, `throws` clauses as documentation rather than enforcement, and stack traces. The syntax is the familiar `try` / `catch` / `finally` / `throw`. There are no error value types and no propagation operators.

## 15.1 The Hierarchy

Four root types live in `package cajeta.error`, implicitly available to every program:

```text
Throwable                       carries `message`
└── Exception                   adds `cause` — the chain of causality
    ├── UnrecoverableException  the alarm: the program should not continue
    └── RecoverableException    normal failure the caller may handle
```

A user-defined exception extends one of the two tiers. The choice is the author's statement about whether callers are expected to handle it. Domain exceptions live in their owning packages (`cajeta.io.file.IoException`, `cajeta.time.DateTimeException`, …), each extending a root.

`Exception(message)` and `Exception(message, cause)` are the constructor shapes. `getCause()` returns an `Optional<Throwable>`, and printing walks the cause chain so every layer's contribution is visible.

## 15.2 `throw` and Handler Selection

`throw expr;` throws a `Throwable`. Unwinding proceeds to the nearest enclosing `try` with a `catch` clause whose type matches the thrown value — exact type or supertype — testing a `try`'s clauses in order and selecting the first match. Every owning local between the throw and the handler drops before the handler runs (Allocation §4.2), so a handler observes a world already cleaned up behind the throw.

**Example 15.2-1.** Clause order and the cause chain.

```cajeta
import cajeta.error.Exception;
import cajeta.error.RecoverableException;
public class LowError extends RecoverableException {
    public LowError(#String m) { super(#m); }
}
public final class C {
    public static int32 run() {
        try {
            throw heap LowError("disk");
        } catch (LowError e) {
            System.stdout.println("specific: " + e.getMessage());
        } catch (RecoverableException e) {
            System.stdout.println("general");
        }
        try {
            throw heap Exception("wrapped", heap LowError("root"));
        } catch (Exception e) {
            System.stdout.println("caught: " + e.getMessage());
            return 2;
        }
    }
}
System.stdout.println(C.run());     // specific: disk / caught: wrapped / 2
```

Throwing an `UnrecoverableException` terminates the process after the drop chain unwinds. It is not intended to be handled, and matching it in a general `catch` does not change its meaning.

An uncaught throw in a script unit prints the message and a trace and exits non-zero (Script Units §18).

> *Discussion.* The grammar admits a multi-catch clause, `catch (A | B e)`, and as of 0.27.0 only the first alternative is honored — the rest are parsed and dropped, so a throw of `B` passes the clause by and escapes to the next enclosing handler. The clause reads as a handler and does not act as one. The gap is recorded as a disabled pinning test in `test/expression/CatchMatchingTests.cpp`, and this section governs once it passes. Until then, write one clause per type.

## 15.3 `finally`

A `finally` block runs on every exit from the `try` it guards. Normal completion of the guarded block runs it, so does a `return` out of the block, so does a throw the `try` handles, and so does a throw that passes through unhandled. A `try` may carry a `finally` with no `catch` clause, which guards without handling — the block runs and the throw continues to the next enclosing handler.

The guarded block's owning locals drop before the `finally` body runs, on both the normal and the exceptional path (Allocation §4.2). A `finally` body sees the guarded block's locals already released.

A `throw` from inside a `finally` replaces the exception in flight. The original is not delivered and is not recorded as its cause, and the handler that runs is the one selected for the new throw.

**Example 15.3-1.** Drop order on the unwind path. The owning local is released, then the `finally` runs, then the exception reaches the outer handler.

```cajeta
import cajeta.error.Exception;
public class Probe {
    int32 id;
    public Probe(int32 id) { this.id = id; }
    public ~Probe() { System.stdout.println("drop " + this.id); }
}
public final class C {
    public static int32 run() {
        try {
            try {
                Probe p = heap Probe(1);
                throw heap Exception("boom");
            } finally {
                System.stdout.println("inner finally");
            }
        } catch (Exception e) {
            System.stdout.println("outer catch");
        }
        return 0;
    }
}
```

`C.run()` prints `drop 1`, then `inner finally`, then `outer catch`.

**Example 15.3-2.** A `return` out of the guarded block still runs the `finally` before the value leaves.

```cajeta
public final class C {
    public static int32 run() {
        try {
            System.stdout.println("try");
            return 7;
        } finally {
            System.stdout.println("finally");
        }
    }
}
```

`C.run()` prints `try`, then `finally`, then returns 7.

Statement syntax and clause placement are Statements §13.5.

## 15.4 `throws` Clauses

A method's `throws` clause lists the `RecoverableException` subtypes that can flow out of it. The clause documents. The compiler warns when a call site does not acknowledge a declared throw, and never rejects — there is no enforced checked-exception cascade.

## 15.5 Stack Traces

`Throwable.getStackTrace()` returns `StackFrame[]`. Each frame carries the declaring type, method, source file, and line. Frames of script units render as `<script>` with the host file and line — the synthesized wrapper class and entry never appear (Script Units §18). Tracebacks in diagnostics name the user's source positions the same way.

> *Discussion.* Trace capture on every `RecoverableException` throw is deliberately not promised — capture has a cost, and the recoverable tier is the hot one. A structured-diagnostics refactor (typed context fields, stable ids, one schema spanning compile-time and runtime diagnostics, semantic traces for fiber `await` chains) is specified in draft and will revise this section when it ships.

## 15.6 Diagnostics as API

Compile-time diagnostics carry stable `CAJETA_ERROR_*` / `CAJETA_WARN_*` codes (Introduction §1.3.2): the code is the contract, the message text is prescriptive prose for the human or agent reading it, and both render identically under JIT execution and ahead-of-time compilation. A program's error behavior — what is thrown, what is caught, what terminates — is identical in every program shape (Execution §20).
