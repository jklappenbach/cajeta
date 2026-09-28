---
id: language-lambdas
applies-to: [cajeta/language/lambdas, cajeta/language/closures]
title: Lambdas — function types and the three capture modes
description: "Function types are primitive type-formers; capture semantics decide correctness: primitives copy by value, heap values borrow, # transfers ownership into a closure that outlives its frame."
---

# Lambdas & captures — the capture mode is the whole game

Function types are primitive type-formers: `(int32, int32) -> int32` is a
type; values of it are callable directly. There is no `Runnable` /
`@FunctionalInterface` indirection, and primitives flow unboxed. Parameter
types may be omitted when context pins the function type; otherwise
annotate — uninferable parameters are a compile error. Method references are
sugar: `MathUtil::abs` (static), `obj::method` (captures `obj`).

## The three capture modes

1. **Primitives capture by value** — copied at capture time; later writes to
   the original are invisible to the closure. Writing *to* a value-captured
   primitive inside the lambda is a **compile error** (it would only touch
   the private copy). For shared mutable state, wrap the value in a class and
   mutate through the capture.
2. **Heap values (class instances, arrays, strings) capture by borrow** —
   the closure sees live state; the original still owns. Correct only while
   the closure is used and dropped **inside** the owner's scope.
3. **`#` transfers ownership into the closure** — for closures that outlive
   the frame (returned, stored). `() -> #c.next()` moves `c` into the
   closure; the factory body cannot touch `c` afterward, and the capture
   drops when the closure drops.

Choosing wrong is failure class (a): a borrow-capturing closure that escapes
its captures' scope reads freed memory. **No diagnostic catches it today**, so
when the closure must escape, transfer (`#`) what it captures.

## Measured capture lifetimes

Each row is pinned by `test/ownership/ClosureCaptureLifetimeTests.cpp`, in the
factory shape a middleware or callback registry takes: a factory builds a
lambda over a value, stores the lambda in a keeper, and returns the keeper.

| Spelling in the factory | The captured value | Test |
|---|---|---|
| `(x) -> x + cfg.v`, `cfg` a `#` formal | freed when the factory returns, then read through freed memory | `capturedSharpFormal` |
| `(x) -> x + cfg.v`, `cfg` a local | the same | `capturedLocal` |
| `(x) -> x + cfg.v`, `cfg` a plain formal the caller keeps | correct while the caller keeps it, never freed by the keeper | `capturedPlainFormalCallerKeeps` |
| `(x) -> x + #cfg.v` (member position) | moved into the closure, freed once when the closure goes | `sharpInsideTheBody*` |
| lambda borrows `cfg`, then `keeper.owned = #cfg` | kept alive by the keeper, freed once with it | `keeperOwnsTheCaptureInAField` |
| `(x) -> f(#cfg, x)` (argument position) | **not** moved: freed when the factory returns | `DISABLED_sharpArgumentInsideTheBodyMovesIntoTheClosure` |

- A capture is a borrow unless `#` marks it, even when the captured name is a
  `#` formal. Declaring the factory's parameter `#T` does not keep it alive.
- `#` moves a capture only in member position (`#cfg.v`, `#c.next()`). The
  same `#` as a call argument inside the body does not, so the capture dies
  with the factory. When the lambda passes its capture on, have the keeper own
  it in a field instead, which is explicit and measured.

## Worked example (verified: returns 68)

```cajeta
package dev.cajeta.skills;

public class Tally {
    public int32 v;
    public Tally() { this.v = 0; }
    public void bumpBy(int32 n) { this.v = this.v + n; }
    public int32 value() { return this.v; }
}

public class Cell {
    public int32 v;
    public Cell() { this.v = 0; }
    public int32 next() { this.v = this.v + 1; return this.v; }
}

public class LambdasDemo {
    public static () -> int32 makeCounter() {
        Cell c = heap Cell();
        return () -> #c.next();       // closure takes ownership of c
    }

    public static int32 run() {
        (int32, int32) -> int32 add = (a, b) -> a + b;
        int32 five = add(2, 3);

        int32 k = 10;
        (int32) -> int32 mul = (x) -> x * k;
        k = 20;
        int32 fifty = mul(5);          // 50, not 100 — k copied at capture

        int32[] xs = [1, 2, 3, 4];
        Tally t = stack Tally();
        xs.stream().forEach((x) -> t.bumpBy(x));
        int32 sum = t.value();         // 10 — borrow sees live state

        () -> int32 fn = LambdasDemo.makeCounter();
        int32 one = fn();
        int32 two = fn();              // owned state survives across calls

        return five + fifty + sum + one + two;   // 68
    }
}
```

## Sharp edges

- Array literals are brackets — `[1, 2, 3, 4]`. The brace form
  `{1, 2, 3, 4}` is retired (`CAJETA_ERROR_ARRAY_BRACE_INIT_RETIRED`);
  braces build aggregates (`Point { x: 1 }`) and dispatch tables only.
- A lambda's parameter types come from the declared function type or the
  target parameter slot — a bare `(a, b) -> ...` assigned to `var`-less,
  untyped context cannot compile.
- Streams over arrays (`xs.stream()`) are the idiomatic inline-consumption
  site for borrow captures — see the `cajeta.lang` stream skills.
