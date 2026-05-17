# `cajeta.lang` — Root types

The classes every cajeta program implicitly has access to: `Object`,
`String`, `Encoding`, plus `Optional<T>` and `Pair<K, V>` which live
in `cajeta.lang` because so many APIs across the rest of the stdlib
return them.

## `Object` — universal root

Every class implicitly extends `Object` via the auto-extend pass in
`CajetaLlvmVisitor::visitClassDeclaration`. The base contract:

```cajeta
public class Object {
    public boolean operator==(Object obj);    // identity by default
    public int64 hash();                       // identity-seeded by default
    public String toString(Encoding e = UTF_8);
    public Object clone();                     // shallow field-by-field copy
}
```

**Defaults are identity-based, not structural.** Two distinct
instances with the same field values compare unequal and hash
differently by default. Structural value-equality is opt-in via
`@AutoHash` (see Hashing.md) or by overriding `operator==` /
`hash()` manually.

**Override pair enforcement.** If a class declares `operator==`,
it must also declare `hash()` (and vice versa). The contract — equal
values hash equally — is structurally protected by requiring both
halves to be authored together. `toString` has no pair requirement.

### Examples

```cajeta
// Identity defaults — two distinct objects compare unequal.
Foo a = heap Foo();
Foo b = heap Foo();
boolean same = (a == b);   // false (identity)

// Override pair: both must be declared together.
public class Money {
    public int64 cents;
    public String currency;

    public boolean operator==(Object obj) {
        if (!(obj instanceof Money)) { return false; }
        Money other = (Money) obj;
        return this.cents == other.cents
            && this.currency.equals(other.currency);
    }

    public int64 hash() {
        int64 h = this.cents.hash();
        h = Hash.combine(h, this.currency.hash());
        return h;
    }
}
```

### Status

Implemented: implicit extension, identity `hash()`, identity
`operator==`, the override pair check. `clone()` / `toString` defaults
tracked in Features.md.

Pinned by `test/parser/AutoHashTests.cpp` (override-pair semantics).

## `String` — immutable, encoding-aware

Internal storage: UTF-8 byte array plus a cached code-point count.
A `String` carries a tagged mode internally — **owned** (heap-
allocated; `heap String(...)`) or **view** (borrowed over bytes that
live elsewhere; `String.viewOf(...)`).

```cajeta
public final class String {
    // Owned construction — always allocates.
    public String();
    public String(byte[] bytes, Encoding encoding);
    public static String fromCodePoints(int32[] codePoints);
    public static String repeat(String s, int64 n);

    // View construction — borrows from `source`. No alloc / memcpy.
    public static String viewOf(byte[N]& source,
                                 Encoding encoding = Encoding.UTF_8);
    public static String viewOf(byte[]& source, int64 byteCount,
                                 Encoding encoding = Encoding.UTF_8);
    public static String cString(byte[N]& source,
                                  Encoding encoding = Encoding.UTF_8);

    // Promote view → owned.
    public String toOwned();

    // Inspection
    public int64 size();                       // code-point count
    public int64 byteCount();                  // raw byte count
    public boolean isEmpty();
    public int32 codePointAt(int64 index);
    public boolean equals(String other);
    public int32 compare(String other);
    public int64 hash();

    // Search
    public int64 indexOf(String needle);
    public int64 indexOf(String needle, int64 fromIndex);
    public int64 lastIndexOf(String needle);
    public boolean contains(String needle);
    public boolean startsWith(String prefix);
    public boolean endsWith(String suffix);

    // Transformation — always returns a fresh OWNED String.
    public String substring(int64 start, int64 endExclusive);
    public String concat(String other);
    public String replace(String from, String to);
    public String toUpperCase();
    public String toLowerCase();
    public String trim();
    public String[] split(String separator);
    public String[] lines();

    // Encoding round-trip
    public byte[] getBytes(Encoding encoding);
}
```

`String` multiple-inherits `Stream<int32>` so `for (cp in
someString) { ... }` works and so `size()` is consistent across the
rest of the collection hierarchy. Iteration is over code points, not
bytes — bytes are accessible via `getBytes(Encoding.UTF_8)` when
needed.

### Owned vs. view

Both modes are the same type, so user code mostly doesn't notice.
The differences:

- **Allocation.** `heap String(...)` allocates and copies.
  `String.viewOf(...)` doesn't.
