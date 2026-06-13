# `cajeta.lang.String` — immutable UTF-8 text

Single source of truth for the String surface in Cajeta. Captures
the 2026-05-18 design pass with the user (15 questions + tidy-up),
annotated against what has since shipped.

Status: **partially implemented.** A core surface is live in
[`runtime/src/cajeta/lang/String.cajeta`](../../../runtime/src/cajeta/lang/String.cajeta)
and pinned by `test/expression/StringMethodsTests.cpp`; the larger
design surface below (factories, `format`/`printf`, `StringBuilder`,
encoding interchange, locale-aware case folding, graphemes) is still
**planned**. Each subsection notes its status.

> **What ships today** (verified against the source + tests):
> - Storage layout `{ int8[] bytes; int32 byteLength; int32 mode;
>   int32 cachedCpLength; }`; constructors `String()` and the
>   view-mode `String(#int8[] bytes, int32 byteLength)`.
> - `count()` (codepoints, cached), `size()` (bytes), `isEmpty()`,
>   `hash()` (FNV-1a), `equals(String)`.
> - `charAt(int32)`→`int8` (byte-indexed), `byteAt(int32)`,
>   `codepointAt(int32)`→`char` (O(N) walk).
> - `indexOf(String)`, `startsWith`, `endsWith`, `contains` (all
>   byte-based).
> - `substring(int32 begin, int32 end)` — **byte-indexed, copying**
>   (not a view, not codepoint-indexed — see § Substring).
> - `toUpperCase()`, `toLowerCase()`, `trim()`, `replace(String,
>   String)` — ASCII-only, no `Locale` argument.
> - `+` concatenation (lowered to `__cajeta_str_concat`).
> - String literals materialize as **view-mode** (`mode = 1`) class
>   `String` instances over `.rodata`.
>
> Names/semantics in the design below that DIFFER from what shipped
> are flagged inline. Notably the shipped sizing methods are
> `count()` / `size()`, not `length()` / `byteLength()`; and `String`
> carries value-equality via its `hash()` override, **not** by
> overriding `operator==`.

---

## Storage model

- **UTF-8 internally, backed by `int8[]`.** Most files, network
  protocols, JSON, source code, and config formats already speak
  UTF-8 — zero-copy at most I/O boundaries; ASCII fast path is
  bytewise. The "O(N) to index by character" cost is real but doesn't
  matter for almost any task users actually do (iterate, compare,
  decode, write); when it does matter (cursor placement, line
  breaks), graphemes are the right unit anyway and UTF-16 code units
  don't help.
- **Immutable.** Once constructed, a String's bytes never change.
  `StringBuilder` (see below) is the companion for incremental
  construction.
- **Strings are never dropped.** Cajeta-allocated String instances
  (and their UTF-8 payloads) persist for the process's lifetime once
  created. The drop chain does NOT register String allocations; no
  per-string reclamation runs at scope exit. This makes view-mode
  Strings (substring slices, literal views, JSON token spans)
  unconditionally safe — the parent's bytes are guaranteed live for
  as long as any code can reach the view.
- **Tagged owned/view mode.** A String carries an internal mode bit
  that records the bytes' origin, not its drop-eligibility:
  - **Owned** (`mode = 0`) — bytes were heap-allocated for this
    String. The String holds the canonical reference to them.
  - **View** (`mode = 1`) — bytes are borrowed from somewhere else:
    static storage (string literals), another String's payload
    (substring slices), or a buffer the codec layer owns (JSON
    token spans). Neither mode triggers a drop.
- **One type, not two.** Owned and view share the same `String`
  surface; the mode is implementation-detail. `#`-transfer of a
  String moves the reference but doesn't change the
  never-drop policy.

```cajeta
class String {
    int8[] bytes;          // UTF-8 payload
    int32  byteLength;
    int32  mode;           // 0 = owned, 1 = view
    int32  cachedCpLength; // -1 = not yet computed; codepoint count
}
```

---

## `char` is a 32-bit Unicode codepoint

