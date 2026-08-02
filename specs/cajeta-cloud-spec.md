# cajeta-cloud — one API for the services every cloud provides

## 1. Definition

### 1.1 Purpose

Every major provider sells substantially the same dozen services under different
names, different wire protocols, and different SDKs. An application that stores
objects, drains a queue, publishes to a topic, and invokes a function is doing
four ordinary things — but written against one provider's SDK it becomes
unportable, untestable without an account, and unrunnable on-prem.

`cajeta-cloud` is one API across that set. Code is written against a **port** —
a provider-neutral interface for one service family. A **provider adapter** binds
that port to S3, Service Bus, Pub/Sub, or Lambda. Choosing between them is
configuration, not a code change.

Three commitments are what make the abstraction hold where others have not:

- **Features are discoverable at runtime, never assumed** (§3). Providers differ
  in ways that matter. A port declares optional capabilities; a caller queries
  them programmatically; an adapter that cannot honour one says so **at
  configuration time**, not mid-write.
- **Every port ships with an in-memory driver** (§1.4), as part of the port's own
  definition rather than as an afterthought. The full API is exercisable with no
  account, no network, and no bill.
- **Conformance is a suite, not a claim** (§10). One test suite defines each
  port's behaviour, and every driver passes it, the in-memory one included.

The **object store** is the first vertical and is specified here in full
(§4–§8). The stream port follows (§12). The rest of the family is enumerated
below and specified as consumers appear.

### 1.2 The service families

The abstraction is viable because these services converged. Each row is a
genuine commonality, not a lowest common denominator — the differences within a
row are handled by capability negotiation (§3), not by truncating the API.

| Family | What it is | AWS | Azure | GCP |
|---|---|---|---|---|
| **object store** | keyed blobs in a flat namespace; range reads, conditional writes | S3 | Blob Storage | Cloud Storage |
| **queue** | point-to-point, competing consumers, redelivery on failure | SQS | Service Bus / Storage queues | Cloud Tasks, Pub/Sub pull |
| **topic** | fan-out publish/subscribe to many independent subscribers | SNS | Service Bus topics, Event Grid | Pub/Sub |
| **stream** | partitioned, ordered, replayable log consumed by position | Kinesis, MSK | Event Hubs | Pub/Sub Lite, Managed Kafka |
| **key-value** | low-latency keyed records with conditional writes | DynamoDB | Cosmos DB, Table Storage | Firestore, Bigtable |
| **SQL** | managed relational database | RDS, Aurora | Azure SQL, Postgres | Cloud SQL, AlloyDB |
| **cache** | in-memory cache with eviction | ElastiCache, MemoryDB | Cache for Redis | Memorystore |
| **secrets** | credentials and config, versioned and rotatable | Secrets Manager, Parameter Store | Key Vault | Secret Manager |
| **notification** | delivery to people and devices — email, SMS, push | SES, SNS mobile push | Communication Services, Notification Hubs | Firebase Cloud Messaging |
| **managed compute** | invoke a function or container, no host to manage | Lambda, Fargate | Functions, Container Apps | Cloud Functions, Cloud Run |

**Queue, topic, and stream are three families, not one.** Providers blur them —
Pub/Sub serves as both topic and queue, SNS fans out to SQS — but their
contracts differ in ways a caller cannot ignore. A queue gives each message to
one consumer and redelivers on failure. A topic gives every message to every
subscriber and typically does not replay. A stream is an ordered log a consumer
walks by position and can re-read. Collapsing them would force each to lie about
at least one of ordering, replay, or delivery fan-out.

**Identity is cross-cutting, not a row.** Every adapter needs credentials,
request signing, and role assumption. That is §9.5's concern in each adapter
rather than a port of its own.

Status of each family, and what would trigger specifying it:

