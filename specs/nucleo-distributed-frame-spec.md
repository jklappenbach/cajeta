# nucleo-distributed-frame — distributed execution for the relational planner

## 1. Definition

### 1.1 Purpose

Cajeta already has **both halves** of a distributed analytics engine and no seam
between them:

- `cajeta.nucleo.frame` is a lazy relational planner — `Plan` builds op chains
  without running them, `Opt` rewrites them (predicate pushdown), `Exec`
  executes at a terminal, over columnar `Table<T>` with `join`, `groupBy`/`agg`.
  **Single node.**
- `cajeta-cluster` is a consistent-hash ring with a partitioned registry,
  routing, and size-aware fan-out over `cajeta-gossip` (SWIM membership +
  failure detection), with **no external coordinator** — no Zookeeper, no etcd.
- `cajeta-cloud` (specified 2026-08-01) provides the blob port; the older
  `cajeta-cloud-objectstore` is only a pattern sample with no adapter.

What is missing is the layer that makes the planner run *on* the fabric:
partitioned tables, an exchange (shuffle) operator, distribution-aware
optimization, and distributed aggregation and join.

This spec adds that layer.

### 1.2 Scope basis

The distributed-analytics stack as it is conventionally deployed:
Hadoop (HDFS, YARN, MapReduce), Hive, Spark (RDDs, DataFrames, MLlib,
Streaming), Kafka, and the operational surface around them — tuning, scheduling,
and resource management. §10 records what a second pass over that stack added
over the initial scoping.

### 1.3 How that stack maps onto cajeta

| Stack component | Cajeta today | Gap |
|---|---|---|
| HDFS — distributed storage | `cajeta-cloud-objectstore` | partitioned dataset layout (§7) |
| Zookeeper — coordination | gossip + ring give *discovery*; consensus is **designed out** (§1.3.1) | one `ObjectStore` conditional put (§7.7) |
| YARN — resource management | `cajeta-cluster` ring/routing/fan-out | task scheduling (§6) |
| MapReduce — programming model | `ParallelDriver` fork/join, **single node** | distributed shuffle (§4) |
| Hive — SQL over big data | `nucleo.frame` `Plan`/`Opt`/`Exec` | distributed execution (§5) |
| Spark — lazy distributed DAG | `nucleo.frame` **is** this, single-node | everything below |

The honest summary: cajeta is equal to that stack in planning, ahead on
*discovery*, and has no distributed execution at all.

### 1.3.1 Coordination — discovery yes, consensus designed out

Zookeeper does two separable jobs. Cajeta covers the first and **eliminates the
need for the second**:

| Zookeeper job | Cajeta |
|---|---|
| membership, liveness, service discovery | ✅ `cajeta-gossip` + `cajeta-cluster` |
| leader election, locks, consistent metadata | **not needed — see below** |

An earlier draft of this spec claimed Zookeeper was simply "not needed", which
was true only of discovery; a later draft over-corrected and called for a
consensus library. Both were wrong. The design below removes the requirement.

- **Driver election — there is none.** The driver is the *submitting* process,
  as in Spark. A dead driver kills its own query; the client retries.
- **Central scheduler — none exists.** It would only arbitrate between
  applications, and §1.5.5 makes multi-tenancy a non-goal.
- **Metadata catalog — immutable.** One manifest per dataset version, written
  once (§7.1, §7.6).
- **Write locks — none.** Copy-on-write plus an atomic manifest commit; the
  loser of the race retries.

**The one primitive this requires: a conditional put on `ObjectStore`** —
`putIfAbsent` / `putIfMatch(etag)` — giving optimistic concurrency. See §7.7.
This is how Iceberg and Delta Lake shed the Hive metastore and Zookeeper.

**Why a stale writer cannot corrupt state.** Every mutation funnels through CAS
on the manifest, so the manifest version *is* a fencing token. The classic lease
failure — win a lease, stall past expiry, wake and write — cannot occur: the
stalled writer targets version `N`, someone already committed `N`, and its write
fails. **Correctness rests on CAS at the point of mutation, never on who holds a
lease.** Leadership is therefore advisory, useful only to avoid duplicated
background work.

Consensus returns only if (a) a mutation cannot be one atomic CAS, (b) an
exclusive side effect lands outside the object store where no fencing token can
be checked, or (c) §1.5.5's multi-tenancy non-goal is reversed.

### 1.4 Scope

