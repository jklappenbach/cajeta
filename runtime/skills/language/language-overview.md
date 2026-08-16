---
id: language-overview
applies-to: [cajeta.language, cajeta/language]
title: The cajeta language — delta map from Java/C++, routing, and traps
description: What cajeta is (Java surface, per-position ownership, no GC), what it is not, and which language skill to read before writing each kind of code.
---

# The cajeta language — orientation

Cajeta reads like Java but is not garbage-collected: every heap value has
exactly one owner and is reclaimed deterministically at scope exit. Ownership
is decided per POSITION and is runtime-conditional on both sides of a call —
between names the spelling decides (`=` lends, `#=` transfers), at an argument
the CALLER decides (`f(x)` lends, `f(#x)` transfers), and at a result the
CALLEE does, on the return flag: a `#T` result is a forced transfer and must be
received with `#=`, not `=`. Write Java-shaped code, but read
`cajeta/language/ownership` before any code that stores, returns, or passes a
heap value, or your first crash will be the compiler stopping you (good) or
memory misuse you shipped (bad).

## Task → skill routing

| About to write… | Read first |
|---|---|
| Anything at all (first cajeta code) | this skill + `cajeta/language/ownership` |
| Declarations, primitives, `heap`/`stack`, records, enums, views | `cajeta/language/types` |
| Classes, inheritance, interfaces, operators | `cajeta/language/classes` |
| Templated (generic-looking) types or methods | `cajeta/language/templates` |
| Lambdas or anything that captures | `cajeta/language/lambdas` |
| `spawn` / `async` / concurrent code | `cajeta/language/concurrency` |
| `throw` / `try` / resource cleanup | `cajeta/language/errors` |
| Annotations, `@Builder`-style synthesis, DI | `cajeta/language/annotations` |
| A stdlib capability (I/O, JSON, collections, …) | `cajeta.stdlib` (router) |
| Project setup, build, run, test | `cajeta.toolchain` |

## What cajeta is NOT (Java/C++ instincts that misfire)

- **No `new`.** Construction names its placement: `heap Point(3, 4)` or
  `stack Point(3, 4)`. Omitting the prefix is a compile error.
- **No GC, no `delete`.** Owners drop at their block's closing `}` (LIFO, also
  on the exception path). Lifetime bugs are compile errors, not leaks.
- **No `int`/`long`/`float`/`double`/`byte`.** Widths are explicit (`int32`,
  `float64`, …); a byte buffer is `int8[]`/`uint8[]`. **No implicit numeric
  widening** — every cross-width conversion is a cast.
- **No erasure generics.** Templates monomorphize; `T` is a real type at
  compile time. Say (and think) "templates", never "generics".
- **No try-with-resources / RAII keyword.** Drop-on-scope *is* the resource
  pattern; there is nothing to close-in-a-header (`cajeta/language/errors`).
- **No default methods in interfaces**, no interface state.
- **`var` parses but does not resolve yet** — write the type.
- **`System.out` doesn't exist** — output is `System.stdout.println(...)`
  (`import cajeta.lang.System`).

## Reserved words that bite

`spawn`, `scope`, `async`, `await`, `detach` are structured-concurrency
keywords — you cannot name a method or local `scope`. `annotation` is the
type-kind keyword (there is no `@interface`). `structure` and `goto` are
reserved with no syntax; `true`/`false`/`null` are literals. Full table:
`docs/guide/06-keywords.md`.

## Sixty seconds of cajeta

```cajeta
package dev.cajeta.skills;

import cajeta.lang.System;

public class Point {
    public int32 x;
    public int32 y;

    public Point(int32 x, int32 y) {
        this.x = x;
        this.y = y;
    }

    public int32 distSq() { return this.x * this.x + this.y * this.y; }
}

public class Hello {
    public static int32 run() {
        Point p = heap Point(3, 4);
        System.stdout.println("distSq = " + p.distSq());
        return p.distSq();      // p drops at scope exit — no delete, no GC
    }
}
```

Run it: `cajeta jit-run <source-root> dev.cajeta.skills.Hello.run` (prints
`distSq = 25`, exits 25). Toolchain detail: `cajeta/toolchain/jit-run`.

## Depth

The full narrative lives in `docs/guide/` (chapters 06–22); language
specification under `docs/specification/lang/`. Skills carry the minimum an
expert needs — the guide carries everything.
