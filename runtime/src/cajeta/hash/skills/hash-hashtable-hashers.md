---
id: hash-hashtable-hashers
applies-to: [cajeta/hash/XXHash3, cajeta/hash/SipHash, cajeta/hash/DefaultHasher]
title: Picking and driving an int64 hash-table hasher (XXHash3 / SipHash / DefaultHasher)
description: When to use XXHash3 vs SipHash vs DefaultHasher to produce a 64-bit map key, plus the shared write*/finish() workflow and native-state ownership.
---

# int64 hash-table hashers

Three `Hasher` implementations that turn a stream of values into a 64-bit map key.
Pick by **threat model and reproducibility**, then drive them all the same way:
construct → `write*` → `finish()`.

## Which one

| Want | Use | Why |
|------|-----|-----|
| Hash internal / compiler-shaped / non-attacker data fast | `XXHash3` | ~10x faster per byte; multi-GB/s |
| Hash **attacker-controlled** keys feeding a live HashMap/cache | `SipHash` | 128-bit secret key blocks collision-flooding (HashDoS) |
| Match the compiler-synthesized `Object.hash()` shape, or compose a structural hash with extra context | `DefaultHasher` | Process-seeded XXH3-64 — the exact shape `Object.hash()`/`@AutoHash`/`String.hash()` emit |

Notes that prevent dead ends:
- **`XXHash3` is already HashDoS-resistant for the default path.** Its no-arg
  constructor and `Object.hash()` use a secret per-process seed (`Hash.processSeed()`),
  so an attacker can't precompute colliding keys for *your* process. Reach for
  `SipHash` only when you need a caller-chosen 128-bit key (e.g. a key rotated/derived
  per tenant or shared across a cluster) — not merely because input is untrusted.
- These produce **64-bit non-cryptographic** digests. For ETag/checksum use `MD5`;
  for security hashes (SHA-2/3, BLAKE) use the future `cajeta.crypto` library — there
  is none here.
- `DefaultHasher` is *not* a separate algorithm: it wraps an `XXHash3`. Use it for
  intent ("same shape as `Object.hash()`"), not for speed.

## Members and roles

- `Hasher` (`cajeta/hash/Hasher`) — the shared streaming contract. Code against this
  type to swap algorithms by changing only the constructor.
- `XXHash3` — fast XXH3-64; backs the others.
- `SipHash` — keyed SipHash-2-4; constructed with a 128-bit key as two `int64` halves.
- `DefaultHasher` — delegates every `write*`/`finish()` to a backing `XXHash3`.
- `Hash` (`cajeta/hash/Hash`) — static facade: `processSeed()` (per-process seed, never
  zero, aborts the process on entropy failure), `identity(obj)`, `combine(a, b)`.

## The shared workflow (all three)

Construct, feed width-named primitives / `String` / `Object`, then `finish()`:

```cajeta
import cajeta.hash.XXHash3;

XXHash3 h = heap XXHash3();   // process-seeded
h.writeInt32(record.id);
h.writeString(record.title);
int64 key = h.finish();       // 64-bit map key
```

Width matters: `writeInt16` and `writeInt32` of the same number produce **different**
digests — the byte sequence is part of the contract, so never pass a widened value.
`write*` methods return `void` in v1 (no fluent chaining); use separate statements.

`finish()` is **terminal but non-destructive**: it returns the digest and leaves the
state intact. To compute a fresh digest with the same instance, call `reset()` first
(which discards accumulated input and re-seeds / re-keys). Behavior of `write*` after
`finish()` without `reset()` is algorithm-defined — don't rely on it.

## Ownership and lifecycle (the sharp edge)

Each streaming instance owns **native state** behind `public pointer state` (an
`XXH3_state_t` / `cajeta_siphash_state`). Construct with `heap`; the destructor
(`~XXHash3` / `~SipHash`) frees that native state when the object is dropped. So:

- Let the instance drop at scope end — do not free `state` yourself, and do not copy
  the `pointer` out and use it past the owner's lifetime (use-after-free / double-free).
- `reset()` reuses the same native allocation for another digest — cheaper than building
  a new instance in a tight loop.
- `XXHash3.reset()` re-derives from the stored `seed`; `SipHash.reset()` re-initializes
  from the stored `k0`/`k1` (SipHash state can't be re-derived without the key, which is
  why both halves are retained on the instance).
- `DefaultHasher` owns its backing `XXHash3` (a `heap` field); dropping the
  `DefaultHasher` drops the backing, which frees the native state. Nothing extra to do.

## Reproducibility and seeds

The no-arg `XXHash3()` / `DefaultHasher()` use `Hash.processSeed()`, so digests differ
across process restarts — perfect for in-process maps, wrong for persisted/cross-process
keys. For stable digests use the explicit-seed constructor and ship the seed:

```cajeta
import cajeta.hash.DefaultHasher;
import cajeta.hash.Hash;

int64 seed = Hash.processSeed();        // capture in the source process
DefaultHasher replay = heap DefaultHasher(seed);   // rebuild elsewhere
replay.writeObject(order);
int64 digest = replay.finish();
```

`SipHash` is inherently reproducible: the same key + bytes give the same digest in any
process. `XXHash3.hashSeeded(...)` / `SipHash.hashKeyed(...)` are one-shot statics for
when you have all the bytes already and don't need a streaming object.

## One-shot vs streaming

When you hold a complete `int8[]`, skip the object:

```cajeta
import cajeta.hash.SipHash;

// attacker-controlled key, caller-chosen 128-bit key (k0, k1)
int64 key = SipHash.hashKeyed(data, data.count(), k0, k1);
```

Streaming feeds incrementally and is order-/width-sensitive; the streamed result equals
the one-shot of the concatenated bytes (verified in
`test/parser/SipHashTests.cpp::streamingMatchesOneShot`). Use streaming to fold several
fields (or a structural `writeObject` plus extra context) into one key without first
copying them into a single buffer.

`writeBytesRange(data, offset, length)` currently **ignores `offset`** on `XXHash3` and
`DefaultHasher` — it always hashes from index 0 for `length` bytes. Slice yourself if
you need a true offset.

See also: `cajeta/hash/Hasher` (the contract and width rules) and `cajeta/hash/Hash`
(`processSeed`/`combine`/`identity`).
