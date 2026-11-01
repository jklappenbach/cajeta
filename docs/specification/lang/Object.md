# `cajeta.lang.Object` — universal root

Every class implicitly extends `Object` via the auto-extend pass in
`CajetaLlvmVisitor::visitClassDeclaration`. There is no syntax to
opt out, and there is no parallel root hierarchy. The members
declared on `Object` are inherited by every user class, and they are
overridable per the rules below.

```cajeta
public class Object {
    @Native("__cajeta_object_hash")      public int64  hash();   // default: identity
    @Native("__cajeta_object_to_string") public String toString(); // default: null
    @Native("__cajeta_object_clone")     public Object clone();    // default: null

    // Equality is a STATIC operator on Object, defaulting to hash() equality.
    public static boolean operator== (Object a, Object b);   // default: a.hash() == b.hash()

    ~Object();                            // virtual destructor; empty default
}
```

Verified against [`runtime/src/cajeta/lang/Object.cajeta`](../../../runtime/src/cajeta/lang/Object.cajeta).

Status: implemented for implicit extension and the identity
`hash()` / `operator==` defaults. The three `@Native` methods are
declared with native stubs, and `__cajeta_object_to_string` and
`__cajeta_object_clone` currently return `null` (see
`runtime/native/cajeta_runtime.c`). Structural synthesis ships via
the `@AutoHash` and `@ToString` annotations, in v1 for primitive fields
only, as described below. `clone` synthesis is tracked in
specs/Features.md.

> **Note on `operator==`.** It is declared as a static two-arg
> operator, `operator==(Object a, Object b)`, and its default body is
> `a.hash() == b.hash()`. The default is null-safe, so two nulls are
> equal and one null is not. Because `hash()` is virtual,
> a class shifts its equality semantics by overriding `hash()`
> alone. `String` does exactly this, and it does not override
> `operator==`. `!=` is auto-derived as the negation.

---

## Defaults are identity, not structural

Two distinct instances with the same field values compare unequal
and hash differently by default, which is the same shape as Java's
`Object.equals` / `Object.hashCode`. Structural value-equality is
opt-in via `@AutoHash` (see [Hashing.md](../hash/Hashing.md)) or by
overriding `operator==` and `hash()` manually.

```cajeta
Foo a = heap Foo();
Foo b = heap Foo();
boolean same = (a == b);   // false — identity, not structural
```

This is deliberate. Java's experience with structural defaults
hidden behind `==`, and the resulting `==` vs `.equals()` foot-gun,
informed the choice to keep `==` as the language-level operator that
always means "this comparison goes through `operator==`." What
varies is what `operator==` itself does in each class. The default
on `Object` is identity, and user overrides flip it to value-equal.

`String` (see [String.md](./String.md)) is the most notable
override. `String.operator==` is value equality, so `"abc" == "abc"`
returns `true` regardless of allocation identity. That choice is
locked because Java's identity-`==`-on-String design caused an
enormous fraction of real-world Java bugs.

---

## The equal-implies-same-hash contract

If two values compare equal under `operator==`, they MUST also hash
equally under `hash()`. HashMap / HashSet rely on this invariant, and
violating it means an inserted key becomes un-findable.

> **In Cajeta's default model the contract is self-enforcing.**
> Because the inherited static `operator==` is `hash()` equality,
> overriding `hash()` alone or applying `@AutoHash` keeps `==` and
> `hash()` in lockstep automatically. This is why `String` overrides
> only `hash()`. The foot-gun below only arises if a class declares
> its own `operator==` that diverges from its `hash()`.

The lint pass surfaces unpaired overrides. A class that
declares one of `operator==` / `hash()` but inherits the other from
`Object` triggers the `equals-hash-pair` lint warning (see
[LintRules.md](LintRules.md) § `equals-hash-pair`). The build
proceeds, and the warning makes the likely bug visible at compile time
without blocking iteration. Suppress it via
`@SuppressLint("equals-hash-pair")` for the rare case where the
class is not a map / set key.

A class that defines neither inherits the identity defaults from
`Object`, and both halves are consistent because identity-equal implies
identity-hash.

A class that defines `operator==` for value equality but inherits
identity `hash()` from `Object` is the common Java foot-gun. Equal
values produce different bucket indices, and stored entries become
un-findable. The lint warning surfaces this at compile time, before
the misbehaving HashMap operation later. Earlier drafts
specified this as a compile error, and it was demoted to a lint on
2026-05-18. The runtime safety net and the visible diagnostic are enough,
and users should not be blocked from compiling while iterating.

---

## `hash()` — pluggable algorithm

The default `Object.hash()` returns a pointer-identity hash mixed
with the per-process seed (see
[`cajeta.hash.Hash.identity`](../../../runtime/src/cajeta/hash/Hash.cajeta)).
Subclasses override `hash()` directly when they want different
semantics, or they apply the `@AutoHash` annotation to synthesize a
structural hash over the class's fields without writing the body.

### Algorithm-class registry

