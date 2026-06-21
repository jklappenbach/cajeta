---
id: reflect-Constructor
applies-to: [cajeta/reflect/Constructor]
title: Constructor — reflective heap construction of a class
description: Use Constructor.heapInstance() / heapInstance(int64[]) to allocate a new owned instance via the per-class adapter, after the @Sealed access check.
---

# Constructor

A single declared constructor of a class, reached reflectively. **Support/handle type
— you never `heap Constructor(...)` it yourself; you receive it from
`Class.getConstructor(index)`** (see `cajeta/reflect/Class`). Its job is to build a
fresh instance of that class without knowing the type statically:
`heapInstance()` (no-arg) and `heapInstance(int64[])` (with marshalled args).

It does **not** parse strings, look up constructors by parameter types, or construct by
class name — to pick a constructor you iterate `Class.getConstructorCount()` and match on
`getParameterCount()`. For name-based bounded construction use `Class.heapInstance<T>(name)`
/ `Class.forName<T>` instead (that path does not expose a `Constructor`).

## Obtaining one

You do not construct it. Get it from a `Class`:

```cajeta
import cajeta.lang.Object;
import cajeta.reflect.Class;
import cajeta.reflect.Constructor;

User seed = heap User();
Class<?> c = Class.of(seed);
Constructor ctor = c.getConstructor(0);   // declared ctor at index 0 — owned (#Constructor)
```

`getConstructor` returns an **owned** `#Constructor` (drops on scope). It is a thin
handle over the class RTTI plus a constructor index; cheap to make, holds no native
resource, nothing to close.

## Building an instance

Both forms return a new **owned** `#Object` (the caller owns it; it drops on scope unless
moved). Construction is **always on the heap** — the concrete type, hence its size, is
unknown at the call site, so no stack alloca is possible.

No-arg constructor:

```cajeta
Constructor ctor = c.getConstructor(0);
Object o = ctor.heapInstance();           // allocates, installs vtable, runs the ctor
```

With arguments — pack **one `int64` per user-visible parameter, in declared order**, into
an owned `int64[]`. The implicit `this` is excluded from the count and the array:

```cajeta
// match the 1-arg constructor User(int32 startId), then build with startId = 99
int32 i = 0;
while (i < c.getConstructorCount()) {
    Constructor ctor = c.getConstructor(i);
    if (ctor.getParameterCount() == 1) {
        int64[] args = heap int64[1];     // owned; one slot per user parameter
        args[0] = (int64) 99;             // widen each arg to int64, declared order
        Object o = ctor.heapInstance(args);
        // ... use o ...
    }
    i = i + 1;
}
```

The resulting `Object` is fully formed: its vtable is installed and the constructor body
ran, so it is immediately usable for further reflection (e.g. `Class.getInt32(o, field)`,
`Method.invoke`).

## Access control — @Sealed gate

Reflective construction is **default-open**, with one exception enforced *inside* both
`heapInstance` forms before any allocation: constructing through a **private constructor of
a `@Sealed` class** throws `cajeta.reflect.IllegalAccessException`. A private constructor
of a non-sealed class, or any public/protected constructor, is allowed.

```cajeta
import cajeta.reflect.IllegalAccessException;

Constructor ctor = c.getConstructor(noArg);   // a private ctor of a @Sealed class
try {
    Object o = ctor.heapInstance();
} catch (IllegalAccessException e) {
    // denied: sealed + private
}
```

## Other accessors (introspection, not construction)

`getIndex()`, `getParameterCount()` (excludes `this`), `getModifiers()` →
`#Modifiers`, `getParameter(i)` → `#Parameter`, and the annotation surface
(`getAnnotationCount()`, `getAnnotation(i)` → `#Annotation`,
`getAnnotationName(i)` → `#String`, `hasAnnotation(name)`). Each `get*` that returns a
class type returns an **owned** value. There is no parameter-*type* matching here — use
`getParameter(i)` and inspect it via `cajeta/reflect/Parameter`.
