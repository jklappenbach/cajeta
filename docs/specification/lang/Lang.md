# `cajeta.lang` — Root types

The classes every cajeta program implicitly has access to: `Object`,
`String`, `Encoding`, plus `Optional<T>` and `Pair<K, V>` which live
in `cajeta.lang` because so many APIs across the rest of the stdlib
return them.

`System` is a sibling namespace (not a class) that exposes process-
level I/O, env-var access, and tunable string properties via compiler
intrinsics — see **[`lang/System.md`](System.md)**.

`Cajeta` is a second such namespace. Its one surface a program is
likely to reach for is **`Cajeta.owned(formal)`** — whether *this*
call surrendered the formal's title — for algorithms that genuinely
branch on ownership, such as an interning pool that adopts what it is
given and copies what it is only lent. Correctness never requires it:
stores spell `#=` and let the field or slot bit record what the caller
did. See **[`lang/MemoryModel.md`](MemoryModel.md)**. (`Cajeta.moveMask()`,
the positional transfer-word read this replaced, is retired.)

## `Object` — universal root

See **[`lang/Object.md`](Object.md)** for the full spec
(implicit-extends invariant, the four methods, override-pair
enforcement, `@Hash` algorithm-class registry, equal-implies-same-
hash contract, pending design questions on `Hasher` interface +
security-level enforcement).

## `String` — immutable, encoding-aware

See **[`lang/String.md`](String.md)** for the full spec and
roadmap. `String` is a **class** (`cajeta.lang.String`), not a
primitive. Internal storage is a UTF-8 `int8[]` plus a byte count, a
mode discriminator, and a cached code-point count:

```cajeta
public class String {
    public int8[] bytes;        // UTF-8 payload
    public int32  byteLength;
    public int32  mode;         // 0 = owned, 1 = view (borrowed bytes)
    public int32  cachedCpLength; // -1 until first count()
    ...
}
```

A `String` carries a tagged mode internally — **owned** (`mode = 0`)
or **view** (`mode = 1`, borrowed over bytes that live elsewhere).
String literals lower directly to view-mode instances over `.rodata`.

### Shipped surface

Verified in [`runtime/src/cajeta/lang/String.cajeta`](../../../runtime/src/cajeta/lang/String.cajeta)
and pinned by `test/expression/StringMethodsTests.cpp`:

```cajeta
public class String {
    public String();                              // empty, owned
    public String(#int8[] bytes, int32 byteLength); // adopts the buffer

    // Sizing — count() / size(), NOT length()/size-by-chars.
    public int64 count();         // code-point count (cached)
    public int64 size();          // raw byte count (byteLength)
    public boolean isEmpty();

    // Equality / hash — value semantics via hash() (FNV-1a).
    public boolean equals(String other);          // byte-for-byte
    public int64   hash();

    // Indexing (byte-based unless noted)
    public int8 charAt(int32 idx);                // byte, returns int8
    public int8 byteAt(int32 idx);
    public char codepointAt(int32 cpIdx);         // O(N) walk, returns char

    // Search (byte-based)
    public int64   indexOf(String needle);
    public boolean contains(String needle);
    public boolean startsWith(String prefix);
    public boolean endsWith(String suffix);

    // Transformation — return a fresh OWNED #String.
    public #String substring(int32 begin, int32 end); // BYTE-indexed, copying
    public #String replace(String from, String repl);
    public #String toUpperCase();                 // ASCII-only
    public #String toLowerCase();                 // ASCII-only
    public #String trim();                        // ASCII whitespace
}
```

`+` concatenation is also shipped (lowered to `__cajeta_str_concat`,
pairwise). There is no `Stream` integration on `String` yet, so
enhanced-for over a String is not wired.

### Owned vs. view (design — not yet wired)

Both modes are the same type. A view borrows bytes it doesn't own; an
owned String holds heap bytes. The owned/view **drop distinction is
not yet implemented** — the type system collapses the two, so
helper-produced Strings currently leak at scope exit
(`test/parser/OwnedStringDropTests.cpp`). The named view API
(`viewOf` / `toOwned` / `cString`) from earlier drafts does **not**
exist and is disavowed — explicit duplication is the reserved `clone()`
method (clone semantics — spec lineage: element-ownership §6, whose
type-argument layer is superseded by title-tracking while clone's
contract is unchanged: reference types shallow-copy via the
RTTI walk; value types copy through the COW value hooks). The only view
entry point today is the `String(#int8[], int32)` constructor. See [`lang/String.md` § Memory model](String.md#memory-model).

### Examples

```cajeta
// Method intrinsics — routed through __cajeta_str_* runtime helpers.
String trimmed = "  hello  ".trim();             // "hello"
boolean hasFoo = "foobar".contains("foo");       // true
int64  idx     = "hello world".indexOf("world"); // 6
String sub     = "hello world".substring(6, 11); // "world" (byte range)
int64  cps     = "héllo".count();                // code points
```

### Status

Shipped (pinned by `StringMethodsTests`): `+`, `.contains`,
`.indexOf`, `.substring`, `.toUpperCase`, `.toLowerCase`, `.trim`,
`.replace`, `.equals`, `.size`, `.count`, `.isEmpty`, `.charAt`,
`.startsWith`, `.endsWith`, `.codepointAt`, `.hash`.

**Not implemented** (planned — see `String.md`): static factories
(`fromUtf8`/`fromBytes`/`of`/`repeat`/`join`/`fromCodePoints`),
`getBytes`, `split`/`lines`, `lastIndexOf`, `compareTo`/`Comparable`,
`Locale`-aware case folding, `format`/`printf`, `StringBuilder`,
codepoint/byte iteration streams, and the codepoint-indexed view
`substring`.

## `Encoding` enum

The enum **ships** as
[`cajeta.lang.Encoding`](../../../runtime/src/cajeta/lang/Encoding.cajeta)
with 12 members. The byte ↔ text conversion methods that *consume* it
(`String.fromBytes` / `String.getBytes`) are not yet implemented, so
today an `Encoding` value is just selected by name; the runtime
String path is UTF-8 throughout.

```cajeta
public enum Encoding {
    UTF_8,
    UTF_16_LE, UTF_16_BE,
    UTF_32_LE, UTF_32_BE,
    ASCII,
    LATIN_1,            // ISO-8859-1
    WINDOWS_1252,
    GB18030,            // CJK simplified
    SHIFT_JIS,          // CJK Japanese
    EUC_KR,             // CJK Korean
    BIG_5               // CJK traditional
}
```

Conversion error behavior is governed by the companion
[`EncodingErrorPolicy`](../../../runtime/src/cajeta/lang/EncodingErrorPolicy.cajeta)
enum (`{ FAIL, REPAIR }`, FAIL default — both shipped). Operations
that can't represent a code point under `FAIL` throw
[`EncodingException`](../../../runtime/src/cajeta/lang/EncodingException.cajeta)
(a `RecoverableException` subtype, shipped).

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
Multiple-inherits-Stream — not yet, tracked in specs/Features.md.
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
