# collections-overhaul — every collection reviewed, corrected, and worth its hot path

## 1. Definition

A full review-and-rewrite pass over `cajeta.collection` (and the collection
surface it leans on: `Pair`, the HashMap streams). The 0.15.0
uniform-transfer conversion (uniform-transfer-semantics units 2–3) made the
containers own their elements, but it converted the ownership spellings
in place — it did not re-examine each class for correctness, efficiency, or
leftover pre-transfer scaffolding. Julian's direction (2026-08-03): "go over
every collection … be concerned about efficiency, and whether code is really
necessary or whether it can be simplified. Collections represent the hot
path, and we need to make sure that this code is something we can be proud
of."

Non-goals: the `map.update` / value-replacement design question (deferred at
uniform-transfer 4.2.4); HashMap iteration redesign (`entries()` stays a
snapshot stream); the disk-backed `ltm` package; `Sort.cajeta`'s algorithmic
internals (only its ownership interplay is in scope).

## 2. Findings driving the work (enumerated)

### 2.1 Efficiency
- 2.1.1 Miss-path allocations: `heap T[1]` mints a malloc+free for EVERY
  miss/peek/pop/empty-get — ~15 sites across HashMap, LinkedList, Heap,
  BPlusTree, RedBlackTree, ImmutableList. `stack T[1]` is verified to
  zero-init for both primitive and class T (probe 2026-08-03).
- 2.1.2 `HashMap.remove` heap-allocates the key-clearing zero (`heap K[1]`)
  purely to dodge a malloc-aliasing trap; a stack zero cannot alias malloc,
  killing both the cost and the trap.
- 2.1.3 `HashSet.remove` probes twice (containsKey + remove); the sentinel
  value returned by `map.remove` already answers membership in one probe.
- 2.1.4 `ArrayList.appendAll` / `ArrayList(#T[])` grow by repeated doubling
  inside the add loop — pre-reserving once turns O(n log n) element moves
  into O(n).
- 2.1.5 `Heap.push` inlines its grow path (ArrayList deliberately splits the
  cold grow out-of-line and documents why; Heap should match).
- 2.1.6 `Cache.evict` walks the whole LRU list although expiry is monotone
  along it (LRU order IS access order) — stop at the first non-expired node.
- 2.1.7 `Heap.pop` self-moves `data[0] #= data[0]` when popping the last
  element.

### 2.2 Duplication / simplification
- 2.2.1 HashMap's probe loop is written out four times (get, containsKey,
  remove, operator#[]); BPlusTree's leaf scan twice (get, containsKey).
- 2.2.2 `LinkedList.add` == `addTail` and `addFirst` == `addHead`, duplicated
  verbatim (~40 lines).
- 2.2.3 HashMap's ctrl-fill loop appears in fillCtrlEmpty AND resize.
- 2.2.4 Duplicated/stale comment blocks: HashMap.remove carries two doc
  comments; BPlusTree.splitLeaf repeats a title-stores line.

### 2.3 Correctness / honesty
- 2.3.1 Stale docs that now lie: `ArrayList.appendAll` says "Does NOT
  consume `other`" above a `#ArrayList<T>` formal that consumes it;
  LinkedList's class doc says "Values are NOT owned by the nodes" (they are,
  since 2.3); Cache says twice "HashMap has no no-arg constructor" (it
  does); ArrayList's doc says "no insert/remove yet".
- 2.3.2 `ArrayList.sort`'s ownership caveat ("a list holding owned elements
  must not be sorted") now condemns every `ArrayList<class T>` — under
  uniform transfer all resident elements are owned. The truth: sorting is
  bit-safe exactly when no slot has been `#=`-extracted (all bits
  uniform 1); the caveat must say that instead.
- 2.3.3 The Immutable* trio predates uniform transfer: their ctors copy
  element BORROWS out of the source (`copy[i] = src.get(i)`) while the docs
  promise "safe to share without defensive copies" — false for class T (the
  snapshot dangles when the source drops). They must own like every other
  container: consuming ctors (`#ArrayList<T>` / `#T[]`) with extraction
  moves.
- 2.3.4 Class-K instantiation landmines to document (not fix here):
  `keysInOrder` on both trees and `Collectors.toList`'s accumulator lend
  borrows into `#T` formals — fine for primitive K/T, compile errors for
  class K/T. Each gets an explicit doc note.
- 2.3.5 `CacheNode`'s ctor takes plain `V` and relies on the caller's `#`
  to set the bit; the contract is really "the node owns its value" and the
  formal should say `#V`. (The key is a deliberate borrow-alias of the
  map's canonical key — that stays, documented.)

### 2.4 Missing surface (found by real consumers — the build-tool plugins)
- 2.4.1 `ArrayList.insert(int32, #T)`, `removeAt(int32) -> #T` (flagged),
  `clear()`; bounds checks on get/set (silent OOB reads inside spare
  capacity today).
- 2.4.2 `HashSet()` no-arg ctor (HashMap has one) and `stream()` over the
  members (a set that cannot be iterated).
- 2.4.3 (landed alongside, stdlib-adjacent) `String.compareTo`,
  `Path.list()`, `Path.toString()` — added 2026-08-03 while unblocking the
  plugins; their tests belong to this overhaul's sweep.

## 3. Use cases
- 3.1 As the build-tool coverage plugin, when I `removeAt` the last element
  of a bottom-N bucket, the list shrinks by one and the removed element's
  title comes back to me (or the drop fires if I discard it).
- 3.2 As any map consumer, when I `get` a missing key in a tight loop, no
  heap allocation happens on the miss path.
- 3.3 As a cache owner with TTL armed, when `evict()` runs on a mostly-fresh
  cache, it touches only the expired tail, not every entry.
- 3.4 As a reader of the stdlib, every doc comment describes the code below
  it — including who owns what and which instantiations are supported.
- 3.5 As an ImmutableList<String> holder, my snapshot stays valid after the
  source list is dropped.
