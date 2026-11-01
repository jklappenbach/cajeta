# Tour: data-parallel work with `cajeta.simd`

This tour walks from processing bytes one at a time to classifying 16 bytes
per instruction. That leap is what lets a JSON scanner beat Jackson. It
assumes you have met `cajeta.io.Buffer`. The spec is
`docs/specification/math/Simd.md`.

The running example is the heart of a fast JSON scanner. It finds every quote
and structural character in a block, with one operation per block instead of
one per byte.

## 0. The pain we're removing

```cajeta
// Scalar: one byte, one branch, every iteration.
while (p < n) {
    int8 c = buf.byteAt(p);
    if (c == (int8) 34) { /* quote */ }
    p = p + 1;
}
```

Even with SWAR (8 bytes/step) you are doing per-byte logic. SIMD does the
comparison itself 16 lanes wide. One instruction compares 16 bytes to `"` and
hands you a 16-bit mask of where they matched.

## 1. A vector is 16 lanes you operate on at once

```cajeta
// Vector<T,N> is a built-in value type — no import. The same type the GPU
// kernels use; here on the CPU it's a SIMD register.

i8x16 v = i8x16.load(buf, p);          // 16 bytes from the buffer at offset p
i8x16 ones = i8x16.splat((int8) 1);    // 16 copies of 1
i8x16 inc = v.add(ones);               // every lane + 1, in one instruction
```

A `Vector<int8, 16>` (`i8x16`) is a value. It lives in a SIMD register, copies
freely, and has no ownership and no drop. `load` is unaligned, and `splat`
broadcasts a scalar to every lane.

## 2. Compare → mask: the workhorse

This is the operation that makes JSON fast. You compare every lane to a byte
and get back a bitmask, with bit `i` set iff lane `i` matched.

```cajeta
i8x16 block = i8x16.load(buf, p);
int32 quotes = block.eqMask((int8) 34);   // '"'  — bit i set where byte i is a quote
int32 slashes = block.eqMask((int8) 92);  // '\'

// Find the first quote in the block, branch-free:
if (quotes != 0) {
    int64 at = p + (int64) Cajeta.ctz64((int64) quotes);   // first matching lane
}
```

`eqMask` is one `pcmpeqb` + `pmovmskb` on x86, and one `cmeq` + reduce on ARM.
You went from 16 compares-and-branches to two instructions.

## 3. Classify many characters at once — `tableLookup`

JSON has six structural characters (`{ } [ ] : ,`). Instead of six masks, use
a 16-entry classifier table and one `tableLookup`, the `pshufb` primitive. The
low nibble of each byte indexes the table, so you tag whole classes of bytes
in one op.

```cajeta
// table[n] is nonzero for nibbles that can begin a structural char; the result
// is a per-lane "maybe structural" tag you refine with one compare.
i8x16 table = i8x16.load(classTable, 0);
i8x16 tags = block.tableLookup(block.and(i8x16.splat((int8) 0x0F)).asIndices());
int32 structural = tags.eqMask(...);   // structural-char lanes
```

The full classifier, the simdjson recipe, is in the JSON scanner. This is the
engine under it.

## 4. Cross-lane logic stays on integers

The clever part of a JSON scanner is deciding which bytes are inside strings,
so a `,` in `"a,b"` is not a separator. That is a running parity over the
quote mask. You do it on the integer masks rather than the vectors, because
integers have `ctz`, shifts, and `clmul`:

```cajeta
int64 q = ...;                    // quote bits for this block
int64 inString = Simd.clmul(q, -1);   // prefix-XOR: 1 where we're inside a string
int64 realStructural = structuralBits & (~inString);
// then ctz-iterate realStructural to emit structural indices — no per-byte work
```

`Simd.clmul` (carryless multiply) computes the whole prefix-XOR in one
instruction. That is what lets simdjson mask strings branch-free.

## 5. Why this differentiates Cajeta

Stages 2–4 of this, which are escape handling, the index stream, and stage 2
walking it, are in the JSON scanner (`docs/specification/codec/json/Json.md`).
The tour is about the shape, which is per-block rather than per-byte. A
handful of SIMD ops classify 16 (or 32) bytes, where the scalar reader did a
branch per byte. That ratio is the difference between "0.5× Jackson" and
"faster than Jackson". First-class native SIMD is a capability the JVM cannot
match, and it is a concrete edge over Java and other competitors.

## 6. What you learned

| You have | Use |
|---|---|
| A block of bytes to test against one char | `load` + `eqMask` (§2) |
| Many character classes | a class table + `tableLookup` (§3) |
| A running cross-lane decision (in-string) | extract masks, `clmul`/`ctz` on ints (§4) |
| Lane-wise math (add/min/and/…) | the `Vector<T,N>` ops (§1) |

SIMD is the same `Buffer` bytes, classified 16–32 at a time. It is how a
Cajeta JSON parser outruns Jackson, on a capability the JVM cannot offer.
