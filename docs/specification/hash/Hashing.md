# Hashing — `cajeta.hash`

Non-cryptographic *and* cryptographic digesting. Collision-resistant
fingerprints (`Sha256`, `Blake3`) now ship in this package; only
authenticated/keyed primitives (HMAC, AEAD, signatures) are deferred to
the future `cajeta.crypto` peer library.

This is the cajeta surface for the runtime `__cajeta_hash_*` symbols
plus the algorithm classes that wrap them. Every cajeta value has a
`hash()` either synthesized (`@AutoHash` / default `Object.hash()`)
or hand-overridden; this doc covers the algorithms those hashes
ultimately route through.

**Decision guide — pick the right hash:**

| Need | Use |
|------|-----|
| `Object.hash()` / hash-map key (compiler does this) | `DefaultHasher` (XXH3-64) |
| fast 64-bit fingerprint / cache key (non-crypto) | `XXHash3.hash` |
| **128-bit fingerprint — the MD5 replacement** | **`XXHash3.hash128`** |
| untrusted input (HTTP body, user strings) | `SipHash` |
| cryptographic digest / content-addressing | `Sha256` or `Blake3` |
| fast crypto + extendable output (XOF) | `Blake3` |
| WebSocket handshake (protocol-fixed) | `Sha1` |
| **legacy interop only** (S3 Content-MD5, old ETags) | `MD5` |

> Do **not** pick `MD5` for new code — it's broken *and* the slowest hash
> here. Its 128-bit-fingerprint job is now `XXHash3.hash128` (faster, lower
> collision odds); its crypto job is `Sha256` / `Blake3`.

Status: `Hash` utility namespace + per-primitive intrinsics complete;
`Hasher` interface + `MD5` + `SipHash` + `XXHash3` (incl. **`hash128`**,
XXH3-128) + `DefaultHasher` shipped. Cryptographic digests shipped:
`Sha1` (WebSocket handshake), `Sha256` (SHA-NI accelerated), and `Blake3`
(AVX-512 hash-16, with XOF). `RapidHash` still designed-only.

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

