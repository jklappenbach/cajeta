---
id: reflect-sealed-access
applies-to: [cajeta/reflect/Field, cajeta/reflect/Method, cajeta/reflect/Constructor, cajeta/reflect/IllegalAccessException]
title: The @Sealed reflective access-control protocol
description: How Field/Method/Constructor gate access to private members of @Sealed classes and raise IllegalAccessException
---

# @Sealed reflective access control

Reflection in `cajeta.reflect` is **default-open**: `Field.getInt32/setInt32/...`,
`Method.invokeScalar/invokeInt32/invokeObject/invokeBoxed/...`, and
`Constructor.heapInstance` reach **any** member regardless of declared visibility — a
*private* field of an ordinary class is reflectively readable. The single exception is
the cross-class invariant this skill documents:

> A **private** member of a class annotated **`@Sealed`** is barred. The accessor runs
> `checkAccess()` **before** any read/write/invoke/construct and throws
> `IllegalAccessException` if the member is sealed-off.

So the gate fires on exactly two conditions together — member is `private` **and** its
declaring class is `@Sealed`. Public/protected members of a `@Sealed` class stay
reachable; private members of a non-`@Sealed` class stay reachable.

## Members and roles

- **`Field`** — `checkAccess()` precedes every `getInt32/getInt64/getBoolean/`
  `getFloat32/getFloat64/setX`/`getBoxed`. Backed by native
  `__cajeta_reflect_field_blocked`.
- **`Method`** — `checkAccess()` precedes every `invokeScalar/invokeInt32/`
  `invokeFloat32/invokeFloat64/invokeObject/invokeBoxed` overload. Backed by
  `__cajeta_reflect_method_blocked`.
- **`Constructor`** — `checkAccess()` precedes both `heapInstance()` and
  `heapInstance(int64[])`. Backed by `__cajeta_reflect_ctor_blocked`.
- **`IllegalAccessException extends cajeta.error.RecoverableException`** — the thrown
  type. Recoverable by design: a tool walking members can **catch and skip** the sealed
  ones rather than aborting the whole walk.

`checkAccess()` is `private` on each of the three — you never call it; it is the
invariant the public accessors enforce for you.

## How it is decided (compiler + runtime)

The decision is data-driven, not per-member accessor code. For a `@Sealed` class the
compiler **omits the private members' cases** from the synthesized reflect adapters and
sets the per-member "blocked" RTTI bit. The accessor's `checkAccess()` reads that bit
(`accessBlocked(rtti, index) != 0`) and throws. This is why reflection can tell
**"sealed off"** apart from **"no such member"** — the latter is a different error
surface; a blocked member still exists, it is just denied.

## Call sequence (the protocol)

1. Obtain the type: `Class.of(obj)` (or `Class<?>`).
2. Obtain the member object: `getField(i)` / `getMethod(i)` / `getConstructor(i)` —
   these never throw for visibility; they just hand back a `#Field`/`#Method`/
   `#Constructor` view bound to `(rtti, index)`.
3. Access through it: `getInt32(o)` / `invokeInt32(o)` / `heapInstance()`. **This** step
   runs `checkAccess()` first and is where `IllegalAccessException` is raised.

So the throw is always at *use* time, never at *lookup* time. Wrap the use site, not the
`getField`/`getMethod`/`getConstructor` call.

## Ownership / lifecycle

- The member views are owned (`#Field`/`#Method`/`#Constructor`); obtaining one does not
  touch the gate.
- `IllegalAccessException`'s constructor takes a `#String` message **by transfer**
  (`IllegalAccessException(#String message)`); the accessors pass a string literal. Its
  `cause` is left unset (`0`). The thrown exception is owned by the unwinder; read
  `e.message` (a borrowed view over the live exception — copy if you must keep it past
  the `catch`) inside the handler.
- On the *non-blocked* paths ownership follows the underlying accessor as usual
  (`heapInstance` returns an owned `#Object`; `invokeObject`/`invokeBoxed` return owned
  `#Object` per the invoked signature — see those methods' own caveats about borrow
  returns; scalar/field reads return plain values).

## What this protocol does NOT do

- It does **not** gate protected/public members, and does **not** gate any member of a
  non-`@Sealed` class — those reflect openly. Do not expect an exception there.
- It is **not** a general Java-style accessibility model: there is no
  `setAccessible(true)` escape hatch, and no per-call-site caller check. The only axis is
  `private` + `@Sealed`.
- `@Sealed` here (reflection access control, REFL-3.3 decision D1) is unrelated to the
  `sealed`/`non-sealed` **class modifier** (permitted-subclass restriction) parsed
  elsewhere — different feature, do not conflate.
- The gate is not bypassable by constant-folding: a `@Sealed final` class's private
  field stays un-folded so the runtime exception still fires.

## Worked example (mirrors `ReflectionTests`)

```cajeta
package test;
import cajeta.reflect.Class;
import cajeta.reflect.Field;
import cajeta.reflect.IllegalAccessException;

@Sealed
public class Vault {
    private int32 secret;
    public int32 open;
    public Vault() { this.secret = 42; this.open = 7; return; }
}

public final class M {
    public static int32 run() {
        Vault v = heap Vault();
        Field locked = Class.of(v).getField(0);   // secret (private) — lookup never throws
        try {
            int32 x = locked.getInt32(v);         // checkAccess() fires here
            return 0;                             // unreachable for a sealed-private field
        } catch (IllegalAccessException e) {
            // e.message: "reflective access to a private field of a @Sealed class is denied"
            // recoverable: skip this member and keep walking
        }
        Field pub = Class.of(v).getField(1);      // open (public) — reachable
        return pub.getInt32(v);                   // -> 7
    }
}
```

The same shape applies to `Method` (`Class.of(s).getMethod(i).invokeInt32(s)`) and
`Constructor` (`Class.of(seed).getConstructor(i).heapInstance()`), each throwing the
correspondingly-worded `IllegalAccessException`. The `Class` index-form convenience
(`Class.of(v).getInt32(v, 0)`) enforces the identical gate.