Partitioned tables; the exchange operator; distributed aggregation, join, and
sort; scheduling and fault tolerance; the partitioned-dataset storage format.

### 1.5 Non-goals

- **1.5.1** A SQL parser. Hive's interface is HiveQL; cajeta's is the typed
  `Table<T>` API. A SQL front-end is a separate question.
- **1.5.2** Streaming / continuous queries — **batch first, but see §10.1.**
  Kafka and Spark Streaming are a large, well-defined surface, so this is a
  deliberate deferral, not an omission.
- **1.5.3** Reimplementing HDFS. `ObjectStore` is the storage abstraction.
- **1.5.4** YARN/Hadoop wire compatibility, or running on an existing Hadoop
  cluster. This is cajeta's own fabric.
- **1.5.5** Multi-tenancy, quotas, and cluster-wide resource governance.
- **1.5.6** GPU-distributed execution.

### 1.6 Systems

`cajeta.nucleo.frame` (`Plan`, `Opt`, `Exec`, `Table`, `Join`, `Aggs`),
`cajeta.nucleo.column` (+ `ArrowFfi`), **`dev.cajeta.codec.parquet`**
(on-disk partition format — already implemented), `cajeta-cluster`,
`cajeta-gossip`,
`cajeta-cloud-objectstore`, `cajeta.io.net`, `cajeta.wire` (compression for
shuffle), `cajeta.hash` (partitioning), `cajeta.concurrent`, `dev.cajeta.unit`.

---

## 2. Feature: partitioned tables

- **2.1** When a distributed table is created, it is a set of **partitions**,
  each a local `Table<T>` on some node, and the type and schema guarantees of
  `Table<T>` are unchanged.
- **2.2** When a partitioned table's partitioning is read, its scheme is known
  — hash on named columns, range, or unpartitioned — because every optimization
  in §5 depends on knowing it.
- **2.3** When a table is partitioned by hash on a column set, rows with equal
  keys land in the same partition, using `cajeta.hash` so the assignment is
  stable across nodes and runs.
- **2.4** When partitions are unevenly sized, skew is measurable, since skew is
  the dominant cause of poor distributed performance and must be observable
  rather than inferred from wall-clock.
- **2.5** When against a single node is run, the same API works with one
  partition and no network — the local case is not a special case, mirroring
  how `cajeta-cluster` makes primavera's connection model run unchanged from
  one node to a fleet.

---

## 3. Feature: the cluster seam

- **3.1** When the engine needs the node set, it comes from `cajeta-gossip`'s
  membership view; no separate coordinator is introduced.
- **3.2** When a partition must be placed, `cajeta-cluster`'s consistent-hash
  ring decides its owner, so placement and routing reuse the existing directory
  rather than a parallel one.
- **3.3** When a node joins or leaves, partition ownership moves per the ring's
  rebalancing, and in-flight work is handled per §6.4.
- **3.4** When cluster state is queried, node count, liveness, and per-node
  load are visible to the scheduler.

---

## 4. Feature: exchange — the shuffle operator

This is MapReduce's shuffle, expressed as a plan node. It is the one primitive
that makes everything else in this spec possible.

- **4.1** When a plan requires a different partitioning than its input has, an
  **exchange** node is inserted that repartitions rows across nodes.
- **4.2** When an exchange runs, rows are routed by partition key, sent over
  `cajeta.io.net`, and reassembled — the map-side partition, the transfer, and
  the reduce-side merge.
- **4.3** When data crosses the network, it moves in the **columnar**
  representation, batched, and optionally compressed via `cajeta.wire` — a row-
  at-a-time shuffle would forfeit the columnar layout's entire advantage.
- **4.4** When an exchange is unnecessary because the input is already
  correctly partitioned, it is elided (§5.2).
- **4.5** When a shuffle exceeds memory, it spills to disk rather than failing
  — the workload this spec exists for does not fit in RAM by definition.
- **4.6** When a shuffle is running, bytes moved and per-partition skew are
  reported, because shuffle is where distributed queries go to die.

---

## 5. Feature: distribution-aware planning

- **5.1** When a plan is forced, `Opt` runs as today and then a distribution
  pass inserts the minimum set of exchanges — **exchange count is the primary
  cost driver**, not operator count.
- **5.2** When an operation's required partitioning is already satisfied, no
  exchange is inserted — a `groupBy` on the column the table is already hash-
  partitioned by is a purely local aggregation.
