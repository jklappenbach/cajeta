---
id: collection-ordered-trees-component
applies-to: [cajeta/collection/BPlusTree, cajeta/collection/RedBlackTree, cajeta/collection/BPlusTreeNode, cajeta/collection/RedBlackNode]
title: Ordered (sorted-key) maps — B+ tree vs red-black tree
description: Choosing and using cajeta's in-memory sorted-key maps (BPlusTree, RedBlackTree) and their internal node types.
---

# Ordered maps: B+ tree vs red-black tree

Two in-memory **sorted-key** maps. Reach for them when you need keys kept in
order — `min`, `max` (RB only), ascending key listing, or a future range scan —
which `HashMap` cannot give you. Both are insert/lookup only today:
**deletion is deferred** in both, so there is no `remove`/`delete`.

| You need… | Use |
|---|---|
| range scans / shallow cache-friendly index / on-disk later | **`BPlusTree`** (linked leaves, order-32 branching) |
| classic balanced BST, `max` as well as `min` | **`RedBlackTree`** (CLRS, parent-pointered) |
| unordered key→value, fastest point lookup | not here — `cajeta.collection.HashMap` |
| larger-than-memory / disk-backed ordered map | not here — `cajeta.collection.ltm.LtmBPlusTree` (the in-memory `BPlusTree` is its shape basis) |
| delete an entry | not supported in either yet |

Default to `BPlusTree` for index-style workloads; pick `RedBlackTree` when you
need `max()` or a textbook BST. Both are `O(log n)` put/get.

## Members and roles

- **`BPlusTree<K, V>`** / **`RedBlackTree<K, V>`** — the entry points you
  instantiate and call. All public map operations live here.
- **`BPlusTreeNode<K, V>`** / **`RedBlackNode<K, V>`** — internal support
  types. The tree heap-allocates, links, splits, rotates, and recolours these
  itself; their fields are `public` only so the tree's own code can rewrite
  links directly. **You do not construct or touch nodes as a caller** — never
  appear in the public API surface except as `private` internals.

## Ordering — the one rule that gates K

Both order keys by the `<` / `>` operators on `K`, exactly like
`cajeta.lang.Math.min`/`max`. Equality is "neither `<` nor `>`".

- **Primitive `K`** (`int32`, `int64`, `float`, …) works today.
- **Class `K`** needs `operator<` / `operator>` overloads. These do not yet
  link through template specializations (the v1 limitation `Math` documents), so
  a class key compiles but won't compare correctly until that lands. A class K
  should keep "neither `<` nor `>`" consistent with its `==`.

## Usage — instantiate, put, query

```cajeta
import cajeta.collection.BPlusTree;
import cajeta.collection.ArrayList;

BPlusTree<int32, String> index = heap BPlusTree<int32, String>();
index.put(42, "answer");
index.put(42, "same key updates in place");  // count stays 1

String hit = index.get(42);             // value, or K/V zero value on miss
boolean known = index.containsKey(7);   // disambiguate miss from present-and-zero
int32 lo = index.min();                 // smallest key

ArrayList<int32> keys #= index.keysInOrder();  // owned, see below
```

`RedBlackTree<K, V>` is interchangeable for this and adds `max()`:

```cajeta
import cajeta.collection.RedBlackTree;

RedBlackTree<int32, String> idx = heap RedBlackTree<int32, String>();
idx.put(42, "answer");
idx.put(7, "lucky");
int32 hi = idx.max();   // 42  (BPlusTree has no max())
```

Shared surface: `put` / `get` / `containsKey` / `min` / `keysInOrder` /
`count` (returns `int64`) / `isEmpty`. `RedBlackTree` additionally has `max`.

## Miss / empty semantics

`get`, `min`, and `max` return the **type's zero value** when the key is absent
or the tree is empty (the stdlib miss-path convention) — they do not throw and
do not return null for value types. Use `containsKey` to tell "absent" from
"present with a zero value".

## Ownership across the boundary

- **`keysInOrder()` returns `#ArrayList<K>` — ownership transfers to you.** The
  `#` marks the move; the caller owns and is responsible for the returned list.
  Iterate it with `count()` / `get(int32)`.
- `put(K key, V value)` — keys/values are stored by the tree; pass primitives or
  the values you intend the map to hold. No `#` on these parameters.
- The tree **owns all its nodes**. They are an implementation detail: do not
  retain, free, or hand-build `BPlusTreeNode` / `RedBlackNode`.

## What these do NOT do

- **No `remove` / deletion** in either tree (deferred until the error model and
  test harness land). Treat both as insert/update/lookup ordered maps.
- **`BPlusTree` has no `max()`** — only `min()`. Use `RedBlackTree` if you need
  the largest key.
- No range-scan API yet, though `BPlusTree`'s leaves are linked in ascending
  order to make one cheap later; today the ordered walk is `keysInOrder()`.
- No iterator/cursor type — ascending traversal is the materialized
  `keysInOrder()` list.

See `cajeta.collection.ltm.LtmBPlusTree` for the disk-backed, larger-than-memory
variant, `cajeta.collection.HashMap` for unordered lookup, and `cajeta.lang.Math`
for the same `<` / `>` ordering convention.
