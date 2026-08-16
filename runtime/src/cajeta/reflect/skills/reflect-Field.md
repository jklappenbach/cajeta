---
id: reflect-Field
applies-to: [cajeta/reflect/Field]
title: Reflective field access via Field (typed get/set + getBoxed)
description: Read/write a class field reflectively with type-matched getInt32/setInt32/... or type-erased getBoxed; every accessor runs the @Sealed gate.
---

# Field — reflective access to one declared field

A `Field` is the object handle for one declared field of a class. Use it to
read/write that field's value on an instance reflectively: typed accessors
(`getInt32`/`setInt32`, `getInt64`, `getBoolean`, `getFloat32`, `getFloat64`)
when you know the field's type, or `getBoxed` to read a field of unknown type as
an owned `#Object`. It is a **support/handle type**, not a value: you read its
metadata (`getName`, `getModifiers`, annotations) and use it as the access point
to instance state.

## You do NOT construct this

Receive a `Field` from `Class.getField(index)` (borrow a `Class` first via
`obj.getClass()` or `T.class`). `getField` returns an **owned `#Field`** — you
own it; it is independent of the `Class` it came from. The public
`Field(pointer rtti, int32 index)` constructor exists but is a runtime-internal
detail; do not build one yourself.

For one-off access prefer the `Class` index-form shortcuts
(`Class.getInt32(o, index)`, `Class.getBoxed(o, index)`) — see
`cajeta/reflect/Class`. Reach for a `Field` object when you want to hold the
handle, read its name/modifiers/annotations, or access the same field repeatedly.

## The accessor must match the field's type

Access is **data-driven**: each accessor loads/stores at the field's byte offset
with no runtime type check beyond width. Calling the wrong-width accessor
silently reinterprets memory (e.g. `getInt32` over-reads a 1- or 2-byte field).
Pick the accessor by the field's real type — query it with `getTypeFlags()` /
`getModifiers()` if unsure, or use `getBoxed` which dispatches on the field kind.

Methods that matter (all take the target `Object o`; all run the access gate first):

- `int32 getInt32(Object o)` / `void setInt32(Object o, int32 v)`
- `int64 getInt64(Object o)` / `void setInt64(Object o, int64 v)`
- `boolean getBoolean(Object o)` / `void setBoolean(Object o, boolean v)`
- `float32 getFloat32(Object o)` / `void setFloat32(...)`, `float64 getFloat64` / `setFloat64`
- `#Object getBoxed(Object o)` — owned boxed value; see below
- `#String getName()` (owned), `int32 getModifierFlags()`, `#Modifiers getModifiers()` (owned),
  `int64 getTypeFlags()`, `int32 getIndex()`
- annotations: `int32 getAnnotationCount()`, `#String getAnnotationName(int32 i)` (owned),
  `#Annotation getAnnotation(int32 i)` (owned), `boolean hasAnnotation(String name)`

`o` is **borrowed** by every accessor (not freed, not stored). Typed getters
return primitives by value; setters mutate `o` in place.

## getBoxed — type-erased read, and what it refuses

`getBoxed(Object o)` returns the field value as an **owned `#Object`**, boxing a
primitive into its `cajeta.lang` wrapper (`Int32`, `Boolean`, `Float64`,
`Int8`/`UInt16`/`Char`, ...). Use it from generic consumers (serializers,
inspectors) walking fields of unknown type. It does **NOT** handle:

- **reference fields** — handing the held reference back as an owned `#Object`
  would double-drop (the object still owns it); reflection has no borrow-return
  surface yet.
- **un-wrapped primitives** — 128-bit ints, half/quad/ML floats, raw pointer.

Both raise `cajeta.reflect.UnsupportedReflectionException`. For those, read with
a typed accessor and box manually.

## The @Sealed access gate (every accessor)

Reflective access is default-open, but **every** get/set (typed and `getBoxed`)
first runs `checkAccess()`: if the field is `private` AND its declaring class is
`@Sealed`, it throws `cajeta.reflect.IllegalAccessException`. Public fields of a
`@Sealed` class, and private fields of an un-sealed class, stay reachable.
Metadata accessors (`getName`, modifiers, annotations) are not gated.

## Example

```cajeta
import cajeta.reflect.Class;
import cajeta.reflect.Field;
import cajeta.reflect.IllegalAccessException;
import cajeta.lang.Object;

// Roundtrip a typed field (User.id is field 0, an int32).
User u = heap User();
Field f #= Class.of(u).getField(0);   // owned #Field
f.setInt32(u, 77);
int32 id = f.getInt32(u);            // 77

// Type-erased read of an unknown-type primitive field.
Object boxed #= f.getBoxed(u);        // owned #Object (an Int32 here)

// A private field of a @Sealed class is barred.
try {
    int32 secret = Class.of(vault).getField(0).getInt32(vault);
} catch (IllegalAccessException e) {
    // sealed-private access denied
}
```

## Lifecycle & state

No `close`/dispose; the returned `#Field` (and owned `#String`/`#Object`/
`#Modifiers`/`#Annotation` results) drop on scope exit like any owned value.
A `Field` is immutable and holds no instance state — it is the target `Object`
that mutates on set. Safe to reuse across many objects of the same class.

See `cajeta/reflect/Class` for obtaining instances and the index-form shortcuts,
and `cajeta/reflect/Method` for `invokeBoxed` (the method-side counterpart to
`getBoxed`).
