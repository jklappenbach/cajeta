---
id: hash-Hasher
applies-to: [cajeta/hash/Hasher]
title: Hasher — uniform streaming-hash interface
description: The streaming-hash contract every algorithm implements; swap algorithms by changing only the constructor.
---

# Hasher — the streaming-hash contract

`Hasher` is the **interface** in package `cajeta.hash` that every algorithm class
(`XXHash3`, `SipHash`, `MD5`, `DefaultHasher`, …) implements. Program your hash loop
against a `Hasher` variable; pick the algorithm by which class you `heap`-construct.
Swapping algorithms is a one-line change to the constructor — the loop body never moves.

**This is a contract type, not an access point — you do not construct a `Hasher`.**
You obtain a concrete instance with `heap <Algo>(...)` and hold it as a `Hasher`:

```cajeta
import cajeta.hash.Hasher;
import cajeta.hash.XXHash3;

Hasher h = heap XXHash3();        // swap algorithm here only: heap SipHash(key0, key1), heap MD5(), heap DefaultHasher()
h.writeInt32(point.x);
h.writeInt32(point.y);
h.writeString(point.label);
int64 digest = h.finish();
```

## Construction & ownership

- You never write `heap Hasher()`; instantiate a concrete algorithm class.
  See `cajeta/hash/XXHash3`, `cajeta/hash/SipHash`, `cajeta/hash/MD5`,
  `cajeta/hash/DefaultHasher` for each one's constructor and seeding.
- `heap <Algo>()` yields an **owned** reference; the `Hasher` variable owns the instance
  and it drops at scope end. No `close()` exists on the contract.
- The `write*` arguments are **borrowed** — no `#` transfer. `writeString(String s)` and
  `writeObject(Object obj)` do not consume or retain their argument; the caller keeps
  ownership. `finish()` returns a plain `int64` value (no ownership).

## The methods that matter

All feed bytes into the running digest and return **`void`** (see v1 limitation below):

- **Width-named, never overloaded.** `writeInt16(int16)` vs `writeInt32(int32)` (and the
  `uint*`, `writeInt8`, `writeInt64`, `writeFloat32/64`, `writeBoolean`) each pin an exact
  byte sequence. This is deliberate: a silently widened primitive would change the digest.
  Pass the value at its real width — `writeInt32` and `writeInt16` of the same number
  produce **different** digests.
- `writeBytes(int8[])`, `writeBytesRange(int8[], int64 offset, int64 length)`,
  `writeString(String)` (hashes the underlying byte content), `writeObject(Object)`
  (hashes the structural / `@AutoHash` byte representation).
- `int64 finish()` — terminates the stream and returns the 64-bit digest of everything
  written so far.

## Lifecycle & what the contract does NOT do

- **`finish()` is terminal.** Behavior after the first `finish()` call is
  **algorithm-defined**; the contract only promises a digest from the bytes written so
  far. Do not reuse a `Hasher` after `finish()` unless the concrete class documents it.
  There is **no `reset()` on this interface** (a concrete class like `XXHash3` may add
  one — that is not part of the contract; don't rely on it through a `Hasher`).
- **No fluent chaining.** v1 ships `void` write returns (not `return this`) to dodge
  interface-covariance edge cases. Chain with separate statements, not `h.writeX().writeY()`.
- **64-bit only.** The digest is one `int64`; this contract has no wide-digest variant.
  Cryptographic-strength wide digests are a future `cajeta.crypto` concern, not here.
- **No errors.** No method declares a thrown error or returns a sentinel; `finish()`
  always yields a value.

## Related

- One-shot static hashing (no streaming object) and the convenience facade: see the
  `Hash` namespace and each algorithm's static `hash*` methods (e.g. `XXHash3.hash`).
- For digests matching the compiler-synthesized `Object.hash()` / snapshot replay across
  processes, use `cajeta/hash/DefaultHasher` (process-seeded XXH3-64).