Cajeta redefines `char` (previously i8, the C definition) to mean
**a Unicode codepoint** — backed by `i32`
(`src/cajeta/type/CajetaType.cpp:406`). `'c'` is `char` 99; `'é'` is
233; `'😀'` is 0x1F600 — all 32-bit. Character arithmetic still works
for ASCII; emoji and CJK work natively. The 8-bit byte already has a
perfect name (`int8` — Cajeta has no `byte` type, byte buffers are
`int8[]`); there's no good reason for `char` to also mean "byte" in
2026.

The Java 16-bit `char` is the cause of the entire surrogate-pair
mess. We have the luxury of redefining; no Cajeta user code uses
the old `char` type, so there's no breakage.

---

## Equality — value, always

`"abc" == "abc"` returns `true` regardless of allocation identity.
**Value equality, not pointer equality.** Java's `==`-on-String is
pointer equality (`s1 == s2` is false for two distinct String
objects containing the same characters); you have to remember
`.equals()`. That's the cause of an enormous fraction of real-world
Java bugs. Python / Rust / Go / Swift all use value equality on
strings for `==`; Cajeta joins them.

**How it works (shipped).** `String` does **not** declare its own
`operator==`. It overrides only `hash()` (FNV-1a over the UTF-8
bytes), and `Object`'s static `operator==` defaults to
`a.hash() == b.hash()` — so two Strings with identical bytes compare
equal through that path. The explicit byte-for-byte check is
`equals(String other)`; prefer it when hash-collision-exact
semantics matter (`==` compares 64-bit hashes, ~2⁻⁶⁴ collision
probability). See [Object.md](./Object.md) § *operator== — equality flows through hash()*.

```cajeta
public int64   hash();             // FNV-1a; overrides Object.hash()
public boolean equals(String other); // byte-for-byte
```

Pointer equality remains available via `Hash.identity(s1) ==
Hash.identity(s2)` if anyone ever genuinely needs it.

---

## Comparable + ordering

Status: **planned.** `String.cajeta` does not yet implement
`compareTo` / `compareToIgnoreCase`, does not declare `implements
Comparable<String>`, and `Locale` does not exist in the codebase
(see [Locale.md](./Locale.md)). The design:

```cajeta
public class String implements Comparable<String> {
    public int32 compareTo(String other);
    public int32 compareToIgnoreCase(String other, Locale locale);
}
```

