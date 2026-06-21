---
id: collection-LtmPager
applies-to: [cajeta/collection/ltm/LtmPager]
title: LtmPager — fixed-size file-backed buffer pool behind LtmBPlusTree
description: How to drive LtmPager's frame pool, page-id node graph, and the pin/LRU-evict ownership model where a fetched node is a borrowed frame valid only while resident or pinned.
---

# LtmPager&lt;K,V&gt;

`LtmPager<K,V>` is the **fixed-size, file-backed buffer pool** that makes
`LtmBPlusTree` larger-than-memory: at most `capacity` node-page images are resident at
once, the rest live on disk and are paged in on demand. It is a **support type, not an
entry point** — `LtmBPlusTree` constructs and owns one internally. Construct it directly
**only** to build a *different* page-based structure on the same paging machinery (you
then drive page layout yourself). For an ordered map, use `LtmBPlusTree`; see package
skill `cajeta/collection/ltm`.

## The one thing to get right: fetched nodes are borrowed frames

`fetch` and `allocNode` return a `LtmBPlusTreeNode<K,V>` that **belongs to a frame slot
in the pool, not to you.** It is a borrowed reference, valid only while that page stays
resident. Any later `fetch`/`allocNode` may evict the least-recently-used **unpinned**
frame — including the one you are holding — and reassign its slot to the new page. After
that, your old reference is to an image the pool has dropped (dangling).

To keep a node alive across further paging (e.g. the root-to-leaf path during a split),
**`pin` it, then `unpin` when done**:

```cajeta
import cajeta.collection.ltm.LtmPager;
import cajeta.collection.ltm.LtmBPlusTreeNode;

LtmBPlusTreeNode<K, V> n = pager.fetch(pager.getRoot());
pager.pin(n.pageId);          // n is now safe across more fetch/allocNode calls
LtmBPlusTreeNode<K, V> child = pager.fetch(n.childIds[0]);  // could evict, but not n
// ... mutate n ...
pager.markDirty(n.pageId);    // REQUIRED: pool tracks dirtiness, not the node
pager.unpin(n.pageId);        // balance every pin
```

- Eviction "frees" the victim by **reassigning `frameNode[idx]`** to the replacement —
  Cajeta drop-by-assignment, no `delete`/`free`. There is nothing for you to free; never
  free a returned node.
- `pin`/`unpin`/`markDirty` are **looked up by page id and are no-ops if the page is not
  resident.** Pin/mark *while you hold the resident node*; marking an already-evicted
  page silently does nothing and the mutation is lost.

## Construction & ownership

```cajeta
import cajeta.collection.ltm.LtmPager;
import cajeta.wire.Encoder;

// path, keyEnc, valEnc, order, pageSize, capacity
LtmPager<String, int64> pager = heap LtmPager<String, int64>(
        "/var/db/index.ltm", keyEnc, valEnc, 128, 4096, 64);
LtmBPlusTreeNode<String, int64> root = pager.allocNode(true);  // resident + dirty
pager.setRoot(root.pageId);
pager.flush();
```

- The ctor **opens the file itself** (`OpenMode.READ_WRITE`) and **moves** it (`#`) into a
  field so the fd lives for the pager's lifetime — you do not pass or manage a `File`.
- `keyEnc`/`valEnc` are **borrowed**, held for the pager's lifetime; keep them alive.
- An **empty** file gets a fresh header (`rootPageId = -1`, `nextPageId = 1`,
  `entryCount = 0`). An **existing** file's page-0 header is read back, and **the stored
  `order` overrides the passed `order`** — same rule the package skill notes for the tree.

## Pages & the page-id node graph

- The file is fixed-size pages. **Page 0 is the header** (magic `0x42504C54`, order, root
  page id, next page id, entry count). Pages ≥ 1 each hold one serialized node, at offset
  `pageId * pageSize`.
- Nodes reference children by **page id (`int64`), never pointer**: `-1` = "no page",
  `0` = header (never a real node). So a parent can be resident while its children are on
  disk. Field shapes (`keys`/`values`/`childIds`/`nextLeafId`) live in the
  `cajeta/collection/ltm/LtmBPlusTreeNode` skill.

## Methods that matter

- `fetch(int64 pageId) -> LtmBPlusTreeNode<K,V>` — resident node, reading+deserializing on
  a miss (may evict). Returns a **borrowed, unpinned** frame (see above).
- `allocNode(boolean leaf) -> LtmBPlusTreeNode<K,V>` — new page (`pageId` from
  `nextPageId`), **resident and already dirty**, borrowed/unpinned. `true` = leaf.
- `markDirty(int64 pageId)` — flag for write-back on evict/flush. **Call after every
  mutation** (the pool tracks dirtiness in its own `frameDirty[]`; setting the node's own
  `dirty` field does *not* schedule write-back).
- `pin` / `unpin(int64 pageId)` — refcounted resident lock against eviction; balance them.
- `getRoot()/setRoot(int64)`, `getOrder() -> int32`, `getEntryCount()/incEntryCount()` —
  header accessors held in memory until flushed.
- `flush()` — write all dirty resident frames + header, then `file.sync()`. Keeps open.
- `close()` — `flush()` then close the fd.

## Lifecycle, state, invariants

- **No durability until `flush()`/`close()`; no drop-on-scope close** — closing is
  explicit. Dropping a pager without `close` loses dirty frames and the live header
  (durability rule detailed in the `cajeta/collection/ltm` package skill).
- **Mutable, single-threaded, not thread/fiber-safe** — parallel frame arrays plus a
  shared LRU `clock`, no locks.
- **Capacity must exceed the max simultaneously-pinned set.** Eviction requires an
  unpinned victim; if every frame is pinned there is no victim and the eviction path
  indexes the pool at `-1` (crash). Size `capacity` above the deepest pinned root-to-leaf
  path plus any nodes a split pins at once, and never leak a pin.
- **A node must serialize within one page.** `serializeNode` packs into a single
  `pageSize` buffer with no bounds guard — oversized entries overflow it. Choose `order`
  so `order + 1` max-size encoded entries fit `pageSize`.

## Errors

No pager-specific exception types. File I/O errors propagate from the underlying `File`
(open/seek/read/write/sync). `fetch`/`markDirty`/`pin`/`unpin` on a non-resident or
unknown page id are silent no-ops (they return/ignore via the `-1` "not found" frame
index), not errors — a silently-lost write is the failure mode to watch for, not a throw.

## What it does NOT do

- No `delete`/`free` of pages, and no on-disk free list — `nextPageId` only grows; evicted
  pages are reused as frame *slots*, not reclaimed on disk.
- Does not observe node mutations — you must `markDirty`.
- Does not create parent directories or choose a page size for you.
