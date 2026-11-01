# `cajeta.lang.String` — immutable UTF-8 text

> **RE-CORED (slices plan 6.2.2, [`slice-spec.md`](slice-spec.md) §8.2):** String's storage
> is now the 16-byte tagged Utf8 core. The legacy `mode` field, the `bytes`/`byteLength`
> fields, and the 24-byte SSO region are gone. `lenTag` carries the byte length (mask
> `0x1FFFFFFF`) plus the tag bits. Text `<= 12 B` lives Inline in the wrapper, in the 12
> bytes of aux+base, with no buffer at all. This is the SSO successor, with the cap reduced
> from 24 to 12. Longer text is a pointer form `{off = aux, root header = base}` whose bits
> say how the root is held. bit31 means one rc stake, which drop releases and copies retain.
> bit30 means a stakeless borrow. bit29 means a static root, used for literals. No bits means
> an owned sole root, which drop frees through the owner-drop seam, so there is zero rc
> traffic unless a slice escapes. `substring`/`trim` stay zero-copy for `> 12 B` windows and
> normalize `<= 12 B` results to Inline, per spec §8's normalization rule. Byte-level
> operations live in C natives (`cajeta_rt_string.c`). The facade exposes `byteLength()`,
> which is a method now, `byteOffset()`, `root()`, which is null for Inline, `toBytes()`, and
> `byteAt()`. The `const char*` ABI (`__cajeta_string_cstr`) hands out full-window roots
> directly and materializes Inline/windowed forms into a per-thread ring scratch. `clone()`
> on a windowed String still detaches. `Utf8` and String now share one representation, and
> `Utf8.of(String)` is a 16-byte copy plus tag fixup.

This document is the single source of truth for the String surface
in Cajeta. It captures the 2026-05-18 design pass with the user,
which covered 15 questions and a tidy-up, and it is annotated against
what has since shipped.

The status is partially implemented. A core surface is live in
[`runtime/src/cajeta/lang/String.cajeta`](../../../runtime/src/cajeta/lang/String.cajeta)
and pinned by `test/expression/StringMethodsTests.cpp`. The larger
design surface below, which covers factories, `format`/`printf`,
`StringBuilder`, encoding interchange, locale-aware case folding and
graphemes, is still planned. Each subsection notes its status.

> **What ships today** (verified against the source + tests):
> - Storage layout `{ int32 lenTag; int32 aux; int8[] base;
>   int32 cachedCpLength; }` (the tagged core, described in the banner);
>   constructors `String()` and the adopting
>   `String(#int8[] bytes, int32 byteLength)`.
> - `count()` (codepoints, cached), `size()` (bytes), `isEmpty()`,
>   `hash()` (FNV-1a), `equals(String)`.
> - `charAt(int32)`→`int8` (byte-indexed), `byteAt(int32)`,
>   `codepointAt(int32)`→`char` (O(N) walk).
> - `indexOf(String)`, `startsWith`, `endsWith`, `contains` (all
>   byte-based).
> - `substring(int32 begin, int32 end)` is byte-indexed and copying.
>   It is neither a view nor codepoint-indexed; see § Substring.
> - `toUpperCase()`, `toLowerCase()`, `trim()`, `replace(String,
>   String)`, all ASCII-only with no `Locale` argument.
> - `+` concatenation (lowered to `__cajeta_str_concat`).
> - String literals materialize as constant class-String instances.
>   Text `<= 12 B` is Inline, packed into the aux/base constants, and
>   longer text is a Static pointer form (bit29) over a `.rodata` root.
>
> Names and semantics in the design below that DIFFER from what
> shipped are flagged inline. Notably, the shipped sizing methods are
> `count()` / `size()` rather than `length()` / `byteLength()`, and
> `String` carries value equality via its `hash()` override. It does
> not override `operator==`.

---

## Storage model

- **UTF-8 internally, backed by `int8[]`.** Most files, network
  protocols, JSON, source code, and config formats already speak
  UTF-8, so most I/O boundaries are zero-copy, and the ASCII fast
  path is bytewise. The "O(N) to index by character" cost is real,
  but it does not matter for almost any task users actually do, such
  as iterate, compare, decode and write. When it does matter, for
  cursor placement or line breaks, graphemes are the right unit
  anyway and UTF-16 code units do not help.
- **Immutable.** Once constructed, a String's bytes never change.
  `StringBuilder` (see below) is the companion for incremental
  construction.