- **Lifetime.** A view's lifetime is tied to the source it borrows
  from. The borrow checker rejects escapes; `.toOwned()` is the
  documented escape hatch when persistence is needed.
- **Mutating operations.** `concat` / `replace` / `toUpperCase` /
  etc. always return a fresh owned String — they can't mutate
  underlying bytes (the source might not be writable). A view's
  `substring(start, end)` can return a sub-view when the result
  stays within the source (cheap, no alloc); pass through
  `.toOwned()` if the substring needs to escape.

### Examples

```cajeta
// Owned construction
String s = heap String("Hello, world", Encoding.UTF_8);
int64 len = s.size();                     // 12 code points
String upper = s.toUpperCase();

// View — borrows bytes, no allocation
byte[100] buf;
// ... fill buf ...
String view = String.viewOf(buf, Encoding.UTF_8);
boolean ok = view.startsWith("HTTP/");

// Promote view → owned when persistence needed
String pinned = view.toOwned();

// Method intrinsics — routed through __cajeta_str_* runtime helpers.
String trimmed = "  hello  ".trim();
boolean hasFoo = "foobar".contains("foo");
int64 idx = "hello world".indexOf("world");
String[] parts = "a,b,c".split(",");
```

### Status

The intrinsic surface (`+`, `.contains`, `.indexOf`, `.substring`,
`.toUpperCase`, `.toLowerCase`, `.trim`, `.replace`, `.equals`,
`.size`, `.isEmpty`, `.split`) is shipped — pinned by
`test/expression/StringMethodsTests.cpp`.

Owned vs view distinction (`viewOf`, `toOwned`, lifetime tying) is
designed; the runtime carries owned-string-drop tracking
(pinned by `test/parser/OwnedStringDropTests.cpp`) but the explicit
view-mode API isn't yet exposed. Tracked in Features.md.

`String.fromCodePoints`, `String.repeat`, `.lines`, `getBytes`,
`codePointAt`, `compare`, `lastIndexOf` — designed, not implemented.

## `Encoding` enum

Designed. Not yet shipped as an explicit enum — runtime intrinsics
hardcode UTF-8 for the String path.

```cajeta
public enum Encoding {
    UTF_8,
    UTF_16_LE,
    UTF_16_BE,
    UTF_32_LE,
    UTF_32_BE,
    ASCII,
    ISO_8859_1,
    WINDOWS_1252,
}
```

Operations that can't represent a code point in the target encoding
throw `EncodingException` (a `RecoverableException` subtype). Lossy
modes ("replace with `?`", "skip") arrive later via an optional
`Encoder` strategy class.

## `Optional<T>` — value-typed sum

See Streams.md § "Optional<T> as a single-shot stream". Designed to
multiple-inherit `Stream<T>` (one-element-or-empty).

```cajeta
public class Optional<T> {
    boolean present;
    T value;

    public Optional(boolean present, T value);
    public boolean isPresent();
    public boolean isEmpty();
    public T get();                            // throws if !present
    public T orElse(T defaultValue);
}
```

### Examples

```cajeta
Optional<int32> some = heap Optional<int32>(true, 42);
Optional<int32> none = heap Optional<int32>(false, 0);

if (some.isPresent()) {
    int32 v = some.get();
}

int32 fallback = none.orElse(99);              // 99

// Returned from Stream.findFirst:
Optional<int32> hit = xs.stream().findFirst((int32 v) -> { return v > 3; });
```

### Status

Construction, `isPresent`/`isEmpty`/`get`/`orElse` — shipped.
`get()` on empty throws `CAJETA_ERROR_NONE_UNWRAP`.
Multiple-inherits-Stream — not yet, tracked in Features.md.
Pinned by `test/parser/OptionalTests.cpp` (6 tests) and
`test/parser/OptionalAndAllocateTests.cpp` (6 tests).

## `Pair<K, V>` — two-field value type

```cajeta
public class Pair<K, V> {
    K first;
    V second;

    public Pair(K first, V second);
    public K first();
    public V second();
}
```

Used as the element type for `Map<K, V>` streams (`HashMap` extends
`Stream<Pair<K, V>>` via multiple inheritance — see Collections.md).

### Examples

```cajeta
Pair<int32, int32> p = heap Pair<int32, int32>(3, 4);
int32 a = p.first();                           // 3
int32 b = p.second();                          // 4
```

### Status

Construction + getters shipped. Pinned by `test/parser/PairTests.cpp`
(3 tests). Multiple inheritance from anything else not yet relevant.
