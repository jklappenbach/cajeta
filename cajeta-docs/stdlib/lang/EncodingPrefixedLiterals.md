# Encoding-prefixed byte-array literals

Status: **designed, not implemented.** Tracked as task #164.

A compile-time syntax that materializes a string literal as a
typed byte array in a named encoding — no `String` allocation, no
decode/re-encode round trip. The intent is ergonomic interop with
code that wants raw bytes: file I/O, sockets, hashing, wire
protocols, embedded magic numbers.

```cajeta
uint8[]  hello  = utf8 "Hello";      // 5 bytes: 0x48 0x65 0x6C 0x6C 0x6F
int8[]   utf16  = utf16le "Hi";      // 4 bytes: 0x48 0x00 0x49 0x00
uint32[] cps    = utf32 "café";      // 4 codepoints: 0x63 0x61 0x66 0xE9
int8[]   ascii  = ascii "abc";       // 3 bytes; compile error if any byte > 0x7F
int8[]   latin1 = latin1 "ä";        // 1 byte: 0xE4
```

The literal is a **static constant** — same lowering shape as the
post-Phase 2b-β class String literal: a private `{ i64 count, [N x T]
data }` global in `.rodata`. No allocation cost, no drop entry,
identical contents may dedupe in the LLVM pass pipeline.

## Why a keyword (vs. a function call)

A function form like `utf8("Hello")` keeps the grammar untouched but
loses the compile-time guarantee that the argument is a literal —
callers could pass a runtime-computed `String`, dragging in the full
encode/decode path. The keyword form is unambiguous in expression
position and pins the encoding at parse time, so the compiler can
fold the bytes into the binary without invoking any encoder code.

Trade-off: the keyword adds reserved words (`utf8`, `utf16le`,
`utf16be`, `utf32`, `ascii`, `latin1`). Cajeta's lexer already
reserves a generous keyword set, and these names are unlikely to
collide with user identifiers in practice.

## Encodings to ship in v1

| Keyword   | Element  | Notes                                                |
|-----------|----------|------------------------------------------------------|
| `utf8`    | `uint8`  | Default; identical bytes to `String`'s storage       |
| `utf16le` | `uint8`  | UTF-16 little-endian byte stream                     |
| `utf16be` | `uint8`  | UTF-16 big-endian byte stream                        |
| `utf32`   | `uint32` | One codepoint per element; host-endian              |
| `ascii`   | `uint8`  | Compile error on any byte > 0x7F in the literal     |
| `latin1`  | `uint8`  | ISO-8859-1; compile error on codepoints > 0xFF      |

Single-byte legacy encodings (cp1252, latin-* siblings) are deferred;
add them lazily when a concrete user shows up.

## Open design questions (deferred until implementation begins)

1. **`uint8[]` vs `int8[]`**: leaning `uint8[]` (no sign-extension
   surprises when hashing or compared bytewise), but `int8[]` matches
   the existing `cajeta.lang.String` storage. Likely allow either via
   implicit conversion at the boundary.
2. **Null-terminate?** No — byte-array literals carry their length
   in the CajetaArray header. Callers needing a C-string call
   `String.fromBytes(...)` or append a terminator explicitly.
3. **Encoding-error policy** (e.g. `ascii "café"`, `latin1 "你好"`):
   compile error by default. A `_lossy` variant (`utf8_lossy "..."`,
   `ascii_lossy "..."`) substituting U+FFFD / `?` is a possible
   follow-up.
4. **Mixed-encoding concat** (`utf8 "a" + utf16le "b"`): rejected.
   Concatenation across encodings requires a re-encode and defeats
   the purpose of the literal-time guarantee. Users wanting that go
   through `String` explicitly.
5. **Interaction with `String`**: a plain `"Hello"` is already a
   class String view-mode instance (post Phase 2b-β). The
   encoding-prefixed form is the opt-in byte-array path; the
   conversion from String → bytes is `s.getBytes(Encoding.UTF8)`
   (per `String.md` § getBytes), not implicit.

## Implementation sketch

1. **Lexer**: add the six encoding keywords as ordinary tokens. The
   parser only accepts them in the encoding-prefixed-literal
   alternative on `primary`, so they remain usable as identifiers in
   non-expression positions (field names, etc.) once we decide on
   that point.
2. **Grammar** (`CajetaParser.g4`):
   ```antlr
   primary
       : ...
       | encodingPrefix STRING_LITERAL
       ;
   encodingPrefix
       : 'utf8' | 'utf16le' | 'utf16be' | 'utf32'
       | 'ascii' | 'latin1'
       ;
   ```
3. **AST**: a new `EncodingPrefixedLiteralExpression` AST node
   carrying the encoding keyword + raw literal text. `resolveTypes`
   pins `resolvedType` to the encoding's array type (`uint8[]` /
   `uint32[]`).
4. **Codegen** (`generateCode`): mirror the post-Phase 2b-β class
   String literal codegen — emit a private constant `{ i64 count, [N
   x T] data }` global, return its address. The bytes are
   pre-encoded at compile time via a small per-encoding emit helper
   (UTF-8 is the source-bytes pass-through; UTF-16/UTF-32 walks the
   source decoded codepoints and emits per the encoding rules; ASCII
   / Latin-1 walks codepoints and errors on the first out-of-range
   one).

## Companion design notes

- This feature does NOT supersede `S-105 Encoding enum` — that's the
  runtime path (decoding a `int8[]` of unknown provenance into a
  String, with error-policy options). The literal form is
  compile-time and always succeeds (or fails to compile) on the
  syntactic literal.
- The `String.getBytes(Encoding)` companion (designed in
  `String.md`) returns a freshly allocated `int8[]` whose contents
  match what the corresponding encoding-prefixed literal would have
  produced for the same source codepoints — useful for round-trip
  testing of the encoder.
