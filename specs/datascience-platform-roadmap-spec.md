# Spec: Cajeta data-science platform roadmap (`datascience-platform-roadmap`)

## 1. Definition

### 1.1 Purpose

A **roadmap spec** — a master index, not an implementable feature. It groups and
sequences the eleven specs produced by the data-science scoping pass
(2026-07-31 → 08-01), answering one question: **what goes in stdlib, what goes
in `dev.cajeta.ml`, and what needs to be new.**

Companion to `research-platform-roadmap-spec.md`, which covers the *research
platform* axis (PyTorch/JAX table stakes — autodiff, modules, distributed
training). This covers the **data-science/analytics** axis: the sklearn,
statsmodels, NetworkX, Spark, and matplotlib roles. The two overlap only at
`dev.cajeta.ml`.

### 1.2 The placement doctrine already exists

`cajeta-ml-v3-spec` §1 settled it: *"core, foundational libraries in stdlib,
keeping it light"* — stdlib keeps **math** (Tensor/linalg/stats), **data**
(frame/column/sparse), and the compiler-integrated differentiation; the neural
framework moves out to a library. This roadmap applies the same rule to
everything the scoping pass surfaced rather than inventing a new one.

### 1.3 The placement test

Three questions, in order:

1. **Is it domain-neutral** — useful to someone who will never fit a model?
   → **stdlib**. Distances, distributions, XML, colour spaces qualify.
2. **Is it the estimator surface** — `fit`/`predict` over feature matrices,
   conforming to `Estimator`/`Predictor`/`Transformer`?
   → **`dev.cajeta.ml`**.
3. **Otherwise** → **its own library**. Different data shape, different oracle,
   different consumers, or an evolving/adversarial surface.

### 1.4 Non-goals

- **1.4.1** Re-deciding `cajeta-ml-v3`'s consolidation.
  `nucleo.nn`/`nucleo.optim` move into `dev.cajeta.ml` and leave stdlib in
  v0.14.0. Settled.
- **1.4.2** Implementation sequencing within any one spec. That is its plan's
  job.
- **1.4.3** The research-platform axis. See the companion roadmap.

---

## 2. Group A — stdlib (`cajeta.*`) additions

Everything here passes test §1.3.1: domain-neutral, stable, and with consumers
beyond data science.

| Addition | Package | From | Why stdlib |
|---|---|---|---|
| **Distance & similarity kernels** — euclidean, manhattan, chebyshev, minkowski, cosine; `pdist`/`cdist` | `cajeta.math` | 3 specs — see §5.1 | pure math over tensors; **three separate libraries need it** |
| Binomial / Bernoulli / Poisson — PMF, CDF, sampling | `cajeta.math.stats` | `stdlib-completion` §3 | `gammaLn`/`betainc` already provide the machinery; API gap only |
| Hypothesis tests — t-test, chi-square, ANOVA | `cajeta.math.stats` | `stdlib-completion` §4 | general statistics; `tCdf`/`betainc` already there |
| KL divergence | `cajeta.math.stats` | `ml-unsupervised` §9.1 | information theory; t-SNE is merely its first consumer |
| **Unicode normalization** (NFC/NFD/NFKC/NFKD) | `cajeta.lang.String` | `cajeta-docs` §7.1 | string *correctness*, not NLP — composed and decomposed forms must compare equal; `String` has `toLowerCase` but no normalization |
| OKLab colour space | `cajeta.math.Color` | `cajeta-chart` §13.4 | general colour science; `Color` already does sRGB↔linear |
| Bundled-resource read API | **`cajeta.resource`** (new) | `buildtool-resources` §3 | an *archive* concern, not a filesystem one — the distinction is what keeps `capabilities: []` true |
| `EXCHANGE` plan node + pluggable `Exec` | `cajeta.nucleo.frame` | `nucleo-distributed-frame` §9.2 | keeping it out would fork the plan representation |
| Nested/complex column types (struct/array/map) | `cajeta.nucleo.column` | `nucleo-distributed-frame` §10.5.3 | schema-level; `Table<T>`'s record derivation must handle it |
| Vector index (HNSW/IVF) | `cajeta.nucleo.frame` | *already committed*, `nucleo-frame-spec` §9.4.3 | the hook exists; RAG is the trigger |
| **Removed:** `nucleo.nn`, `nucleo.optim` | — | `cajeta-ml-v3` §2.2 | → `dev.cajeta.ml`, v0.14.0 |