- **Owned Strings are dropped; borrowed and static forms are not.** The
  tag decides what scope exit reclaims. An owned sole root (no bits)
  frees through the array owner-drop seam, an rc-staked window
  (bit31) releases its stake, and a static root (bit29, literals) or
  stakeless borrow (bit30) reclaims nothing. In every case the
  wrapper itself is freed (`__cajeta_string_drop`, claim-gated). A
  stakeless view is therefore NOT unconditionally safe. It must not
  outlive its parent, so copy it, or take a window that holds an rc
  stake. This is also why a `#String` result must be received with
  `#=` (spec §4.6). The receiving local's drop is what frees it.
- **The tagged core (6.2.2).** The length word IS the discriminator:
  - **Inline** (`len <= 12`, no tag bits) means the text lives in the
    wrapper itself. It is self-contained, with no buffer and no rc.
    Every `<= 12 B` result is built Inline by normalization, so
    pointer forms are unambiguous.
  - **Pointer forms** (`len > 12`): `aux` is the window byte offset,
    and `base` is the ROOT CajetaArray header. The text is at
    `base[8 + aux ..]`.
    - **bit31** means the wrapper holds one rc stake on the root,
      used for windowed shared views. Drop releases the stake, and
      copies retain it.
    - **bit30** means a stakeless BORROW of a heap root, the
      compiler's scope-proven substring downgrade.
    - **bit29** means a STATIC root, used for literals and views of
      them, with no rc.
    - **no bits** means an OWNED sole root. Drop frees it through the
      owner-drop seam, which claims or releases, so there is zero
      shared-table traffic for never-shared strings.
- **One type.** All forms share the same `String` surface, and the
  tag is an implementation detail behind `byteLength()`,
  `byteOffset()`, `root()`, `toBytes()`, and `byteAt()`.

```cajeta
class String {
    int32  lenTag;         // len | tag bits (mask 0x1FFFFFFF)
    int32  aux;            // Inline text bytes 0..3 / window offset
    int8[] base;           // Inline text bytes 4..11 / root header
    int32  cachedCpLength; // -1 = not yet computed; codepoint count
}
```

---

## `char` is a 32-bit Unicode codepoint

Cajeta redefines `char`, previously i8 as in C, to mean a Unicode
codepoint backed by `i32` (`src/cajeta/type/CajetaType.cpp:406`).
`'c'` is `char` 99, `'é'` is 233, and `'😀'` is 0x1F600, all 32-bit.
Character arithmetic still works for ASCII, and emoji and CJK work
natively. The 8-bit byte already has a perfect name in `int8`.
Cajeta has no `byte` type, and byte buffers are `int8[]`, so there
is no good reason for `char` to also mean "byte" in 2026.

The Java 16-bit `char` is the cause of the entire surrogate-pair
mess. We have the luxury of redefining it. No Cajeta user code uses
the old `char` type, so there is no breakage.

---

## Equality — value, always

`"abc" == "abc"` returns `true` regardless of allocation identity.
Equality is value equality. Java's `==` on String is pointer
equality, so `s1 == s2` is false for two distinct String objects
containing the same characters, and you have to remember
`.equals()`. That is the cause of an enormous fraction of real-world
Java bugs. Python, Rust, Go and Swift all use value equality on
strings for `==`, and Cajeta joins them.

This is how it works as shipped. `String` does not declare its own
`operator==`. It overrides only `hash()`, FNV-1a over the UTF-8
bytes, and `Object`'s static `operator==` defaults to
`a.hash() == b.hash()`, so two Strings with identical bytes compare
equal through that path. The explicit byte-for-byte check is
`equals(String other)`. Prefer it when hash-collision-exact
semantics matter, since `==` compares 64-bit hashes, with ~2⁻⁶⁴
collision probability. See [Object.md](./Object.md) § *operator== — equality flows through hash()*.

```cajeta
public int64   hash();             // FNV-1a; overrides Object.hash()
public boolean equals(String other); // byte-for-byte
```

Pointer equality remains available via `Hash.identity(s1) ==
Hash.identity(s2)` if anyone ever genuinely needs it.

---

## Comparable + ordering

The status is planned. `String.cajeta` does not yet implement
`compareTo` / `compareToIgnoreCase` and does not declare `implements
Comparable<String>`, and `Locale` does not exist in the codebase
(see [Locale.md](./Locale.md)). The design:

```cajeta
public class String implements Comparable<String> {
    public int32 compareTo(String other);
    public int32 compareToIgnoreCase(String other, Locale locale);
}
```

`compareTo` is lexicographic on the UTF-8 bytes. UTF-8 byte order
matches codepoint order by construction, which is a UTF-8 property,
so this is also lexicographic in Unicode codepoint sequence. This is
the standard behavior for any non-locale-aware sort.

