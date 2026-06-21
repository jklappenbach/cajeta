---
id: reflect-Method
applies-to: [cajeta/reflect/Method]
title: Reflectively invoking a method — picking the right invoke variant
description: Invoke a reflected method, choosing the invoke overload by return type and packing args as int64[]; routes through the declaring-class adapter so statics work.
---

# `cajeta.reflect.Method` — reflective invocation

A **support / handle** type: you do **not** construct it. You receive a
`#Method` from `Class.getMethod(index)` (see `cajeta/reflect/Class`). It pairs the
declaring class's RTTI pointer with a method index; calling its `invoke*` methods
lowers to a real LLVM call through the class's synthesized invoke adapter.

The decision that matters: **pick the invoke variant by the method's return type.**
There is no single `invoke` — choose the overload that matches what the target
returns, or the value crosses the ABI boundary wrong (FP returns in particular).

## Routing — return type → invoke method

| Target returns | Call | Hands back |
|---|---|---|
| any scalar/pointer, widened | `invokeScalar(o, …)` | `int64` (low bytes = value; `void`→0) |
| `int32` | `invokeInt32(o, …)` | `int32` (narrowed from the int64 path) |
| `float32` | `invokeFloat32(o, …)` | real `float32` (via FP register) |
| `float64` | `invokeFloat64(o, …)` | real `float64` (via FP register) |
| a reference (`heap T`) | `invokeObject(o, …)` | `#Object` (owned — see below) |
| unknown / type-erased | `invokeBoxed(o, …)` | `#Object` wrapper, or `null` for `void` |

`invokeFloat32/64` exist because FP returns come back in an FP register, not the
int64 ret buffer — do **not** route a float through `invokeScalar` and cast.

## Passing arguments

`o` is the receiver (`this`). For a **static** method pass `null` — invocation
resolves the adapter from the *declaring class* RTTI, not from `o`, so a null
receiver still dispatches. Each invoke family has three arg forms:

- **no-arg**: `m.invokeInt32(o)`.
- **`int64[] args`**: one element per user parameter, in declared order, scalars as
  their raw bits and objects/pointers as the reference. The implicit `this` is
  **not** included. You must supply exactly `m.getParameterCount()` elements.
- **fiber-stack arity overloads** (1–3 raw `int64` args, e.g.
  `m.invokeInt32(o, (int64) 5)`): same contract, but no heap `int64[]` — the
  native builds the arg buffer on the calling fiber's stack. Use for small, fixed
  arg counts. (`invokeScalar`/`invokeInt32` only; not the FP/Object/Boxed forms.)

## Ownership & lifecycle

- The `#Method` is owned/drop-tracked like any heap object; no `close()`.
- `invokeObject` / `invokeBoxed` return an **owned `#Object`** (drop-tracked).
  Ownership follows the *target method's* signature: a method returning `heap T`
  transfers ownership to you — the safe, common case. A method returning a
  **borrow** must **not** be invoked this way: reflection can't see the borrow's
  lifetime, so treating the result as owned risks a double free.
- `invokeBoxed` returns **`null`** for a `void` target (after running it for side
  effects); primitives come back boxed in their `cajeta.lang` wrapper
  (`int32`→`Int32`, `float64`→`Float64`, `boolean`→`Boolean`, …).

## Errors

- `IllegalAccessException` — every invoke path runs an access check first.
  Invocation is default-open, but a `@Sealed` class bars invoking its **private**
  methods. Catch this around invokes of members you didn't declare.
- `UnsupportedReflectionException` (from `invokeBoxed` only) — the return type is a
  128-bit int, a half/quad/ML float, or `pointer`; auto-boxing those returns isn't
  wired. Catch it and box manually off the typed invoke surface.

## What it does NOT do

- No name/signature *lookup*: `getMethod` is index-based on `Class`; `Method`
  carries no name-based resolution. Use `m.getName()` / `m.getParameterCount()` to
  identify the one you want while scanning indices.
- No argument type checking or coercion — you pack raw bits; a wrong width or a
  wrong element count is undefined, not an exception.
- `invokeBoxed` does not widen unsupported types — it throws rather than silently
  misreport the boxed type.

## Example — scan for a 1-arg method and invoke it

```cajeta
import cajeta.reflect.Class;
import cajeta.reflect.Method;

User u = heap User();          // id = 10
Class<?> c = Class.of(u);
int32 result = -1;
int32 i = 0;
while (i < c.getMethodCount()) {
    Method m = c.getMethod(i);
    if (m.getParameterCount() == 1) {       // addId(delta) -> id + delta
        result = m.invokeInt32(u, (int64) 5);   // 15; no heap int64[]
    }
    i = i + 1;
}
```

Reference-returning, with the owned result read back reflectively:

```cajeta
Method m = Class.of(f).getMethod(0);   // #Cell make()
Object cell = m.invokeObject(f);       // owned; ownership from `heap Cell`
int32 v = (cell == null) ? -1 : Class.of(cell).getInt32(cell, 0);
```

For obtaining the `Method` and for `getName`/`getParameter`/annotation accessors,
see `cajeta/reflect/Class` and `cajeta/reflect/Parameter`.
