# String Hashing via XXH3 Spec

## 1. Definition

### 1.1 Purpose
Replace `String.hash()`'s hand-rolled, byte-at-a-time FNV-1a loop with a call to
the runtime's already-shipped XXH3-64 (`__cajeta_hash_bytes`), so string-keyed
hashing is multi-GB/s instead of one multiply-xor per byte — closing the largest
remaining gap on the `hashmap-string` benchmark.

### 1.2 Problem
`String.hash()` (runtime/src/cajeta/lang/String.cajeta) loops over `byteLength`,
folding each byte with `h = (h ^ b) * FNV_PRIME`. This is a scalar dependency
chain: one byte per iteration, no vectorization. The runtime already exposes
`__cajeta_hash_bytes(const uint8_t*, int64) -> int64` backed by
`XXH3_64bits_withSeed` (with the per-process hash-flood seed) — used by
`cajeta.hash.XXHash3` and the primitive hashers — but `String.hash()` never wired
to it. The String.hash() doc even names this as the intended follow-up.

### 1.3 Approach
Add a pure-codegen compiler intrinsic `Cajeta.hashBytes(int8[] buf, int64 len)`
that GEPs the array data pointer (header + 8, the established array-data ABI used
by `loadU64`) and emits `call i64 @__cajeta_hash_bytes(dataPtr, len)`. Rewrite
`String.hash()` to return `Cajeta.hashBytes(this.bytes, (int64) this.byteLength)`
(empty/`null` bytes hash via the `len == 0` path).

### 1.4 Scope
- One new intrinsic in `MethodCallExpression.cpp`.
- One method body change in `String.cajeta`.

### 1.5 Non-goals
- No change to the hash **contract** (still content-based, still int64). Values
  change (FNV - XXH3) but equal strings still hash equal and unequal strings still
  (almost surely) differ — the only contract HashMap relies on.
- No change to primitive or identity hashing.

## 2. The intrinsic

### 2.1 Requirements
- 2.1.1 `Cajeta.hashBytes(int8[] buf, int64 len)` resolves to type `int64`.
- 2.1.2 It computes the array data pointer as `GEP(i8, header, 8)` and calls
  `__cajeta_hash_bytes(dataPtr, len)`, returning the i64 result.
- 2.1.3 It is available in both JIT and AOT (resolves via `getRuntimeFunction`,
  which links the runtime symbol into the emitting module).

### 2.2 Use cases
- 2.2.1 As stdlib code, when I call `Cajeta.hashBytes(bytes, n)` on a non-null
  byte array, then I get the XXH3-64 hash of the first `n` bytes (seeded).

## 3. String.hash()

### 3.1 Requirements
- 3.1.1 `"foo".hash() == "foo".hash()` (equal content - equal hash), and a String
  built by concatenation hashes equal to the literal of the same bytes.
- 3.1.2 The empty String and a null-bytes String hash to a stable value (the
  `len == 0` XXH3 seed result), and equal to each other.
- 3.1.3 `s.hash()` as a HashMap key produces correct `get`/`containsKey` results
  for thousands of distinct string keys (no spurious collisions breaking lookup —
  collisions are tolerated by the table, but correctness must hold).

### 3.2 Use cases
- 3.2.1 As a user, when I use `String` keys in a `HashMap` / `ImmutableMap`, then
  lookups return the value I put, and the `hashmap-string` bench `checkResult()`
  passes.
- 3.2.2 As a user, when I call `s.hash()` on identical content in two String
  instances, then I get identical values (value-keyed semantics preserved).
