# Spec — PageCache eviction use-after-free (XpuPageCache)

> **Root cause CORRECTED 2026-06-30.** This was filed as a "generic-class
> field-offset miscompilation." Direct diagnosis (user-space repro + JIT IR dump +
> gdb) **disproved that**: every `PageCache`/`CacheNode` struct body and GEP is
> correct in both AOT and JIT. The real defect is a **use-after-free** rooted in the
> documented v1 field-ownership model — not codegen. The old field-offset framing is
> retained only in the git history of this file.

## 1. Definition

### 1.1 Purpose
`cajeta.xpu.PageCache<K,V>` is a residency cache that **borrows** the key objects its
consumer lends it (`admit(K key, V value)` takes `key` by borrow; the consumer reuses
the same key objects across `admit`/`touch`/`isResident`/`evictedKey`). On eviction the
cache drops the victim's `CacheNode`, and the node's **synthesized drop frees the
node's `key`/`value` fields** — but those are *borrows*. So evicting a tile frees the
consumer's still-live key object, leaving a dangling pointer → SIGSEGV on the next use.
`XpuPageCacheTests` (4 tests) all trip this on the first eviction.

### 1.2 Why it happens (the model)
Per `docs/specification/lang/FieldOwnership.md` **Solution B** (as shipped): auto-drop
emits an **unconditional** free for every class-ref field; the global **live-set claim**
inside each free dispatcher makes the call idempotent, so *the first dropper to claim an
address frees it*. That handles double-free, and works for use cases 1–3 (Optional/
Pair/ArrayStream) because there the alias-holder drops at the **same scope exit** as the
owner, after all uses. PageCache violates that timing: the owned `CacheNode` is dropped
**mid-scope** (during eviction) while the consumer's key object lives on — so the node's
field-drop wins the live-set claim and frees a key the consumer still owns and uses.

`HashMap` already avoids this for its own entries with a per-slot `owned[]` bitmask
(bit0=key, bit1=val); a plain `put` leaves the bits 0 so borrowed keys/values are never
dropped. `CacheNode` is a plain class with no such tracking, so its drop frees
everything.

### 1.3 Scope
- **In scope:** make `PageCache` eviction sound — never free a borrowed key/value —
  and green `XpuPageCacheTests`, without regressing `cajeta.collection.Cache` /
  `DnsCacheTests` (which pass today because DNS keys are cache-owned and not reused).
- **Non-goals:** changing the language ownership model or the `@Borrow`-rejected
  decision; a lifetime tracker (FieldOwnership.md Phase 6+); the `HttpServer`
  `toGeneric` lambda null-deref (separate item).

### 1.4 Established facts (the diagnosis, as evidence)
- **F1** Struct + GEPs correct: `%"test.RPageCache<Tag,int32>" = { ptr, ptr, ptr, ptr,
  i32, ptr }`; `capacity@4`, `entries@1`, `evicted@5` in both `--emit=ir` and the
  crashing JIT dump. Not a layout bug.
- **F2** gdb backtrace: crash in `__cajeta_vtable_lookup` ← `Object::operator==` ← the
  `victim != a` comparison — i.e. an operand is freed memory.
- **F3** Eviction battery: `b != c` (both resident) ✓; `a != b` / `a == a` (a evicted)
  → SIGSEGV; **capacity=3 (no eviction) ✓**. The crash requires an eviction.
- **F4** Mechanism isolated with no HashMap: constructing one `CacheNode` over a
  borrowed key and dropping the node frees the key (`a == a` then SIGSEGV). Nulling the
  node's `key`/`value` before it drops → key survives.
- **F5** Arrays don't own elements: a borrowed key stored in a dropped `K[]` survives —
  buffer-free doesn't recurse into elements. (Basis for the fix.)

## 2. Observable defect (use cases)
- **UC-2.1** As a tile consumer, when I `admit` three tiles into a capacity-2 cache, the
  oldest is evicted; when I then use that evicted key object, the program SIGSEGVs
  because the cache freed it.
- **UC-2.2** As a tile consumer, `evictedKey()` returns a pointer into freed memory
  (the victim's key was freed by the node drop), so any method call / comparison on it
  faults.
- **UC-2.3** A capacity that is never exceeded (no eviction) never faults — proving the
  fault is the eviction drop, not construction or lookup.

## 3. Requirements on the fix
- **R1** Eviction MUST NOT free a key or value the consumer lent the cache; consumer
  key objects stay valid across any number of evictions.
- **R2** No memory leak: the cache must still reclaim its own internal storage (nodes /
  arrays / map) at end of life.
- **R3** Behaviour unchanged for the residency contract: LRU order, `touch` promotion,
  `evictedKey` victim reporting, `getOrDefault` fallback.
- **R4** No regression to `cajeta.collection.Cache` or `DnsCacheTests`.

## 4. Acceptance criteria
- **4.1** `XpuPageCacheTests` — all 4 (`lruEvictsOldest`, `lruEvictOrderWithTouch`,
  `touchFeedbackAndGet`, `readmitUpdatesNoEvict`) green.
- **4.2** A minimal regression test pinning the borrow contract: evict a key object,
  then use it — must not fault. (Compiler-level, `xpu`-free, so it can't silently
  return.)
- **4.3** `runtime/src/cajeta/xpu/PageCache.cajeta` restored clean and idiomatic; the
  chosen storage representation is intentional (see plan), not a workaround hack.
- **4.4** No regression: full sweep, and `Cache`/`DnsCacheTests` stay green.
- **4.5** Root cause documented in memory (supersedes the field-offset framing) so the
  ownership/eviction interaction is understood, not just patched.
