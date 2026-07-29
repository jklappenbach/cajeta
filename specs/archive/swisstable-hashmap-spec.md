# SwissTable HashMap Spec

## 1. Definition

### 1.1 Purpose
Replace the linear-probe, parallel-array `HashMap` / `ImmutableMap` storage
engine with a **SwissTable** (SSE2-style open addressing with SIMD metadata
probing), so that `get` / `put` / `containsKey` are competitive with — and where
possible faster than — the world-class SIMD hash tables in the cross-language
profile suite (Rust `hashbrown`, C++ `ankerl`/`std::unordered_map`).

### 1.2 Problem
The current engine stores one `int8 state` byte per slot and probes **one slot
per loop iteration**, touching three separate parallel arrays (`keys`, `vals`,
`state`) — three cache lines per probe step, one comparison per step, and a
branchy inner loop the optimizer cannot vectorize. Against SIMD SwissTables this
loses by 1.7-3x on integer keys and is last-place on string keys.

### 1.3 Approach
cajeta already ships the exact SSE2 toolkit the SwissTable algorithm needs, used
today by `CsvIndex` / `JsonIndex`:
- `Cajeta.vload16(int8[], int64) -> Vector<int8,16>` — unaligned 16-byte load.
- `v.eqMask((int8) needle) -> int32` — per-lane equality packed to a 16-bit mask.
- `Cajeta.ctz64(int64) -> int32` — index of first set bit.
- bit iteration `m = m & (m - 1)` to clear the lowest set bit.

A **control byte** array replaces `state`: one metadata byte per slot holding
either a 7-bit hash fragment (`h2`, top bit clear = FULL) or a sentinel
(`EMPTY = 0x80`, `DELETED = 0xFE`). A single `vload16` + `eqMask` tests 16 slots
at once: matches against `h2` find candidate keys; a match against `EMPTY`
terminates the probe. Probing advances by **groups of 16** using triangular
offsets (`stride += 16`), which guarantees full table coverage for power-of-two
capacities.

### 1.4 Scope
- Rewrite `cajeta.collection.HashMap<K,V>` storage to SwissTable.
- Rewrite `cajeta.collection.ImmutableMap<K,V>` index to SwissTable (read-only:
  no DELETED, no resize).
- Update the three `HashMap*Stream` view classes to the control-byte predicate.
- Preserve the entire **public API and observable semantics** (see 5).

### 1.5 Non-goals
- No change to the hashing of keys (`K.hash()`); that is a separate spec
  ([[string-hash-xxh3-spec]]).