| Family | Status | Trigger |
|---|---|---|
| object store | **specified** (§4–§8) | `cajeta-dqe` §7, `buildtool-resources` asset cache |
| stream | **specified** (§12) | streaming ingest; absorbs the former `cajeta-kafka-client` |
| queue, topic, notification | named | a consumer needing async work dispatch |
| key-value, SQL, cache | named | an external-source connector for `cajeta-dqe` |
| secrets | named | first adapter deployed outside a dev environment |
| managed compute | named — invocation only, see §1.3 | none today |

### 1.3 Data plane, not control plane

The port model fits **using** a service. It fits **provisioning** one badly:
provisioning is where providers diverge most, where the abstraction would leak
worst, and where infrastructure-as-code tools already win.

The line: **invoking** managed compute — calling a function, submitting a job,
reading its result — is data plane and fits a port. **Creating** that function,
attaching its IAM role, and setting its concurrency limits is control plane and
stays out (§1.9.3).

**Raw VM `compute` and `workflow` were dropped from the family on this basis**
(§13.6). VM lifecycle is almost entirely control plane, and a workflow port
would be a second-rate Step Functions. What remains of compute here is
invocation, nothing more.

### 1.4 What the interface library contains — and what it must not

`cajeta-cloud` itself is **ports, in-memory drivers, and conformance testkits.
Nothing else.** Every real backend — provider or local — is a separate artifact
(§1.7).

A port is not complete until it has:

1. **The port** — the interface, its types, its error taxonomy (§11.1), and its
   capability declarations (§3).
2. **An in-memory driver** — a genuine implementation of the port backed by
   memory, faithful to the contract *including its failure modes*. A driver
   whose conditional write always succeeds makes every concurrency test lie
   (§9.4).
3. **A conformance suite** — the executable definition of the port's behaviour
   (§10), which the in-memory driver passes and every external adapter must
   also pass.

**The capability argument is what makes this a rule rather than a preference.**
`cajeta-cloud` declares `"capabilities": []` — no filesystem, no network. A
filesystem-backed driver would force the interface library to declare filesystem
capability, and every consumer of a pure interface would inherit it. Memory
drivers keep the declaration literally true. This is the same boundary that
keeps `dev.cajeta.ml` at `capabilities: []` while distributed training lives in
its own archive.

So the in-memory driver is not a concession to testing convenience — it is the
only implementation that can live here at all.

### 1.5 Local implementations are a separate release, and are not committed

A local/on-prem implementation library is **not part of this spec and not
promised**. If one is released, it is its own artifact (`cajeta-cloud-local`)
with its own spec, its own capability declarations, and its own version
lifecycle — exactly like `cajeta-cloud-aws`.

What follows is the criteria for *whether* such a library could cover a given
family, recorded now so the question is settled when it is asked rather than
argued from scratch.

**The test: a local implementation is possible when the port's contract can be
honoured entirely inside infrastructure you control.** It fails in exactly one
situation — when the service's essential behaviour is reaching *outside* that
boundary. You can self-host a log, a database, and a blob store. You cannot
self-host a carrier's SMS network or Apple's push notification service.

That splits the family three ways.

**Tier A — a local implementation is achievable in full.**

| Family | What it would be |
|---|---|
| **object store** | a filesystem directory; conditional put from **atomic rename** |
| **queue** | a durable local queue — visibility timeout, redelivery, and dead-lettering are all local behaviours |
| **topic** | in-process fan-out to registered subscribers |
| **stream** | a segmented append-only log on disk; partitions are files, positions are offsets |
| **key-value** | an embedded store with conditional writes |
| **cache** | an in-process map with the same eviction policy |
| **SQL** | an embedded engine, or a self-hosted server — with §13.8's dialect caveat |
| **secrets** | an encrypted local store, or a self-hosted vault |

**Tier B — achievable in part, with the gap declared rather than hidden.** This
would need no new mechanism: the local driver declares the outbound capabilities
unsupported through §3, exactly as a provider adapter does.

| Family | Local could cover | Local could not cover |
|---|---|---|
| **notification** | email via a local SMTP relay; delivery status | SMS to a carrier, push to APNs/FCM |
| **managed compute** | invoking the handler in-process or in a local container | provider concurrency limits, cold-start behaviour, IAM execution context |

