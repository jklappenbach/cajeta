# String

`cajeta.lang.String` — immutable UTF-8 text, the single string type for
Cajeta. Strings are read-only: transformations return a fresh `#String` rather
than mutating in place, while query methods (`count`, `size`, `contains`,
`indexOf`, `startsWith`, `codepointAt`, ...) read without allocating.
`substring` and `trim` are zero-copy — the result is a windowed view sharing
the source's root buffer, not a copy. The owned constructor
`String(#int8[] bytes, int32 byteLength)` takes ownership of the caller's
buffer and frees it when the String drops — the builder path every
buffer-producing API uses. Equality is content-based: `==` is a null-safe
byte-exact compare (`operator==` delegates to `equals`), so strings key
cleanly into `HashMap` and `HashSet`.

```cajeta
String s = "  Hello, World  ";
String clean = s.trim();                 // "Hello, World" — zero-copy window
boolean hi = clean.startsWith("Hello");  // true
int64 comma = clean.indexOf(",");        // 5
String loud = clean.toUpperCase();       // "HELLO, WORLD"
int64 cps = clean.count();               // 12 — codepoints, not bytes
int8[] buf = heap int8[2];
buf[0] = (int8) 104;                     // 'h'
buf[1] = (int8) 105;                     // 'i'
String own = heap String(#buf, 2);       // "hi" — takes ownership of buf
```

## Methods

| Signature | |
|---|---|
| `String()` ⚑ | Constructs the empty String (the empty literal `""` produces an equivalent value) |
| `String(#int8[] bytes, int32 byteLength)` ⚑ | Ownership-transfer constructor — takes the caller's byte buffer and owns it (this String's drop frees it) |
| `int64 hash()` | Content-based hash overriding Object's identity hash |
| `int64 count()` | Number of Unicode characters (codepoints); O(N) on first call, then cached |
| `int64 size()` | Byte size of the underlying data buffer |
| `boolean isEmpty()` | True when this String carries zero bytes |
| `boolean equals(String other)` | Byte-for-byte equality |
| `static boolean operator== (String a, String b)` | Null-safe, byte-exact value `==` (delegates to `equals`) |
| `int8 charAt(int32 idx)` | Byte at the given byte index, or 0 if out of range |
| `int64 indexOf(String needle)` | First byte-index where `needle` appears, or -1 |
| `int64 indexOfFrom(String needle, int64 start)` | `indexOf` starting the scan at byte `start` |
| `boolean startsWith(String prefix)` | True iff `prefix`'s bytes appear at the start |
| `boolean endsWith(String suffix)` | True iff `suffix`'s bytes appear at the end |
| `boolean contains(String needle)` | True iff `needle`'s bytes appear anywhere |
| `#String substring(int32 begin, int32 end)` | Half-open byte-indexed window `[begin, end)` — zero-copy, shares the root buffer |
| `#String toUpperCase()` | New String with ASCII lowercase mapped to uppercase |
| `#String toLowerCase()` | New String with ASCII uppercase mapped to lowercase |
| `#String trim()` | Strips leading and trailing ASCII whitespace — zero-copy window |
| `#String replace(String from, String repl)` | New String with every non-overlapping occurrence of `from` replaced by `repl` |
| `int8 byteAt(int32 index)` | Raw byte at the given byte index |
| `#int8[] toBytes()` | The effective byte window as a fresh caller-owned array — the view-safe replacement for reading `.bytes` raw |
| `char codepointAt(int32 cpIdx)` | Unicode codepoint at the given codepoint index |

⚑ = `@EntryPoint`

The compiler rewrites `substring`/`trim` calls to the internal borrow-mode
variants `substringView`/`trimView` when the receiving local provably never
leaves its scope; they are not intended for direct use.

## See also

- Tour: [HashDemo](../../../samples/tour/src/main/cajeta/tour/hash/HashDemo.cajeta)
- Source: [`runtime/src/cajeta/lang/String.cajeta`](../../../runtime/src/cajeta/lang/String.cajeta)
- [StringBuilder](StringBuilder.md) — amortized O(N) accumulation of a String
- [Slice](Slice.md) — the same zero-copy windowing for arrays