**Also stdlib-adjacent but tooling, not library:** `buildtool-resources`
(`src/main/resources/` packaging + hash-pinned asset resolution) lands in the
build tool.

---

## 3. Group B — `dev.cajeta.ml`, the one ML library

Everything passing test §1.3.2. `cajeta-ml-v3` already makes this library both
the sklearn *and* torch role; these extend the classical half.

| Spec | What lands here |
|---|---|
| `ml-classification-gaps` | **all of it** — LDA/QDA, k-NN metric + weights, cost-aware & L1 logistic, stratified/repeated/searched model selection, encoders, kernel regression, PR curve, classification report, feature selection, bootstrap |
| `ml-trees-ensembles` | **all of it** — CART, pruning, bagging, forests, AdaBoost/GBM. (`XGBClassifier` lands in `dev.cajeta.xgboost`, §3.1) |
| `ml-unsupervised` | **all of it** — K-medoids, GMM, hierarchical + dendrogram, DBSCAN, cluster metrics, t-SNE |
| `cajeta-ml-v3` incl. §13 | already scoped — grad, nn, optim, train, io, data, LoRA, transformers |

Rationale: every one of these is `fit`/`predict` over a feature matrix under the
existing protocol, composes in `Pipeline`, and pins against scikit-learn. Putting
them anywhere else would split the estimator surface.

### 3.1 One item goes to `dev.cajeta.xgboost`

`ml-trees-ensembles` §9 (`XGBClassifier`) belongs in the existing xgboost
library, not here — it is an objective + API surface on the existing booster,
under that library's bit-exact parity contract.

---

## 4. Group C — new libraries

Everything failing tests 1 and 2. Ordered by dependency depth.

| Library | Spec | Depends on | Why it is not `dev.cajeta.ml` |
|---|---|---|---|
| **`dev.cajeta.font`** | `cajeta-font` *(queued)* | `cajeta.resource` | general facility — text rendering has consumers far beyond charts |
| **`dev.cajeta.timeseries`** | `ml-timeseries` | `cajeta.math`, ml (`Metrics`) | oracle is **statsmodels**, not sklearn; `Predictor` fits badly (no `predict(x)` on unseen rows) |
| **`dev.cajeta.graph`** | `ml-graph-analytics` | stdlib only | graph analytics **is not machine learning** and uses no estimator protocol |
| **`dev.cajeta.recsys`** | `ml-recsys` | ml, docs, timeseries | data shape is (user, item) pairs and ranked lists, not feature rows |
| **`dev.cajeta.chart`** | `cajeta-chart` | font; `nucleo.frame` at L3 only | its own domain; split at the L2/L3 seam so non-frame consumers avoid the dependency |
| **`dev.cajeta.docs`** | `cajeta-docs` | font, `dev.cajeta.codec` (XML), `String` normalization | **the** text/document library: document model (pages→sentences→elements), readers, normalization, tokenization, TF-IDF, chunking, subword. Absorbs the former `cajeta-text` |
| **`dev.cajeta.dqe`** | `nucleo-distributed-frame` | `nucleo.frame`, cluster, gossip, `dev.cajeta.cloud` (+ CAS, §5.5) | the **distributed query engine** (Hadoop/Spark role). Stdlib must not depend on external siblings |
| **`dev.cajeta.ml.dist`** | `cajeta-ml-dist` | dqe, ml | distributed **execution strategy** for ml's estimators — never a second implementation (§7.5). Name unsettled — see §5.8 |
| **`dev.cajeta.rag`** | `cajeta-rag` | docs, ml, graph, `nucleo.frame` vector index | retrieval: BM25 + inverted index, hybrid fusion, reranking, context assembly, citation, **plus the relation graph and code corpus** (§9–§11 of that spec) |

Package names derive mechanically from the repository name: strip `cajeta-`,
prefix `dev.cajeta.`. Every existing sibling follows it — `cajeta-cluster` →
`dev.cajeta.cluster`, `cajeta-codec` → `dev.cajeta.codec`, `cajeta-ml` →
`dev.cajeta.ml`. (`cajeta-cloud-objectstore` publishes `org.cajeta.cloud.objectstore`
because it is a teaching sample, not a shipped library.)