**Tier C — not achievable.** The service *is* the provider's fleet; there is
nothing to reimplement. No family is in this tier today — raw VM `compute`, its
only member, was dropped from the enumeration entirely (§13.6). The tier is kept
because a future family may land in it.

#### 1.5.1 If local ships, grade is declared and never inferred

The hazard would not be a missing local implementation. It is one good enough
for tests that someone then deploys. Any such library declares, per driver:

- **dev-grade** — single process, correctness-faithful, no durability or
  concurrency guarantees beyond the host.
- **deployable** — supportable as a real on-prem deployment: durable across
  restart, safe under concurrent access, with an operational surface (§11.4).

Both would be legitimate; conflating them is not. Both pass the same conformance
suite — grade is operational envelope, not correctness.

- **1.5.2** When any driver is published, in this library or another, it passes
  this library's conformance suite (§10.4) — that is the only thing that makes
  substitution trustworthy.
- **1.5.3** When a driver cannot reach a capability, it reports it unsupported
  through §3, so a deployment needing it fails at configuration time rather than
  at first use.
- **1.5.4** When drivers are swapped, it is configuration (§2.2) — the same
  application binary runs against memory, an on-prem backend, and a cloud
  account without a code change.

> Consequence, accepted: **testing against this library alone means testing
> against memory.** That is sufficient for `cajeta-dqe`'s conditional-put design,
> which is a concurrency contract rather than a durability one (§10.3), but it
> does not exercise durability across process restart. Anything needing that
> tests against a real adapter.

### 1.6 The governing rule — capability negotiation, never lowest-common-denominator

The standard failure of cloud abstraction layers is exposing only what every
provider shares, so the abstraction becomes useless exactly where it matters.

**This library does not truncate. It negotiates.** A port declares optional
capabilities; a caller queries them; an adapter that cannot honor one **says so
at configuration time**.

This is load-bearing, not stylistic. `cajeta-dqe`'s entire no-consensus design
(`nucleo-distributed-frame` §1.3.1) rests on conditional put. If an adapter
silently lacked it, the failure would appear as **data corruption**, not as a
startup error.

### 1.7 Structure — ports here, every backend elsewhere

```
cajeta-cloud          ports + in-memory drivers + conformance testkits
                      capabilities: []  — no filesystem, no network, no SDK deps
cajeta-cloud-aws      S3, SQS, SNS, …   (needs cajeta-http, SigV4, XML)
cajeta-cloud-azure    Blob, Service Bus, …
cajeta-cloud-gcp      Cloud Storage, Pub/Sub, …
cajeta-cloud-local    filesystem, embedded — NOT COMMITTED, see §1.5
```

Interfaces are cheap; backends are the weight. Splitting at this seam means
`cajeta-dqe` does not drag an Azure SDK it will never call — and, per §1.4,
means the interface library needs no capabilities at all. `cajeta-cloud-local`
is drawn here to fix its place in the layout, not to promise it.

Precedent: JDBC (API plus drivers) and gocloud.dev (portable APIs, per-cloud
drivers, `memblob`/`memdocstore` in-memory drivers).

### 1.8 Scope

The port/adapter/capability model; the three-implementation rule (§1.4); the
conformance testkit pattern; the **object-store port** in full (§4–§8); the
**stream port** in full (§12); the family enumerated in §1.2.

### 1.9 Non-goals

- **1.9.1** **Implementing the services.** These are clients. Cajeta does not
  build an S3, a Kafka, or a database.
- **1.9.2** **Speculative ports.** The families in §1.2 beyond object store and
  stream are **named, not specified** — a cloud abstraction built ahead of its
  consumers gets the abstractions wrong, because it guesses at usage instead of
  extracting it. Naming them now fixes the taxonomy; it does not commit the API.
- **1.9.3** **Control plane** — provisioning, IAM administration, billing,
  infrastructure as code, quota management (§1.3).
- **1.9.4** A unified query language across providers.
- **1.9.5** Cross-provider replication or migration tooling.

