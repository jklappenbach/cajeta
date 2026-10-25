# xxhash3-writebytesrange-offset — `XXHash3.writeBytesRange` ignores its offset (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 8 while
> checking every digest against the xxHash 0.8.3 reference constants.

## 1. Definition
On v0.46.3 `XXHash3.writeBytesRange(bytes, offset, length)` hashes from the
start of the array whatever `offset` says. Two related findings from the same
pass: `hashSeeded` always takes the native path even above 240 bytes, so the
SIMD long-input path is reachable only through `hashLong` and the
process-seeded `hash()`; and there is no slice or offset hashing API besides
this one.

## 2. Use cases
- **2.1** As a caller, when I hash a window of a buffer with
  `writeBytesRange`, then the digest equals the reference digest of that
  window, and a test pins a non-zero offset against the reference constant.
- **2.2** As a caller of `hashSeeded` on a long input, then I get the same
  path and speed as `hash()`.

## 3. Acceptance
The hash benchmarks' reference checks stay green; a new test with a non-zero
offset passes.