`compareToIgnoreCase(other, Locale)` does case-folded comparison via
the Unicode case-folding tables, and `Locale` controls
Turkish-specific `I`/`ı`/`İ` and similar regional case rules.

String implements `Comparable<String>` for use as a TreeSet/TreeMap
key and for sorted-collection compatibility.

---

## Hashing — FNV-1a (shipped)

```cajeta
int64 hash();   // overrides Object.hash()
```

The shipped algorithm is FNV-1a. It is not the Java polynomial mix.
The offset basis is `0xCBF29CE484222325`, then `h = (h XOR b) *
0x100000001B3` per UTF-8 byte (`String.cajeta`). It is deterministic
and content-sensitive, with no per-process seed mixing yet, so it
does not currently resist an offline-precompute or online-probe
adversary. Seeded, DoS-resistant String hashing (XXH3-64 +
`Hash.processSeed`) is the planned follow-up, and the runtime symbol
`__cajeta_hash_bytes` already exists. See
[Object.md § hash()](./Object.md#hash--pluggable-algorithm) for the
broader pluggable-algorithm model and the security tradeoffs.

---

## Encoding interchange

The `Encoding` and `EncodingErrorPolicy` enums and the
`EncodingException` class ship
([`Encoding.cajeta`](../../../runtime/src/cajeta/lang/Encoding.cajeta),
[`EncodingErrorPolicy.cajeta`](../../../runtime/src/cajeta/lang/EncodingErrorPolicy.cajeta),
[`EncodingException.cajeta`](../../../runtime/src/cajeta/lang/EncodingException.cajeta)),
but the conversion methods that consume them, `fromBytes` and
`getBytes`, are not yet implemented. See § Construction. The
shipped `Encoding` enum carries 12 members, the design list below
plus `LATIN_1` and the four CJK encodings, matching the source.

External byte data flows in and out through an `Encoding` enum + an
`EncodingErrorPolicy` enum:

```cajeta
public enum Encoding {
    UTF_8,
    UTF_16_LE, UTF_16_BE,
    UTF_32_LE, UTF_32_BE,
    ASCII,
    LATIN_1,                  // ISO-8859-1
    WINDOWS_1252,
    GB18030,                  // CJK simplified
    SHIFT_JIS,                // CJK Japanese
    EUC_KR,                   // CJK Korean
    BIG_5                     // CJK traditional Chinese
}

public enum EncodingErrorPolicy {
    FAIL,        // throw EncodingException (default)
    REPAIR       // substitute U+FFFD on UTF targets, '?' on legacy,
                 // codec-specified marker on CJK
}
```

`FAIL` is the default everywhere. `REPAIR` is opt-in. There is no
`IGNORE` policy, because silent data loss is strictly worse than
substitution. The user has zero hint that anything went wrong, while
substitution at least produces detectable output.

This is the stdlib-wide pattern for parsing-policy enums. See the
*Parsing convention* note below.

### `EncodingException`

```cajeta
class EncodingException extends RecoverableException {
    int64    offset;            // position in source string
    int32    codepoint;         // offending codepoint (or 0 on byte-side)
    int32    sourceEncoding;    // Encoding ordinal (e.g. Encoding.UTF_8)
    int32    targetEncoding;    // Encoding ordinal
    String   reason;            // "codepoint U+1F600 not representable in ASCII"
}
```

In the shipped source the encoding fields are stored as `int32`
ordinals rather than `Encoding` references, and the constructor is
`EncodingException(String message, int64 offset, int32 codepoint,
int32 sourceEncoding, int32 targetEncoding, String reason)`.

The exception carries enough to fix the bug. It holds the source
position, the offending codepoint, both encodings, and a short
human-readable reason. Python's `UnicodeEncodeError` is the model.

---

## Construction / factories

The status is planned. None of the static factories below
(`fromUtf8`, `fromBytes`, `repeat`, `join`, `fromCodepoint(s)`,
`of(...)`) exist in `String.cajeta` today. What ships is two
instance constructors. The no-arg `String()` builds an empty, owned
String. The ownership-transfer `String(#int8[] bytes, int32 byteLength)`
ADOPTS the caller's buffer, so a `<= 12 B` payload copies Inline and
the buffer is freed immediately, and a longer payload becomes this
String's owned root. Both are pinned by
`test/parser/StringViewCtorTests.cpp`. String literals are lowered
directly to view-mode instances by the compiler. The factory surface
below is the design target:

```cajeta
public class String {
    // UTF-8 fast paths
    public static #String fromUtf8(int8[] bytes, int32 len);
    public static #String fromUtf8Unchecked(int8[] bytes, int32 len);

    // Generic encoding ingestion
    public static #String fromBytes(int8[] bytes, int32 len, Encoding enc);
    public static #String fromBytes(int8[] bytes, int32 len, Encoding enc,
                                    EncodingErrorPolicy policy);

    // Convenience builders
    public static #String repeat(String s, int32 count);
    public static #String join(String separator, String[] parts, int32 partsLen);
    public static #String fromCodepoint(char c);
    public static #String fromCodepoints(char[] cps, int32 len);

    // Numeric / primitive convenience (matches Java String.valueOf)
    public static #String of(int32 v);
    public static #String of(int64 v);
    public static #String of(float32 v);
    public static #String of(float64 v);
    public static #String of(boolean b);
    public static #String of(char c);
    public static #String of(Object obj);            // calls obj.toString()
}
```

`fromUtf8` validates the input and throws `EncodingException` on
malformed bytes. `fromUtf8Unchecked` skips validation, for
trusted-source paths where the cost matters.

All factory results are heap-owned `#String`. Computed-at-runtime
values cannot point at static storage, so none of them is view-mode.

### Encoding interchange — out

```cajeta
public #int8[] getBytes(Encoding enc);
public #int8[] getBytes(Encoding enc, EncodingErrorPolicy policy);
```

These are symmetric with `fromBytes`, and the default policy is
`FAIL`. `getBytes(UTF_8)` is the no-conversion fast path. It returns
a fresh copy of the internal buffer, since the caller may mutate the
returned `int8[]`.

---

## Iteration

These accessors ship. Note the names, since the sizing methods are
`count()` / `size()` rather than `length()` / `byteLength()`:

```cajeta
public int64 count();               // codepoints (cached after first walk)
public int64 size();                // bytes (byteLength)
public boolean isEmpty();
public int8 byteAt(int32 byteIdx);          // O(1), no bounds check
public int8 charAt(int32 byteIdx);          // O(1), byte-indexed, returns int8
public char codepointAt(int32 cpIdx);       // O(N) walk, returns char
```

The following are planned and not yet implemented, since there is
no `Stream` integration on String yet:

```cajeta
public Stream<char> codepoints();           // O(1) per next()
public Stream<int8> bytes();                // O(1) per next()
```

Enhanced-for over a String iterates codepoints. This is planned:

```cajeta
for (char c : "ciao") {
    println(c);
}
// → 'c', 'i', 'a', 'o'
```

The loop is sugar for `s.codepoints()`. The iteration unit is the
Unicode codepoint, the same shape as Rust's `chars()`. Each `c` is a
`char`, which is an i32 codepoint.

Byte iteration is opt-in via `s.bytes()`. It is useful for codecs
and scanners that need to walk the raw UTF-8 representation.

---

## Substring + slicing

```cajeta
public #String substring(int32 begin, int32 end);   // shipped
```

The shipped behavior is byte-indexed and copying. The live
implementation in `String.cajeta` takes a half-open byte range
`[begin, end)`, with indices clamped to `[0, byteLength]` and an
inverted range yielding empty. It allocates a fresh `int8[]`, copies
the slice, and returns a heap-owned `#String`. It is not
codepoint-indexed and it is not a zero-copy view. The doc comment in
the source notes that view-mode slicing is a planned follow-up that
needs pointer arithmetic on `int8[]` (`&this.bytes[begin]`), which
the language has no surface syntax for yet. It is pinned by
`StringMethodsTests`, where `substring(6, 11)` on `"hello world"`
yields `"world"`.

### Planned: codepoint-indexed view slicing

The design target is a zero-copy, codepoint-indexed slice:

> Return a view-mode instance (`mode = 1`) whose `bytes` field points
> into the parent's UTF-8 payload. The call is O(1) after the
> codepoint→byte walk, with no copy, and the return type is plain
> `String` without `#` because no ownership crosses.

This depends on the owned/view drop distinction, which is also not
yet wired. Today's String-producing helpers (`concat`, `substring`,
`toUpperCase`, …) `malloc` and currently leak at scope exit, because
the type system collapses owned and borrowed `String` into one type.
`test/parser/OwnedStringDropTests.cpp` holds the fix outline. The
"Strings are never dropped" framing below is a design rationale for
why view sharing is safe. It is not a description of a committed
permanent policy, since the source's drop chain is designed to
reclaim owned bytes (`mode == 0`) once the distinction lands.