- No AVX-512 / 64-byte groups; 16-byte SSE2 groups via `vload16` only.
- No `@AutoHash` changes; no new public methods.
- The DELETED-vs-EMPTY-on-remove heuristic (abseil's chain-shortening trick) is
  deferred — v1 always writes DELETED tombstones, cleared on resize, matching the
  current engine's remove semantics exactly.

## 2. Control-byte layout

### 2.1 Requirements
- `ctrl` is an `int8[]` of length `cap + 16`. Slots `[0, cap)` hold real control
  bytes; the trailing 16 bytes **mirror** `ctrl[0..15]` so a `vload16` starting at
  any slot in `[0, cap)` reads a valid 16-byte window without overrunning.
- Control values: `EMPTY = (int8) 0x80` (-128), `DELETED = (int8) 0xFE` (-2),
  `FULL = h2` where `h2` is in `[0, 127]` (top bit clear).
- A freshly allocated `ctrl` array (calloc-zeroed `0x00`) is **not** valid —
  `0x00` is a FULL byte (h2 = 0). The constructor/resize MUST fill `ctrl` with
  `EMPTY` before use (8-byte `storeU64` of `0x8080808080808080` + tail).
- `cap` MUST be a power of two and `>= 16` (group coverage needs `cap >= 16`).

### 2.2 Use cases
- 2.2.1 As the engine, when I set the control byte at slot `i < 16`, then I also
  write the mirror at `ctrl[cap + i]`, so group loads near the table end see the
  same metadata.
- 2.2.2 As the engine, when I allocate a fresh table of capacity `cap`, then every
  byte of `ctrl[0, cap+16)` reads `EMPTY` before any insert.
- 2.2.3 As the engine, when a constructor is given a capacity below 16 or not a
  power of two, then it rounds up to the next power of two `>= 16`.

## 3. Hash splitting

### 3.1 Requirements
- `h = key.hash()` (int64). The index uses the low bits: `h1 = h & (cap - 1)`.
- The control fragment uses the top 7 bits: `h2 = (int8)((h >> 57) & 0x7F)`.
  Masking to 7 bits makes the result identical whether `>>` is arithmetic or
  logical, and guarantees `h2` never collides with `EMPTY`/`DELETED`.

### 3.2 Use cases
- 3.2.1 Given a well-mixed hash, when two keys differ in either their low `n` bits
  or their top 7 bits, then they are distinguished without a full key compare.

## 4. Probing

### 4.1 Lookup (`get` / `containsKey` / removal scan)
- 4.1.1 Start `pos = h1`, `stride = 0`. Each iteration `vload16(ctrl, pos)`:
  - For each set bit of `eqMask(h2)`: slot `i = (pos + lane) & (cap - 1)`; if
    `keys[i] == key`, hit.
  - If `eqMask(EMPTY) != 0`, the key is absent — stop (miss).
  - Else `stride += 16; pos = (pos + stride) & (cap - 1)`.

### 4.2 Insert (`put`)
- 4.2.1 Single pass: track `insertSlot` = first slot whose control byte is EMPTY
  **or** DELETED seen during the probe. On any `h2` match with `keys[i] == key`,
  replace value and return. On reaching a group with an EMPTY lane, the key is
  absent — insert at `insertSlot` (reuses a tombstone if one was passed).
- 4.2.2 Inserting into an EMPTY slot increments the non-empty counter
  (`usedSlots`); inserting into a DELETED slot does not. When
  `usedSlots * 4 > cap * 3` (0.75 load, tombstones included), resize.
- 4.2.3 `resize` doubles `cap`, allocates a fresh EMPTY table, and reinserts every
  FULL entry (control byte `>= 0`) with a no-equality-check insert (all keys
  unique by construction); tombstones vanish. Published onto `this` via `#`.

### 4.3 Use cases
- 4.3.1 As a user, when I `put` two keys that collide in the same group, then both
  are stored and independently retrievable.
- 4.3.2 As a user, when I `remove` then `put` repeatedly without growth, then the
  table reuses tombstones and probe length stays bounded (compacts on resize).
- 4.3.3 As a user, when I insert at a slot near the end of the table such that the
  group wraps past `cap`, then the entry lands at `(pos + lane) & (cap - 1)` and is
  retrievable (mirror-byte correctness).
- 4.3.4 As a user, when inserts cross the 0.75 load threshold, then the table
  doubles and all entries remain retrievable.

## 5. Preserved public semantics

### 5.1 Requirements (HashMap)
- 5.1.1 Constructor `HashMap(int64 initialCapacity)`; capacity normalized per 2.2.3
  (previously required a caller-supplied power of two — now tolerant).
- 5.1.2 `put`, `get`, `containsKey`, `remove`, `count`, `operator[]`,
  `operator[]=`, `keys()`, `values()`, `entries()` keep their signatures and
  observable behavior, including the get/`[]` miss returning the V zero value and
  `remove` returning whether the key was present.
- 5.1.3 K may be class-typed or primitive; V likewise; no boxing.
- 5.1.4 Views remain rejected as K or V (`CAJETA_ERROR_VIEW_AS_CLASS_FIELD`),
  inherited from the `K[] keys` / `V[] vals` fields.

### 5.2 Requirements (ImmutableMap)
- 5.2.1 Constructor `ImmutableMap(ArrayList<Pair<K,V>>)`, last-wins duplicates,
  dense insertion-order `keyArr`/`valArr`, `get`/`containsKey`/`operator[]`/
  `count`/`isEmpty`/`keyAt`/`valAt` unchanged. The internal index becomes a
  SwissTable `ctrl` + slot→entry-index map instead of the `int64[] table` of
  `entryIndex+1`.

### 5.3 Use cases
- 5.3.1 Every existing HashMap / ImmutableMap / stream / AutoHash / primitive-key
  test passes unchanged.
- 5.3.2 The `hashmap-int`, `hashmap-string`, and new `hashmap-ro` profile benches
  build and run with correct `checkResult()`.

## 6. Stream views

### 6.1 Requirements
- `HashMapKeyStream` / `HashMapValueStream` / `HashMapEntryStream` take the `ctrl`
  array in place of `state`; the "occupied" predicate becomes `ctrl[i] >= 0`
  (FULL = top bit clear) instead of `state[i] == 1`. Snapshot/split semantics
  unchanged.

### 6.2 Use cases
- 6.2.1 As a user, when I stream `keys()` / `values()` / `entries()` after inserts
  and removes, then exactly the live (FULL) entries are yielded, in slot order.
- 6.2.2 As a user, when I split a stream for parallel traversal, then the union of
  the shares yields each live entry exactly once.