### 1.10 Systems

`cajeta.io`, `cajeta.io.net.http` / `cajeta-http` (adapters),
`cajeta.codec` (**XML** — S3 responses are XML, §9.2), `cajeta.hash` (SigV4,
content hashing), `cajeta.wire`, `cajeta.time`, `dev.cajeta.unit`.

---

## 2. Feature: the port model

- **2.1** When code depends on a port, it depends on `cajeta-cloud` alone —
  **never on a provider SDK** — so swapping providers or substituting the
  in-memory driver needs no production-code change.
- **2.2** When an adapter is selected, it is configuration, not a code change.
- **2.3** When a provider is added, it implements the port and passes its
  conformance suite (§3); no consuming code changes.
- **2.4** When a port evolves, adapters that do not implement a new optional
  capability keep working and report it unsupported (§3.2).
- **2.5** When only the object-store port is used, the queue, SQL, KV, and
  cache ports are not linked.

---

## 3. Feature: capability negotiation

- **3.1** When an adapter's capabilities is queried, the result is a definitive
  answer **before** performing any operation.
- **3.2** When an unsupported capability is invoked, it fails immediately with
  a message naming the capability and the adapter — never a silent no-op, never
  a degraded substitute.
- **3.3** When my application requires a capability, it is possible to **assert
  it at startup**, so a misconfigured deployment fails on launch rather than
  mid-write. `cajeta-dqe` asserts conditional put this way.
- **3.4** When a capability is partially supported (conditional create but not
  conditional overwrite), the distinction is expressible — a single boolean
  would force a lie.
- **3.5** When the docs is read, every capability states which adapters support
  it and any provider caveats.

---

## 4. Feature: the object-store port — core operations

- **4.1** When an object under a key is put, its bytes and content type are
  stored, and the operation is atomic — a reader sees either the whole prior
  object or the whole new one.
- **4.2** When an object is obtained, its bytes and metadata are returned, and
  a missing key is an **explicit absence**, not a null or an empty buffer.
- **4.3** When an object is deleted, whether it existed is reported.
- **4.4** When a large object is streamed, it is possible to read and write
  incrementally without materializing it in memory.
- **4.5** When an object within a provider is copied, it is server-side where
  supported (a capability, §3), not a download-and-reupload.

---

## 5. Feature: the object-store port — listing

Non-negotiable for `cajeta-dqe`: dataset versions and partitions cannot be
discovered without it.

- **5.1** When keys are listed by prefix, the result is matching keys with size
  and last-modified.
- **5.2** When a listing is large, it **paginates**, and can resume from a
  continuation token rather than holding it all in memory.
- **5.3** When a listing uses a delimiter, common prefixes are returned, so a
  flat namespace can be walked as if it were directories.
- **5.4** When a listing is returned, its ordering is documented —
  lexicographic where the provider guarantees it, unordered otherwise. Code
  that assumes sorted keys breaks silently against a provider that does not
  promise it.

---

## 6. Feature: the object-store port — range reads

- **6.1** When a byte range is obtained, only that range is transferred.
- **6.2** When a range is unsatisfiable, it fails explicitly rather than
  returning a truncated body.

> `cajeta-dqe` §7.3's columnar pruning depends entirely on this. Without range
> GET, "read one column" becomes "download the whole partition" and the columnar
> format's main advantage disappears.

---

## 7. Feature: the object-store port — conditional writes

The primitive that replaces a consensus library
(`nucleo-distributed-frame` §1.3.1).

- **7.1** When **if-absent** is put, it succeeds only when the key does not
  exist, and a loser learns it lost.
- **7.2** When **if-match** on an ETag or generation is put, it succeeds only
  when the stored version matches.
- **7.3** When an object is read, the version token is returned (**ETag /
  generation**) needed for §7.2.
- **7.4** When two writers race, **exactly one wins** and the loser can re-read
  and retry — optimistic concurrency, no locks, no leader.
