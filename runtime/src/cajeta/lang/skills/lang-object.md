---
id: lang-object
applies-to: [cajeta/lang/Object]
title: Object — the universal class root (identity hash, toString, clone, ==, drop)
description: Default pointer-identity hash/== and stub toString/clone every class inherits; when and how to override for value-keyed (HashMap) use.
---

# Object

Universal root of `cajeta.lang`. **Every class implicitly extends `Object`** (the
compiler's auto-extend pass), so the methods below are inherited by every type unless
overridden. You never write `extends Object` and you never instantiate `Object`
directly — it is the base, not an access point.

## The one decision: identity vs. value semantics

`Object`'s defaults are **identity-based, like Java's `Object`, not Rust's
`derive(Hash)`**. If you put a class instance in a `HashMap`/`HashSet` key and expect
two field-equal instances to collapse to one entry, you must change that:

| You want…                                  | Do this                                              |
|--------------------------------------------|------------------------------------------------------|
| key by object identity (same heap pointer) | nothing — inherit the default `hash()`/`==`          |
| key by field values (`Point(1,2)` equal)   | annotate the class `@AutoHash`, **or** override `hash()` |
| value equality but custom field selection  | override `hash()` manually                           |

Overriding `hash()` is sufficient: `operator==` is `this.hash() == other.hash()` and is
**virtual through `hash()`**, so a value-based `hash()` automatically yields value
equality (this is exactly how `String` works — it overrides only `hash()`).

## Inherited methods

```cajeta
@Native("__cajeta_object_hash")      public int64  hash();      // identity hash
@Native("__cajeta_object_to_string") public String toString();  // returns null (stub)
@Native("__cajeta_object_clone")     public Object clone();     // returns null (stub)
public static boolean operator== (Object a, Object b);          // a.hash()==b.hash()
~Object() { }                                                    // empty virtual drop
```

- **`hash() -> int64`** — pointer address mixed through SplitMix64 with a per-process
  seed. Distinct instances ⇒ distinct hashes. Well-distributed for bucket keys, O(1).
  The per-process seed is OS-entropy seeded at startup: hashes are stable within a run,
  **not** across restarts (defeats hash-flooding). To override, return a structural value;
  thread multiple fields with `Hash.combine(a.hash(), b.hash())` (order-sensitive) from
  `cajeta.hash.Hash`.
- **`toString() -> String`** — **currently returns `null`** (placeholder until the
  synthesizer + `String` surface land). Do not rely on it; override it if you need debug
  output now. Borrowed/owned semantics of the returned `String` are not yet defined.
- **`clone() -> Object`** — **currently returns `null`** (placeholder). The planned
  default is a *shallow* copy: value-typed fields `memcpy`'d, class-typed fields shared by
  reference (Java-style). Override for deep copies. Subclass overrides narrow the return
  type to the declaring class, so callers need no cast.
- **`operator==`** — `static`, **null-safe**: two `null`s are equal, one `null` is not,
  and the `null` check lowers to a pointer compare (non-recursive, no segfault on null
  vtable). `!=` is auto-derived as its negation — do not declare `operator!=`.

## Override contract & sharp edges

- **Equal values must hash equal.** If you override `hash()`, keep `==` consistent
  (today that means: only override `hash()`; `operator==` dispatches through it).
- **`@AutoHash`** synthesizes a structural `hash()` over all fields (supports integral,
  float, and class-typed fields via virtual dispatch). Unhashable fields produce a
  compile-time diagnostic naming the class and offending field. Without it a class keeps
  the inherited identity hash.
- **Collision caveat:** value-based `hash()` collides at ~2^-64. For cryptographic /
  exact-text equality, write an explicit byte comparison — do not trust `==`.
- **`operator==(Object)` is the only `==` available** — there is no per-field structural
  `==` generated; equality is always routed through `hash()`.

## Lifecycle

`~Object()` is the empty root of every class's **virtual** destructor chain. Dropping any
instance through any base reference dispatches to the most-derived `~T()`, runs field
auto-drops, walks ancestors in reverse declaration order, then `__cajeta_free`s the
instance once. `Object` itself owns no resources. Override `~T()` in a subclass for
deterministic resource release at scope exit; set a source breakpoint on `~T()` (under
`--debug-info`) to observe when an instance is destructed.

## Example — value-keyed class

```cajeta
package app;

import cajeta.hash.Hash;

// Field-value identity for use as a HashMap/HashSet key.
@AutoHash                       // or drop this and write hash() by hand (below)
public class Point {
    public int32 x;
    public int32 y;
    public Point() { return; }
}

public final class Demo {
    public static int32 run() {
        Point a = heap Point();  a.x = 42;  a.y = 99;
        Point b = heap Point();  b.x = 42;  b.y = 99;
        return a == b ? 1 : 0;   // 1: @AutoHash makes equal fields hash (and ==) equal
    }
}
```

Hand-written equivalent, when you want control over which fields participate:

```cajeta
public class Point {
    public int32 x;
    public int32 y;
    public int64 hash() {
        return Hash.combine((int64) x, (int64) y);   // == follows automatically
    }
}
```

## See also

- `cajeta/hash/Hash` — `Hash.identity(obj)`, `Hash.combine(a, b)`, `Hash.processSeed()`
  for hand-written overrides and identity-keyed maps.
- `cajeta/hash/DefaultHasher` — the streaming hasher the structural synthesizer feeds.
- `cajeta/lang/String` — the canonical content-hashing override (`hash()` only).
