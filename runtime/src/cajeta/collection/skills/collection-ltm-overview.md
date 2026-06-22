---
id: collection-ltm-overview
applies-to: [cajeta/collection/ltm]
title: cajeta.collection.ltm — disk-backed larger-than-memory B+ tree
description: Routing and ownership map for LtmBPlusTree over LtmPager over LtmBPlusTreeNode — the file-backed, larger-than-memory ordered map.
---

# cajeta.collection.ltm — larger-than-memory ordered map

Use this package when you need an **ordered key→value index that outgrows RAM**. It is
the same B+ tree shape as `cajeta.collection.BPlusTree`, but every node is a fixed-size
**page on disk**, paged in on demand. At most `capacity` pages are resident at once, so
the map can far exceed memory. If your data fits in RAM, use `cajeta.collection.BPlusTree`
(or `HashMap`) instead — this package pays for file I/O and serialization on every miss.

## What you instantiate vs. what you receive

| Type | Role |
|------|------|
| `LtmBPlusTree<K,V>` | **Entry point.** The algorithm + public map API. This is the only type you normally construct. |
| `LtmPager<K,V>` | Support. File-backed buffer pool the tree owns internally; constructed *for* you by `LtmBPlusTree`. Use directly only to build a different page-based structure. |
| `LtmBPlusTreeNode<K,V>` | Support. Plain in-memory image of one page; you only touch it if you call `LtmPager` directly. |
| `cajeta.wire.Encoder<T>` | Support (from `cajeta.wire`). You supply two: one for keys, one for values. |

## Routing — task → where to start

- **Open/create an index, put/get** → construct `LtmBPlusTree<K,V>`; see example below.
- **Make writes durable** → `flush()` (keeps file open) or `close()` (flush + release fd).
- **Smallest key / size / emptiness** → `min()`, `count()`, `isEmpty()`.
- **Delete a key** → *not provided.* This is an insert/lookup index; deletion is deferred,
  same as the in-memory `BPlusTree`. There is no `remove`/`erase`.
- **Range scan / iterate in order** → *no public iterator in v1.* Leaves are singly linked
  (`nextLeafId`), but the tree exposes no cursor; do not look for `iterator()`/`entries()`.
- **Build a non-B+tree page structure on disk** → use `LtmPager` directly (it is generic
  over page images), but you must drive serialization layout yourself.

## Construction & ownership

```cajeta
import cajeta.collection.ltm.LtmBPlusTree;
import cajeta.wire.Encoder;

Encoder<int32> ke = heap I32Enc();   // your Encoder impls (one per format)
Encoder<int32> ve = heap I32Enc();

// path, keyEnc, valEnc, order (branching factor), capacity (resident pages)
LtmBPlusTree<int32, int32> t =
    heap LtmBPlusTree<int32, int32>("/var/db/users.idx", ke, ve, 128, 256);

t.put(50, 100);
int32 v = t.get(50);     // 100; a miss returns the zero value, NOT a sentinel
t.close();               // <-- durability boundary: flush + close the fd
```

- **`capacity` is the resident-page budget**, not an entry cap. The tree holds at most
  `capacity` `LtmBPlusTreeNode` images at once and evicts LRU-unpinned frames to disk; the
  on-disk tree is unbounded. A capacity far smaller than the page count is fine (and is
  exactly what the tests exercise).
- **`order` (branching factor)** is fixed at first creation and stored in the file header.
  Reopening an existing file **ignores the passed `order` — the stored one wins.** Choose
  `order` so `order + 1` max-size encoded entries fit one page (default page size 4096).
- **`Encoder`s are borrowed**, held for the tree's lifetime; keep them alive at least as
  long as the tree. `Encoder.encode`/`decode` each return heap-owned (`#`) buffers/values —
  the pager consumes those internally; you never see them.

## Durability & lifecycle — the load-bearing rule

Writes land in resident page frames and the header *in memory*. **Nothing is durable until
`flush()` or `close()`.** Dropping the tree without closing loses unflushed dirty pages and
the live header (root id, next-page id, entry count). Always `close()` before discarding the
index; reopen reads `order`/root/next/entry back from the header, so a clean close is what
makes "persist across reopen" work.

There is **no drop-on-scope close** — closing is explicit. Internally the pager opens the
`File` with `OpenMode.READ_WRITE` and *moves* it (`#`) into a field so the fd survives the
constructor; you do not manage the file handle.

## Misses return the zero value (no nulls, no exceptions)

`get(key)` and `min()` return the **type's zero value** when the key/tree is absent, not
null and not a thrown exception. For a `V` where zero is a legitimate stored value, guard
lookups with `containsKey(key)` first — there is no `Optional`-style result.

## Ordering & key-type limits

Keys are ordered by `<` / `>` on `K` (like `cajeta.collection.BPlusTree` and
`cajeta.lang.Math`). Primitive `K` works today; a class `K` must define `operator<` /
`operator>` — the v1 limitation. Equality is derived as `!(a<b) && !(b<a)`.

## How the pieces collaborate (internal flow)

`LtmBPlusTree` runs the algorithm and calls its private `LtmPager` for all storage:
`fetch(pageId)` to page a node in (evicting an LRU-unpinned victim, writing it back if
dirty), `allocNode(leaf)` to create a new page, `markDirty(pageId)` after mutating, and
`pin`/`unpin` to keep a page resident. During `put`, the tree records the root-to-leaf path
and **pins** those pages so a split's `allocNode` can't evict nodes it is still editing,
unpinning them at the end. Reads hold no page across a `fetch`, so they need no pinning.
Nodes reference children by **page id (`int64`), never pointer** (`-1` = "no page",
`pageId 0` = file header), so a parent can be resident while a child is on disk. Eviction
"frees" a page by **reassigning the frame slot** — Cajeta's drop-by-assignment; there is no
`delete`/`free` here.

## Pointers

- Algorithm + map API detail: class skill for `cajeta/collection/ltm/LtmBPlusTree`.
- Buffer pool, pages, header, (de)serialization, pin/evict: `cajeta/collection/ltm/LtmPager`.
- Page image fields: `cajeta/collection/ltm/LtmBPlusTreeNode`.
- Encoders you must supply: `cajeta/wire/Encoder`.
- In-memory equivalent (data fits in RAM): `cajeta/collection/BPlusTree`.