- **7.5** When an adapter cannot guarantee conditional writes, it **declares
  the capability unsupported** (§3.2), so a design depending on it fails at
  startup rather than corrupting data.
- **7.6** When read-after-write consistency is needed, the adapter states its
  guarantee — it varies by provider and by operation.

---

## 8. Feature: multipart and large objects

- **8.1** When an object exceeds the provider's single-request limit, multipart
  upload is used, transparently or explicitly per the adapter's documented
  policy.
- **8.2** When a multipart upload fails partway, the incomplete upload is
  abortable and does not become a readable object.
- **8.3** When parts can upload concurrently, they do, bounded by a
  configurable limit.

---

## 9. Feature: adapters

- **9.1** When the **in-memory driver** is used, the full port works with no
  account, no network, and no filesystem — so the whole `cajeta-dqe`
  conditional-put design is exercisable from a bare checkout. It ships in this
  library (§1.4); every other driver named here does not.
- **9.2** When the **S3** adapter is used, requests are signed with **SigV4**,
  responses parsed as **XML** (`cajeta.codec`), and conditional put mapped to
  `If-None-Match` / `If-Match`.
- **9.3** When the **Azure Blob** or **GCS** adapter is used, conditional
  writes map to ETag conditions and generation preconditions respectively.
- **9.4** When the **in-memory driver** is used, it models the port
  **faithfully — including losing conditional writes**. A driver whose
  conditional put always succeeds would make every concurrency test lie
  (`nucleo-distributed-frame` §7.9).
- **9.5** When an adapter needs credentials, they come from the environment or
  an explicit configuration object, **never hardcoded**, and are never logged.
- **9.6** When an adapter opens a network connection, it declares the network
  capability in `cajeta.json` — the port library itself declares none.

---

## 10. Feature: the conformance testkit

The library's most valuable artifact: what makes "swap the provider" trustworthy
rather than aspirational.

- **10.1** When an adapter is written, one conformance suite exercises the
  whole port, and the adapter passes or fails it.
- **10.2** When my adapter declares a capability, the suite **tests that
  capability**; when it declares it unsupported, the suite asserts it fails
  cleanly per §3.2.
- **10.3** When the suite tests conditional writes, it runs **concurrent**
  writers and asserts exactly one wins (§7.4) — the property everything
  downstream depends on.
- **10.4** When the suite is run against the in-memory driver and against a
  real provider, the same assertions hold for both.
- **10.5** When a provider behaves differently in a way the port permits
  (listing order, consistency), the suite tests the *documented* guarantee, not
  an incidental behaviour.

---

## 11. Feature: errors and operability

- **11.1** When an operation fails, the error distinguishes **not-found**,
  **access-denied**, **precondition-failed**, **transient**, and **provider-
  error** — retry logic cannot be written against one opaque failure.
- **11.2** When a failure is transient, retry with exponential backoff and
  jitter is available, with the policy configurable.
- **11.3** When a request fails, the message names the provider, the operation,
  and the key — never a bare provider status code.
- **11.4** When observability is needed, request counts, latencies, bytes
  transferred, and retries are exposed.

---

## 12. Feature: the stream port

Absorbs the former standalone `cajeta-kafka-client`. Kafka becomes one adapter
among several.

### 12.1 Scope — partitioned, ordered, resumable consumption

The port covers what Kafka, Kinesis, GCP Pub/Sub, and Event Hubs genuinely
share: a stream divided into ordered sub-streams, each with a monotonic
position, consumed resumably from a stored position.

**This shape was verified against the Kinesis API, not assumed.** Two findings
changed the design; both are cheap now and expensive to retrofit.

### 12.2 Position is an opaque token — never an integer

- **12.2.1** When a consumption position is recorded, it is an **opaque token**
  that is stored and handed back, never a number to interpret.

> Kafka offsets are `int64`. **Kinesis sequence numbers are strings** — the API
> reference gives the pattern `0|([1-9]\d{0,128})`, up to 129 digits. A port
> typing position as an integer silently excludes Kinesis.

### 12.3 The sub-stream set is dynamic

