---
id: collection-graph-Digraph
applies-to: [cajeta/collection/graph/Digraph]
title: Digraph<N> — interned directed graph with CSR adjacency
description: The foundational directed-graph core — intern node payloads once, then traverse dense int32 ids over flat CSR arrays; multi-root reachability, BFS/DFS orders, Kahn topological sort, cycle detection, and Kosaraju SCC.
---

# Digraph&lt;N&gt;

The directed-graph core in `cajeta.collection.graph`. Call graphs, dependency
graphs, and property-graph layers build on it; the `dev.cajeta.graph`
analytics library is its intended heavy consumer.

**Decide fast:**
- You have payloads (names, keys, records) and directed relations → `intern`
  each payload once, keep the `int32` ids, `addEdge(from, to)`.
- "Which nodes can these roots reach?" → `reachableFrom(int32[] roots)` — a
  mask, multi-root in ONE traversal.
- Visit order → `bfsOrder(root)` / `dfsOrder(root)` (preorder).
- Dependency order / cycle check → `topoOrder()` (Kahn) / `isCyclic()`.
- Cycle membership / condensation → `sccLabels()` / `sccCount()` (Kosaraju).

## The shape: intern once, ids everywhere

The graph is the **single owner** of its node payloads (`intern(#N)` — the
`#` surrenders the value), and every operation after interning speaks dense
`int32` ids over flat arrays. Adjacency is CSR (offsets + targets, built
lazily by counting sort on the first query after a mutation), so traversal is
cache-linear with no per-edge objects and no boxing — a representation a
`@Kernel` could consume directly.

## The N contract

`N` must be a **class type** with `int64 hash()` (every class inherits it;
`String`'s is content-based) and `boolean equals(N)`. Interning matches hash
first, confirms with `equals` — a collision costs a comparison, never a wrong
id. Box primitives (`Int32`, …) to use them as nodes.

## Worked example

```cajeta
import cajeta.collection.graph.Digraph;
import cajeta.lang.String;

Digraph<String> g = heap Digraph<String>();
int32 a = g.intern("a");          // payload surrendered; id returned
int32 b = g.intern("b");
int32 c = g.intern("c");
g.intern("a");                    // dedup: same id as `a`, offered value dropped
g.addEdge(a, b);
g.addEdge(b, c);

int32[] roots #= heap int32[1];
roots[0] = a;
int8[] seen #= g.reachableFrom(roots);   // mask: seen[c] == 1

int32[] order #= g.topoOrder();          // a, b, c
boolean dag = g.isCyclic() == false;     // true
```

## Semantics worth knowing

- **`intern` is add-or-get.** Offering an equal payload consumes (drops) the
  offered value and returns the existing id. `idOf` is the borrow-only probe
  (-1 when absent).
- **`addEdge` ignores unknown ids** rather than trapping — graphs assembled
  from external data (parsed call graphs, wire formats) meet dangling
  references routinely, and dropping the edge is the wanted degradation.
- **Mutate freely, query freely** — the CSR indexes rebuild lazily after any
  `intern`/`addEdge`; queries never mutate, so a built graph is safe to share
  read-only.
- **`topoOrder()` on a cyclic graph is short**: members of cycles are absent,
  and `isCyclic()` is exactly that length test.
- **Multi-root is the primitive**: `reachableFrom` takes an id array and
  charges one traversal total, not one per root.

## Sharp edges

- `intern`/`idOf` scan hashes linearly (exact-hash hits then `equals`).
  Fine to tens of thousands of nodes; a hashed index is a planned upgrade,
  not a contract change.
- `sccLabels()` labels ascend in a **topological order of the condensation**:
  every cross-component edge goes smaller→larger label, so processing
  components by ascending label is dependency-first by construction.
- `dfsOrder` is preorder with successors in adjacency order; it is iterative
  (explicit stack), so deep chains cannot overflow.
