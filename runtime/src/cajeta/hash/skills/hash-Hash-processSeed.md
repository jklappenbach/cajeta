---
id: hash-Hash-processSeed
applies-to: [cajeta/hash/Hash.processSeed]
title: Hash.processSeed — the process-wide hash seed (capture-and-ship)
description: Read the per-process random seed mixed into every cajeta hash; capture it to reproduce hashes on another machine.
---

# Hash.processSeed — the process-wide hash seed

`public static int64 processSeed()` returns this process's random hash seed — a
plain `int64` **value** (no ownership, nothing to free). The number is **stable for
the lifetime of the process, differs across restarts, and is never zero**. It is the
seed the runtime **mixes into every hash this namespace produces** (`Hash.identity`,
`Hash.combine`, the primitive specializations, the SipHash byte path, and the
compiler-synthesized `Object.hash()`).

```cajeta
import cajeta.hash.Hash;

stack int64 seed = Hash.processSeed();   // stable per process, never zero
```

Reach for this in exactly one situation: **snapshot-replay seed alignment** — you
hash structures in process A, persist a hash-keyed snapshot, and need the *same*
hashes when replaying it in process B. Because B has a different process seed, its
`Object.hash()` / `Hash.*` values won't match A's. So **capture A's `processSeed()`,
ship it with the snapshot, and on B feed that captured seed into an
explicitly-seeded hash path** (see `cajeta/hash/DefaultHasher`) instead of relying on
B's process default. There is **no setter** — `processSeed()` only reads B's own seed;
you cannot rebind the process default, so replay must thread the captured value
through explicitly-seeded hashing.

## Failure mode — abort, not a return code

The seed is initialized once from OS entropy at process start: `getrandom(2)` on
Linux, `getentropy(3)` on macOS/BSD, `BCryptGenRandom` on Windows. **There is no
fallback.** If entropy is unavailable (rare — mid-boot or a severely degraded
system) the runtime logs and **aborts the process with
`CAJETA_ERROR_ENTROPY_UNAVAILABLE`**. Consequence for the caller: `processSeed()`
**never throws and never returns an invalid or zero value** — by the time it returns,
a strong non-zero seed is guaranteed. There is nothing to try/catch here.

## Side effects & cost

- O(1). The first hash-related access lazily performs the one-time entropy syscall;
  every call after that (including this one) is a bare load that folds into hot code.
- No receiver state, no filesystem, no allocation. Pure read.

## Gotchas

- **Not a stable cross-run key.** The value changes every restart. Do not persist it
  as a long-lived identifier or precompute hashes against it offline — its only
  cross-process use is the capture-and-ship pattern above.
- **Never zero.** Don't write `if (seed == 0)` sentinel logic; zero never occurs.
- **Don't reimplement the mix.** The seed is already folded into every `Hash.*`
  result and `Object.hash()`; you only need this value when bridging to an
  *external*, explicitly-seeded hash computation.

## Related

- `cajeta/hash/Hash` — the namespace (`identity`, `combine`) this method sits in.
- `cajeta/hash/DefaultHasher` — the seeded XXH3-64 hasher behind `Object.hash()`;
  the place to feed a captured seed for replay.