`compareTo` is **lexicographic on the UTF-8 bytes**. UTF-8 byte order
matches codepoint order (by construction — that's a UTF-8 property),
so this is also lexicographic in Unicode codepoint sequence. Standard
behavior for any non-locale-aware sort.

`compareToIgnoreCase(other, Locale)` does case-folded comparison via
the Unicode case-folding tables; `Locale` controls Turkish-specific
`I`/`ı`/`İ` and similar regional case rules.

Implements `Comparable<String>` for use as a TreeSet/TreeMap key and
for sorted-collection compatibility.

---

## Hashing — FNV-1a (shipped)

```cajeta
int64 hash();   // overrides Object.hash()
```

The shipped algorithm is **FNV-1a** (not the Java polynomial mix):
offset basis `0xCBF29CE484222325`, then `h = (h XOR b) *
0x100000001B3` per UTF-8 byte (`String.cajeta`). Deterministic and
content-sensitive, with **no per-process seed mixing yet** — so it
does not currently resist an offline-precompute or online-probe
adversary. Seeded, DoS-resistant String hashing (XXH3-64 +
`Hash.processSeed`) is the planned follow-up; the runtime symbol
`__cajeta_hash_bytes` already exists. See
[Object.md § hash()](./Object.md#hash--pluggable-algorithm) for the
broader pluggable-algorithm model and the security tradeoffs.

---

## Encoding interchange

Status: the **`Encoding` and `EncodingErrorPolicy` enums and the
`EncodingException` class ship**
([`Encoding.cajeta`](../../../runtime/src/cajeta/lang/Encoding.cajeta),
[`EncodingErrorPolicy.cajeta`](../../../runtime/src/cajeta/lang/EncodingErrorPolicy.cajeta),
[`EncodingException.cajeta`](../../../runtime/src/cajeta/lang/EncodingException.cajeta)),
but the conversion methods that consume them (`fromBytes`,
`getBytes`) are **not yet implemented** — see § Construction. The
shipped `Encoding` enum carries 12 members (the design list below
plus `LATIN_1`, and the four CJK encodings), matching the source.

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

`FAIL` is the default everywhere. `REPAIR` is opt-in. **No `IGNORE`
policy** — silent data loss is strictly worse than substitution
because the user has zero hint anything went wrong. Substitution at
least produces detectable output.

This is the stdlib-wide pattern for parsing-policy enums — see
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

(In the shipped source the encoding fields are stored as `int32`
ordinals, not as `Encoding` references; the constructor is
`EncodingException(String message, int64 offset, int32 codepoint,
int32 sourceEncoding, int32 targetEncoding, String reason)`.)

Carries enough to fix the bug: source position, the offending
codepoint, both encodings, and a short human-readable reason.
Python's `UnicodeEncodeError` is the model.

---

## Construction / factories

Status: **planned.** None of the static factories below
(`fromUtf8`, `fromBytes`, `repeat`, `join`, `fromCodepoint(s)`,
`of(...)`) exist in `String.cajeta` today. What ships is two
instance constructors: the no-arg `String()` (empty, owned) and the
view-mode `String(#int8[] bytes, int32 byteLength)` (`mode = 1`,
pinned by `test/parser/StringViewCtorTests.cpp`). String literals
are lowered directly to view-mode instances by the compiler. The
factory surface below is the design target:

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
malformed bytes. `fromUtf8Unchecked` skips validation — for
trusted-source paths where the cost matters.

All factory results are **heap-owned `#String`**, never view-mode.
Computed-at-runtime values can't point at static storage.

### Encoding interchange — out

```cajeta
public #int8[] getBytes(Encoding enc);
public #int8[] getBytes(Encoding enc, EncodingErrorPolicy policy);
```

Symmetric with `fromBytes`. Default policy `FAIL`. `getBytes(UTF_8)`
is the no-conversion fast path (returns a fresh copy of the
internal buffer, since the caller may mutate the returned `int8[]`).

---

## Iteration

Shipped accessors (note the names — the sizing methods are
`count()` / `size()`, **not** `length()` / `byteLength()`):

```cajeta
public int64 count();               // codepoints (cached after first walk)
public int64 size();                // bytes (byteLength)
public boolean isEmpty();
public int8 byteAt(int32 byteIdx);          // O(1), no bounds check
public int8 charAt(int32 byteIdx);          // O(1), byte-indexed, returns int8
public char codepointAt(int32 cpIdx);       // O(N) walk, returns char
```

Planned (not yet implemented — no `Stream` integration on String
yet):

```cajeta
public Stream<char> codepoints();           // O(1) per next()
public Stream<int8> bytes();                // O(1) per next()
```

**Enhanced-for over a String iterates codepoints (planned):**

```cajeta
for (char c : "ciao") {
    println(c);
}
// → 'c', 'i', 'a', 'o'
```

Sugar for `s.codepoints()`. The iteration unit is the Unicode
codepoint — same shape as Rust's `chars()`. Each `c` is a `char`
(= i32 codepoint).

Byte iteration is opt-in via `s.bytes()` — useful for codecs /
scanners that need to walk the raw UTF-8 representation.

---

## Substring + slicing

```cajeta
public #String substring(int32 begin, int32 end);   // shipped
```

**Shipped behavior: byte-indexed and copying.** The live
implementation in `String.cajeta` takes a half-open **byte** range
`[begin, end)` (indices clamped to `[0, byteLength]`; an inverted
range yields empty), allocates a fresh `int8[]`, copies the slice,
and returns a heap-owned `#String`. It is **not** codepoint-indexed
and **not** a zero-copy view — the doc comment in the source notes
that view-mode slicing is a planned follow-up that needs pointer
arithmetic on `int8[]` (`&this.bytes[begin]`), which the language
has no surface syntax for yet. Pinned by `StringMethodsTests`
(`substring(6, 11)` on `"hello world"` → `"world"`).

### Planned: codepoint-indexed view slicing

The design target is a zero-copy, codepoint-indexed slice:

> Return a view-mode instance (`mode = 1`) whose `bytes` field points
> into the parent's UTF-8 payload — O(1) after the codepoint→byte
> walk, no copy, return type plain `String` (no `#`) because no
> ownership crosses.

This depends on the owned/view drop distinction, which is **also not
yet wired**: today's String-producing helpers (`concat`,
`substring`, `toUpperCase`, …) `malloc` and currently leak at scope
exit because the type system collapses owned and borrowed `String`
into one type (see `test/parser/OwnedStringDropTests.cpp` for the
fix outline). The "Strings are never dropped" framing below is a
*design rationale* for why view sharing is safe, not a description of
a committed permanent policy — the source's drop chain is designed
to reclaim owned bytes (`mode == 0`) once the distinction lands.

The trade-off vs Java 6's shared-char[] substring (which was
abandoned in Java 7+): Java's char[] sharing leaked the *entire*
backing array — a 10-char substring kept a 1 MB source file alive
in the heap. Cajeta's planned view substring avoids re-rooting the
whole parent buffer by carrying an explicit `(bytes, byteLength)`
window. Workloads that produce hundreds of MB of unique String
content should materialize through `int8[]` for transient work or
bound their String creation.

