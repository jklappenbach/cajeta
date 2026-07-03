# Guid

`cajeta.lang.Guid` — a 128-bit globally-unique identifier (UUID), backed by a
single `uint128`. Cajeta's native 128-bit integers make this a true
value-width identifier — no struct-of-two-longs, no byte array — so
comparison, hashing, and storage are single-word operations. Immutable: the
value is set once at construction. `equals(Guid)` is the exact 128-bit
comparison; `==` and hash-keyed collections route through `hash()`, which
folds 128 bits into 64 and carries the standard ~2⁻⁶⁴ collision caveat.

```cajeta
Guid a = Guid.random();                 // fresh version-4 UUID
Guid b = Guid.parse("01234567-89ab-cdef-fedc-ba9876543210");
Guid c = Guid.fromHalves((uint64) 255L, (uint64) 1L);
boolean same = a.equals(b);             // exact 128-bit compare — false
String text = b.toString();             // canonical lowercase 8-4-4-4-12
```

## Methods

| Signature | |
|---|---|
| `Guid(uint128 value)` | Wrap an existing 128-bit value |
| `static #Guid of(uint128 value)` ⚑ | Box an existing 128-bit value |
| `static #Guid fromHalves(uint64 hi, uint64 lo)` ⚑ | Compose from a high and low 64-bit half |
| `static #Guid random()` ⚑ | A fresh random (version-4) UUID |
| `static #Guid parse(#String s)` ⚑ | Parse a canonical 36-character `8-4-4-4-12` hex GUID; throws `GuidFormatException` on malformed input |
| `uint128 value()` | The raw 128-bit value |
| `uint64 high()` | The most-significant 64 bits |
| `uint64 low()` | The least-significant 64 bits |
| `boolean equals(Guid other)` | Exact 128-bit equality (no hash collisions) |
| `int64 hash()` | Value-based hash over all 128 bits |
| `#String toString()` | Canonical lowercase `8-4-4-4-12` hex form |

⚑ = `@EntryPoint`

## See also

- Tour: [GuidDemo](../../../samples/tour/src/main/cajeta/tour/lang/GuidDemo.cajeta)
- Source: [`runtime/src/cajeta/lang/Guid.cajeta`](../../../runtime/src/cajeta/lang/Guid.cajeta)
- [String](String.md) — `parse` / `toString`'s text form