---

## 5. Decisions this grouping forces

### 5.1 Distance kernels move to stdlib — a revision

`ml-unsupervised` §10.1 and `ml-recsys` §10.2 both recommended a shared kernel
module **inside `dev.cajeta.ml`**. Laying out the grouping shows that was wrong.

Three consumers — k-NN (`dev.cajeta.ml`), similarity (`dev.cajeta.recsys`), and
clustering (`dev.cajeta.ml`) — but **recsys is a separate library**, so an ml-owned
kernel would make recsys depend on the entire ML library to compute a cosine.
Distances are pure functions over tensors with no estimator semantics.

> **Revision: distance and similarity kernels belong in `cajeta.math`.**
> Update `ml-unsupervised` §10.1 and `ml-recsys` §10.2 when their plans are
> written.

### 5.2 `cajeta-docs` absorbs text processing — revised 2026-08-01

An earlier draft split text handling into a separate `dev.cajeta.text`. **That
split is withdrawn.** Julian: docs should cover *"everything that we need to do
with text, text files, etc."*, understanding a document's parts — pages, lines,
sections, paragraphs, sentences, and elements — at whatever granularity ML or
another application needs.

One library, `dev.cajeta.docs`, owns the document model plus normalization,
tokenization, sentence segmentation, vectorization, chunking, and subword
tokenizers. Tokenization without document structure is the weaker abstraction:
sentence segmentation needs to know where a page break falls, and chunking needs
section boundaries.

**The line that remains:** docs *understands and decomposes* text; `cajeta-rag`
*finds* it. So TF-IDF stays in docs (it vectorizes, and `ml-recsys` §8 needs it
with no retrieval involved) while **BM25 and the retrieval index move to rag**
(scoring a query against a corpus is finding).

**One piece goes lower still:** Unicode normalization belongs on
`cajeta.lang.String`, not here. Composed and decomposed forms of identical text
must compare equal — that is string correctness, not NLP, and `String` has
`toLowerCase` but no NFC/NFD today.

### 5.3 Time-series matrix estimation straddles two libraries

`ml-recsys` §10 (trajectory matrix → completion → forecast, mSSA) uses a
time-series construct to do matrix completion. Cleanest split: **`timeseries`
owns the Hankel/trajectory transform**, **`recsys` owns matrix completion**, and
mSSA composes them — making recsys depend on timeseries. Confirm at plan time.

### 5.4 Naming — direct over metaphorical (decision 2026-08-01)

Julian: *"I'd rather have direct naming than a ton of cute riffs on confection
that no one will remember."* So: **`cajeta-dqe`** (distributed query engine),
**`cajeta-ml-dist`** (distributed ML), **`cajeta-rag`**,
**`cajeta-cloud`**.

**`cajeta-dml` was rejected** (2026-08-01). DML already means *Data Manipulation
Language*, and sitting beside `cajeta-dqe` — a query engine — that is the reading
a database reader reaches for first. `cajeta-ml-dist` also sorts and reads next
to `cajeta-ml`, reinforcing §5.6: it is an execution *strategy* over ml's
estimators, not a parallel ML stack.

Two naming corrections recorded so they are not repeated:

- **`cajeta-log` was mine and was wrong** — one character from the existing
  `cajeta-logging` sibling, meaning something entirely different.
