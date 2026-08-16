---
id: reflect-member-model
applies-to: [cajeta/reflect/Class, cajeta/reflect/Field, cajeta/reflect/Method, cajeta/reflect/Constructor, cajeta/reflect/Parameter, cajeta/reflect/Modifiers]
title: Walking a class's members by integer index — Field, Method, Constructor, Parameter, Modifiers
description: The count-then-index navigation pattern off Class, the borrow-vs-owned object graph, and that getField/getMethod/getConstructor hand back freshly heap-allocated owned objects.
---

# Walking a class's members

There is **no name lookup and no "all members" accessor** here. To reach a field, method, constructor, parameter, or modifier set you always do the same thing: ask `Class` for a **count**, then index `0..count-1`. Pick the access shape:

- **Want the member as an object you can carry/pass** → `c.getField(i)` / `c.getMethod(i)` / `c.getConstructor(i)` → an owned `#Field`/`#Method`/`#Constructor`.
- **Want one scalar fact (a name, a param count) cheaply, no object** → the index-form accessors directly on `Class`: `c.getFieldName(i)`, `c.getMethodName(i)`, `c.getMethodParamCount(i)`, `c.getFieldOffset(i)`. Same index space as the object form.
- **Want a parameter** → only via a member object: `method.getParameter(i)` / `ctor.getParameter(i)` (Class has no parameter accessor).
- **Want the access/declaration flags** → `getModifiers()` on any of Class/Field/Method/Constructor → an owned `#Modifiers`.

There is **no `getField(String)`**, no `#Field[] getFields()`, and the navigation accessors do **not** bounds-check — an out-of-range index yields an empty name / a degenerate object, not an exception. Loop with the count.

## Members and roles

- **`Class<T>`** — the root and the only **entry point** here. Hands out everything else. Obtain via `Class.of(obj)` or `Class.forName(name)`. See `cajeta/reflect/Class`.
- **`Field` / `Method` / `Constructor`** — one declared member each. Lightweight handles: each holds just `{ pointer rtti, int32 index }` (a borrow of the owning class's RTTI plus its declared-member index). They carry **no back-reference to the `Class`**. The actual reflective work — `Field` get/set, `Method.invoke*`, `Constructor.heapInstance` — lives on these and is documented in their own class skills, not here.
- **`Parameter`** — one user-visible parameter of a `Method` or `Constructor`. Holds `{ rtti, ownerIsCtor, ownerIndex, index }`. The implicit `this` is excluded, so `index` is the user-visible position.
- **`Modifiers`** — a value object over the packed modifier `int32` (`isPublic()`, `isStatic()`, `isFinal()`, plus class-only `isSealed()`/`isRetained()`). See `cajeta/reflect/Modifiers`.

## Object graph & ownership — the load-bearing part

```
Class  (BORROW, process-lifetime, never freed)
  ├─ getField(i)       → #Field        (OWNED, fresh heap, you drop)
  ├─ getMethod(i)      → #Method       (OWNED, fresh heap, you drop)
  │     └─ getParameter(i) → #Parameter (OWNED, fresh heap, you drop)
  ├─ getConstructor(i) → #Constructor  (OWNED, fresh heap, you drop)
  │     └─ getParameter(i) → #Parameter (OWNED, fresh heap, you drop)
  └─ getModifiers()    → #Modifiers    (OWNED, fresh heap, you drop)
```

- The `Class` is a **borrow** of a compiler-cached per-type instance — you never free it (`Class c = Class.of(o);` registers no drop). It is **not** the owner of the member objects.
- Every `getField`/`getMethod`/`getConstructor`/`getParameter`/`getModifiers` call **allocates a brand-new heap object** and **transfers ownership** to you (the `#` return). Two `getMethod(0)` calls give two distinct objects, not a cached singleton. Your local owns it and drops it at scope exit — there is nothing to `close()`.
- Because a member object is a self-contained `{rtti, index}` snapshot, it **outlives the `Class` local** and can be returned or stored freely; the RTTI it borrows is process-lifetime, so the handle never dangles.
- String results from these objects (`getName()`, `getTypeName()`) are themselves owned `#String`s — see the per-class skills.

## The cross-class call sequence

`Class.of(obj)` → `getMethodCount()` → loop `getMethod(i)` → `m.getParameterCount()` → loop `m.getParameter(j)`. Identical shape for fields (`getFieldCount`/`getField`) and constructors (`getConstructorCount`/`getConstructor`). The member object's index always equals the `i` you passed.

Indices are **declaration order** and stable per process. Reflective access through these objects (e.g. `Field.getInt32`, `Method.invokeScalar`, `Constructor.heapInstance`) throws `IllegalAccessException` only for a private member of a `@Sealed` class — navigation/construction of the *handle* itself never throws.

## Worked example — navigate to a parameter

Mirrors `ReflectionTests.parameterIntrospection`: find the 1-arg method and read its first parameter.

```cajeta
import cajeta.reflect.Class;
import cajeta.reflect.Method;
import cajeta.reflect.Parameter;

Class<?> c = Class.of(u);                  // borrow — not freed
int32 count = c.getMethodCount();
int32 i = 0;
while (i < count) {
    Method m #= c.getMethod(i);             // owned #Method, dropped each iteration
    if (m.getParameterCount() == 1) {
        Parameter p #= m.getParameter(0);   // owned #Parameter, dropped at scope end
        if (p.getName() == "delta" && p.getTypeName() == "int32") {
            return 1;
        }
    }
    i = i + 1;
}
return 0;
```

`getConstructor` follows the same pattern — `Constructor ctor = c.getConstructor(i); int32 n = ctor.getParameterCount();` then `ctor.getParameter(j)`.

## When to use this navigation vs. the rest of the package

- Use this count-then-index walk whenever you enumerate members of an **unknown** class (serializers, DI scanners, inspectors).
- If you only need a single fact and want to avoid the per-member heap allocation, prefer the index-form `Class` accessors (`getFieldName(i)`, `getMethodParamCount(i)`) over building the object.
- To find classes (not members) — `Class.forName`, `allClasses`, `classesAnnotated`, `subtypes<T>` — and for the reflective *operations* on a found member, see the `cajeta/reflect/Class`, `cajeta/reflect/Field`, `cajeta/reflect/Method`, and `cajeta/reflect/Constructor` class skills.
