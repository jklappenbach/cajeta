# StringBuilder

`cajeta.lang.StringBuilder` — growable UTF-8 byte buffer for building a
`String` in amortized O(N). Repeated `s = s + chunk` on an immutable `String`
is O(N²) (every `+` copies the whole accumulator); accumulate into a
`StringBuilder` instead and materialize once with `toString()`. The first 64
bytes live in an inline buffer embedded in the object, so a
`stack StringBuilder()` build of ≤ 64 bytes does zero heap allocation during
the build; a build that exceeds 64 bytes spills once to a doubling heap buffer
and continues with byte-identical output.

```cajeta
StringBuilder sb = stack StringBuilder();
sb.append("Hello, ");
sb.append("world");
String s #= sb.toString();   // "Hello, world"
```

## Methods

| Signature | |
|---|---|
| `StringBuilder()` ⚑ | Create an empty builder; no heap allocation until the build exceeds 64 bytes |
| `int32 count()` | Number of UTF-8 bytes accumulated so far |
| `boolean isEmpty()` | `true` when nothing has been appended yet |
| `void append(String s)` | Append the UTF-8 bytes of `s` |
| `void appendBytes(int8[] src, int32 off, int32 len)` | Append `len` bytes from `src` starting at `off` |
| `#String toString()` | Materialize the accumulated bytes into a fresh owned `String` (one final copy of exactly `count()` bytes) |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/lang/StringBuilder.cajeta`](../../../runtime/src/cajeta/lang/StringBuilder.cajeta)
- [String](String.md) — the immutable result type
