---
id: reflect-Constructor-heapInstance
applies-to: [cajeta/reflect/Constructor.heapInstance]
title: Constructor.heapInstance — reflective new (no-arg and int64[]-packed-args)
description: Build a fresh owned #Object via a reflected constructor — no-arg heapInstance() vs heapInstance(int64[]) arg marshalling, the @Sealed gate, and caller ownership of the result.
---

# Constructor.heapInstance

The reflective `new`. Two overloads on a `Constructor` you already hold (get it from
`Class.getConstructor(i)` — see `cajeta/reflect/Constructor`):

```cajeta
public #Object heapInstance();              // no-arg constructor
public #Object heapInstance(int64[] args);  // one int64 slot per user parameter
```

Both **allocate on the heap, install the vtable, run the constructor body**, and return
the newly built object. Always heap — the concrete type (hence its size/escape) is
unknown at the call site, so no stack alloca can be emitted; there is no
`stackInstance` form today.

## Return ownership

Returns an **owned `#Object`** — the caller owns it and it drops on scope unless moved.
The object is fully formed and immediately usable for further reflection (`Class.getInt32(o, …)`,
`Method.invoke…`). Never returns null on success; failure throws (below).

## Picking the overload / marshalling args

- **No params** → `heapInstance()`.
- **N user params** → `heapInstance(args)` where `args` is an **owned `int64[]` of length
  exactly `getParameterCount()`**, **one slot per user-visible parameter in declared
  order**. The implicit `this` is excluded from both the count and the array. Widen each
  argument to `int64` at the call site (`args[k] = (int64) value;`). You build and own the
  array; pass it in.

There is **no** parameter-*type* matching and **no** by-name/by-signature lookup here: to
target a specific constructor you iterate `Class.getConstructorCount()` and match on
`getParameterCount()` (and inspect types via `getParameter(i)` if needed). For name-based
bounded construction use `Class.heapInstance<T>(name)` instead — that path returns an
`Optional<T>`, not a `#Object`, and exposes no `Constructor`.

## Preconditions & call sequence

1. `Class.of(obj)` (or a `Class<?>`).
2. `getConstructor(i)` — lookup, never throws on visibility.
3. `heapInstance()` / `heapInstance(args)` — **this** step runs `checkAccess()` first,
   then constructs.

## Failure modes

`checkAccess()` runs **before any allocation** in both overloads and throws
`cajeta.reflect.IllegalAccessException` (a `RecoverableException`) when the constructor is
**private AND its class is `@Sealed`** — the only access bar; reflective construction is
otherwise default-open (public/protected ctors, or private ctors of non-sealed classes,
all build freely). Full protocol: `cajeta/reflect/IllegalAccessException` and the
cross-cutting `reflect-sealed-access` skill. Wrap the **use** site, not `getConstructor`.

Marshalling caveats: the `int64[]`-arg form is for 64-bit-fittable scalar parameters
widened to `int64`; it does not coerce or box reference/wide-primitive parameter types for
you.

## Side effects

Heap allocation + vtable install + the constructor body runs (so any field init or side
effect the ctor performs happens). No filesystem/process effects intrinsic to the call.

## Examples (mirror `test/parser/ReflectionTests.cpp`)

No-arg:

```cajeta
import cajeta.lang.Object;
import cajeta.reflect.Class;
import cajeta.reflect.Constructor;

Constructor ctor = Class.of(seed).getConstructor(0);
Object o #= ctor.heapInstance();             // owned #Object; drops on scope
```

With args — find the 1-arg ctor `User(int32 startId)`, build with `startId = 99`:

```cajeta
User seed = heap User();
Class<?> c = Class.of(seed);
int32 i = 0;
while (i < c.getConstructorCount()) {
    Constructor ctor #= c.getConstructor(i);
    if (ctor.getParameterCount() == 1) {
        int64[] args = heap int64[1];       // owned; length == getParameterCount()
        args[0] = (int64) 99;               // widen each arg, declared order
        Object o #= ctor.heapInstance(args); // owned new instance; id field == 99
    }
    i = i + 1;
}
```
