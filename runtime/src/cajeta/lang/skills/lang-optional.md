---
id: lang-optional
applies-to: [cajeta/lang/Optional]
title: Optional<T> — value-typed present/empty sum
description: Construct, inspect, and extract from cajeta.lang.Optional<T>; guard get() which throws on empty.
---

# Optional<T> — present-or-empty value

`cajeta.lang.Optional<T>` is a **value type** (a sum of `present`/`empty`) for
"a `T`, or nothing." `T` is unconstrained: primitive, class reference, struct, or
another `Optional`. This is a **support/value type**, not an access point — you
build it directly with its constructor and read it back; you don't obtain it from
a factory yet. It is the standard return for "maybe a value" APIs (e.g.
`Channel<T>.receive()` returns `Optional<T>`).

## Construction & storage

There is **no `Some`/`None` factory in v1** — construct via the two-arg ctor:

```cajeta
public Optional(boolean present, #T value)
```

- `present == true` + the held value, or `present == false` for empty.
- The `value` slot is **still consumed even when empty** — pass a zero/default
  (e.g. `0`), it is just ignored.
- `value` is `#T`: ownership **transfers in** at the call site for owning `T`.

Choose storage like any class in the unified-class model — `stack` for a
stack-resident instance (subject to the usual lifetime rules), `heap` for a
heap-allocated one:

```cajeta
import cajeta.lang.Optional;

public final class S {
    public static int32 run() {
        Optional<int32> hit  = stack Optional<int32>(true, 42);
        Optional<int32> miss = stack Optional<int32>(false, 0);

        int32 a = -1;
        if (hit.isPresent()) { a = hit.get(); }   // 42, guarded
        int32 b = miss.orElse(-1);                 // -1, never throws
        return a + b;                              // 41
    }
}
```

## The methods that matter

- `boolean isPresent()` / `boolean isEmpty()` — inspection; never throw.
- `T get()` — returns the held value **when present**. On an **empty** Optional it
  **throws** (see gotcha). Returns the value by `T`'s normal convention.
- `T orElse(T fallback)` — held value when present, else `fallback`. **Never
  throws** — prefer this over `get()` whenever you have a sensible default.

## Gotcha: get() on empty THROWS — guard it

`get()` on an empty Optional throws integer `1` (`CAJETA_ERROR_NONE_UNWRAP`,
the error-model #205 throw shape) — a dedicated exception class lands later. It
does **not** return a zero/default. (The class-level header doc comment in the
source still says "returns zero-init"; that is stale — the method body and
`OptionalTests.getOnEmptyThrows` both throw.) Either guard with `isPresent()`
first, use `orElse(...)`, or catch:

```cajeta
import cajeta.lang.Optional;

public final class S {
    public static int32 run() {
        Optional<int32> empty = stack Optional<int32>(false, 0);
        int32 result = -1;
        try {
            result = empty.get();
        } catch (Exception e) {
            result = (int32) e;   // 1 == CAJETA_ERROR_NONE_UNWRAP
        }
        return result;            // 1
    }
}
```

## What it does NOT do (v1)

No combinators — `map` / `filter` / `flatMap` / `fold` / `orElseGet` /
`orElseThrow` / `ifPresent` / `inspect` are deferred (P6.3). No `Some`/`None`
factories (await generic-static-call syntax). No `Stream<T>` integration yet
(P6.4+). The whole v1 surface is: ctor, `isPresent`/`isEmpty`, `get`, `orElse`.