Compare the trade-off with Java 6's shared-char[] substring, which
was abandoned in Java 7+. Java's char[] sharing leaked the entire
backing array, so a 10-char substring kept a 1 MB source file alive
in the heap. Cajeta's planned view substring avoids re-rooting the
whole parent buffer by carrying an explicit `(bytes, byteLength)`
window. Workloads that produce hundreds of MB of unique String
content should materialize through `int8[]` for transient work or
bound their String creation.

---

## Standard methods

A subset ships today, verified in `String.cajeta` and
`StringMethodsTests`: `indexOf(String)` (returns `int64`, byte
index), `startsWith`, `endsWith`, `contains`, `toUpperCase()`,
`toLowerCase()`, `trim()`, `replace(String find, String
replacement)`. The case and trim methods are ASCII-only and take no
`Locale`. Everything else below is planned, namely the `Locale`
overloads, the no-arg `Locale.ROOT` defaults, `split`,
`lastIndexOf`, `indexOf(char)`, `indexOf(String, fromCp)`,
`replaceFirst`, `replace(char, char)`, `trimStart`/`trimEnd`,
`padStart`/`padEnd`, `equalsIgnoreCase*` and `isBlank`. The full
design set:

```cajeta
// Search
public int32 indexOf(String s);
public int32 indexOf(String s, int32 fromCp);
public int32 indexOf(char c);
public int32 lastIndexOf(String s);
public int32 lastIndexOf(char c);
public boolean contains(String s);
public boolean contains(char c);
public boolean startsWith(String prefix);
public boolean endsWith(String suffix);

// Split
public #String[] split(String separator);
public #String[] split(char separator);
public #String[] splitLines();      // \n / \r\n / \r

// Trim
public #String trim();              // ASCII whitespace, both ends
public #String trimStart();
public #String trimEnd();
// trimUnicode() — v2, needs Unicode whitespace tables

// Replace
public #String replace(String find, String replacement);    // all
public #String replaceFirst(String find, String replacement);
public #String replace(char from, char to);

// Case
public #String toLower(Locale locale);
public #String toUpper(Locale locale);
public #String toLower();                                    // Locale.ROOT
public #String toUpper();                                    // Locale.ROOT
public boolean equalsIgnoreCase(String other, Locale locale);
public boolean equalsIgnoreCaseAscii(String other);

// Padding
public #String padStart(int32 totalCpLength, char fillChar);
public #String padEnd(int32 totalCpLength, char fillChar);

// Predicates
public boolean isBlank();           // ASCII whitespace only
// isBlankUnicode() — v2
```