The standard algorithms live in `cajeta.hash` as concrete classes
implementing the uniform [`Hasher`](../../../runtime/src/cajeta/hash/Hasher.cajeta)
streaming contract. The classes that ship today
(`runtime/src/cajeta/hash/`):

| Class           | Algorithm                | Use case                                      |
|-----------------|--------------------------|-----------------------------------------------|
| `XXHash3`       | XXH3-64                  | fast general-purpose; the default backing     |
| `SipHash`       | SipHash (keyed)          | HashDoS-resistant for user-input keys         |
| `MD5`           | MD5                      | legacy interop / checksums (not secure)       |
| `Sha1`          | SHA-1                    | legacy interop (not secure)                   |
| `Sha256`        | SHA-256                  | cryptographic digest                          |
| `DefaultHasher` | XXH3-64 + process seed   | the compiler-default hasher (facade)          |
| `Hash`          | static facade            | `identity` / `combine` / `processSeed` helpers |

`DefaultHasher` wraps `XXHash3` constructed with the per-process
seed. The synthesizer and collections couple to `DefaultHasher` and
never to `XXHash3` directly, so the backing algorithm can swap. There is
no `Murmur3` or `FNV1a` class. Earlier drafts of this doc named
them, but the shipped registry is the list above. The SHA / MD5
classes live in `cajeta.hash` today rather than in a separate
`cajeta.crypto` package.

### `@AutoHash` annotation

```cajeta
@AutoHash
public class Point {
    int32 x;
    int32 y;
}

@AutoHash
public class Session {
    String userId;
    String token;
    @Exclude Instant lastSeenAt;                 // skip transient field
}
```

`@AutoHash` synthesizes a structural `hash()` override on the
annotated class (`src/cajeta/method/SynthesizedHashMethod.cpp`). The
synthesized body seeds the accumulator from the per-process seed
(`__cajeta_hash_seed`), walks the class's declared fields in order,
hashes each via the field's own `hash()`, where primitive fast-paths use
FNV-1a and class fields dispatch virtually, and folds them together
with `__cajeta_hash_combine`.

There are v1 limitations. Only primitive and class-reference fields are
supported. Struct-field and array-field hashing are not yet
implemented, and they raise a diagnostic naming the `@AutoHash`'d class
and the offending field. `@AutoHash` is a bare annotation in v1,
with no per-class algorithm-selection parameter. To pick a specific
algorithm, declare `hash()` manually and drive the chosen `Hasher`.

The field-level `@Exclude`, which is `@AutoHash.Exclude`, skips a field
from the synthesized walk. That is useful for cached timestamps, transient
connection handles, and anything that is runtime state without being value
identity. The parallel `@ToString` annotation
(`SynthesizedToStringMethod.cpp`) synthesizes a debug `toString()`
the same way, honoring the same `@Exclude`.

### `String.hash()` — FNV-1a over the UTF-8 bytes

`String` overrides `hash()` with FNV-1a, the Fowler–Noll–Vo 1a
variant. It starts from the 64-bit offset basis `0xCBF29CE484222325`,
then for each payload byte `b` computes `h = (h XOR b) * 0x100000001B3`
(see `runtime/src/cajeta/lang/String.cajeta`). It is deterministic
and content-sensitive, but it carries no per-process seed mixing yet.
Seeded, DoS-resistant String hashing, using XXH3-64 and `processSeed`
to match the rest of `cajeta.hash`, is a follow-up that needs the
`int8[]`→`uint8_t*` `@Native` bridge, and the runtime symbol
`__cajeta_hash_bytes` already exists. The empty-bytes case hashes
to the offset basis itself.

Because there is no seed mix today, String hashing does not yet
defend against an offline-precompute or online-probe adversary. The
security-level-enforcement story, meaning how to require a DoS-resistant
hash for attacker-facing collections, is the remaining open piece,
and it is deferred to its own session.

---

## `operator==` — equality flows through `hash()`

`Object`'s `operator==` is a static operator whose default body
is `a.hash() == b.hash()`, and it is null-safe. For the default identity
`hash()` this behaves as pointer equality, because SplitMix64 is bijective
on `pointer ⊕ seed`. For a class that overrides `hash()` with value
semantics it becomes value equality, without the class touching
`operator==` at all. That is exactly how `String` works. It
overrides only `hash()` and inherits the static `operator==`.

So the idiom for value-equal classes is to override `hash()` or
apply `@AutoHash`, and equality follows automatically:

```cajeta
@AutoHash                       // synthesizes hash() over cents + currency
public class Money {
    public int64 cents;
    public String currency;
}

// or, equivalently, by hand:
public class Money {
    public int64 cents;
    public String currency;

    public int64 hash() {
        int64 h = Hash.combine(__cajeta_hash_seed(), this.cents.hash());
        return Hash.combine(h, this.currency.hash());
    }
}
```

> **Hash-collision caveat.** Since `==` compares 64-bit hashes, two
> distinct values collide with probability ~2⁻⁶⁴. For
> collision-sensitive semantics, such as exact legal-text comparison or
> cryptographic equality, use an explicit byte-by-byte method.
> `String.equals(String)` is the byte-for-byte entry point.

