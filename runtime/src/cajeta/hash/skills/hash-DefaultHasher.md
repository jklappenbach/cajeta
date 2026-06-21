---
id: hash-DefaultHasher
applies-to: [cajeta/hash/DefaultHasher]
title: DefaultHasher — the process-seeded XXH3 the compiler's Object.hash() uses
description: Build digests in the same family as compiler-synthesized Object.hash()/@AutoHash; default-seed to match them, explicit-seed to replay a digest from another process.
---

# DefaultHasher — match the compiler's `Object.hash()`

`DefaultHasher` is the concrete `Hasher` (package `cajeta.hash`) that
compiler-synthesized `Object.hash()`, `@AutoHash` field-folding, and `String.hash()`
route through: **process-seeded XXH3-64**. Reach for it in exactly two situations:

- **Compose** extra context (a tenant, request ID, deploy epoch) alongside an object and
  get a digest in the *same family* as the synthesized one → use the **default** constructor.
- **Snapshot replay**: reproduce a digest computed in another process → use the
  **explicit-seed** constructor with a seed captured from `Hash.processSeed()`.

If you just want any streaming hash and don't care about matching the compiler, program
against the `cajeta/hash/Hasher` interface and pick any algorithm — there is no reason to
name `DefaultHasher` specifically. If you only need `obj.hash()` itself, the compiler
already emits it; you do not construct a `DefaultHasher` for that.

**Access-point flag:** entry point — you `heap`-construct it. (It is also what the
compiler couples to internally; the compiler targets `DefaultHasher`, *not* `XXHash3`
directly, so the backing algorithm can swap without changing synthesized hashing.)

## Construction & ownership

```cajeta
import cajeta.hash.DefaultHasher;
import cajeta.hash.Hash;

// Default seed — digests match compiler-synthesized Object.hash() in THIS process.
DefaultHasher hasher = heap DefaultHasher();
hasher.writeObject(order);      // structural fold of the object (@AutoHash shape)
hasher.writeString(tenantId);   // extra context: tenant
hasher.writeInt64(epoch);       // extra context: deploy epoch
int64 digest = hasher.finish();

// Snapshot replay — rebuild with a seed shipped from the source process.
int64 seed = Hash.processSeed();          // captured where the digest was first computed
DefaultHasher replay = heap DefaultHasher(seed);
replay.writeObject(order);
int64 reproduced = replay.finish();
```

- `DefaultHasher()` seeds from `Hash.processSeed()` (the per-process random seed, never
  zero). Two `DefaultHasher()` instances in the same process agree; **across processes
  they differ** unless you thread the seed through with the explicit-seed form.
- `DefaultHasher(int64 seedArg)` takes the seed **by value** (no `#`); plain `int64`.
- `heap DefaultHasher(...)` yields an **owned** reference; the variable owns it and it
  drops at scope end. The public `backing` field is an `XXHash3` holding native state that
  is freed when that `XXHash3` drops — so the whole graph is released automatically at
  scope exit. There is **no `close()`**; do not free anything by hand.

## Methods & errors

Every `write*` and `finish()` simply delegates to the backing `XXHash3`. Their
signatures, the width-named-not-overloaded rule, the borrowed (non-`#`) argument
semantics, `void` returns, no fluent chaining, and "no errors / no sentinels" all come
from the contract — see `cajeta/hash/Hasher`; not repeated here. `finish()` returns a
plain `int64` value (no ownership) and is **terminal**: do not write or `finish()` again
after the first `finish()`.

## Sharp edges

- **`writeBytesRange(data, offset, length)` ignores `offset` in v1** — hashing always
  starts at index `0` and folds `length` bytes from the front. If you need a true
  mid-array slice, copy the slice first and pass it to `writeBytes`.
- **Matching the compiler is seed-dependent, not just algorithm-dependent.** A digest
  reproduces only when *both* the algorithm and the seed match. Default-constructed
  digests are stable within a process run but not persistable across runs — capture the
  seed for anything you store or send.
- Field order matters: the bytes you write define the digest, so write composed context
  in a fixed order on both the producing and the replaying side.

## Related

- The write/`finish` contract and the algorithm-swap rule: `cajeta/hash/Hasher`.
- The backing algorithm and its seeding: `cajeta/hash/XXHash3`.
- The process seed source and one-shot facade: the `Hash` namespace (`Hash.processSeed`).
