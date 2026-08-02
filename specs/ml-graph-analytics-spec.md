# ml-graph-analytics — networks, centrality, and community detection

## 1. Definition

### 1.1 Purpose

Cajeta has no graph analytics. The name collisions are misleading and worth
stating up front: `cajeta.gfx.RenderGraph` is a GPU render-pass graph,
`research/sigraph/` is SIGGRAPH rendering papers, and neither is graph theory.
There is no `Graph` type, no adjacency structure, no traversal, no centrality,
and no community detection anywhere in the ecosystem.

This spec covers the network-analysis surface: graph construction, traversal and
shortest paths, the centrality family, and modularity-based community detection.

### 1.2 Scope basis

The standard network-analysis surface — graph construction from edge lists,
traversal and shortest paths, the centrality family, and modularity-based
community detection — scoped against NetworkX and python-louvain.

### 1.3 The parity oracles

- **NetworkX** — graph construction and the centrality family.
- **python-louvain** (`community`) — `best_partition` and its modularity
  objective.

Pin versions in the plan. NetworkX is well maintained and a credible oracle;
python-louvain is a single-purpose package and should be treated with the same
caution §1.3 of `ml-recsys-spec.md` applies to Surprise.

### 1.4 Scope

Directed and undirected graphs with weights and attributes; construction from
edge lists; traversal, connectivity, and shortest paths; degree, eigenvector,
PageRank, betweenness, and closeness centrality; Louvain community detection and
modularity.

### 1.5 Non-goals

- **1.5.1** Graph neural networks. A distinct architecture family with no
  consumer here (`cajeta-ml-v3-spec` §13.7.2).
- **1.5.2** Graph *drawing* and layout. Cajeta has no charting library and
  whether it should is an open product question (§8.1), not something this spec
  assumes.
- **1.5.3** Graph databases, persistence formats beyond edge lists, and
  distributed/out-of-core graphs.
- **1.5.4** Dynamic/temporal graphs.
- **1.5.5** Flow algorithms (max-flow, min-cut) and matching. No consumer yet.

### 1.6 Systems

`cajeta.nucleo.sparse.CsrMatrix` (sparse adjacency),
`cajeta.nucleo.frame.Table` (edge lists), `cajeta.math.Tensor`,
`cajeta.math.linalg.LinAlg` (power iteration for §4.3),
`cajeta.collection` (queues, priority queues for §3),
`cajeta.math.random.Generator`, `dev.cajeta.unit`.

---

## 2. Feature: graph construction and representation

- **2.1** When an empty graph and add nodes and edges is created, both are
  stored, and adding an edge between unknown nodes creates them — NetworkX's
  behaviour.
- **2.2** When an undirected graph is built, an edge `(u,v)` is the same edge
  as `(v,u)`; in a directed graph they are distinct.
- **2.3** When a graph from an edge-list `Table` naming source and target
  columns is built, the result is the graph, with any remaining columns
  optionally attached as edge attributes — NetworkX's `from_pandas_edgelist`.
- **2.4** When edges carry weights, every algorithm that accepts a weight names
  which attribute it uses, and unweighted is the default rather than an
  implicit weight of 1 hidden in the code.
- **2.5** When node identifiers are arbitrary (strings, integers, non-
  contiguous), they are mapped to contiguous internal indices and back, so
  external identity survives a round trip.
- **2.6** When a duplicate edge to a simple graph is added, it updates rather
  than duplicating; a multigraph keeps both and distinguishes them by key.
- **2.7** When basic properties is read, node count, edge count, degree per
  node, density, and neighbor iteration are all available.
- **2.8** When the graph is large and sparse, adjacency is backed by
  `CsrMatrix` rather than a dense matrix, so memory is proportional to edges
  rather than nodes squared.
- **2.9** When for the adjacency matrix explicitly is asked, the result is it,
  with the node ordering documented and stable.

---

## 3. Feature: traversal, connectivity, and shortest paths

Not a headline feature, but §4.4's betweenness is defined over all-pairs
shortest paths and cannot be built without them.

- **3.1** When breadth-first or depth-first from a node is traversed, the
  result is the visit order deterministically for a given graph.
- **3.2** When for connected components is asked, the result is them; for
  directed graphs, weakly and strongly connected are distinguished rather than
  conflated.
- **3.3** When the shortest path between two nodes is requested, unweighted
  graphs use BFS and weighted graphs use Dijkstra, returning both the path and
  its length.
- **3.4** When a graph has negative weights, Dijkstra is refused with the
  reason named rather than returning a silently wrong answer.
- **3.5** When no path exists between two nodes, that is an explicit answer,
  not an infinite or zero distance.
- **3.6** When all-pairs shortest paths are requested, the implementation is
  documented as to complexity, since §4.4 depends on it and this is the scaling
  wall of the whole spec.

---

## 4. Feature: centrality

- **4.1** When degree centrality is computed, each node's value is its degree
  divided by `n−1` — the proportion of nodes it is connected to.
- **4.2** When degree centrality on a directed graph is computed, in-degree and
  out-degree variants are both available and named.
- **4.3** When eigenvector centrality is computed, it is found by power
  iteration on the adjacency matrix with configurable `maxIter` (default 100)
  and `tol` (default 1e-6), and an optional weight attribute — matching
  NetworkX's signature.
- **4.4** When eigenvector centrality fails to converge within `maxIter`, it
  raises or warns loudly; it never returns the last iterate as though it had
  converged.