Compiler intrinsics. `value.hash()` on a **primitive** receiver lowers
directly to the matching `__cajeta_hash_*` runtime helper (no boxing, no
method dispatch — handled in `MethodCallExpression.cpp` ~3521, gated on the
receiver's `PRIMITIVE_FLAG`):

| Type | `hash()` lowering |
|------|-------------------|
| `int8` / `int16` / `int32` | sign-extend to i32 → `__cajeta_hash_int32` |
| `uint8` / `uint16` / `uint32` | zero-extend to i32 → `__cajeta_hash_int32` |
| `int64` / `uint64` | `__cajeta_hash_int64` |
| `float32` / `float64` | `__cajeta_hash_float32` / `__cajeta_hash_float64` |
| `boolean` | zero-extend i1→i8 → `__cajeta_hash_boolean` |

Coercion rules match `SynthesizedHashMethod`, so a field hashed via `@AutoHash`
and the same field hashed via `x.hash()` agree.

Non-primitive receivers are **not** part of this intrinsic:

- **Class types** dispatch `obj.hash()` virtually into the synthesized /
  overridden `hash()`.
- **`String`** overrides `hash()` directly. The v1 algorithm is **FNV-1a**
  (64-bit, offset basis `0xCBF29CE484222325`, prime `0x100000001B3`),
  content-sensitive but **not** process-seeded — the seeded XXH3-64 path
  (matching the rest of `cajeta.hash`) is a documented follow-up that still
  needs the `int8[]→uint8_t*` `@Native` bridge; the runtime symbol
  (`__cajeta_hash_bytes`, XXH3-64) already exists. See `String.cajeta` § `hash`.
- **Byte buffers** (`int8[]`) hash through `__cajeta_hash_bytes` (XXH3-64) at
  the call sites that need it (e.g. inside `XXHash3.hash`); there is no
  bare `someInt8Array.hash()` array intrinsic.

Pinned by `test/expression/HashTests.cpp`, `test/parser/AutoHashTests.cpp`,
and `test/parser/StringHashTests.cpp`.

## `@AutoHash` — shipped

Class annotation. The compiler synthesizes a structural `hash()`
that walks every non-static field, calls `field.hash()` on each
(using the per-primitive intrinsic above for primitives, the class's
own `hash()` for class types), and threads the results through a
seed-mixed combiner. The `@Data` and `@Value` bundles imply it, so any
of `@AutoHash` / `@Data` / `@Value` triggers synthesis. A hand-written
no-arg `hash()` always wins (the synthesizer skips). Driven by
`CajetaClass::synthesizeAutoHash` (`CajetaClass.cpp:1204`); the body is
emitted by `SynthesizedHashMethod`.

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
    // --- 128-bit (XXH3-128): the MD5 replacement for fingerprints ---
    public static #int8[] hash128(int8[] data, int64 len);                 // 16-byte digest (low64 LE, high64 LE)
    public static void hash128Into(int8[] data, int64 len, int64 seed, int8[] out16); // zero-alloc
    public static int64 hash128Low(int8[] data, int64 len);
    public static int64 hash128High(int8[] data, int64 len);
    public static #String hash128Hex(int8[] data, int64 len);              // 32-char canonical hex
    public static #String hash128HexSeeded(int8[] data, int64 len, int64 seedArg);
    public void update(int8[] data, int64 len);
    public void reset();
    // ... full Hasher surface
    public int64 finish();
}
```

The bulk path (inputs > 240 B) is a pure-Cajeta AVX-512 `Vector<int64,8>`
long path that's bit-identical to the native XXH3 reference and beats every
competitor's library on this metric (the crown). `hash128`'s low64 equals the
64-bit digest on that path (XXH3 derives both from the same accumulators).

### `Sha256` (cryptographic checksum / content-addressing) — shipped

SHA-256 (FIPS 180-4). The native bridge uses x86 **SHA-NI** behind a CPUID
probe (scalar fallback), making it the fastest SHA-256 in the comparison suite.
Use for content-addressing and interop; not a keyed/authenticated primitive.

```cajeta
public final class Sha256 implements Hasher {
    public static #int8[] hash(int8[] data, int64 len);          // 32-byte digest
    public static #int8[] hashString(String s);
    public static #String hashHex(int8[] data, int64 len);       // lowercase 64-char hex
    public static #String hashStringHex(String s);
    public void update(int8[] data, int64 len);
    public #int8[] digest();
    public void reset();
    // ... full Hasher surface
    public int64 finish();    // first 8 bytes of digest, little-endian
}
```

### `Blake3` (fast cryptographic hash + XOF) — shipped

BLAKE3 — the modern fast cryptographic fingerprint and the recommended
content-addressing hash for new code. 256-bit default digest plus an
**extendable output (XOF)** of any length (the first 32 bytes equal the
standard digest). The bulk path is an AVX-512 `hash16` (16 chunks in
parallel, CPUID-gated, scalar fallback).

```cajeta
public final class Blake3 implements Hasher {
    public static #int8[] hash(int8[] data, int64 len);          // 32-byte digest
    public static void hashInto(int8[] data, int64 len, int8[] out32); // zero-alloc
    public static #String hashHex(int8[] data, int64 len);       // lowercase 64-char hex
    public static #int8[] hashString(String s);
    public static #String hashStringHex(String s);
    public static void hashXof(int8[] data, int64 len, int8[] out); // fill out.count() bytes (XOF)
    public void update(int8[] data, int64 len);
    public #int8[] digest();
    public void reset();
    // ... full Hasher surface
    public int64 finish();    // first 8 bytes of digest, little-endian
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

### `MD5` (legacy interop only — **do not use for new code**) — shipped

Kept only for protocols that fix MD5 (S3 `Content-MD5`, existing ETags). It's
broken *and* the slowest hash here. New code: `XXHash3.hash128` (non-crypto
128-bit) or `Sha256` / `Blake3` (crypto).

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
| 128-bit fingerprint (cache keys, ETags, content) | `XXHash3.hash128` |
| Untrusted input (HTTP body, user strings) | `SipHash.hashKeyed` |
| Cryptographic digest / content-addressing | `Sha256.hashHex` / `Blake3.hashHex` |
| Fast crypto hash + extendable output (XOF) | `Blake3.hashXof` |
| WebSocket handshake digest (protocol-fixed) | `Sha1` |
| **Legacy interop only** (S3 Content-MD5, old ETags) | `MD5.hashHex` |
| Identity-based (graph nodes, weak refs) | `Hash.identity` |
| Authenticated/keyed crypto (HMAC, AEAD, signatures) | **`cajeta.crypto`** (future library) |

Worked end-to-end in the tour: `samples/tour/.../tour/hash/HashDemo.cajeta`.

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
| `XXHash3.hash128` (XXH3-128) | `test/parser/XXHash3_128Tests.cpp` | shipped |
| `String.hash()` (FNV-1a) | `test/parser/StringHashTests.cpp` | shipped |
| `SHA-1` | `test/parser/Sha1Tests.cpp` | shipped |
| `SHA-256` | `test/parser/Sha256Tests.cpp` | shipped |
| `BLAKE3` (+ XOF) | `test/parser/Blake3Tests.cpp` | shipped |
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