---

## Standard methods

Status: a subset ships today (verified in `String.cajeta` +
`StringMethodsTests`): `indexOf(String)` (returns `int64`, byte
index), `startsWith`, `endsWith`, `contains`, `toUpperCase()`,
`toLowerCase()`, `trim()`, `replace(String find, String
replacement)`. The case/trim methods are **ASCII-only** and take
**no `Locale`** (the `Locale` overloads, the no-arg `Locale.ROOT`
defaults, `split`, `lastIndexOf`, `indexOf(char)`,
`indexOf(String, fromCp)`, `replaceFirst`, `replace(char, char)`,
`trimStart`/`trimEnd`, `padStart`/`padEnd`, `equalsIgnoreCase*`,
`isBlank` — everything else below) are **planned**. The full design
set:

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

**No-arg `toLower()` / `toUpper()` default to `Locale.ROOT`** —
locale-independent Unicode behavior, no thread-local foot-gun
(Java's Turkish-locale-on-CI-box bug class).

Skipped: `concat` (use `+`), `chars()` alias for `codepoints()` (one
name per concept), `hashCode()` alias for `hash()` (same).

---

## `operator+` — concatenation (shipped, pairwise)

```cajeta
String result = a + b + c + d;
```

**Shipped today (pairwise).** A `+` on two `String` operands lowers
to a single `__cajeta_str_concat` call: each operand's underlying
C-string is extracted, concatenated into a fresh `malloc`'d buffer,
and re-wrapped in a heap-owned class `String`
(`BinaryOpExpression.cpp`, `BINARY_OP_ADD`). A chain `a + b + c + d`
is therefore evaluated left-associatively as N−1 pairwise concats
(with N−2 intermediates), **not** the batched single-allocation
primitive originally designed.

> **Planned: JEP 280-style single-call concat.** The design target
> is for the compiler to walk the whole `+`-chain, compute the exact
> total byte length once, allocate ONE buffer, and copy each piece
> in — modeled on Java's JEP 280 (Java 9+), one allocation per chain.
> Not yet implemented.

**In a loop, each `+`-chain still allocates** — the right semantics
for immutable strings, but a perf trap. The planned `StringBuilder`
(see below — not yet implemented) is the intended fix; the lint rule
`string-concat-in-loop` (see [LintRules.md](../../LintRules.md))
catches the bad shape.

---

## `String.format` / `String.printf`

Status: **planned.** Neither `String.format` nor `String.printf`
exists in `String.cajeta`. (Runtime-side `{}`-substitution *does*
ship, but only as part of the `System.stdout.println(fmt, ...)`
intrinsic — see [System.md](./System.md), not as a `String` method.)
The design:

Two methods, two notations. Ship both.

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

**No auto-detection.** A literal `%` in a `{}` template (or vice
versa) is a footgun if both notations are accepted by one method.
Two methods, the choice is explicit.

### Compile-time template validation

When the `template` arg is a literal string, the compiler validates
at parse time:

- Python-style: `{}` count matches `args` count; named `{foo}`
  references resolve.
- printf-style: `%s` / `%d` / `%f` / etc. specifier count matches
  `args` count, and types are compatible (`%d` requires an integer
  arg, `%s` requires anything-Object, etc.).

Mismatches surface via the lint rule **`format-template-arg-mismatch`**
(see [LintRules.md](../../LintRules.md) — added in the String spec
pass). At runtime, the format methods raise `IllegalArgumentException`
if a non-literal template doesn't match its args.

---

## `StringBuilder` — full robust API

Status: **planned.** There is no `StringBuilder` class in the
codebase yet. The full design:

Companion mutable builder. Single-threaded; concurrent use is
explicit `Lock` at the transaction boundary (see [Memory model
notes on concurrency](#memory-model) below).

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

**`toString()` does NOT reset the builder.** Same shape as Java —
you can call it multiple times; each call copies the current state
to a fresh `#String`. Use `take()` for the one-shot build-and-emit
case (transfers the buffer to the new String, resets the builder
for reuse).

**Indexing is codepoint-based for user-facing methods** (insert,
delete, replace, indexOf, codepointAt), byte-based for raw-byte
accessors (byteAt, byteLength, capacityBytes). Method names make the
unit unambiguous at every call site.

**`reverse()` is codepoint-reverse, not grapheme-reverse.** `"é"`
(combining-mark sequence) reverses to `"́e"` — visually wrong.
Grapheme-aware reverse needs Unicode tables; see v2 plan below.

---

## Memory model

> **Implementation status.** The owned/view drop distinction this
> section describes is **not yet wired**. Because the type system
> currently collapses owned and borrowed `String` into one type, the
> drop chain can't safely free String locals, so helper-produced
> Strings (`concat`/`substring`/`toUpperCase`/…) presently leak at
> scope exit (`test/parser/OwnedStringDropTests.cpp`). The
> source's drop chain is *designed* to reclaim owned bytes once an
> `OwnedString` marker lands; `String.empty()` (the empty-string
> singleton) and the `transfer-on-view-string` lint are likewise
> planned. Read the rules below as the design target.

- **Strings are never dropped.** This is the global lifetime rule:
  Cajeta-allocated String instances and their UTF-8 payloads persist
  for the process's lifetime. The drop chain does NOT register
  String allocations, and scope exit doesn't reclaim Strings.
- **Owned String** (`mode = 0`) — bytes were heap-allocated for this
  String. The String holds the canonical reference but isn't itself
  drop-eligible — bytes outlive every visible scope.
- **View String** (`mode = 1`) — bytes point into static storage
  (string literals), into another String's payload (substring
  slices), or into a buffer the codec layer owns (JSON token spans).
  View-mode Strings are also never dropped (consistent with the
  global rule); the buffer's parent owns the lifetime.
- **`#`-transfer of a String** moves the reference between holders
  but doesn't change reclamation behavior. The never-drop policy
  applies regardless of how many holders the same String has.
- **`#` on view-mode is a no-op.** `#"literal"` doesn't transfer
  anything (there's nothing to transfer). The compiler emits the
  lint warning **`transfer-on-view-string`** (see
  [LintRules.md](../../LintRules.md)) so the meaningless `#` is
  visible. Suppress via `@SuppressLint("transfer-on-view-string")`
  for the rare deliberate case.
- **Memory-cost trade-off.** Long-running processes that produce
  unbounded distinct String content will grow without bound under
  the never-drop policy. Workloads with that shape (high-churn
  text-rewriting pipelines, log accumulators) should hold transient
  text as `int8[]` and only materialize to `String` at egress, or
  be redesigned to bound their String creation. This is the same
  trade Strings make in Java's intern pool / .NET's interned
  string table; Cajeta extends it to ALL Strings, not just
  literals, to simplify substring / view semantics.
- **Empty-String singleton.** A single shared `""` view-mode String
  sits in static storage; `String.empty()` returns it. No allocation
  per empty literal.
- **Concurrency.** String is immutable so it's freely shareable
  across threads. `StringBuilder` is single-threaded; concurrent
  writers wrap their transaction in a `Lock`. **No `StringBuffer`-
  style synchronized variant** — Java's StringBuffer is widely
  considered a mistake (single-method synchronization is the wrong
  granularity; transaction-level locking is what users actually need).
  The field-level `@ReadWriteLock` annotation idea raised during
  design is deferred to the broader concurrency design pass —
  applies to many things, not just StringBuilder.

---

## Locale dependency

Several methods take a `Locale`: `toLower(Locale)`, `toUpper(Locale)`,
`compareToIgnoreCase(other, Locale)`, `equalsIgnoreCase(other,
Locale)`. The no-arg overloads default to `Locale.ROOT` (Unicode
tables without language-specific overrides).

`Locale` is a separate spec (tracked in
[Features.md](../../../Features.md)) — BCP 47-shaped value class,
no thread-local default. Both `Locale` and String ship in tandem so
the case-folding surface is coherent on day one.

---

## v2 grapheme-cluster roadmap

The unit a human reader thinks of as one character (emoji + skin
tone + ZWJ = 1 grapheme) is the *grapheme cluster*. Codepoint
iteration is the v1 default because graphemes need the Unicode
Grapheme Cluster Break tables (~150 KB compiled data) and aren't
strictly necessary for the most common text operations.

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

- `trimUnicode()` — Unicode whitespace (U+00A0, U+2028, U+2029, etc.)
- `isBlankUnicode()` — Unicode whitespace predicate

The codepoint methods don't go away in v2 — codec / scanner work
genuinely wants codepoint or byte granularity. Graphemes are a
*new layer*, not a replacement.

---

## Parsing convention (cross-cutting)

Cajeta stdlib parsing APIs follow a uniform convention established
in this design pass:

- **`FAIL` as default policy.** Throw on input that doesn't fit the
  expected shape — don't silently coerce.
- **`{ FAIL, REPAIR }` enum shape** per domain (e.g.
  `EncodingErrorPolicy`, `JsonReadPolicy`, `NumberParsePolicy`).
  Each enum is per-domain (the semantics of REPAIR differ — substitute
  for encoding, lenient-syntax for JSON, saturate for numeric — but
  the *shape* is uniform).
- **Per-call parameter, not thread-local default.** No
  action-at-a-distance. Test code can't accidentally widen
  production tolerance.
- **No `IGNORE` policy.** Silent data loss is worse than
  substitution.

Documented here because String is the first place a user encounters
the convention via `EncodingErrorPolicy`.

---

## Cross-references

- [Object.md](./Object.md) — the universal-root spec; covers
  `hash()` pluggable model and `operator==` defaults that String
  overrides.
- [Lang.md](../Lang.md) — broader `cajeta.lang` overview;
  historically the String section lived inline there.
- [LintRules.md](../../LintRules.md) — the three lint rules
  String introduces: `string-concat-in-loop`, `transfer-on-view-
  string`, `format-template-arg-mismatch`.
- [Hashing.md](../Hashing.md) — `cajeta.hash.Hash` namespace and
  the `@Hash` algorithm-class registry.
- [ErrorModel.md](../ErrorModel.md) — `EncodingException` extends
  `RecoverableException`; `RecoverableException` doctrine.
- [`Locale.md`](./Locale.md) (pending) — companion spec.

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

- **Q7 security-level enforcement** — how to mark "this HashMap
  must use a DoS-resistant hash" (annotation, capability, runtime
  check). Deferred to its own session.
- **Locale spec** — needs companion design doc before String can
  fully ship.
- **Concurrency-annotation design** — the field-level
  `@ReadWriteLock` idea raised during StringBuilder discussion is a
  general concurrency-model concern, not String-specific. Deferred.