- **5.3** When a filter can run before an exchange, existing predicate pushdown
  does so, since filtering before a shuffle is the single highest-value rewrite
  in a distributed plan.
- **5.4** When a plan is inspected, the distributed plan is printable with its
  exchanges, partition counts, and estimated data movement — an opaque
  distributed plan is untunable.
- **5.5** When projection can be pushed into the scan, only the needed columns
  are read from storage (§7.3).

---

## 6. Feature: execution, scheduling, and fault tolerance

- **6.1** When a plan executes, it is split into **stages** at exchange
  boundaries, and each stage runs as tasks — one per partition.
- **6.2** When tasks are scheduled, they prefer the node already holding their
  input partition; moving computation to data is the point.
- **6.3** When a task fails, it is retried on another node, and repeated
  failure fails the query with the underlying cause — never a partial result
  presented as complete.
- **6.4** When a node dies mid-query, gossip's failure detector reports it and
  the affected tasks are rescheduled; the query survives a node loss.
- **6.5** When a stage's output is expensive to recompute, it can be
  checkpointed, so recovery does not replay the whole plan.
- **6.6** When a query runs, per-stage timing, rows, and bytes are collected,
  so a slow query is diagnosable.
- **6.7** When work is executed locally within a node, it uses the existing
  `ParallelDriver` fork/join machinery — intra-node parallelism is not
  reinvented.

---

## 7. Feature: partitioned dataset storage

- **7.1** When a partitioned table is written, it is stored as a set of objects
  in `ObjectStore` under a documented layout, with a manifest naming
  partitions, schema, and row counts.
- **7.2** When it back is read, partitioning is recovered from the manifest, so
  a table written hash-partitioned reads back as such and §5.2 can skip the
  exchange.
- **7.3** When a subset of columns is read, only those are fetched — columnar
  pruning is the reason to use this format.
- **7.4** When the manifest carries per-partition statistics (min/max per
  column, null counts), whole partitions are skipped by predicate — partition
  pruning, the largest win in analytical scans.
- **7.5** When a partition is written, it is **Parquet**, using the existing
  `dev.cajeta.codec.parquet` implementation — `ParquetWriter`,
  `ParquetColumnReader`, `ThriftCompactReader`, dictionary and RLE encoding,
  and column statistics all exist today. This spec **does not design a storage
  format**; Parquet is what Iceberg and Delta use and what §7.3's column
  pruning and §7.4's statistics-based partition skipping are built around.
- **7.5.1** When a Parquet partition is read, column pruning (§7.3) uses its
  column chunks and predicate pushdown (§7.4) uses its row-group statistics —
  both are properties of the format, not of this engine.
- **7.5.2** When in-memory interchange is needed, `nucleo.column`'s `ArrowFfi`
  remains the zero-copy path; Parquet is the **on-disk** representation and
  Arrow the **in-memory** one. They are complementary, not alternatives.
- **7.6** When a write fails partway, no partially-written dataset becomes
  readable — the manifest commits last.
- **7.7** When two writers commit concurrently, the manifest is written by
  **conditional put** (`putIfAbsent` / `putIfMatch(etag)`), exactly one wins,
  and the loser re-reads and retries. This is the atomic primitive the whole
  no-consensus design in §1.3.1 rests on.
- **7.8** When the object-store backend cannot guarantee conditional put, that
  is reported at configuration time, not discovered as corruption — the
  guarantee varies by provider and must be surfaced.
- **7.9** When against the `FakeObjectStore` testkit is tested, it models CAS
  **faithfully**, including losing races. A fake whose conditional put always
  succeeds would make every concurrency test lie.

---

## 8. Feature: the distributed operator set

- **8.1** When an aggregation runs, it is **partial aggregate → exchange →
  final aggregate**, so only per-partition partials cross the network. This is
  precisely MapReduce's combiner, and it is why `Aggs` must be expressible as
  partial + merge.
- **8.2** When an aggregate cannot be split into partial and merge (an exact
  median, say), that is stated and the full shuffle cost is explicit rather
  than hidden.
- **8.3** When two tables partitioned on the join key is joined, it is local
  with no exchange.
- **8.4** When one side is small, a **broadcast join** ships it to every node
  instead of shuffling the large side.
- **8.5** When neither applies, both sides are shuffled to a common
  partitioning and joined locally.
