---
id: reflect-Method-invokeBoxed
applies-to: [cajeta/reflect/Method.invokeBoxed]
title: Method.invokeBoxed — type-erased reflective call returning an owned #Object
description: Invoke a method generically and get its result boxed into a cajeta.lang wrapper; handle the unsupported-type fallback.
---

# `Method.invokeBoxed`

Use this when you hold a `Method` whose return type you do **not** know at compile time
(serializers, DI containers, inspectors) and want one uniform result. It invokes the
method and hands back the result as an owned `#Object`, boxing any primitive into its
`cajeta.lang` wrapper. If you *do* know the return type, prefer the typed surface
(`invokeScalar` / `invokeInt32` / `invokeFloat32` / `invokeFloat64` / `invokeObject`) —
those skip the boxing allocation and the unsupported-type cliff below.

## Signatures

```
public #Object invokeBoxed(Object o)              // no-argument call
public #Object invokeBoxed(Object o, int64[] args) // raw-packed args
```

- `o` — the `this` receiver; pass `null` for a `static` method.
- `args` — one `int64` element per user-visible parameter (exclude the implicit `this`),
  in declared order: scalars as their bits, objects/pointers as the reference. Supply
  exactly `getParameterCount()` elements with matching types. `args` is borrowed (read,
  not freed); pass `null` for a no-arg method.

## Return: boxing map and ownership

The result is **owned (`#Object`)** — it is drop-tracked; you free it (or pass `#` it on).

- `void` → `null` (the method still runs for its side effects).
- `boolean`/`int32`/`int64`/`float32`/`float64` → `Boolean`/`Int32`/`Int64`/`Float32`/`Float64`
  (the W1 wrappers; box exactly). Read the value back via `Class.of(o).getInt32(o, 0)`
  (or `getFloat64`/`getBoolean`/…) on the wrapper's field 0.
- `int8`/`int16`/`uint8`/`uint16`/`uint32`/`uint64`/`char` → `Int8`/`Int16`/`UInt8`/`UInt16`/`UInt32`/`UInt64`/`Char` (W2).
- a reference return passes through `invokeObject` — the boxed `#Object` **is** the
  returned instance. Ownership then follows the invoked method's signature: a method
  returning `heap T` transfers ownership to you (the safe, common case). A method that
  returns a **borrow** must not be reached this way — reflection can't see the borrow's
  lifetime, so treating it as owned risks a double free.

## Failure mode — catch and fall back

A primitive that has no boxing path yet raises `UnsupportedReflectionException` (a
`RecoverableException`) instead of silently widening (which would misreport the boxed
type) or returning an ambiguous `null`. This covers `int128`/`uint128`, the half/quad/ML
floats (`float16`/`bfloat16`/`float128`/fp8), and `pointer`. The intended recovery is to
catch it and call the typed invoke surface for that member, boxing manually:

```cajeta
import cajeta.lang.Object;
import cajeta.reflect.Class;
import cajeta.reflect.Method;
import cajeta.reflect.UnsupportedReflectionException;

// p : the receiver; m : a Method whose return type we don't know.
int64[] a = heap int64[1];
a[0] = (int64) 5;                      // one raw arg per parameter, declared order
try {
    Object o #= m.invokeBoxed(p, a);   // owned #Object (null if the method is void)
    if (o != null) {
        // generic consumer: read field 0 via the wrapper's typed accessor
        int32 v = Class.of(o).getInt32(o, 0);
    }
} catch (UnsupportedReflectionException e) {
    // no wrapper for this return type — box manually off the typed surface, e.g.
    // Float128.of((float128) m.invokeScalar(p, a))
}
```

## Gotchas

- The visibility gate runs first: invoking a `private` method of a `@Sealed` class throws
  `IllegalAccessException` (default-open otherwise).
- FP returns route through the FP-register natives, not the int64 scalar path, so the
  value crosses correctly — don't reconstruct floats from `invokeScalar` bits.
- W3/W4 wrapper *classes* (e.g. `Float128`) exist, but reflective auto-boxing of those
  *returns* is not wired (the invoke ret buffer is 8 bytes; no half/bfloat/sub-byte
  return natives yet) — hence the exception, not a value.
