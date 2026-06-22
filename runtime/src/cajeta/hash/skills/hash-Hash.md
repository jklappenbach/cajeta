---
id: hash-Hash
applies-to: [cajeta/hash/Hash]
title: Hash — static facade for identity hashing, hash combining, and the process seed
description: Use Hash.identity for same-object keys, Hash.combine for manual hash() overrides, Hash.processSeed for the per-process seed.
---

# Hash — the static hash facade

**Access point, all-static — there is nothing to construct.** `Hash` is the
cajeta-side surface for three runtime hash primitives. Route by task:

| Want to…                                                              | Use                                |
|-----------------------------------------------------------------------|------------------------------------|
| key on *same heap object* (IdentityHashMap, observer/weak-ref tables) | `Hash.identity(obj)`               |
| thread field hashes in a hand-written `hash()` override               | `Hash.combine(a, b)`               |
| read the per-process seed (e.g. replay a hash-table snapshot)         | `Hash.processSeed()`               |
| run a named algorithm (XXH3, SipHash, MD5)                            | sibling classes, **not** here      |
| cryptographic hashing (SHA-2/3, BLAKE) or KDF/AEAD                    | future `cajeta.crypto`, **not** here |

It does **not** do structural hashing — the compiler synthesizes `Object.hash()`
for that; `combine` is only the mixer that synthesis emits between field hashes,
exposed for manual overrides.

## Usage

```cajeta
import cajeta.hash.Hash;

// Same-heap-object identity key
stack int64 id = Hash.identity(obj);

// Manual hash() override: same mixer the structural synthesizer emits,
// so manual and synthesized bodies agree for identical fields in order
public int64 hash() {
    return Hash.combine(this.name.hash(), this.value.hash());
}

// Process seed (stable per process, differs across restarts, never zero)
stack int64 seed = Hash.processSeed();
```

## The methods (all `public static`)

- `int64 identity(Object obj)` — pointer bit pattern mixed through SplitMix64 with
  the process seed. Parameter is the universal root `cajeta.lang.Object`, so any
  class reference passes without a cast. **Null-safe**: `null` is valid and hashes
  to the seed-mixed zero pointer. O(1).
- `int64 combine(int64 a, int64 b)` — Boost `hash_combine` + SplitMix64 finalizer.
  Deterministic and **order-sensitive**: `combine(a, b) != combine(b, a)` in
  general. O(1).
- `int64 processSeed()` — the process-wide random seed, seeded once at process
  start from OS entropy (`getrandom`/`getentropy`/`BCryptGenRandom`). Stable for
  the process lifetime; **never zero**. O(1).

## Ownership & lifecycle

Nothing crosses an ownership boundary. `identity` **borrows** `obj` (no `#` on the
parameter — it does not take, retain, or free the object; it only reads the
pointer). Every return is a primitive `int64` (no ownership). No instances, so no
construction, disposal, or `close()`.

## Sharp edges

- **Seed mixing is everywhere.** The process seed is folded into every hash this
  namespace produces — that is the HashDoS defense, but it also means hashes are
  **not comparable across processes** unless you pin `processSeed()` and thread it
  through both ends (the only reason `processSeed` is exposed).
- **No entropy fallback.** If OS entropy is unavailable at process start the
  runtime aborts with `CAJETA_ERROR_ENTROPY_UNAVAILABLE` rather than degrading to
  weak entropy — a startup failure, not a call-site exception you can catch.
- **`identity` is not structural equality.** Two structurally-equal-but-distinct
  objects get different identity hashes; that is the point. For value equality use
  the synthesized `Object.hash()` (see `cajeta.lang.Object`).