- **`caramelo` is not reused.** It is retired (`cajeta-ml-v3` §1), but not a
  clean slate: its README still describes an ML/scientific-compute framework and
  `src/caramelo/spatial/SpatialIndex.cajeta` survives, so its history points at
  ML, not consensus. *(Separately: that `SpatialIndex` may be relevant to
  `nucleo-frame-spec` §9.4.3's HNSW/IVF hook — worth a look.)*

### 5.5 No coordinator — consensus is designed out (decision 2026-08-01)

An earlier draft of this roadmap called for a `cajeta-coordinator` consensus
library, on the grounds that gossip provides discovery but not leader election,
locks, or a consistent metadata store. **That need dissolves under a different
design, and the library is dropped.**

Four things appeared to need consensus. None do:

| Apparent need | Resolution |
|---|---|
| Query-driver election | **Don't elect.** Spark has no driver election either — the driver *is* the submitting process. If a driver dies its query dies; the client retries. |
| Central scheduler (YARN ResourceManager, Spark Master — both ZK-HA'd) | Only arbitrates *between* applications. `nucleo-distributed-frame` §1.5.5 already makes multi-tenancy a non-goal, so no cluster singleton exists. |
| Metadata catalog | **Immutable versioned manifests** in the object store. Never mutated. |
| Write locks | **Copy-on-write + atomic manifest commit.** The loser of the race retries. |
| Streaming controller | Gone — `cajeta-cloud`'s stream port talks to a cluster that coordinates itself (§5.7). |

**What replaces it: one atomic primitive.** Rows 3 and 4 both reduce to needing
a **conditional put** on `ObjectStore` — `putIfAbsent`, or `putIfMatch(etag)`.
That yields optimistic concurrency: two writers race to commit manifest `v7`,
exactly one wins, the loser re-reads and retries.

This is the industry's answer rather than a workaround — Iceberg and Delta Lake
both moved *off* Hive metastore and Zookeeper once object stores gained atomic
conditional writes (S3 `If-None-Match`, GCS generation preconditions, Azure ETag
conditions; a filesystem adapter gets it from atomic rename).

**`cajeta-cloud-objectstore` does not have it today** — its interface is
`put`/`get`/`exists`/`delete`, self-described as "a minimal worked-example
surface." Adding conditional put is the prerequisite, and it is roughly two
methods against a Raft implementation.

**Why a stale leader cannot corrupt anything — the fencing argument.** Because
every mutation funnels through CAS on the manifest, the manifest version *is* a
fencing token. This defeats the classic lease failure (Kleppmann's Redlock
critique): a node wins a lease, stalls past its expiry in GC, another node takes
over, the first wakes and writes. Here the stalled node's commit targets version
`N`, someone already committed `N`, and **its write simply fails**.

That inverts the usual design: **correctness depends on CAS at the point of
mutation, never on who holds a lease.** Leadership becomes *advisory* — an
optimization to avoid duplicated background work (compaction, manifest
expiry), where being wrong costs CPU rather than data.

**What would bring consensus back**, recorded so it is not rediscovered the hard
way:

1. A mutation that cannot be expressed as **one atomic CAS**. Iceberg avoids
   this by funnelling every change through a single pointer swap; a genuine
   multi-object transaction would break the argument.
2. **Exclusive side effects outside the object store** — writing to a system
   that cannot check a fencing token.
3. **Multi-tenancy**, if §1.5.5 is ever reversed. A cross-application resource
   arbiter is a real singleton.

### 5.6 `cajeta-dqe` and `cajeta-ml-dist` are separate libraries

The engine is valuable to someone doing distributed SQL who will never fit a
model, so folding it under a "dml" name would hide it. Spark's own answer was one
project with core/SQL underneath and MLlib on top; this mirrors that as two
libraries. Keeping them apart also protects §7.5: distributed k-means must be an
execution *strategy* for the same estimator, not a parallel implementation — the
MLlib-versus-scikit-learn split Python never recovered from.

### 5.7 Streaming is a client, not a broker — and it folds into `cajeta-cloud`

An earlier draft listed a streaming *library* implying the Kafka role itself.
Julian: *"Are you suggesting that we rewrite kafka?"* No — and the roadmap should
not have implied it.

Kafka is a decade of replication protocol, exactly-once semantics, transactional
writes, log compaction, and tiered storage. Reimplementing it is a multi-year
project and no differentiator for cajeta. **The requirement here
is the ability to consume a stream, not to be a broker.**

**Folded into `cajeta-cloud` as the §12 stream port** (2026-08-01) rather than a
standalone library — Kafka becomes one adapter beside Kinesis, GCP Pub/Sub, and
Event Hubs.

The port shape was **verified against the Kinesis API rather than assumed**, and
that check changed the design twice: **position must be an opaque token**
(Kinesis sequence numbers are strings up to 129 digits; Kafka offsets are
`int64`, so an integer position silently excludes Kinesis), and **the sub-stream
set is dynamic** (Kinesis shards split, merge, and close, requiring consumers to
follow successors; Kafka partitions never do). A Kafka-shaped port would have
broken on the first resharding.

- It needs no coordination of its own: the Kafka cluster brings that (ZK or
  KRaft), and consumer-group membership is a broker-side protocol the client
  merely participates in.
- `cajeta.wire.Compressor` already lists **gzip, snappy, lz4, zstd** — exactly
  Kafka's codec set. The compression half is done.
- Sequence it **last**. Nothing else depends on it.

### 5.8 `dev.cajeta.ml.dist` — the one package name still unsettled

The mechanical derivation gives `cajeta-ml-dist` → `dev.cajeta.ml.dist`. Two
things to check before it is committed:

1. **Nesting across archives.** `dev.cajeta.ml` is a separate archive. Whether
   cajeta permits `dev.cajeta.ml.dist` to ship in a *different* archive — a
   split package — is a toolchain question, not a style one. Java modules
   forbid it; classpath Java merely discourages it. Verify before adopting.
2. **Collision with distributed neural training.** `research-platform-roadmap`
   §6.1's `distributed-training-spec` is the natural other claimant of "ml.dist",
   and the two are deliberately separate libraries (`cajeta-ml-dist` §1.5.2).
   This spec exists and that one does not, so it takes the name — but the
   neural one must then be named for what it does (parallelism strategy) rather
   than for being distributed, and **must not** land under `dev.cajeta.ml.*`.

Resolve both when `cajeta-ml-dist`'s plan opens. If nesting is disallowed, the
fallback is `dev.cajeta.mldist`, which is uglier but unambiguous.

### 5.9 *(resolved — see §5.5.)* `cajeta-dqe` avoids consensus entirely.

### 5.10 Codec placement — stdlib vs `dev.cajeta.codec` (corrected 2026-08-01)

An earlier draft put the XML parser in stdlib `cajeta.codec`. **Corrected: it
goes to `dev.cajeta.codec`.**

`cajeta-codec` is a real library (v0.7.1, not a sample) and its README states
the split: *"The core stdlib ships `cajeta.codec.{json, csv}`; this library adds
the specialized formats most programs never touch."* Stdlib's JSON is
substantial — 13 files — and `cajeta-codec` has none.

There is also a hard constraint: a standalone `.cja` **cannot extend the
stdlib-owned `cajeta.codec.*` namespace** through classpath linking, so it would
be `dev.cajeta.codec.xml` regardless. All three consumers — `cajeta-docs`
(DOCX), the `cajeta-cloud-aws` adapter (S3 responses), general use — are
external libraries that can depend on it freely.

**A larger finding came out of the same check: `dev.cajeta.codec` already
implements Parquet, ORC, Avro, Ion, and Protobuf** — 7–8 source files each,
including `ParquetWriter`, `ParquetColumnReader`, and `ThriftCompactReader`.
The README lists them as "planned"; it is stale.

That resolves `nucleo-distributed-frame` §7.5, which specified an
"Arrow-compatible" partition format as though it needed designing. **Parquet
already exists**, it is what Iceberg and Delta use, and its column chunks and
row-group statistics are exactly what §7.3's column pruning and §7.4's partition
skipping require. Arrow stays the *in-memory* interchange; Parquet is the
*on-disk* format.

### 5.11 Fragmentation is real but consistent with the project

Ten new libraries is a lot. Two mitigations: the ecosystem already works this way
(cajeta-ml, -xgboost, -gossip, -cluster, -cloud-objectstore, -primavera, -olla,
-http, -codec, -collection, -logging, -robotica), and every one above has a
distinct oracle, data shape, or consumer set.

If consolidation is wanted, the two defensible merges are **`graph` into `ml`**
(smallest, and the "not ML" argument is principled but thin) and **`text` into
`doc`** (adjacent, though it would force ml-v3 to depend on a PDF parser to get a
tokenizer — which argues against).

---

## 6. Sequencing — three phases

*(Rewritten 2026-08-04; decided 2026-08-01. This SUPERSEDES the original
"four independent chains" doctrine — the chains below remain accurate as a
**dependency structure**, but execution is phased, not parallel.)*

- **P0 — hold.** No new track work until the ml session shipped its release
  with the deferred defect fixes. *(Cleared: dev.cajeta.ml 0.4.0 on Olla
  2026-08-02; toolchain v0.16.0 released 2026-08-04.)*
- **P1 — stdlib + toolchain, batched into ONE release.** `stdlib-completion`,
  `buildtool-resources`, and the buildtool defect work are fast-tracked ahead
  of **every** external library, because they ship on the TOOLCHAIN release
  cycle rather than a library's — a stdlib gap blocks its consumers for a
  whole release, however small the gap is.
- **P2 — external libraries, in dependency order:**
  - **2a** unblocked the moment stdlib lands: `ml-graph-analytics` (stdlib
    only), `cajeta-font`.
  - **2b** the ML surface, **SEQUENCED not parallel** — `ml-classification-gaps`
    → `ml-trees-ensembles` → `ml-unsupervised` → `cajeta-ml-v3` §13 — three
    specs land in one codebase (`dev.cajeta.ml`) and will contend.
  - **2c** documents + recsys: `cajeta-docs`, `ml-timeseries`, `ml-recsys`.
  - **2d** scale: `nucleo-distributed-frame` → `cajeta-dqe` → `cajeta-ml-dist`;
    `cajeta-cloud` object store.
  - **2e** charting: `cajeta-text-shaping`, then `cajeta-chart` C1–C4.
  - **2f** `cajeta-rag` — genuinely last.

The original dependency chains, still valid as structure:

```
CHAIN 1 — presentation
  buildtool-dependency-classpath [DEFECT: manifest deps inert]
    └─ buildtool-resources ── cajeta.resource
         └─ dev.cajeta.font  ← the gate on all charting
              └─ dev.cajeta.chart

CHAIN 2 — analytics (longest value, fewest blockers)
  cajeta.math: distances + distributions + KL
    └─ dev.cajeta.ml: classification-gaps → trees-ensembles → unsupervised
         ├─ dev.cajeta.timeseries
         ├─ dev.cajeta.recsys   (also needs text)
         └─ dev.cajeta.graph    (stdlib only — can start immediately)

CHAIN 3 — documents & retrieval
  cajeta.codec: XML
    └─ dev.cajeta.docs   (PDF also needs font, CHAIN 1)
         └─ cajeta-rag    (also needs frame vector index)

CHAIN 4 — scale
  ObjectStore conditional put (CAS)   ← ~2 methods; replaces a consensus library
    └─ nucleo.frame seams (EXCHANGE, pluggable Exec, nested types)
         └─ cajeta-dqe          (distributed query engine)
              ├─ cajeta-ml-dist     (distributed ML over ml's estimators)

```

### 6.1 What to start first

**P1 in full** — `stdlib-completion` leads (its §5.1 distances gate three ML
specs), with `buildtool-resources` batched into the same toolchain release.
*(The original pick of `buildtool-dependency-classpath` is moot: the U1
verdict of 2026-08-02 found the reported defect STALE — manifest deps
already resolve; the real blocker there is
`buildtool-exe-package-name-collision`.)*

### 6.2 What not to start first

**`cajeta-chart`.** It is the largest spec and sits at the end of the longest
chain — build tool → resources → font → chart. Four layers before one chart
renders.

---

## 7. Open questions

- **7.1** *(resolved — §4.)* `dev.cajeta.graph` is its own library. Graph
  analytics is not machine learning and uses the estimator protocol nowhere; it
  depends on stdlib alone.
- **7.2** *(resolved — §4.)* `dev.cajeta.timeseries` is separate. The parity
  oracle differs (statsmodels, not sklearn), the `Predictor` protocol fits
  badly — there is no `predict(x)` over unseen rows — and the domain is
  self-contained. The shared audience is not enough to justify one library.
- **7.3** *(resolved — `buildtool-resources` §11.3.)* `cajeta.resource` is its
  own stdlib package, not part of `cajeta.io`. Reading a bundled resource must
  not require filesystem capability, and that distinction has to be visible in
  the package structure to be enforceable.
- **7.4** *(resolved 2026-08-01 — recsys.)* Ranking metrics (precision@k,
  recall@k, NDCG) live in `dev.cajeta.recsys`, not ml's `Metrics`. They score a
  ranked list, not a prediction vector, so they do not fit `Metrics`' shape.
- **7.5** *(resolved — §5.6.)* Distributed ML is `cajeta-ml-dist`, an execution
  strategy over `dev.cajeta.ml`'s estimators, sitting on `cajeta-dqe`.
- **7.6** *(resolved — §5.7.)* Streaming is `cajeta-cloud`'s **stream port**, a
  client against an existing cluster. Cajeta does not implement a broker.
- **7.7** *(resolved — §5.5.)* Yes. No coordinator; `ObjectStore` gains a
  conditional put instead.