`String`'s value equality is locked. See
[String.md](./String.md) § Equality.

---

## `toString()` — deferred

The default returns `null` until the structural synthesizer lands and
the `cajeta.lang.String` construction surface stabilizes. The
synthesized form will follow `TypeName(field1=value1,
field2=value2, ...)`, which is close to Rust's `#[derive(Debug)]` and
sufficient for `println(x)` during development.

Override it manually for user-facing presentation. The synthesized
default is intentionally debug-shaped. Production user-facing strings
are localization concerns, and they should not fall out of `toString`
automatically.

Tracked in specs/Features.md.

---

## `clone()` — LIVE (2026-07-03, slice-spec §6.4)

`__cajeta_object_clone` performs a shallow copy via the RTTI field
walk, an `allocationSize` memcpy plus a per-field fixup, and returns
ownership (`#Object`):

- **value-typed fields** (primitives, structs, enums) are copied by
  `memcpy` of the bits
- **String fields** get a fresh stake on the same immutable byte buffer.
  There is one wrapper per stake, because without GC aliasing one wrapper
  would use-after-free when either owner drops
- **other class-typed fields** are copied by reference, Java-shallow.
  Override `clone()` to deep-copy when shallow is not right
- **String receiver** detaches. The possibly windowed text
  materializes into a fresh owned buffer, which is the retention-amplification
  valve of slice-spec §4.4

User overrides replace the native default and dispatch normally.

---

## Locked in the 2026-05-18 hashing pass

- **`Hasher` interface** is streaming. The shipped
  contract ([`cajeta.hash.Hasher`](../../../runtime/src/cajeta/hash/Hasher.cajeta))
  exposes width-named `write*` feeders plus a terminal `finish()`:
  ```cajeta
  public interface Hasher {
      void  writeInt8(int8 v);   void writeInt16(int16 v);
      void  writeInt32(int32 v); void writeInt64(int64 v);
      void  writeUInt8(uint8 v); /* …uint16/32/64… */
      void  writeFloat32(float32 v); void writeFloat64(float64 v);
      void  writeBoolean(boolean v);
      void  writeBytes(int8[] data);
      void  writeBytesRange(int8[] data, int64 offset, int64 length);
      void  writeString(String s);
      void  writeObject(Object obj);
      int64 finish();            // terminal — digest of bytes written so far
  }
  ```
  Per-type entry points are explicitly width-named, as `writeInt16` and
  `writeInt32`, so widening cannot silently change the digest. `write*`
  returns `void` in v1, with fluent chaining deferred, so chain with
  separate statements.

- **Algorithm classes are instantiable Hashers.** Write `heap XXHash3()`,
  `heap SipHash()` and so on, then `write*` and `finish()`. `DefaultHasher`
  is the compiler-default facade, XXH3-64 seeded from the process
  seed. The static `Hash` facade exposes `identity` / `combine` /
  `processSeed`.

- **`Hash.processSeed()`** is sourced from the OS cryptographic-
  entropy syscall at process startup. It is stable within a process and
  changes across restarts. It is exposed to the synthesizer as the runtime
  symbol `__cajeta_hash_seed`, folded with `__cajeta_hash_combine`.

- **`@AutoHash` annotation** is bare structural-hash synthesis, with no
  per-class algorithm parameter in v1. Field-level `@Exclude` skips
  transient fields. The parallel `@ToString` synthesizes a debug
  `toString()`. Both are primitive/class-field only in v1, and
  struct/array fields raise a diagnostic.

- **equal/hash override pair check** is a lint warning. The rule ID is
  `equals-hash-pair` in [LintRules.md](LintRules.md), and the user can
  suppress it per class.

## Open questions

There is only one genuinely open follow-up:

1. **Security-level enforcement mechanism.** How do we mark types
   / fields / collections as "must use a DoS-resistant hash"?
   - `@Hash.Secure(min=Strength.HIGH)` on HashMap declarations
   - capability annotation: `@requires("attacker-resistant-hash")`
   - runtime check at HashMap construction in `--release` mode
   This is deferred to a dedicated session.

---

## Cross-references

- [Lang.md](Lang.md) is the broader `cajeta.lang` overview.
  Object historically lived inline there, this doc is the
  authoritative spec going forward, and Lang.md is trimmed to a pointer.
- [String.md](./String.md) covers String's `hash()` override (FNV-1a)
  and its value-equality via `hash()`, since it does not override
  `operator==`.
- [Hashing.md](../hash/Hashing.md) covers the broader hashing doctrine,
  the `cajeta.hash.Hash` namespace, the `@AutoHash` synthesizer design,
  and the per-process seed lifecycle.
- [MultiClassing.md](MultiClassing.md) covers how multi-parent
  hierarchies resolve the implicit `extends Object`. Every class
  extends Object, and multiple-inheritance shares the single Object
  sub-object.
- [`runtime/src/cajeta/lang/Object.cajeta`](../../../runtime/src/cajeta/lang/Object.cajeta)
  is the source declaration and its inline doc.