- **4.4.1** When **PageRank** is computed, the result is the damped stationary
  distribution with a configurable damping factor (default 0.85), `maxIter`, and
  `tol`, matching NetworkX's `pagerank` signature, with optional personalization
  and edge weights.
- **4.4.2** When the graph has **rank sinks** — nodes with no outbound edges,
  which are common in a dependency graph where leaf utilities import nothing —
  their score is redistributed rather than absorbed. This is the whole reason
  damping exists, and the difference from §4.3's undamped eigenvector
  centrality.
- **4.4.3** When PageRank runs on a directed graph, direction is respected;
  reversing the graph ranks by outbound influence instead, and both are
  reachable without rebuilding.
- **4.5** When betweenness centrality is computed, each node's value is the
  fraction of all-pairs shortest paths passing through it, computed by Brandes'
  algorithm rather than naive enumeration.
- **4.6** When normalized betweenness is requested, values are divided by
  `2/((n−1)(n−2))` for undirected graphs, matching NetworkX's `normalized=True`
  default.
- **4.7** When `k` for betweenness is set, it is estimated from `k` sampled
  source nodes under a seed, and the result is documented as an approximation
  rather than presented as exact.
- **4.8** When endpoints are included, they are counted in the shortest-path
  totals, matching the `endpoints` flag.
- **4.9** When closeness centrality is computed, it is the reciprocal of mean
  shortest-path distance, with the disconnected-graph convention documented.
- **4.10** When the graph is disconnected, every centrality measure states its
  behaviour per component rather than silently producing zeros or infinities.

---

## 5. Feature: community detection

- **5.1** When Louvain community detection is run, the result is a node-to-
  community assignment produced by greedy modularity optimization.
- **5.2** When modularity for a partition is computed, the result is the score,
  so partitions from any source can be compared.
- **5.3** When the resolution parameter is set, it scales the null-model term,
  and the default of 1.0 reproduces standard modularity.
- **5.4** When a seed is supplied, the partition is reproducible — Louvain's
  result depends on node visit order, so an unseeded run is not reproducible
  and this must be explicit rather than surprising.
- **5.5** When an initial partition is supplied, the algorithm starts from it.
- **5.6** When edge weights is used, modularity is computed over weights rather
  than counts.
- **5.7** When the result is read, community sizes and the count are directly
  available, not something to tabulate by hand.

---

## 6. Feature: structural measures

- **6.1** When the clustering coefficient is computed, both per-node and graph-
  average forms are available.
- **6.2** When graph density is computed, it is edges over possible edges, with
  the directed and undirected forms distinguished.
- **6.3** When degree distribution is computed, the result is it in a form
  ready for `Stats.histogram`.
- **6.4** When for the diameter or average path length is asked, disconnected
  graphs are handled under a documented rule rather than returning infinity.

---

## 7. Feature: interoperation

- **7.1** When my edge list is a `nucleo.frame.Table`, §2.3 consumes it
  directly, with no manual conversion.
- **7.2** When centrality results are wanted as data, they come back in a form
  that joins back onto a node table by identifier.
- **7.3** When a graph exists, its sparse adjacency can be obtained as a
  `CsrMatrix` for use with `LinAlg`, so spectral methods are reachable without
  a private conversion.

---

## 8. Open questions (resolve at plan time)

- **8.1** *(resolved — charting now exists.)* This item recorded that cajeta
  had "no charting library and none planned" and that whether to build one was
  an open product decision. **That decision was taken: `cajeta-chart-spec`
  defines `dev.cajeta.chart`.** Drawing stays out of scope here — this library
  emits graph *data* and chart renders it — but the question is closed, not
  pending.
- **8.2** *(resolved — see roadmap §4.)* **`dev.cajeta.graph`**, a separate
  library depending only on stdlib. Graph analytics is not machine learning and
  uses the estimator protocol nowhere.
- **8.3** *(resolved 2026-08-01 — exact by default, sampled available.)*
  Betweenness ships exact via Brandes, with the sampled form (§4.7) exposed and
  the crossover documented. This remains the spec's main scaling risk, so the
  crossover must be *measured* rather than asserted.
- **8.4** *(resolved 2026-08-01 — contiguous internally.)* The graph maps node
  identity to contiguous indices internally, with a bidirectional identity map
  at the boundary. It keeps every algorithm array-shaped, which is what makes
  the centrality family tractable.
- **8.5** *(new 2026-08-01 — from `cajeta-rag` §12.6.)* `cajeta-rag`'s relation
  graph needs **typed edges** — `imports`, `calls`, `links-to`, `cites` — and
  this library models edges with attributes, not types. Absorb typed edges here
  rather than letting rag build a second graph. Small addition; confirm the
  representation when this spec's plan opens.

---

## 9. Acceptance criteria (spec-level)

- **9.1** Centrality values pin against the pinned NetworkX version on a fixed
  graph; Louvain pins against python-louvain modularity for a seeded partition.
- **9.2** Every centrality measure is verified on a hand-checkable graph (a
  star, a path, a clique) where the correct values are known by inspection, not
  only against the oracle.
- **9.3** Directed and undirected behaviour is separately tested for every
  algorithm that distinguishes them.
- **9.4** Disconnected graphs are tested for every measure, since that is where
  centrality definitions silently disagree.
- **9.5** Seeded runs are reproducible across runs and thread counts.
- **9.6** Memory is proportional to edges, not nodes squared, verified on a
  sparse graph large enough that a dense adjacency would not fit.