- **8.6** When the join is not inner, `Join.LEFT` and the rest keep their
  existing semantics distributed, verified by test against single-node results.
- **8.7** When globally is sorted, range partitioning by sampled quantiles
  precedes local sorts, so concatenating partitions yields global order.
- **8.8** When any distributed query is run, its result is **identical to the
  single-node result** on the same data. This is the spec's central contract
  and §10.1 makes it the acceptance test.

---

## 9. Open questions (resolve at plan time)

- **9.1** *(resolved 2026-08-01.)* Library boundary. This spans stdlib
  (`nucleo.frame`) and two external siblings (`cajeta-cluster`, `cajeta-cloud`),
  and stdlib must not depend on external libraries. The engine is therefore an
  **external library** depending on all three, with stdlib `nucleo.frame`
  gaining only the seams — a pluggable `Exec`, an `EXCHANGE` plan node (§9.2) —
  that make it extensible. **Package name:
  `dev.cajeta.dqe`**, from repo `cajeta-dqe` by the derivation in
  `datascience-platform-roadmap` §4. An earlier draft proposed
  `dev.cajeta.frame.dist`, which describes the implementation rather than the
  product and does not match the repository name Julian chose.
- **9.2** *(resolved 2026-08-01 — stdlib gains the node.)* `Plan` gets an
  `EXCHANGE` node kind in stdlib. Expressing distribution entirely outside it
  would fork the plan representation, and two plan languages is worse than one
  node kind stdlib does not itself execute.
- **9.3** **Still open — an audit, and a first-unit action.** Can every
  existing `Aggs` aggregate be expressed as partial + merge (§8.1)? Any that
  cannot become §8.2's declared exceptions. This is not a judgement call but a
  file-by-file check, and it must happen before §8's units are sequenced —
  exact median and the quantile family are the known suspects.
- **9.4** *(resolved 2026-08-01 — direct TCP for bulk.)* Shuffle moves bulk
  data over `cajeta.io.net` TCP directly; `cajeta-cluster`'s ring carries
  control only. Routing bulk shuffle through a control plane is a classic
  bottleneck and would make the ring the cluster's throughput ceiling.
- **9.6** *(resolved 2026-08-01.)* The partition format is **Parquet** via the
  existing `dev.cajeta.codec.parquet`, not a new encoding — see §7.5. Remaining
  action: confirm its maturity (reader/writer/statistics coverage) before the
  storage units open. Its README calls the writer "planned" while the code
  ships `ParquetWriter`, `ParquetColumnReader`, and `ThriftCompactReader`, so
  the README is stale and the actual coverage needs reading, not trusting.
- **9.5** *(resolved 2026-08-01 — no.)* `cajeta-cloud-objectstore` is a pattern
  **sample**: `authors:["sample"]`, `capabilities:[]` so it cannot open a
  socket, and it ships no adapter. §7 depends instead on **`cajeta-cloud`**,
  whose blob port supplies prefix listing, range reads (required by §7.3's
  column pruning), multipart, and the conditional put §7.7 needs.
- **9.7** *(resolved 2026-08-01 — tens of nodes.)* The scheduler targets
  **3–50 nodes**, stated explicitly rather than left open. That is what the
  existing `cajeta-gossip`/`cajeta-cluster` substrate supports and what a
  realistic cajeta deployment looks like; it permits the driver to hold cluster
  state in memory and keeps §6's scheduling flat rather than hierarchical.

  **Say so in the docs.** A user who brings 500 nodes must learn the limit from
  the documentation, not from a scheduler that degrades quietly. Revisiting this
  is a redesign of §6, not a tuning exercise — which is exactly why it is
  written down now.

---

## 10. Second-pass findings (2026-08-01)

The initial spec was written from a coarse scoping sweep. A closer pass over the
same stack adds the following; each is recorded rather than silently folded in.

### 10.1 Streaming is real, and it is a separate spec

Streaming is a large fraction of the surface: **Apache Kafka** (broker/producer,
topics and partitions, consumers and replicas, cluster APIs) and **Spark
Streaming** (architecture, windowed aggregation over live sources).

Cajeta has transport (`cajeta.io.net`) and membership (`cajeta-gossip`) but **no
partitioned append-only log, no consumer groups, and no offset management** —
the Kafka role is entirely absent. Recommendation: **`cajeta-cloud`'s stream port** (§12 of that spec) — a *client*
against an existing cluster, not a broker, with Kafka as one adapter beside
Kinesis. Cajeta does not reimplement Kafka; the requirement is the ability to
consume a stream. This spec stays batch. Do not let §4's exchange
operator quietly become a streaming shuffle.