- **12.3.1** When a stream's sub-streams is listed, the result is the currently
  open set — not a count fixed at creation.
- **12.3.2** When a sub-stream **closes**, that is reported, and its
  **successors** can be obtained and consumption continued.
- **12.3.3** When a closed sub-stream is consumed, it is possible to drain it
  to its end before moving to successors, so no record is skipped.

> Kafka partitions only ever increase in count. **Kinesis shards split and
> merge**, and a shard closes — `GetShardIterator` on a closed shard returns an
> iterator for its last sequence number, after which the consumer must follow
> its children. A port assuming a fixed partition set breaks on the first
> resharding. Kafka will simply never exercise §12.3.2.

### 12.4 Starting position

- **12.4.1** When consuming is started, **oldest**, **newest**, **at a stored
  position**, and **at a timestamp** are all available.

> These map one-for-one: Kinesis `TRIM_HORIZON` / `LATEST` /
> `AT_SEQUENCE_NUMBER` / `AT_TIMESTAMP` ↔ Kafka earliest / latest /
> `seek(offset)` / `offsetsForTimes`. This is the cleanest correspondence in the
> port and the reason the abstraction is viable at all.

### 12.5 Consuming and producing

- **12.5.1** When consuming, batches of records arrive with payload, key where
  the provider has one, timestamp, sub-stream identity, and position.
- **12.5.2** When a batch is finished, the position to store for resumption is
  returned — **the port never stores it**, since where it belongs differs per
  deployment.
- **12.5.3** When producing, a record can be published to a stream with an
  optional partition key, and the resulting position returned.
- **12.5.4** When a provider rate limit is hited, it surfaces as a
  **transient** error (§11.1) with retry guidance — Kinesis enforces hard per-
  shard TPS limits and raises `ProvisionedThroughputExceededException`.

### 12.6 What stays adapter-internal

- **12.6.1** When the port is used, **cursor lifecycle is invisible**. Kinesis
  shard iterators expire after five minutes and must be rolled forward via
  `NextShardIterator` on every read; Kafka has no such concept. The port
  exposes *position*; the adapter manages cursors.
- **12.6.2** When an adapter needs external coordination state, it says so in
  its documentation and configuration. Kafka coordinates consumer groups
  **broker-side**; Kinesis requires KCL plus a **DynamoDB lease table** you
  provision. These are not one mechanism and the port does not pretend
  otherwise.

### 12.7 Capabilities — Kafka-only, declared not assumed

- **12.7.1** When **log compaction** is needed, it is a declared capability
  (§3); only Kafka offers it.
- **12.7.2** When **transactional / exactly-once** produce is needed, likewise.
- **12.7.3** When **broker-managed consumer groups** with rebalancing are
  needed, likewise — Kinesis's equivalent is KCL leases (§12.6.2).

---

## 13. Open questions (resolve at plan time)

- **13.1** *(resolved 2026-08-01 — grow it.)* `cajeta-cloud-objectstore` is
  renamed and grown into `cajeta-cloud`. `ObjectStoreContract` is the seed of
  §10's conformance suite and `FakeObjectStore` the seed of §1.4's in-memory
  driver, so the existing work carries forward. Required as part of that:
  republish under **`dev.cajeta.cloud`** (it is `org.cajeta.cloud.objectstore`
  today, the sample namespace), drop `authors:["sample"]`, and rewrite the
  README, which currently describes a teaching sample.
- **13.2** *(resolved)* Port name: **`objectstore`**, matching the existing
  `ObjectStore` type and §1.2's taxonomy. An earlier draft proposed `blob` to
  avoid collision with object *databases* (ObjectDB, ZODB); the established name
  wins, and a future document port can be named for what it is.
- **13.3** *(resolved 2026-08-01 — S3.)* The first external adapter is **S3**.
  The in-memory driver was never a question — it ships with the port (§1.4) — and
  `cajeta-dqe` is testable against it without any adapter, so the first adapter
  should be the first real deployment target rather than another test double.
