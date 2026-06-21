---
id: hash-SipHash
applies-to: [cajeta/hash/SipHash]
title: SipHash — keyed, DoS-resistant 64-bit hash (requires a 128-bit key)
description: Using cajeta.hash.SipHash for attacker-controlled keys — one-shot hashKeyed/hashStringKeyed and the keyed streaming Hasher; construction always needs (k0, k1).
---

# SipHash — keyed, DoS-resistant hash

`SipHash` (SipHash-2-4, 64-bit output) is the algorithm to reach for when the input
is **attacker-controlled** and feeds a HashMap/cache lookup (request body keys, JWT
claim names, user-supplied cache keys). Its 128-bit key stops an attacker from
precomputing colliding inputs. It is an **entry-point** type and an implementation of
`cajeta.hash.Hasher`.

The defining constraint: **there is no default constructor — every path needs a
128-bit key as two `int64` halves `(k0, k1)`**. The one-shot statics take the key as
trailing parameters; the streaming object takes it in the constructor. Pick one key
per logical table and keep it secret/stable.

For non-attacker, internal data use `XXHash3` (~10x faster/byte); for ETag/fingerprint
use `MD5`. See `cajeta.hash.Hasher` for the shared streaming contract (the `write*`
methods and `finish()` semantics) — not repeated here.

## One-shot (the common case)

```cajeta
import cajeta.hash.SipHash;

// keyed digest of a byte buffer — len is a separate arg, not bytes.count()
int64 h = SipHash.hashKeyed(data, data.count(), k0, k1);

// keyed digest of a String's UTF-8 bytes
int64 hs = SipHash.hashStringKeyed(user.name, k0, k1);
```

Both statics are stateless — no object to allocate or free. Note the parameter order:
the key `(k0, k1)` comes **after** the data, so for `hashKeyed` the full call is
`(data, len, k0, k1)`.

## Streaming (multiple fields into one digest)

```cajeta
import cajeta.hash.SipHash;

SipHash h = heap SipHash(k0, k1);   // key required; no `heap SipHash()`
h.writeString(user.name);
h.writeInt32(user.id);              // width-named writes pin the byte sequence
int64 digest = h.finish();

h.reset();                          // re-derive state from the stored key, reuse
h.writeString(other.name);
int64 d2 = h.finish();
```

Use the streaming form only when you need to combine several values; for a single
buffer or string prefer the one-shot statics.

## Construction, ownership & lifecycle

- **Constructor `SipHash(int64 keyLow, int64 keyHigh)`** — the key halves are copied
  into the instance (fields `k0`, `k1`) so `reset()` can re-initialize the native
  state without the caller re-supplying the key. Construct with `heap SipHash(k0, k1)`.
- The instance owns an **opaque native `pointer state`** (a `struct
  cajeta_siphash_state`) allocated in the constructor and **freed automatically by the
  destructor `~SipHash`** — there is **no `close()`/`dispose()`**; reclamation follows
  normal cajeta drop. Do not touch or free `state` yourself.
- `write*` parameters and `update(data, len)` are **borrowed** — no `#` transfer; the
  bytes are absorbed during the call and not retained.
- `finish()` / `hashKeyed` / `hashStringKeyed` return a plain `int64` by value — no
  ownership, nothing to free.

## reset() — required because state can't be re-derived

`reset()` re-initializes the native state from the stored `k0`/`k1`, letting you reuse
one instance for a fresh digest. It exists precisely because SipHash-2-4 state cannot
be rebuilt without the key — that is why the key is stored on the instance rather than
discarded after construction. Call it between digests when reusing an object.

## Implementation note — `allocState` returns `#pointer`

The private `@Native` bridge `allocState(int64, int64)` is declared
`static #pointer` (move-qualified). The `#` is **cajeta-side ownership bookkeeping
only** — required because the multi-param-static-borrow-return rule would otherwise
reject a static method returning a reference-typed value with more than one parameter;
the underlying C function still returns a raw `void*`. This is internal: callers never
invoke `allocState` and never see the `#pointer`.

## What it does NOT do

- **No default / keyless construction** — you must supply `(k0, k1)` everywhere; there
  is no process-seeded convenience variant here (that is `Object.hash()`'s job).
- **No `close()`** — the destructor frees the native state; do not free it manually.
- **`finish()` is terminal** — behavior of `write*` after `finish()` is
  algorithm-defined; call `reset()` before reusing.
- **No non-keyed API** — if you don't have/ want a key, use `XXHash3` instead.
