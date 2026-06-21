---
id: error-ClassCastException
applies-to: [cajeta/error/ClassCastException]
title: ClassCastException — failed unguarded capture cast at a reified interop boundary
description: Recoverable exception thrown by an unguarded capture cast (Foo<int32>)w on a reified-instantiation mismatch; how to catch it and how to branch instead with instanceof.
---

# ClassCastException

A **support / exception type** in `cajeta.error` — you do not construct it to call an
API; you either catch it or, better, structure code so it is never thrown. It signals
that an **unguarded capture cast** `(Foo<int32>) w` was applied to a wildcard value
`Foo<?>` whose runtime reified instantiation was something else (e.g. `Foo<float32>`).
cajeta monomorphizes, so a value widened to `Foo<?>` still carries its concrete
instantiation at runtime; the cast checks that identity and throws this rather than
return a mis-typed pointer (the alternative is undefined behaviour). See
reified-capture-spec.md §3.

It extends `RecoverableException` (→ `Exception` → `Throwable`), so a failed dynamic
capture is a normal, caller-handleable condition: the runtime's unrecoverable detection
chain-walks the thrown vtable, and anything reaching `RecoverableException` without
hitting `UnrecoverableException` stays **catchable** rather than aborting. The inherited
`message : String` is the only readable field that matters here (`cause` is left `0`).

## Prefer the guarded form — don't throw to branch

At an interop boundary where the instantiation is uncertain, use `instanceof` (test) or
its pattern-binding form (test + capture) to branch **without** throwing. Reach for the
unguarded cast only when a mismatch is genuinely a bug you want surfaced.

```cajeta
package test;

public class Box<T> {
    T value;
    public Box(T v) { this.value = v; }
    public T get() { return this.value; }
}

public final class D {
    public static int32 run() {
        Box<int32> bi = heap Box<int32>(99);
        Box<?> w = bi;                       // widened to wildcard

        // Guarded: pattern binding captures the concrete handle, no throw.
        if (w instanceof Box<int32> f) {
            return f.get();                  // f : Box<int32>, same pointer
        }
        return -1;                           // mismatch falls through, nothing thrown
    }
}
```

## Catch it when you do use the unguarded cast

```cajeta
package test;
import cajeta.error.ClassCastException;

public class Box<T> {
    T value;
    public Box(T v) { this.value = v; }
    public T get() { return this.value; }
}

public final class D {
    public static int32 run() {
        Box<int32> bi = heap Box<int32>(7);
        Box<?> w = bi;
        int32 result = -1;
        try {
            Box<float32> bad = (Box<float32>) w;   // mismatch: int32 vs float32
            result = 100;                          // not reached
        } catch (ClassCastException e) {
            result = 7;                            // recover; e.message describes it
        }
        return result;
    }
}
```

The capture cast also works for nested (`Box<Box<int32>>`) and template-supertype
(`Container<int32>` from a `List<int32>` wildcard) targets; the same throw rule applies
on mismatch. A guarded cast after a passing `instanceof` recovers the concrete handle
with no copy (same pointer).

## What it does not do

- It does **not** carry the source/target type names as structured fields — only the
  inherited `message : String`. Read `e.message`; do not look for a `targetType` getter.
- It is **not** thrown by `instanceof` or by the pattern-binding form — those return
  false / skip the branch on a mismatch. Only the **unguarded** `(T) w` cast throws.
- A matching unguarded cast does **not** throw; it just recovers the handle.
- It is not an `UnrecoverableException`, so it will not abort the program — a bare
  uncaught throw still unwinds as a normal recoverable exception.

For the catchable/abort split and the shared `message`/`cause` fields, see the
`RecoverableException` / `Exception` / `Throwable` chain (`cajeta/error`,
docs/specification/error/ErrorModel.md).