- **13.4** *(resolved 2026-08-01 — use `cajeta.codec`'s.)* The S3 adapter parses
  responses with `cajeta.codec`'s XML parser rather than a targeted one.
  `cajeta-docs` needs that parser anyway for the OOXML family, so this is a
  second consumer rather than a new dependency — and a second consumer is what
  keeps a parser honest.
- **13.5** *(action, not a decision.)* **Assess `cajeta-http` against the S3
  adapter's needs before the adapter unit opens** — specifically streaming
  bodies (§4.4), range requests (§6.1), and multipart upload (§8). If it cannot
  stream without materializing, that is a `cajeta-http` gap to fix there, not a
  reason to write a second client here.
- **13.6** *(resolved 2026-08-01 — managed compute only.)* **`compute` (raw
  VMs) and `workflow` are dropped from the family entirely.** Raw VM lifecycle
  is control plane (§1.3) and a workflow port would be a second-rate Step
  Functions. **Managed compute invocation stays** as a named future port —
  calling a function and reading its result is data plane and fits cleanly.
  §1.2's tables are updated accordingly.
- **13.7** *(resolved — §1.4, §1.5.)* What must ship with a port? The port, an
  in-memory driver, and a conformance suite — nothing else lives in this
  library. Local implementations are a separate, uncommitted release; §1.5 gives
  the test for which families one could cover if it is ever built.
- **13.8** *(deferred with the SQL port.)* Whether a future local SQL
  implementation uses an embedded engine or a self-hosted server is not this
  spec's question — §1.5 makes local a separate, uncommitted release. Recorded
  because it is the one tier-A family where local and provider are **not**
  behaviourally interchangeable: dialect, type system, and isolation levels all
  differ, and no conformance suite papers over that. Expect it to constrain the
  SQL port's surface more than any other, and expect capability negotiation to
  be doing the most work there.
- **13.9** *(deferred with the library.)* If `cajeta-cloud-local` is ever
  released, does it ship as one artifact or one per family? A single artifact is
  simpler but would drag an embedded SQL engine into a deployment that only
  wanted a local object store. Not a question this spec has to answer.
- **13.10** *(resolved 2026-08-01 — watch, do not pre-build.)* The in-memory
  driver covers concurrency (§10.3) but not durability across process restart.
  A consumer that genuinely needs restart-durability in test is the trigger to
  build `cajeta-cloud-local` — not convenience, and not anticipation.

---

## 14. Acceptance criteria (spec-level)

- **14.1** One conformance suite passes against the in-memory driver and at
  least one cloud adapter (§10.4).
- **14.2** Concurrent conditional writes yield **exactly one winner**, asserted
  under real concurrency, on every adapter declaring the capability (§7.4,
  §10.3).
- **14.3** An adapter lacking a capability fails **at configuration time** when
  it is asserted, not at first use (§3.3, §7.5).
- **14.4** The in-memory driver loses conditional writes when it should (§9.4)
  — verified by a test that would pass against an always-succeeding driver and
  must not.
- **14.5** A range read transfers only the requested bytes, verified by
  measured transfer volume (§6.1).
- **14.6** A failed multipart upload leaves no readable object (§8.2).
- **14.7** The port library declares **no network capability**; only adapters
  do (§9.6).
- **14.8** No consumer of the object-store port links a provider SDK (§2.1,
  §2.5).
- **14.9** Credentials never appear in logs or error messages (§9.5).
- **14.10** Every specified port ships an in-memory driver that passes the full
  conformance suite (§1.4) — asserted per port, not claimed.
- **14.11** `cajeta-cloud` declares **`"capabilities": []`** and its build
  contains no filesystem or network call, verified against the manifest (§1.4).
  A filesystem-backed driver landing here is a spec violation, not a shortcut.
- **14.12** The full object-store vertical runs end to end with **no network, no
  provider account, and no filesystem**, including concurrent conditional writes
  (§14.2).
- **14.13** No provider SDK, and no local backend, is reachable from
  `cajeta-cloud`'s dependency set (§1.7).
