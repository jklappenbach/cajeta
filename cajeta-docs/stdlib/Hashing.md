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
algorithm classes (`XXHash3`, `RapidHash`, `SipHash`, `MD5`,
`DefaultHasher`) and the `Hasher` interface are designed but not yet
implemented. Tracked in Features.md.

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

## `Hasher` interface — designed, not yet implemented

The intended uniform contract for all algorithm classes. Streaming
writes per-type; one finalize call.

```cajeta
public interface Hasher {
    public Hasher writeBoolean(boolean v);
    public Hasher writeInt8(int8 v);
    public Hasher writeInt16(int16 v);
    public Hasher writeInt32(int32 v);
    public Hasher writeInt64(int64 v);
    public Hasher writeUInt8(uint8 v);
    public Hasher writeUInt16(uint16 v);
    public Hasher writeUInt32(uint32 v);
    public Hasher writeUInt64(uint64 v);
    public Hasher writeFloat32(float32 v);
    public Hasher writeFloat64(float64 v);
    public Hasher writeBytes(byte[] data);
    public Hasher writeBytes(byte[] data, int64 offset, int64 length);
    public Hasher writeString(String s);
    public Hasher writeObject(Object obj);
    public int64 finish();
}
```

## Algorithm classes — designed, not yet implemented

### `XXHash3` (default-backing, fast general-purpose)

Default for non-attacker-controlled hashing. Multi-GB/s throughput.

```cajeta
public final class XXHash3 implements Hasher {
    public XXHash3();                              // process-seed
    public XXHash3(int64 seed);                    // explicit seed
    public static int64 hash(byte[] data);
    public static int64 hash(byte[] data, int64 seed);
    public static int64 hash(byte[] data, int64 offset, int64 length, int64 seed = 0);
    public static int64 hashString(String s);
    public static int64 hashString(String s, int64 seed);
    // ... full Hasher surface
    public int64 finish();
}
```

### `RapidHash` (maximum throughput)

~20-30% faster than XXH3 on medium-to-large inputs; smaller code
footprint. Not the default-backing because xxHash3 has the
ecosystem-stability story (LZ4 / zstd / RocksDB / ClickHouse).
Same surface shape as `XXHash3`.

### `SipHash` (DoS-resistant for untrusted input)

When the input is attacker-controlled (HTTP request body keys,
shared-cache lookups keyed on user-supplied strings).
SipHash-2-4 + 128-bit key.

```cajeta
public final class SipHash implements Hasher {
    public SipHash(byte[16] key);
    public static SipHash withRandomKey();
    public static int64 hash(byte[] data, byte[16] key);
    public static int64 hashString(String s, byte[16] key);
    // ... Hasher surface
}
```

### `MD5` (checksum / identifier, **not security**)

For HTTP ETag, S3 `Content-MD5`, asset fingerprinting, cache key
derivation, database row fingerprinting. Cryptographically broken —
do not use for signatures or auth tags.

```cajeta
public final class MD5 implements Hasher {
    public static byte[16] hash(byte[] data);
    public static byte[16] hashString(String s);
    public static String hashHex(byte[] data);       // lowercase 32-char hex
    public static String hashStringHex(String s);
    public MD5 update(byte[] data);
    public byte[16] digest();
    public String digestHex();
    // ... Hasher surface
}
```

### `DefaultHasher`

What the compiler-synthesized `Object.hash()` uses: process-seeded
XXH3 underneath.

```cajeta
public final class DefaultHasher implements Hasher {
    public DefaultHasher();  // pulls process seed automatically
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
| `Hasher` interface + algorithm classes | — | designed |

## Open items

Tracked in Features.md:

- `Hasher` interface
- `XXHash3`, `RapidHash`, `SipHash`, `MD5`, `DefaultHasher` classes
- `Hash.identity` argument widening from `pointer` to `Object` once
  the universal-root Object lands
