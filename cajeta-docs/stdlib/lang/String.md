# `cajeta.lang.String` — immutable UTF-8 text

Single source of truth for the String surface in Cajeta. Captures
the 2026-05-18 design pass with the user (15 questions + tidy-up).
Implementation pending.

Status: **designed, not implemented.** Tracked as task #157 → next
phase becomes a separate impl task.

---

## Storage model

- **UTF-8 internally, backed by `byte[]`.** Most files, network
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
- **Tagged owned/view mode.** A String carries an internal mode bit:
  - **Owned** — heap-allocated bytes, reclaimed by the drop chain
    on scope exit.
  - **View** — pointer into static storage (string literals) or
    into bytes owned by another holder (rare; only as a future
    optimization for known-safe cases). Drop chain skips view-mode
    Strings.
- **One type, not two.** Owned and view share the same `String`
  surface; the mode is implementation-detail unless you're doing
  `#`-transfer (see [Memory model](#memory-model) below).

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
**a Unicode codepoint**. `'c'` is `char` 99; `'é'` is 233; `'😀'` is
0x1F600 — all 32-bit. Character arithmetic still works for ASCII;
emoji and CJK work natively. The 8-bit byte already has a perfect
name (`int8` / `byte`); there's no good reason for `char` to also
mean "byte" in 2026.

The Java 16-bit `char` is the cause of the entire surrogate-pair
mess. We have the luxury of redefining; no Cajeta user code uses
the old `char` type, so there's no breakage.

---

## Equality — value, always

```cajeta
boolean operator==(Object obj);
```

`"abc" == "abc"` returns `true` regardless of allocation identity.
**Value equality, not pointer equality.** Java's `==`-on-String is
pointer equality (`s1 == s2` is false for two distinct String
objects containing the same characters); you have to remember
`.equals()`. That's the cause of an enormous fraction of real-world
Java bugs. Python / Rust / Go / Swift all use value equality on
strings for `==`; Cajeta joins them.

Pointer equality remains available via `Hash.identity(s1) ==
Hash.identity(s2)` if anyone ever genuinely needs it.

---

## Comparable + ordering

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

## Hashing — polynomial + per-process seed

```cajeta
int64 hash();   // overrides Object.hash()
```

Java-style polynomial mix (`h = 31 * h + c` over the UTF-8 bytes),
folded with `Hash.processSeed()` at the end. The seed mix kills the
offline-precompute attack class — an attacker can't generate
colliding strings locally because they don't know your process's
seed.

This does NOT defend against an online adversary probing latency to
discover collisions in your specific process. For that, opt into
SipHash via `@Hash(SipHash.class)` on the containing value class.
See [Object.md § hash()](./Object.md#hash--pluggable-algorithm) for
the broader pluggable-algorithm model and the security tradeoffs.

---

## Encoding interchange

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
    Encoding sourceEncoding;
    Encoding targetEncoding;
    String   reason;            // "codepoint U+1F600 not representable in ASCII"
}
```

Carries enough to fix the bug: source position, the offending
codepoint, both encodings, and a short human-readable reason.
Python's `UnicodeEncodeError` is the model.

---

## Construction / factories

```cajeta
public class String {
    // UTF-8 fast paths
    public static #String fromUtf8(byte[] bytes, int32 len);
    public static #String fromUtf8Unchecked(byte[] bytes, int32 len);

    // Generic encoding ingestion
    public static #String fromBytes(byte[] bytes, int32 len, Encoding enc);
    public static #String fromBytes(byte[] bytes, int32 len, Encoding enc,
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
public #byte[] getBytes(Encoding enc);
public #byte[] getBytes(Encoding enc, EncodingErrorPolicy policy);
```

Symmetric with `fromBytes`. Default policy `FAIL`. `getBytes(UTF_8)`
is the no-conversion fast path (returns a fresh copy of the
internal buffer, since the caller may mutate the returned `byte[]`).

---

## Iteration

```cajeta
public int32 length();              // codepoints
public int32 byteLength();          // bytes
public boolean isEmpty();
public int8 byteAt(int32 byteIdx);          // O(1)
public char codepointAt(int32 cpIdx);       // O(N) walk

public Stream<char> codepoints();           // O(1) per next()
public Stream<int8> bytes();                // O(1) per next()
```

**Enhanced-for over a String iterates codepoints:**

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
public #String substring(int32 startCp, int32 endCp);
```

**Always copies.** Returns a new owned String holding its own bytes.
No borrowing/view variant — Java 6's shared-char[] substring caused
1 MB files to stay alive because somebody kept a 10-char substring;
copying is the safe default. Cajeta's `#` ownership operator could
make borrow-style work, but we explicitly chose the simple model.

Codepoint-indexed, not byte-indexed. If you need byte-range slicing,
drop to the raw `byte[]` layer.

---

## Standard methods

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

## `operator+` — JEP 280 concat primitive

```cajeta
String result = a + b + c + d;
```

Compiler walks the `+`-chain in the expression, batches all pieces
into a single runtime call (`__cajeta_string_concat(pieces, count)`)
that:

1. Walks the pieces once, computes the exact total byte length
2. Allocates ONE buffer of that size (no intermediate growth)
3. Copies each piece into the buffer
4. Wraps as a heap-owned `#String`

Modeled on Java's JEP 280 (Java 9+). One allocation per chain, not
N-1 intermediates. Faster than StringBuilder for the in-expression
case because there's no buffer resize or defensive copy.

**In a loop, each `+`-chain still allocates one new String per
iteration** — that's the correct semantics for immutable strings.
Use `StringBuilder` for incremental construction inside loops; the
lint rule `string-concat-in-loop` (see
[LintRules.md](../../LintRules.md)) catches the bad shape.

---

## `String.format` / `String.printf`

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
    public StringBuilder append(byte[] bytes, int32 len, Encoding enc);
    public StringBuilder append(byte[] bytes, int32 offset, int32 len, Encoding enc);
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

- **Owned String** — `#String`. Heap-allocated bytes; drop chain
  reclaims on scope exit. Same shape as any other class — `#`
  transfer hands ownership across function / field boundaries.
- **View String** — bytes point into static storage (string literals)
  or borrowed external memory. Drop chain skips view-mode Strings;
  the bytes are owned elsewhere (or never freed in the static case).
- **`#` on view-mode is a no-op.** `#"literal"` doesn't transfer
  anything (there's nothing to transfer). The compiler emits the
  lint warning **`transfer-on-view-string`** (see
  [LintRules.md](../../LintRules.md)) so the meaningless `#` is
  visible. Suppress via `@SuppressLint("transfer-on-view-string")`
  for the rare deliberate case.
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
public #String graphemeSlice(int32 startG, int32 endG);  // substring by graphemes
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
| Encoding storage | UTF-8 backed by `byte[]` |
| Mutability | Immutable; separate StringBuilder |
| `char` type | i32 Unicode codepoint (was i8) |
| Equality | Value (`==` returns true for same content) |
| Hashing | Polynomial + per-process seed |
| Encoding enum | UTF-8/16/32 + ASCII + Latin-1 + Windows-1252 + GB18030 + Shift_JIS + EUC-KR + Big5 |
| Encoding error policy | `{ FAIL, REPAIR }`; FAIL default; no IGNORE |
| Substring | Always copies; codepoint-indexed |
| Iteration | Codepoint via `codepoints()` / for-loop / `char` |
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
