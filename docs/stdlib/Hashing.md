# Hashing — `cajeta.hash`

Non-cryptographic and crypto-broken-but-still-useful (MD5) hashing.
Cryptographic primitives live in the (future) `cajeta.crypto` peer
library.

This is the cajeta surface for the runtime `__cajeta_hash_*` symbols
plus the algorithm classes that wrap them. Every cajeta value has a
`hash()` either synthesized (`@AutoHash` / default `Object.hash()`)
or hand-overridden; this doc covers the algorithms those hashes
ultimately route through.

Status: `Hash` utility namespace + per-primitive intrinsics complete;
`Hasher` interface + `MD5` + `SipHash` + `XXHash3` + `DefaultHasher`
shipped (2026-05-20). `RapidHash` still designed-only.

## `Hash` utility namespace — shipped

```cajeta
public final class Hash {
    public static int64 identity(Object obj);   // identity hash
    public static int64 combine(int64 a, int64 b); // hash-combine
    public static int64 processSeed();          // per-process random seed
}
```

Implementation: `runtime/src/cajeta/hash/Hash.cajeta` — `@Native`-
bridged to `__cajeta_hash_identity` / `__cajeta_hash_combine` /
`__cajeta_hash_seed` in `runtime/native/cajeta_runtime.c`.

### Examples

```cajeta
import cajeta.hash.Hash;

// Identity hashing (used by IdentityHashMap / observer registries).
int64 idHash = Hash.identity(someObj);

// Combine two field hashes — used by hand-written `hash()` overrides
// when the @AutoHash default isn't the right shape.
public int64 hash() {
    int64 h = this.a.hash();
    h = Hash.combine(h, this.b.hash());
    return h;
}

// Per-process seed — used to align with the compiler-synthesized
// Object.hash() for external snapshot/replay scenarios.
int64 seed = Hash.processSeed();
```

Pinned by `test/expression/HashTests.cpp`.

## Per-primitive `hash()` — shipped

Compiler intrinsics. `value.hash()` on a primitive returns a seeded
mix:

| Type | `hash()` returns |
|------|------------------|
| `int8` / `int16` / `int32` / `int64` | value mixed with the process seed |
| `uint8` / `uint16` / `uint32` / `uint64` | same as the signed variant |
| `float32` / `float64` | bitcast to integer, canonicalize ±0 → 0, mix |
| `boolean` | 0 or 1 mixed with seed |
| `pointer` | `Hash.identity(ptr)` |
| `String` | XXH3-64 over UTF-8 bytes, process-seeded |
| `byte[N]` | XXH3-64 over the N bytes, process-seeded |
| class types | `obj.hash()` — recurses into synthesized / overridden hash |

Pinned by `test/expression/HashTests.cpp` and `test/parser/AutoHashTests.cpp`.

## `@AutoHash` — shipped

Class annotation. The compiler synthesizes a structural `hash()`
that walks every non-static field, calls `field.hash()` on each
(using the per-primitive intrinsic above for primitives, the class's
own `hash()` for class types), and threads the results through a
seed-mixed combiner.

```cajeta
@AutoHash
public class Position {
    public int32 x;
    public int32 y;
    public int32 z;
}

// Object.hash() now returns a structural hash over (x, y, z) — no
// boilerplate.
Position p = heap Position { x: 1, y: 2, z: 3 };
int64 h = p.hash();
```

Pinned by `test/parser/AutoHashTests.cpp`.

## `Hasher` interface — shipped

The uniform contract every algorithm class implements. Streaming
writes per-type; one terminal `finish()`. v1 ships `void`-returning
write methods (no fluent chaining) — the spec's earlier `Hasher`-
returning shape collided with interface-return covariance edge cases
that aren't worth working around for v1.

```cajeta
public interface Hasher {
    public void writeBoolean(boolean v);
    public void writeInt8(int8 v);
    public void writeInt16(int16 v);
    public void writeInt32(int32 v);
    public void writeInt64(int64 v);
    public void writeUInt8(uint8 v);
    public void writeUInt16(uint16 v);
    public void writeUInt32(uint32 v);
    public void writeUInt64(uint64 v);
    public void writeFloat32(float32 v);
    public void writeFloat64(float64 v);
    public void writeBytes(int8[] data);
    public void writeBytesRange(int8[] data, int64 offset, int64 length);
    public void writeString(String s);
    public void writeObject(Object obj);
    public int64 finish();
}
```

## Algorithm classes

### `XXHash3` (default-backing, fast general-purpose) — shipped

Default for non-attacker-controlled hashing. Multi-GB/s throughput.

