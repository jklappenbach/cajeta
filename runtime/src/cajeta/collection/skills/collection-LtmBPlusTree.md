---
id: collection-LtmBPlusTree
applies-to: [cajeta/collection/ltm/LtmBPlusTree]
title: LtmBPlusTree — disk-backed ordered map (construct, put/get, close-for-durability)
description: How to construct and use cajeta.collection.ltm.LtmBPlusTree — the file-backed ordered map: heap ctor (path, keyEnc, valEnc, order, capacity), encoders, and the must-close()/flush() durability lifecycle.
---

# LtmBPlusTree<K,V> — the access point for the disk-backed ordered map

This is the **main entry point** of `cajeta.collection.ltm` (it owns an `LtmPager`
internally — you never construct that yourself). Use it when you want an ordered
key→value index that **outgrows RAM**: every node is a page on disk, paged in on demand,
with at most `capacity` pages resident. If your data fits in memory, use
`cajeta.collection.BPlusTree` instead.

For package routing, the entry-vs-support inventory, and the internal pager/page
collaboration, read the package skill `cajeta/collection/ltm` first; this skill is the
per-type construction/method/lifecycle detail.

## Construct it — there is only one constructor

```cajeta
import cajeta.collection.ltm.LtmBPlusTree;
import cajeta.wire.Encoder;

Encoder<int32> ke = heap I32Enc();   // your Encoder<K> impl
Encoder<int32> ve = heap I32Enc();   // your Encoder<V> impl

// (path, keyEnc, valEnc, order, capacity)
LtmBPlusTree<int32, int32> t =
    heap LtmBPlusTree<int32, int32>("/var/db/users.idx", ke, ve, 4, 8);

t.put(50, 100);
int32 v = t.get(50);     // 100
t.close();               // durability boundary — flush + release the fd
```

`LtmBPlusTree(String path, Encoder<K> keyEnc, Encoder<V> valEnc, int32 order, int32 capacity)`

- Opens **or creates** the index file at `path`. A fresh file is laid out with branching
  factor `order`; an existing file is reopened from its header and **the stored `order`
  wins — the passed `order` is ignored on reopen**. (Internally the pager opens the
  `File` with `READ_WRITE` and *moves* it (`#`) into a field; you do not manage the fd.)
- `keyEnc` / `valEnc` are **borrowed**, not consumed — the tree holds the references for
  its whole lifetime, so keep them alive at least as long as the tree. They come from
  `cajeta.wire.Encoder`; `encode`/`decode` return heap-owned (`#`) buffers/values that
  the pager consumes internally (you never see them). See `cajeta/wire/Encoder`.
- `capacity` is the **resident-page budget**, not an entry cap: at most `capacity` page
  images are in memory at once; the on-disk tree is unbounded. A capacity far below the
  page count is fine.
- Choose `order` so `order + 1` max-size encoded entries fit one page (default page size
  4096).
- This does **not** create parent directories — `path`'s directory must exist.

## The methods that matter

| Method | Returns / semantics |
|--------|---------------------|
| `void put(K key, V value)` | Insert or update. O(log n); splits leaf up to a new root on overflow. |
| `V get(K key)` | Bound value, or **the type's zero value on a miss** (not null, no exception). |
| `boolean containsKey(K key)` | Presence test — use before `get` when zero is a valid stored `V`. |
| `K min()` | Smallest key, or the type's zero value if empty. |
| `int64 count()` | Number of entries. |
| `boolean isEmpty()` | True when nothing has been inserted (no root page yet). |
| `void flush()` | Write dirty resident pages to disk, **keep the file open**. |
| `void close()` | Flush **and** release the file; call before discarding the index. |

There is **no `remove`/`erase`** (deletion is deferred — this is an insert/lookup index)
and **no public iterator/range scan** (`iterator()`, `entries()` don't exist; leaves are
internally linked but no cursor is exposed). Misses never throw and never return null.

## Lifecycle — the load-bearing rule

Writes land in resident page frames and the header **in memory only**. **Nothing is
durable until `flush()` or `close()`.** There is **no drop-on-scope close** — dropping the
tree without closing loses unflushed dirty pages and the live header (root id, next-page
id, entry count). Always `close()` before discarding; reopen rebuilds state from the
header, which is why a clean close is what makes persistence-across-reopen work. After
`close()` the tree must not be used again.

## State, ordering, errors

- **Mutable, single-owner, not thread/fiber-safe** — do not share one instance across
  fibers without external synchronization (it mutates an internal buffer pool).
- Keys are ordered by `<` / `>` on `K`; equality is derived as `!(a<b) && !(b<a)`.
  Primitive `K` works today; a class `K` must define `operator<` / `operator>` (v1 limit).
- No exceptions are raised from the map operations themselves; misses return zero values.
  Failures surface from the underlying `LtmPager`/file I/O (see `cajeta/collection/ltm/LtmPager`).

## Minimal round-trip (mirrors the persistsAcrossReopen test)

```cajeta
import cajeta.collection.ltm.LtmBPlusTree;
import cajeta.wire.Encoder;

LtmBPlusTree<int32, int32> t =
    heap LtmBPlusTree<int32, int32>(path, heap I32Enc(), heap I32Enc(), 4, 8);
int32 i = 0;
while (i < 200) { t.put(i, i * 3); i = i + 1; }
t.flush();
t.close();                                   // durable on disk

// fresh tree, cold cache, same file — stored order wins, all keys recovered
LtmBPlusTree<int32, int32> t2 =
    heap LtmBPlusTree<int32, int32>(path, heap I32Enc(), heap I32Enc(), 4, 8);
int32 got = t2.get(60);                      // 180
t2.close();
```

## Pointers

- Package routing & collaboration: `cajeta/collection/ltm`
- Buffer pool / pages / header / pin-evict internals: `cajeta/collection/ltm/LtmPager`
- Encoders you must supply: `cajeta/wire/Encoder`
- In-memory equivalent: `cajeta/collection/BPlusTree`
