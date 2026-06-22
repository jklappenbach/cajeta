---
id: hash-XXHash3
applies-to: [cajeta/hash/XXHash3]
title: XXHash3 — XXH3-64 one-shot statics and streaming hasher
description: Use XXHash3 for fast non-cryptographic 64-bit hashing — one-shot statics or a streaming Hasher that owns native XXH3 state.
---

# XXHash3

The default-backing XXH3-64 hasher in `cajeta.hash`, and the main access point for
fast non-cryptographic 64-bit hashing. `final class XXHash3 implements Hasher`.
Pick it over `SipHash` for non-attacker-controlled input (~10× faster), over
`RapidHash` when you need XXH3 ecosystem interop (LZ4 / zstd / RocksDB).

## Decide: one-shot statics vs streaming instance

- **You have all the bytes already** → call a static. No instance, no lifecycle, no
  native state to free.
- **You accumulate fields incrementally** (hashing a struct's members) → construct an
  instance and use the `Hasher` write methods. The instance owns native state you must
  let drop.

## One-shot statics (no instance, nothing to free)

All return `int64`. `data` is **borrowed** — these read it and do not free it; you keep
ownership.

```cajeta
import cajeta.hash.XXHash3;

int64 a = XXHash3.hash(bytes, bytes.count());            // per-process seed
int64 b = XXHash3.hashSeeded(bytes, bytes.count(), seed); // explicit seed
int64 c = XXHash3.hashString("hello");                    // per-process seed
int64 d = XXHash3.hashStringSeeded("hello", 0xC0FFEEL);   // explicit seed
```

- `hash` / `hashString` seed from `Hash.processSeed()` — stable within one process,
  **randomized per process**. Do NOT persist these digests or compare them across runs.
- `hashSeeded` / `hashStringSeeded` take an explicit `int64` seed — use these for
  digests that must reproduce across processes or be stored.
- `len` is a byte count you pass explicitly (`bytes.count()`); it is not derived from the
  array. The `String` variants read `s.bytes` / `s.byteLength` for you.

## Construction & ownership (streaming)

You **construct** an instance — it allocates a native `XXH3_state_t` in its constructor
and **frees it in its destructor**. There is no `close()`; cleanup is drop-on-scope, so
heap-allocate and let normal cajeta ownership drop it. Do not retain the raw `state`
pointer past the instance.

```cajeta
import cajeta.hash.XXHash3;

XXHash3 h = heap XXHash3();          // per-process seed
XXHash3 g = heap XXHash3(0xC0FFEEL); // explicit seed, reproducible
```

`int64 seedArg` is a value (no `#` transfer). The instance keeps its seed so `reset()`
can re-derive an equivalent fresh state.

## The methods that matter

Streaming write surface (from `Hasher` — see `cajeta/hash/Hasher`); all return `void`,
width-specific, and **not** chainable:

```cajeta
XXHash3 h = heap XXHash3(42L);
h.writeInt32(record.id);
h.writeString(record.title);   // feeds the String's UTF-8 bytes
int64 digest = h.finish();
```

- `update(int8[] data, int64 len)` — feed raw bytes; `data` borrowed, not freed.
- `finish() -> int64` — finalize and return the digest. **Leaves state intact**; it does
  not reset. Calling `finish()` again without `reset()` re-digests the same accumulated
  input.
- `reset()` — discard accumulated input and re-seed from the original seed, so the same
  instance can produce a fresh digest. This is the reuse path (no need to reallocate).

## State, reuse, concurrency

Mutable and stateful: every `write*`/`update` mutates the native state. Reusable via
`reset()`. **Not** thread/fiber-safe — one instance per thread, or guard it.

## Sharp edges

- `writeBytesRange(data, offset, length)` **ignores `offset`** — it always hashes from
  index 0 for `length` bytes. Slice your array yourself if you need a true sub-range.
- Width matters: `writeInt16(x)` and `writeInt32(x)` feed different byte counts and yield
  different digests. Match widths on both sides of any comparison.
- Streaming and one-shot agree only for the **same seed and same byte sequence**: a
  streamed sequence of `update` calls equals `hashSeeded` over the concatenated buffer
  with that seed (verified in `test/parser/XXHash3Tests.cpp`).
- `hash()` routes inputs **> 240 bytes** through a pure-cajeta AVX-512 SIMD long path
  (`hashLong`), ≤ 240 bytes through the native oneshot; both are bit-identical to the
  reference XXH3-64. This is transparent — you do not choose the path.

## Errors

Raises nothing — no exceptions, no null returns. Digests are plain `int64` (any value,
including 0, is valid).

## What this is NOT

- Not cryptographic — never use for security, MACs, or attacker-facing input; use
  `cajeta/hash/SipHash` there.
- Not the compiler's `Hasher` indirection point — synthesized `Object.hash()` /
  `@AutoHash` go through `cajeta/hash/DefaultHasher` (which wraps a process-seeded
  `XXHash3`). Reach for `XXHash3` directly only when you want XXH3 specifically.

@See cajeta/hash/Hasher, cajeta/hash/DefaultHasher, cajeta/hash/Hash
