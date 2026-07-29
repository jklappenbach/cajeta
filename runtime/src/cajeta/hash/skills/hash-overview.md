---
id: hash-overview
applies-to: [cajeta.hash]
title: cajeta.hash orientation — pick an algorithm, pick one-shot vs streaming
description: Routing and library-wide rules for cajeta.hash — choose XXH3/SipHash/MD5/Sha1/Sha256, the Hash facade vs a streaming Hasher, ownership and the sharp edges.
---

# cajeta.hash

Non-cryptographic hashing for hash tables, identity, and fingerprints, plus
SHA-256 for real integrity. Two shapes: the `Hash` static facade (identity,
combine, the process seed) and a family of algorithm classes that all implement
the streaming `Hasher` interface and also expose one-shot statics.

If you just need `Object.hash()` / `@AutoHash` / `String.hash()` behavior, that
already routes through `DefaultHasher` (process-seeded XXH3) — you rarely call
this library directly. Reach in when you need a *specific* algorithm, a keyed
hash, a checksum/digest, or to compose extra context into a structural hash.

## Task → entry point

| I want to… | Use |
|---|---|
| Identity hash of a heap object (for `IdentityHashMap`, weak-ref/observer tables) | `Hash.identity(obj)` |
| Combine two field hashes in a hand-written `hash()` override | `Hash.combine(a, b)` (order-sensitive) |
| Read the per-process seed (snapshot replay, align with `Object.hash()`) | `Hash.processSeed()` |
| Fast general-purpose hash, non-attacker input | `XXHash3` (default; ecosystem-compatible with LZ4/zstd/RocksDB) |
| Hash **attacker-controlled** keys feeding a map/cache (DoS resistance) | `SipHash` (keyed, ~10× slower than XXH3) |
| Compose a structural object hash with extra context, in the compiler's family | `DefaultHasher` (XXH3 + process seed) |
| ETag / Content-MD5 / asset or row fingerprint (NOT security) | `MD5` |
| WebSocket `Sec-WebSocket-Accept` handshake (the ONLY sanctioned SHA-1 use; lives in external `dev.cajeta.http`) | `Sha1` |
| Real integrity: TLS cert fingerprint, download/release checksum | `Sha256` (the one cryptographic hash here) |

Negative rows — not provided here, don't hunt for it:

- **No HMAC / MAC, signatures, KDF, AEAD, password hashing.** SHA-256 is a bare
  digest, not a keyed MAC. Those primitives are out of scope for v1.
- **MD5 and SHA-1 are not security.** Both are broken; never use to defend
  against an adversary forging a match. Use `Sha256` for integrity.
- **No SHA-3 / BLAKE / SHA-512.** Only the algorithm classes listed above ship.
- **No fluent chaining** on `Hasher.write*` — they return `void` in v1; chain as
  separate statements.

## Cross-cutting invariants

- **Native state, freed on scope-drop.** Every streaming class (`XXHash3`,
  `SipHash`, `MD5`, `Sha1`, `Sha256`) holds an opaque `pointer state` to a native
  allocation and frees it in its destructor (`~XXHash3()` etc.). Construct with
  `heap`, let it drop out of scope — do **not** manually free; there is no
  `close()`. The `Hash` facade and the one-shot statics hold no state.
- **Returned digests transfer ownership (`#`).** `MD5.hash`, `Sha1.hash`,
  `Sha256.hash`, their `digest()`, and the hex variants return `#int8[]` /
  `#String` — owned by the caller. The `String`-from-hex constructors take the
  buffer by `#`. The `int64` from `finish()` / one-shot `int64` hashes is a
  plain value, no ownership.
- **Width-named writes are part of the contract.** `writeInt16` vs `writeInt32`
  produce different digests; pass the exact width, never a silently widened
  value, or you change the byte sequence the algorithm sees.
