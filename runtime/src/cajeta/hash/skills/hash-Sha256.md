---
id: hash-Sha256
applies-to: [cajeta/hash/Sha256]
title: Sha256 — cryptographic digest for integrity (one-shot + streaming)
description: Use Sha256 for integrity (TLS fingerprints, release checksums, streaming downloads); digest()/hex() are terminal finalizers, finish() is hash-table-only.
---

# Sha256

The package's **only cryptographic hash** (collision/preimage-resistant SHA-256,
FIPS 180-4). Reach for it for **integrity**: TLS cert fingerprinting, `cvm`
release-checksum verification, and running-checksum streaming downloads. For
hash-table keys or fast non-cryptographic checksums, use `XXHash3` / `SipHash` /
`MD5` instead — not this.

**Access point.** A "start here" type you construct directly. Implements `Hasher`
(`cajeta/hash/Hasher`).

## Pick the path

- **You have all the bytes already** → one-shot statics. No instance, nothing to free
  by you.
- **Bytes arrive in chunks** (download, socket, large file) → construct an instance,
  `update()` per chunk, finalize once.

## One-shot (whole input in hand)

```cajeta
import cajeta.hash.Sha256;
import cajeta.lang.String;

// 32 raw digest bytes — caller owns the returned array.
int8[] digest #= Sha256.hash(bytes, bytes.count());

// 64 lowercase hex chars — the checksum-comparison shape.
String hex #= Sha256.hashHex(bytes, bytes.count());
```

`hashString(String)` / `hashStringHex(String)` are the same over a String's UTF-8 bytes.

## Streaming (chunked input)

```cajeta
import cajeta.hash.Sha256;
import cajeta.lang.String;

Sha256 sum = heap Sha256();
sum.update(chunk1, chunk1.count());   // len is int64; pass an int64 literal (e.g. 7L)
sum.update(chunk2, chunk2.count());
String hex #= sum.hex();               // finalize → 64-char lowercase hex
// string-compare hex against the manifest's published checksum
```

The incremental digest over successive `update()` calls is byte-for-byte equal to the
one-shot digest of the concatenation (FIPS-pinned in `test/parser/Sha256Tests.cpp`).

## Methods that matter

- `static #int8[] hash(int8[] data, int64 len)` — 32 fresh bytes, **ownership
  transferred** to caller.
- `static #String hashHex(int8[] data, int64 len)` — owned 64-char lowercase hex String.
- `void update(int8[] data, int64 len)` — append `data[0..len)` to the running digest.
- `#int8[] digest()` — finalize → owned 32-byte array. **Terminal.**
- `#String hex()` — finalize → owned 64-char hex String. **Terminal.**
- `void reset()` — re-initialize state to hash a new input on the same instance.
- `int64 finish()` — Hasher projection (first 8 digest bytes, little-endian).
  **Hash-table use only — NEVER for integrity.** Integrity callers use `digest()`/`hex()`.

All returns are non-null; the `#` marks ownership transfer to the caller.

## Construction, lifecycle, ownership

`heap Sha256()` allocates the native `struct cajeta_sha256_state`. The instance owns
that state and frees it in its destructor (`~Sha256`) — **drop-on-scope, no manual
close()/free**. The `int8[]`/`String` returned by the finalizers and statics are owned
by the caller (transferred via `#`); the `int8[] data` you pass to `update`/`hash` is
borrowed (not retained, not freed).

## Required call order & reuse

`update()*` then exactly one finalizer. `digest()` and `hex()` **consume the state** —
calling `update()` after a finalizer (without `reset()`) is undefined. To hash another
input on the same instance, call `reset()` first; this avoids reallocating. A single
instance is mutable and **not** thread/fiber-safe — one instance per concurrent stream.

## Sharp edges

- `finish()` looks like a digest but is a 64-bit truncation — only valid as a `Hasher`
  hash-table key. Using it for integrity silently throws away 192 bits of the hash.
- `writeBytesRange(data, offset, length)` **ignores `offset`** in v1 (the native helper
  reads from `data[8..]`); use `update(data, len)` for explicit chunking until v2 adds
  slicing.
- Finalizers are terminal — there is no "peek current digest" without consuming state.
- This class does **not** fetch, download, or compare — it only digests bytes. The
  download/verify orchestration lives in the networking layer (`cajeta.io.net`,
  `HttpClient.downloadTo`).

See `Hasher` (`cajeta/hash/Hasher`) for the streaming write-primitive contract this
type implements, and `MD5` (`cajeta/hash/MD5`) for its non-cryptographic structural twin.