**Note on coordination.** **Zookeeper** is Kafka's coordinator — but a *client*
needs none of it: the cluster brings its own, and
consumer-group membership is a broker-side protocol the client participates in.
The consensus gap in §1.3.1 is real for **this** spec's own needs (driver
election, metadata catalog, write locks), not for streaming.

### 10.2 RDDs — the layer below the relational plan

Spark's building block below DataFrames is the **RDD**: an untyped functional
API (map / filter / reduce over partitions). `nucleo.frame` is the
DataFrame analogue; the RDD analogue is `cajeta.lang.stream` plus
`ParallelDriver`'s fork/join — but **only within one node**.

- **10.2.1** When per-partition functional operations are needed below the
  relational layer, a distributed map/filter/reduce over partitions is
  available, and it is the same abstraction §6.1's stages already schedule.
- **10.2.2** When it is used, the relationship to the single-node `Stream` API
  is documented, so the local and distributed forms are recognizably the same
  idea.

### 10.3 Distributed ML — MLlib's role

**Spark MLlib** is the distributed classical-ML layer: distributed k-means and
TF-IDF, and recommendation over a partitioned corpus.

- **10.3.1** When a clustering model over a partitioned table is fitted, the
  iteration runs distributed and the estimator is the same one `ml-
  unsupervised-spec` defines — a distributed *execution strategy*, not a second
  implementation.
- **10.3.2** When TF-IDF over a distributed corpus is computed, the document-
  frequency pass is a distributed aggregate (§8.1) and produces the same result
  as `ml-recsys-spec` §8.3 single-node.
- **10.3.3** *(scope)* Which estimators earn distributed execution is a plan
  decision. Recommendation: k-means and TF-IDF first — they are the two with
  clean partial-plus-merge formulations.

### 10.4 Operational surface

Spark's operational surface carries several genuine requirements this spec
omitted. §6.2 (data locality) and §8.4 (broadcasting) already cover two.

- **10.4.1** When cluster load changes, executors can be added or removed while
  a job runs — **dynamic resource allocation**.
- **10.4.2** When a node must leave, it is **decommissioned gracefully**: its
  partitions migrate and in-flight tasks complete or are rescheduled, rather
  than relying on §6.4's failure path. A planned shutdown is not a failure and
  should not be handled as one.
- **10.4.3** When data crosses the network or spills, the **serialization
  format is explicit and measurable** — §4.3's columnar batches make this a
  property to verify, not tune blindly.
- **10.4.4** When a job is tuned, per-stage memory, spill volume, GC pressure,
  and parallelism are all observable (extending §6.6).
- **10.4.5** When a deployment mode is chosen, the cluster-management options
  are documented.

### 10.5 Hive gaps in §7's storage story

Hive covers capabilities §7 does not:

- **10.5.1** When an **external table** is registered over foreign data, a
  schema is supplied and the engine reads it in place — without copying and
  without owning its lifecycle.
- **10.5.2** When data arrives in a foreign format (delimited text, JSON), it
  is readable through the same table interface, reusing `cajeta.codec`.
- **10.5.3** When a column holds a **nested or complex type** (struct, array,
  map), it is queryable — something `Table<T>`'s record-derived schema must be
  checked against.
- **10.5.4** When a **view** is defined, it is a named lazy `Plan` reusable as
  a table — which the existing lazy planner makes nearly free.

---

## 11. Acceptance criteria (spec-level)

- **11.1** **Every distributed query returns bit-identical results to the
  single-node execution of the same plan on the same data**, across the whole
  operator set. This is the contract everything else serves.
- **11.2** A query completes correctly when a node is killed mid-execution.
- **11.3** A `groupBy` on an already-correctly-partitioned table inserts
  **zero** exchanges, asserted by inspecting the plan (§5.2).
- **11.4** A broadcast join on a small right side moves bytes proportional to
  the small side, not the large one (§8.4).
- **11.5** A shuffle larger than memory completes by spilling (§4.5).
- **11.6** A partially-failed write leaves no readable dataset (§7.6).
- **11.7** Partition pruning by manifest statistics demonstrably reduces bytes
  read (§7.4).
- **11.8** The same code runs on one node and on many with no source change
  (§2.5).