No-arg `toLower()` / `toUpper()` default to `Locale.ROOT`. That
gives locale-independent Unicode behavior with no thread-local
foot-gun, which is Java's Turkish-locale-on-CI-box bug class.

Three methods are skipped. `concat` is covered by `+`. A `chars()`
alias for `codepoints()` and a `hashCode()` alias for `hash()` are
skipped because there is one name per concept.

---

## `operator+` — concatenation (shipped, pairwise)

```cajeta
String result = a + b + c + d;
```

Concatenation ships today, pairwise. A `+` on two `String` operands
lowers to a single `__cajeta_str_concat` call. Each operand's
underlying C-string is extracted, concatenated into a fresh
`malloc`'d buffer, and re-wrapped in a heap-owned class `String`
(`BinaryOpExpression.cpp`, `BINARY_OP_ADD`). A chain `a + b + c + d`
is therefore evaluated left-associatively as N−1 pairwise concats,
with N−2 intermediates. The batched single-allocation primitive
originally designed has not shipped.

> **Planned: JEP 280-style single-call concat.** The design target
> is for the compiler to walk the whole `+`-chain, compute the exact
> total byte length once, allocate ONE buffer, and copy each piece
> in. This is modeled on Java's JEP 280 (Java 9+), with one
> allocation per chain. It is not yet implemented.

In a loop, each `+`-chain still allocates. That is the right
semantics for immutable strings, but it is a perf trap. The planned
`StringBuilder`, described below and not yet implemented, is the
intended fix, and the lint rule `string-concat-in-loop` (see
[LintRules.md](LintRules.md)) catches the bad shape.

---

## `String.format` / `String.printf`

The status is planned. Neither `String.format` nor `String.printf`
exists in `String.cajeta`. Runtime-side `{}`-substitution does ship,
but only as part of the `System.stdout.println(fmt, ...)` intrinsic
described in [System.md](./System.md), and there is no `String`
method for it. The design:

There are two methods and two notations. Cajeta ships both.

```cajeta
// Python-style {} placeholders (default, modern)
public static #String format(String template, Object... args);

// C/Java-style %s %d (compatibility, familiarity)
public static #String printf(String template, Object... args);
```

