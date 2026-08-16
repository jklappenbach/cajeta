---
id: reflect-Method-invokeObject
applies-to: [cajeta/reflect/Method.invokeObject]
title: Method.invokeObject — reflective call returning an owned #Object
description: Reflectively invoke a reference-returning method; result is an owned #Object — only safe for methods returning heap T, never a borrow.
---

# `Method.invokeObject` — reflective call that returns an owned reference

Use this to reflectively invoke a method **whose declared return type is a reference**
(an object) and get the result back as a live `#Object`. The per-class invoke adapter
stores the returned pointer into its ret buffer, so you get the real reference — not the
int64-widened bit pattern the `invokeScalar` family returns.

```cajeta
import cajeta.lang.Object;
import cajeta.reflect.Class;
import cajeta.reflect.Method;

public class Cell { public int32 v; public Cell(int32 x) { this.v = x; return; } }
public class Factory { public Factory() { return; } public #Cell make() { return heap Cell(42); } }

Factory f = heap Factory();
Method m = Class.of(f).getMethod(0);   // make()
Object o #= m.invokeObject(f);          // o is OWNED — drop-tracked by this scope
int32 v = Class.of(o).getInt32(o, 0);  // 42, read reflectively (no downcast needed)
```

## Signatures
- `public #Object invokeObject(Object o)` — no-argument form (passes a null arg buffer).
- `public #Object invokeObject(Object o, int64[] args)` — `args` holds one raw `int64`
  per user parameter, in declared order (scalars as their bits, objects/pointers whole).
  Supply exactly `getParameterCount()` elements with matching types.

`o` is the `this` receiver. For a STATIC method pass `null` — dispatch resolves through
the method's declaring-class rtti, not the receiver.

## Return ownership — the sharp edge
The returned `#Object` is **owned** and drop-tracked by the caller's scope, exactly as
if you had written `heap T` yourself. This is correct ONLY when the invoked method
returns `heap T` (transfers ownership — the common, safe case).

Do **NOT** call `invokeObject` on a method that returns a **borrow**. Reflection cannot
see the borrow's lifetime; treating it as owned hands you a reference the runtime will
drop — a **double free** when the real owner drops too. There is no reflective check for
this — you must know the target method's signature is `heap T` before calling.

Return is `null` for a method that (legally) returns a null reference.

## Failures
- `IllegalAccessException` — `checkAccess()` runs first; thrown when the method is
  `private` and its declaring class is `@Sealed` (REFL-3.3 D1).

## What it does NOT do
- Does not box primitives, and does not handle `void` — it is reference-return only.
  For a type-erased call over an unknown return type (primitive/void/reference in one
  call), use `invokeObject`'s sibling on this class, `invokeBoxed`
  (`applies-to: cajeta/reflect/Method.invokeBoxed`), which boxes primitives into their
  `cajeta.lang` wrappers and routes reference returns through `invokeObject`.
- Does not narrow or validate `args` against parameter types — a mismatch is undefined;
  pass the right count and bit-layout yourself.

## Obtaining a Method
You do not construct `Method` directly in practice — get one from
`Class.getMethod(i)` / `Class.getMethods()` (`applies-to: cajeta/reflect/Class`).
