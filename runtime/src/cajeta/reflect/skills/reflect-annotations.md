---
id: reflect-annotations
applies-to: [cajeta/reflect/Annotation, cajeta/reflect/Class, cajeta/reflect/Field, cajeta/reflect/Method, cajeta/reflect/Constructor, cajeta/reflect/Parameter]
title: Reading annotations and their arguments off reflected elements
description: How to enumerate annotations on a class/field/method/constructor/parameter and read their argument values by index or key.
---

# Annotation reflection

To read annotations you do **not** instantiate anything — you start from a reflected
*owner* (`Class`, `Field`, `Method`, `Constructor`, or `Parameter`) and ask it for its
annotations. Every owner exposes the **same four-method surface** (REFL-6a):

- `int32 getAnnotationCount()` — how many annotations are declared.
- `#String getAnnotationName(int32 i)` — canonical name of the *i*-th (empty if out of range).
- `#Annotation getAnnotation(int32 i)` — the *i*-th as an `Annotation` object.
- `boolean hasAnnotation(String name)` — exact canonical-name membership test.

`Annotation` then carries the argument values (REFL-6b). It is the only type in this
component you read values from; the owners only locate annotations.

## Canonical names — match these exactly

`hasAnnotation`/`getAnnotationName` use the **canonical** form, not the source spelling:

- A bare `@Component` → `"code.Component"` (single-identifier names default to package `code`).
- A qualified `@pkg.Audited` → `"pkg.Audited"`.

Matching is exact string equality — there is **no** short-name or suffix matching. Pass the
canonical form or you get `false`/no match.

## Object graph & ownership

`Class.of(obj)` returns a **borrowed** `Class` (a process-lifetime cached instance; do not
free it). From it, `getField/getMethod/getConstructor`/`getParameter` give you owner objects,
and `getAnnotation(i)` gives you an **owned** `#Annotation` (drops on scope). An `Annotation`
holds only a *borrow* of the class RTTI plus a locator and reads lazily — it never copies the
metadata, and the RTTI outlives any program value, so the handle stays valid for its scope.

Every value-returning accessor that yields a string (`getName`, `getArgName`, `getArgString`,
`getString`, `getClassRef`, `getArgListString`) returns an **owned** `#String` — you own and
free/drop each one. There is no shared/borrowed-view string here.

## Reading argument values — two routes

By **key** (named args; the lone unnamed arg of `@Order(2)` answers to key `"value"`):

- `int64 getInt(String key)`, `#String getString(String key)`, `boolean getBool(String key)`,
  `#String getClassRef(String key)` (returns the referenced type name of a `Foo.class` arg).
- `int32 getArgIndex(String key)` — resolves a key to an index, or `-1`. Use it to reach a
  **list-valued** argument by key.

By **index** (inspection / iteration):

- `int32 getArgCount()`, `int32 getArgKind(int32 i)`, `#String getArgName(int32 i)`
  (empty for an unnamed arg), `int64 getArgInt(int32 i)`, `boolean getArgBool(int32 i)`,
  `#String getArgString(int32 i)`.

**Kind tags** (from `getArgKind`, `-1` if out of range): `0` int64, `1` string, `2` bool,
`3` classRef, `4` int64-list, `5` string-list, `6` bool-list.

**List-valued args** (`@Tags({"a","b"})`, `@Sizes({1,2})`): locate the arg index (usually via
`getArgIndex("value")`), then `getArgListCount(i)` and `getArgListInt(i,e)` /
`getArgListBool(i,e)` / `getArgListString(i,e)`.

## What this does NOT do — no exceptions, only fallbacks

Wrong-kind and absent reads **never throw**. They return typed fallbacks: `getInt`→`0`,
`getBool`→`false`, `getString`/`getClassRef`→empty `String`, `getArgKind`→`-1` out of range.
So you cannot distinguish "absent" from "present but zero/empty/false" via the typed getters
— use `getArgIndex`/`getArgCount`/`getArgKind` when that distinction matters.

Argument values are captured on **every** owner, parameters included.

## Worked example

```cajeta
package test;

import cajeta.lang.String;
import cajeta.reflect.Class;
import cajeta.reflect.Annotation;

@Component(name = "disk")
public class Widget {
    public Widget() { return; }
}

public final class M {
    public static int32 run() {
        Class<?> c = Class.of(heap Widget());   // borrowed Class
        if (!c.hasAnnotation("code.Component")) { return 1; }

        Annotation a #= c.getAnnotation(0);      // owned #Annotation
        if (a.getArgCount() != 1) { return 2; }
        if (!a.getString("name").equals("disk")) { return 3; }

        // by-index inspection
        if (a.getArgKind(0) != 1) { return 4; } // 1 = string
        if (!a.getArgName(0).equals("name")) { return 5; }
        return 0;
    }
}
```

List-valued, reached by key:

```cajeta
// @Tags({"a", "b", "c"}) on Widget
Annotation a = Class.of(heap Widget()).getAnnotation(0);
int32 idx = a.getArgIndex("value");     // unnamed list arg -> "value"
if (idx < 0) { return 10; }
if (a.getArgKind(idx) != 5) { return 11; }   // 5 = string-list
int32 n = a.getArgListCount(idx);
String first #= a.getArgListString(idx, 0);   // "a"
```

For everything else about the owners (how to obtain `Field`/`Method`/`Constructor`/
`Parameter`, field/method invocation, the class registry queries), see the
`cajeta/reflect/Class` skill.