- **`finish()` is terminal-ish.** It returns the 64-bit digest of bytes written
  so far; behavior after the first `finish()` is algorithm-defined. For MD5/SHA-1/
  SHA-256, `finish()`/`digest()`/`hex()` **consume** the state — call `digest()`/
  `hex()` for the full digest *before* `finish()`, and `reset()` to reuse. (XXH3
  leaves state intact; still use `reset()` to start a fresh digest.)
- **`writeBytesRange(data, offset, length)` ignores `offset` in v1** — hashing
  starts at index 0 across every implementation. Slice yourself if you need a
  true sub-range.
- **`processSeed()` aborts on entropy failure.** The seed is drawn from the OS
  CSPRNG (`getrandom`/`getentropy`/`BCryptGenRandom`) at process start; on
  failure the runtime aborts with `CAJETA_ERROR_ENTROPY_UNAVAILABLE` — there is
  no weak fallback. It is stable per process, never zero, and is mixed into every
  hash the library produces (kills hash-flooding for non-keyed paths).
- **Errors:** these APIs don't throw for normal use; the only hard failure is the
  entropy abort above. `Hash.identity(null)` is valid (hashes the zero pointer).

## Canonical example

```cajeta
import cajeta.hash.XXHash3;
import cajeta.hash.Hasher;

// Streaming: swap the algorithm by swapping the constructor; the loop is identical.
Hasher h = heap XXHash3();          // process-seeded; native state freed on scope-drop
h.writeInt32(record.id);
h.writeString(record.title);
int64 digest = h.finish();          // plain value
```

One-shot, when you already have the bytes:

```cajeta
import cajeta.hash.XXHash3;
int64 d = XXHash3.hash(bytes, bytes.count());   // process-seeded one-shot
```

Integrity (owned digest crosses the boundary):

```cajeta
import cajeta.hash.Sha256;
Sha256 sum = heap Sha256();
sum.update(chunk1, chunk1.count());
sum.update(chunk2, chunk2.count());
String hex = sum.hex();             // #String, owned; terminal — state consumed
// string-compare hex against the published checksum
```

## Disambiguation

- **One-shot static vs streaming class** — use the `*.hash` / `*.hashKeyed` /
  `*.hashHex` statics when the input is one buffer/String; use a `heap` instance +
  `write*`/`update` + `finish`/`digest` when input arrives in pieces (e.g. a
  streaming download) or mixes typed fields.
- **`XXHash3` vs `DefaultHasher`** — `DefaultHasher` *is* process-seeded XXH3 but
  is the type the compiler couples to for `Object.hash()`/`@AutoHash`; use it when
  you want digests in the same family as synthesized hashes. Use `XXHash3`
  directly (esp. the explicit-seed constructor / `hashSeeded`) for reproducible,
  cross-process digests.
- **`XXHash3` vs `SipHash`** — XXH3 for trusted input (faster); SipHash when an
  attacker controls the keys and could otherwise flood your map with collisions.
- **`finish()` vs `digest()`/`hex()`** (MD5/SHA-1/SHA-256) — `finish()` projects
  to a 64-bit `int64` for hash-table use only; `digest()`/`hex()` give the full
  128/160/256-bit digest for fingerprints and integrity.

## Setup

Library `cajeta.hash`; source under `runtime/src/cajeta/hash`. The algorithm
classes bridge to `__cajeta_hash_*` / `__cajeta_<algo>_*` symbols in
`runtime/native/cajeta_runtime.c`, so the runtime native lib must be linked. The
process seed needs OS entropy at startup (works inside chroots/containers — no fd
required).

## Going deeper

Per-class detail (constructors, seeds/keys, full digest widths, reuse) lives in
the class source under `runtime/src/cajeta/hash/`: `XXHash3.cajeta`,
`SipHash.cajeta`, `MD5.cajeta`, `Sha1.cajeta`, `Sha256.cajeta`,
`DefaultHasher.cajeta`, the `Hasher.cajeta` interface, and the `Hash.cajeta`
facade. Spec: `docs/specification/hash/Hashing.md`.