```cajeta
public final class XXHash3 implements Hasher {
    public XXHash3();                              // process-seed
    public XXHash3(int64 seedArg);                 // explicit seed
    public static int64 hash(int8[] data, int64 len);          // process-seeded
    public static int64 hashSeeded(int8[] data, int64 len, int64 seedArg);
    public static int64 hashString(String s);
    public static int64 hashStringSeeded(String s, int64 seedArg);
    public void update(int8[] data, int64 len);
    public void reset();
    // ... full Hasher surface
    public int64 finish();
}
```

### `RapidHash` (maximum throughput) — designed, not implemented

~20-30% faster than XXH3 on medium-to-large inputs; smaller code
footprint. Not the default-backing because xxHash3 has the
ecosystem-stability story (LZ4 / zstd / RocksDB / ClickHouse).
Same surface shape as `XXHash3`.

### `SipHash` (DoS-resistant for untrusted input) — shipped

SipHash-2-4 with 128-bit key, supplied as two int64 halves.

```cajeta
public final class SipHash implements Hasher {
    public SipHash(int64 keyLow, int64 keyHigh);
    public static int64 hashKeyed(int8[] data, int64 len, int64 keyLow, int64 keyHigh);
    public static int64 hashStringKeyed(String s, int64 keyLow, int64 keyHigh);
    public void update(int8[] data, int64 len);
    public void reset();
    // ... full Hasher surface
    public int64 finish();
}
```

### `MD5` (checksum / identifier, **not security**) — shipped

```cajeta
public final class MD5 implements Hasher {
    public static #int8[] hash(int8[] data, int64 len);          // 16-byte digest
    public static #int8[] hashString(String s);
    public static #String hashHex(int8[] data, int64 len);       // lowercase 32-char hex
    public static #String hashStringHex(String s);
    public void update(int8[] data, int64 len);
    public #int8[] digest();
    public void reset();
    // ... full Hasher surface
    public int64 finish();    // first 8 bytes of digest, little-endian
}
```

### `DefaultHasher` — shipped

What the compiler-synthesized `Object.hash()` uses: process-seeded
XXH3 underneath. Delegates the Hasher surface to a backing
`XXHash3` instance.

```cajeta
public final class DefaultHasher implements Hasher {
    public DefaultHasher();              // pulls process seed
    public DefaultHasher(int64 seedArg); // explicit seed for snapshot replay
}
```

## When to pick which

| Use case | Pick |
|----------|------|
| Default Object.hash() (compiler does this) | `DefaultHasher` (synthesized) |
| Hash a buffer / file / blob for fingerprinting | `XXHash3.hash` |
| Cache key derivation, internal table keys | `XXHash3.hash` |
| High-volume hashing, no external interop | `RapidHash.hash` |
| Untrusted input (HTTP body, user strings) | `SipHash.hash` |
| HTTP ETag / S3 Content-MD5 / asset fingerprinting | `MD5.hashHex` |
| Identity-based (graph nodes, weak refs) | `Hash.identity` |
| Real cryptographic security | **`cajeta.crypto`** (separate library) |

## Tests

| Feature | Tests | Status |
|---------|-------|--------|
| Per-primitive `hash()` intrinsic | `test/expression/HashTests.cpp` | shipped |
| `Hash.identity` / `Hash.combine` / `Hash.processSeed` | `test/expression/HashTests.cpp` | shipped |
| `@AutoHash` structural synthesis | `test/parser/AutoHashTests.cpp` | shipped |
| `Hasher` interface | `test/parser/HasherInterfaceTests.cpp` | shipped |
| `MD5` (RFC 1321) | `test/parser/MD5Tests.cpp` | shipped |
| `SipHash` (SipHash-2-4) | `test/parser/SipHashTests.cpp` | shipped |
| `XXHash3` + `DefaultHasher` | `test/parser/XXHash3Tests.cpp` | shipped |
| `RapidHash` | — | designed |

## v1 implementation notes

- `Hasher` interface ships `void`-returning writes (no fluent
  chaining). Spec's earlier `Hasher`-returning shape hit
  interface-return covariance edge cases not worth solving for v1.
- @Native bridge gaps at the boundary: `boolean` parameter doesn't
  coerce to C int (worked around with int8 ternary); `int8[]` return
  doesn't register with the per-thread live-set (worked around with
  caller-allocates-then-fill helpers).
- Static method returning a reference-typed value (`pointer`,
  `int8[]`, class, interface) with >1 parameter must be
  `#`-qualified (move-on-return), otherwise the
  multi-param-borrow-return rule rejects the declaration. See
  MemoryModel.md § Function signatures.

## Open items

- `RapidHash` — runtime + cajeta wrapper (low priority — XXHash3
  covers the same use cases with worse code-size but better
  ecosystem interop).
- `Hash.identity` argument widening from `pointer` to `Object` —
  done; widening shipped alongside the Object root work.
