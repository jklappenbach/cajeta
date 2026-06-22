---
id: hash-digesters
applies-to: [cajeta/hash/MD5, cajeta/hash/Sha1, cajeta/hash/Sha256]
title: Byte-digest family — MD5 / Sha1 / Sha256 (one-shot + streaming)
description: Pick and drive the cajeta byte-digesters — one-shot hash/hashHex statics and streaming update/digest/hex — with correct ownership and terminal-state rules.
---

# Byte digesters: MD5 / Sha1 / Sha256

Three structurally identical fixed-output digest classes in `cajeta.hash`. They do
**not** collaborate with each other — pick exactly one per task. Each is both a
one-shot static helper and a streaming `Hasher`.

## Which one

| Task | Use | Why |
|---|---|---|
| HTTP `ETag`, S3 `Content-MD5`, asset/cache fingerprints | **MD5** (128-bit) | fast checksum; not security |
| WebSocket opening handshake (`Sec-WebSocket-Accept`) — and *only* that | **Sha1** (160-bit) | RFC 6455 protocol constant |
| Integrity / checksum verification (TLS cert fp, release-checksum, streaming download integrity) | **Sha256** (256-bit) | the only **cryptographic** member |

Negative routing — what is **not** here: MD5 and Sha1 are cryptographically broken;
never use them for signatures, MACs, password hashing, or anything an attacker can
forge. For crypto beyond SHA-256 (e.g. BLAKE3) see the peer `cajeta.crypto` library
(not in v1). `finish()` on **any** of these is a hash-table projection, never an
integrity value (see Terminal state). `writeBytesRange`'s `offset` is ignored in v1.

## Members and shape

All three are `final class … implements Hasher` (see `cajeta/hash/Hasher`) holding a
`pointer state` to native digest state, allocated in the constructor and freed by the
destructor (`~MD5` etc.) on scope exit — there is **no** `close()` to call.

Two ways to consume each, with output widths:

| | MD5 | Sha1 | Sha256 |
|---|---|---|---|
| raw digest bytes | 16 | 20 | 32 |
| hex chars | 32 | 40 | 64 |
| one-shot static | `hash` / `hashHex` (+ `hashString` / `hashStringHex`) | same | same |
| streaming finalizers | `digest()` | `digest()` | `digest()` **and** `hex()` |

Only `Sha256` has a streaming `hex()`. For MD5/Sha1, hex from the streaming path means
hex-encoding the `digest()` bytes yourself, or just use the `hashHex` static.

## Ownership (every value that crosses the boundary)

- One-shot `hash(data, len)` → `#int8[]`: a freshly heap-allocated digest array,
  **ownership transferred to caller**. `hashHex(...)`/`hex()` → `#String`: owned String.
- Streaming `digest()` → `#int8[]` owned; `Sha256.hex()` → `#String` owned.
- `update(data, len)` / `write*` **borrow** their arguments — the digester does not
  retain or free them.
- The `int8[] data` you pass to the one-shot statics is borrowed; you still own it.

## Terminal state — pull the full digest BEFORE finish()

`digest()`, `hex()`, and `finish()` are all **terminal**: each finalizes and consumes
the state. After any of them, further `update()`/`write*` are undefined until you call
`reset()` (which re-inits the same instance — no realloc). Critically, `finish()`
(the `Hasher` contract method) returns only the **first 8 digest bytes as a
little-endian int64** — a hash-table projection, **never** an integrity check value. If
you need both the full digest and a 64-bit key, call `digest()`/`hex()` **first**, then
`reset()` and `finish()` — you cannot recover the full digest after `finish()`.

## Examples (with imports)

One-shot, the common case:

```cajeta
import cajeta.hash.MD5;
import cajeta.lang.String;

String etag = MD5.hashHex(bytes, bytes.count());   // owned 32-char hex String
```

Streaming integrity over chunks (the download-checksum path), mirroring `Sha256Tests`:

```cajeta
import cajeta.hash.Sha256;
import cajeta.lang.String;

Sha256 sum = heap Sha256();           // native state allocated; freed on scope exit
sum.update(chunk1, chunk1.count());   // chunks borrowed, not retained
sum.update(chunk2, chunk2.count());
String hex = sum.hex();               // terminal: finalize → owned 64-char hex
// string-compare hex against the manifest's published checksum
```

The incremental digest over successive `update()` calls equals the one-shot digest of
the concatenation (FIPS-pinned for Sha256).

Streaming as a `Hasher` (algorithm chosen by the constructor; width-named writes pin the
byte sequence — do not pass widened primitives):

```cajeta
import cajeta.hash.MD5;

MD5 m = heap MD5();
m.writeInt32(record.id);
m.writeString(record.title);
int64 fingerprint = m.finish();       // first 8 digest bytes as LE int64 — table key only
```

## v1 native-bridge quirks (shared by all three)

- `boolean` doesn't coerce at the `@Native` ABI boundary, so `writeBoolean` materializes
  an int8 via ternary internally.
- `int8[]` returned from `@Native` doesn't register with the per-thread live-set, so the
  one-shot and digest helpers pre-allocate the cajeta-side output array and the C helper
  fills it (why these are `*_into` bridges under the hood).
- `writeBytesRange(data, offset, length)`: `offset` is **ignored** in v1 (slicing is a v2
  follow-up); it hashes `length` bytes from the array start.

See `cajeta/hash/Hasher` for the streaming contract these implement.