```cajeta
String.format("hello {}, you are {}", name, age);
String.format("hello {name}, you are {age}", name, age);   // named
String.format("{:>10.2}", num);                            // width/precision

String.printf("hello %s, you are %d", name, age);
String.printf("%-10s %5d", name, count);                   // width/alignment
```

There is no auto-detection. A literal `%` in a `{}` template, or
the reverse, is a footgun if both notations are accepted by one
method. With two methods, the choice is explicit.

### Compile-time template validation

When the `template` arg is a literal string, the compiler validates
at parse time:

- Python-style: `{}` count matches `args` count; named `{foo}`
  references resolve.
- printf-style: `%s` / `%d` / `%f` / etc. specifier count matches
  `args` count, and types are compatible (`%d` requires an integer
  arg, `%s` requires anything-Object, etc.).

Mismatches surface via the lint rule `format-template-arg-mismatch`,
which was added in the String spec pass (see
[LintRules.md](LintRules.md)). At runtime, the format methods raise
`IllegalArgumentException` if a non-literal template does not match
its args.

---

## `StringBuilder` — full robust API

The status is planned. There is no `StringBuilder` class in the
codebase yet. The full design:

`StringBuilder` is the companion mutable builder. It is
single-threaded, and concurrent use takes an explicit `Lock` at the
transaction boundary (see [Memory model notes on
concurrency](#memory-model) below).

```cajeta
public class StringBuilder {

    // Construction
    public StringBuilder();
    public StringBuilder(int32 initialCapacityBytes);
    public StringBuilder(String s);

    // Appends
    public StringBuilder append(String s);
    public StringBuilder append(char c);
    public StringBuilder append(int32 v);
    public StringBuilder append(int64 v);
    public StringBuilder append(float32 v);
    public StringBuilder append(float64 v);
    public StringBuilder append(boolean b);
    public StringBuilder append(int8[] bytes, int32 len, Encoding enc);
    public StringBuilder append(int8[] bytes, int32 offset, int32 len, Encoding enc);
    public StringBuilder appendLine();
    public StringBuilder appendLine(String s);
    public StringBuilder appendRepeated(char c, int32 count);
    public StringBuilder appendRepeated(String s, int32 count);

    // Inserts (codepoint-indexed)
    public StringBuilder insert(int32 cpIdx, String s);
    public StringBuilder insert(int32 cpIdx, char c);
    public StringBuilder insert(int32 cpIdx, int64 v);

    // Deletes / truncates
    public StringBuilder deleteRange(int32 startCp, int32 endCp);
    public StringBuilder deleteCodepoint(int32 cpIdx);
    public StringBuilder deleteLast();
    public StringBuilder truncate(int32 keepFirstCp);
    public StringBuilder clear();

    // Modifications
    public StringBuilder replace(int32 startCp, int32 endCp, String s);
    public StringBuilder replaceAll(String find, String replacement);
    public StringBuilder setCodepoint(int32 cpIdx, char c);
    public StringBuilder reverse();                  // codepoint-reverse

    // Inspection
    public int32 byteLength();
    public int32 codepointLength();                  // O(N) walk
    public boolean isEmpty();
    public int8 byteAt(int32 byteIdx);
    public char codepointAt(int32 cpIdx);            // O(N) walk
    public int32 capacityBytes();

    // Search
    public int32 indexOf(String s);
    public int32 indexOf(String s, int32 fromCp);
    public int32 indexOf(char c);
    public int32 lastIndexOf(String s);
    public int32 lastIndexOf(char c);
    public boolean contains(String s);
    public boolean contains(char c);

    // Capacity
    public StringBuilder reserve(int32 additionalBytes);
    public StringBuilder shrinkToFit();

    // Output
    public #String toString();           // copy; builder retained
    public #String take();               // transfer + reset
}
```

`toString()` does NOT reset the builder. This is the same shape as
Java, so you can call it multiple times, and each call copies the
current state to a fresh `#String`. Use `take()` for the one-shot
build-and-emit case. It transfers the buffer to the new String and
resets the builder for reuse.

Indexing is codepoint-based for user-facing methods (insert, delete,
replace, indexOf, codepointAt) and byte-based for raw-byte accessors
(byteAt, byteLength, capacityBytes). Method names make the unit
unambiguous at every call site.

`reverse()` reverses codepoints. A combining-mark sequence such as
`"é"` reverses to `"́e"`, which is visually wrong.
Grapheme-aware reverse needs Unicode tables. See the v2 plan below.

---

## Memory model

> **Implementation status — SUPERSEDED; the rules below are history.**
> The owned/view distinction IS wired, and the never-drop policy this
> section states was retired by the tagged-core re-core described in
> the banner at the top of this file. `lenTag` carries the
> discriminator, which is an owned sole root (no bits), an rc stake
> (bit31), a stakeless borrow (bit30), or a static root (bit29). An
> owned `String` local registers `__cajeta_string_drop` at scope exit
> (`LocalVariableDeclaration.cpp`), which tag-dispatches in
> `__cajeta_string_drop_claimed` (`runtime/native/cajeta_rt_core.c`).
> SHARED releases this wrapper's stake, OWNED frees the sole root, and
> BORROW and STATIC are no-ops. Non-escaping concat results are
> bump-allocated and reclaimed by the frame-arena reset. Nothing here
> leaks, and the proposed `OwnedString` marker was abandoned rather
> than deferred.
>
> Two consequences invert what is written below. A bit30 view holds
> no stake, so a view outliving its parent reads freed memory. And
> `#`-transfer DOES change reclamation, since it deactivates the local's
> drop entry. `#String`-returning helpers (`substring`, `trim`,
> `toUpperCase`, `toLowerCase`, `replace`) must be received with `#=`,
> and a plain `=` is `CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER` (spec §4.6).
> `String.empty()`, the empty-string singleton, is still planned.

- **Strings are never dropped.** This is the global lifetime rule.
  Cajeta-allocated String instances and their UTF-8 payloads persist
  for the process's lifetime. The drop chain does NOT register
  String allocations, and scope exit does not reclaim Strings.
- **Owned String** (`mode = 0`) means the bytes were heap-allocated
  for this String. The String holds the canonical reference but is
  not itself drop-eligible, so the bytes outlive every visible scope.
- **View String** (`mode = 1`) means the bytes point into static
  storage (string literals), into another String's payload (substring
  slices), or into a buffer the codec layer owns (JSON token spans).
  View-mode Strings are also never dropped, consistent with the
  global rule, and the buffer's parent owns the lifetime.
- **`#`-transfer of a String** moves the reference between holders
  but does not change reclamation behavior. The never-drop policy
  applies regardless of how many holders the same String has.
- **`#` on view-mode is a no-op.** `#"literal"` does not transfer
  anything, because there is nothing to transfer. The compiler emits
  the lint warning `transfer-on-view-string` (see
  [LintRules.md](LintRules.md)) so the meaningless `#` is
  visible. Suppress via `@SuppressLint("transfer-on-view-string")`
  for the rare deliberate case.
- **Memory-cost trade-off.** Long-running processes that produce
  unbounded distinct String content will grow without bound under
  the never-drop policy. Workloads with that shape (high-churn
  text-rewriting pipelines, log accumulators) should hold transient
  text as `int8[]` and only materialize to `String` at egress, or
  be redesigned to bound their String creation. This is the same
  trade Strings make in Java's intern pool and .NET's interned
  string table. Cajeta extends it from literals to ALL Strings, to
  simplify substring and view semantics.
- **Empty-String singleton.** A single shared `""` view-mode String
  sits in static storage, and `String.empty()` returns it. There is
  no allocation per empty literal.
- **Concurrency.** String is immutable, so it is freely shareable
  across threads. `StringBuilder` is single-threaded, and concurrent
  writers wrap their transaction in a `Lock`. There is no
  `StringBuffer`-style synchronized variant. Java's StringBuffer is
  widely considered a mistake, because single-method synchronization
  is the wrong granularity and transaction-level locking is what
  users actually need. The field-level `@ReadWriteLock` annotation
  idea raised during design is deferred to the broader concurrency
  design pass, since it applies to many things beyond StringBuilder.

---

## Locale dependency

Several methods take a `Locale`: `toLower(Locale)`, `toUpper(Locale)`,
`compareToIgnoreCase(other, Locale)`, `equalsIgnoreCase(other,
Locale)`. The no-arg overloads default to `Locale.ROOT` (Unicode
tables without language-specific overrides).

`Locale` is a separate spec, tracked in specs/Features.md. It is a
BCP 47-shaped value class with no thread-local default. `Locale` and
String ship in tandem so the case-folding surface is coherent on day
one.

---

## v2 grapheme-cluster roadmap

The unit a human reader thinks of as one character is the *grapheme
cluster*, so emoji + skin tone + ZWJ is 1 grapheme. Codepoint
iteration is the v1 default because graphemes need the Unicode
Grapheme Cluster Break tables, about 150 KB of compiled data, and
are not strictly necessary for the most common text operations.

**Planned v2 additions to `String`:**

```cajeta
public Stream<String> graphemes();           // each yielded String is one cluster
public int32 graphemeCount();                // O(N) walk
public String graphemeSlice(int32 startG, int32 endG);  // substring by graphemes (view, no copy)
```

**Planned v2 additions to `StringBuilder`:**

```cajeta
public StringBuilder reverseGraphemes();     // visually-correct reverse
public StringBuilder deleteGraphemeRange(int32 startG, int32 endG);
```

**Planned v2 additions to `String` methods set:**

- `trimUnicode()` handles Unicode whitespace (U+00A0, U+2028, U+2029, etc.)
- `isBlankUnicode()` is the Unicode whitespace predicate

The codepoint methods do not go away in v2, because codec and
scanner work wants codepoint or byte granularity. Graphemes are a
new layer on top of them.

---

## Parsing convention (cross-cutting)

Cajeta stdlib parsing APIs follow a uniform convention established
in this design pass:

- **`FAIL` as default policy.** Throw on input that does not fit the
  expected shape. Never coerce silently.
- **`{ FAIL, REPAIR }` enum shape** per domain, for example
  `EncodingErrorPolicy`, `JsonReadPolicy` and `NumberParsePolicy`.
  Each enum is per-domain, since the semantics of REPAIR differ. It
  substitutes for encoding, allows lenient syntax for JSON, and
  saturates for numeric. The shape is uniform.
- **Per-call parameter.** There is no thread-local default and no
  action at a distance. Test code cannot accidentally widen
  production tolerance.
- **No `IGNORE` policy.** Silent data loss is worse than
  substitution.

It is documented here because String is the first place you
encounter the convention, via `EncodingErrorPolicy`.

---

## Cross-references

- [Object.md](./Object.md) is the universal-root spec. It covers the
  `hash()` pluggable model and the `operator==` defaults that String
  overrides.
- [Lang.md](Lang.md) is the broader `cajeta.lang` overview.
  Historically the String section lived inline there.
- [LintRules.md](LintRules.md) holds the three lint rules String
  introduces: `string-concat-in-loop`, `transfer-on-view-
  string`, `format-template-arg-mismatch`.
- [Hashing.md](../hash/Hashing.md) covers the `cajeta.hash.Hash`
  namespace and the `@Hash` algorithm-class registry.
- [ErrorModel.md](../error/ErrorModel.md) covers the
  `RecoverableException` doctrine. `EncodingException` extends
  `RecoverableException`.
- [`Locale.md`](./Locale.md) is the pending companion spec.

---

## Locked decisions summary (2026-05-18 pass)

| Question | Answer |
|----------|--------|
| Encoding storage | UTF-8 backed by `int8[]` |
| Mutability | Immutable; separate StringBuilder |
| `char` type | i32 Unicode codepoint (was i8) — **shipped** |
| Equality | Value (`==` via `hash()` override, not `operator==`) — **shipped** |
| Hashing | FNV-1a, no seed yet (design target: seeded XXH3) — **shipped** |
| Encoding enum | UTF-8/16/32 + ASCII + Latin-1 + Windows-1252 + GB18030 + Shift_JIS + EUC-KR + Big5 |
| Encoding error policy | `{ FAIL, REPAIR }`; FAIL default; no IGNORE |
| Substring | **Shipped: byte-indexed, copying** (design target: codepoint-indexed view) |
| Iteration | `count()` codepoints shipped; `codepoints()` stream / for-loop **planned** |
| Graphemes | v2, explicit roadmap |
| `operator+` | JEP 280 single-call concat primitive |
| String literal mode | View-mode pointing at static storage |
| `#` on view-mode | No-op + lint warning |
| Empty String | Static singleton |
| `String.format` | Python-style `{}` + `String.printf` for printf-style |
| Format validation | Compile-time when template is literal + lint rule |
| `String.of(...)` | Heap-owned `#String` |
| Standard methods | Full set; ASCII variants for fast paths |
| `compareTo` | Lexicographic on UTF-8 bytes; `Comparable<String>` |
| `toLower / toUpper` | Unicode-aware via `Locale`; no-arg defaults to `Locale.ROOT` |
| `isBlank()` | ASCII whitespace; `isBlankUnicode()` v2 |
| Concurrency | Immutable String → free to share; StringBuilder single-threaded; no StringBuffer |

---

## Open items

- **Q7 security-level enforcement.** How to mark "this HashMap must
  use a DoS-resistant hash", whether by annotation, capability or
  runtime check, is deferred to its own session.
- **Locale spec.** String needs the companion design doc before it
  can fully ship.
- **Concurrency-annotation design.** The field-level `@ReadWriteLock`
  idea raised during the StringBuilder discussion is a general
  concurrency-model concern rather than a String-specific one. It is
  deferred.
