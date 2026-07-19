---
id: lang-string
applies-to: [cajeta/lang/String]
title: String — immutable UTF-8 text (transforms transfer #String, queries don't allocate)
description: How to use cajeta.lang.String — owned vs view storage, codepoint-vs-byte counts, content equality/hash, and which transforms hand back an owned #String.
---

# String — immutable UTF-8 text

`cajeta.lang.String` is the single string type: immutable UTF-8 bytes. You rarely
construct one — string literals produce `String` values, and you receive `String`
from other APIs. It is an everyday value type, not a "start-here" service object.

**Decide fast:**
- Reading/inspecting (length, search, char access) → **query methods, no allocation**, return primitives.
- Producing a changed string (`substring`, `trim`, `toUpperCase`/`toLowerCase`, `replace`) → these **never mutate**; each returns a fresh owned **`#String`** you must bind.
- Comparing by content → use `equals(String)`; hash-keyed collections work via `hash()` (see Ownership/Equality below).

## Construction & ownership

Two real constructors:

- `String()` — empty string (`bytes` null, `byteLength` 0). Equivalent to the `""` literal.
- `String(#int8[] bytes, int32 byteLength)` — **ownership transfer**: takes the
  caller's freshly built HEAP buffer (`#` transfers the array) and OWNS it — the
  String's drop frees `bytes`. This is the builder path (`return heap
  String(#out, n)`); never pass a static or borrowed buffer. Literals do NOT
  route here (literal codegen materializes its own mode-1 view instances).

For owned data you normally get a `String` from a transform method (below) or a literal,
not by hand. There is **no** `String(int8[])` copying constructor and **no**
`fromBytes`/`getBytes` yet — encoding-boundary ingestion is deferred.

## Methods that matter

Queries (no allocation):
- `int64 count()` — number of **codepoints** (UTF-8 aware). O(N) first call, then cached. NOT the byte length.
- `int64 size()` — number of **bytes** (`byteLength`). For ASCII `count() == size()`; for multibyte they diverge. There is intentionally no `length()`.
- `boolean isEmpty()` — `byteLength == 0`.
- `boolean equals(String other)` — byte-for-byte; `false` if `other` is null.
- `int64 indexOf(String needle)` — first **byte** index, or **`-1`** if absent; `0` for empty needle; `-1` for null needle.
- `boolean contains(String)`, `startsWith(String)`, `endsWith(String)` — null arg → `false` (empty arg → `true`).
- `int8 charAt(int32 idx)` / `int8 byteAt(int32 index)` — raw **byte** access. `charAt` returns `0` when out of range; `byteAt` does NOT bounds-check (matches `int8[]` with `--bounds=off`).
- `char codepointAt(int32 cpIdx)` — decoded Unicode scalar at a **codepoint** index; O(N) walk (don't loop it — single-pass instead); out-of-range returns `(char) 0`.

Transforms (return owned `#String`, allocate a fresh buffer):
- `#String substring(int32 begin, int32 end)` — half-open `[begin, end)`, **byte**-indexed, indices clamped to `[0, byteLength]`.
- `#String trim()` — strips leading/trailing ASCII whitespace (bytes `0x00`–`0x20`), Java `trim()` semantics.
- `#String toUpperCase()` / `#String toLowerCase()` — **ASCII-only** case mapping; non-ASCII bytes pass through unchanged.
- `#String replace(String from, String repl)` — replaces non-overlapping `from` with `repl`; returns `this` unchanged if `from` is null/empty or `repl` is null.

## Ownership / lifecycle / equality

- Transform results are **owned** (`#String`): bind to a local to take ownership. An owned `String` drops at scope end; in **owned** mode the drop chain frees `bytes`, in **view** mode it does not.
- Equality is **content-based**. `String` does NOT override `operator==`; the value-equality semantics ride on the `hash()` override (FNV-1a over the bytes), which is enough for `HashMap`/`HashSet` keys. For an explicit byte-for-byte check call `equals(String)` directly.
- Immutable: no method mutates the receiver, so a `String` is freely shareable/reusable.

## Sharp edges (what it does NOT do)

- `count()` is codepoints; `size()`/indices/`indexOf`/`substring`/`charAt` are **bytes**. Mixing them on multibyte text corrupts offsets.
- Case folding is ASCII-only; `trim()` is ASCII whitespace only — no Unicode/locale folding yet.
- No bounds-check exceptions: out-of-range access returns `0`/`-1` (or is UB via `byteAt`), it does not throw.
- Malformed UTF-8 is best-effort (stray continuation bytes skipped), not rejected.
- `count()`/`indexOf` are O(N)/O(N×M) naive walks — fine for v1, not tuned for hotspots.

## Example

```cajeta
import cajeta.lang.String;

String s = "  Hello, Cajeta!  ";            // a literal is a static view
#String trimmed = s.trim();                 // owned: "Hello, Cajeta!"
if (trimmed.contains("Cajeta")) {
    #String sub  = trimmed.substring(7, 13);     // "Cajeta" (byte-indexed)
    #String loud = sub.toUpperCase();            // "CAJETA" (ASCII only)
    int64 cps    = trimmed.count();              // codepoints, not bytes
    int64 at     = trimmed.indexOf("Cajeta");    // 7, or -1 if absent
    #String swapped = trimmed.replace("Cajeta", "World");
}
```
