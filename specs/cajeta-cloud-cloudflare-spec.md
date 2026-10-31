# cajeta-cloud on Cloudflare: R2, Access, Queues and KV (spec)

Status: **approved** by Julian 2026-10-30 ("Approve cloudflare spec"), opened
the same day ("focus on cloudflare adaptors first, instead of AWS"; "go with a
shared s3 lib"). Plan: `agents/cajeta-cloud-cloudflare-plan.md`.
Parent specs: `cajeta-cloud-spec.md` (§1.2 families, §1.6 capability
negotiation, §1.7 structure, §9 adapters, §10 conformance, §11 errors) and
`archive/cajeta-cloud-identity-spec.md` (the identity port).
Sibling: `cajeta-cloud-adapters-spec.md`, whose S3 adapter (built, unreleased,
in `cajeta-cloud-aws`) this spec moves into a shared artifact.
This spec adds only what those leave open; it restates nothing they decide.

## 1. Definition

### 1.1 Purpose

A cajeta application deployed against Cloudflare reaches four of its services
through the cajeta-cloud ports, so the code is written once against
`dev.cajeta.cloud` and Cloudflare is configuration (parent §2.2):

| Cloudflare service | cajeta-cloud family (parent §1.2) | Port today |
|---|---|---|
| R2 | object store | specified, shipped (0.4.0) |
| Access | identity | shipped (0.3.0) |
| Queues | queue | **named, not specified** |
| Workers KV | key-value | **named, not specified** |

Two of the four therefore need a port before they need an adapter. This spec
specifies both ports at the size Cloudflare and the in-memory driver need, and
no larger, by the parent's rule that a family is specified when a consumer
needs it (§1.2's trigger column). The consumer is a primavera application
deployed against Cloudflare.

### 1.2 Artifacts

- **`cajeta-cloud-s3`** (`dev.cajeta.cloud.s3`): the S3 protocol client, which
  is the SigV4 signer, the S3 wire layer (requests, XML replies, error codes,
  dates, addressing) and `S3ObjectStore`. It is configured with an endpoint and
  a signing region, so one implementation serves AWS S3, R2, and any other
  S3-compatible store. It carries no credential chain of its own; the provider
  artifact supplies credentials. Moved out of `cajeta-cloud-aws` (Julian,
  2026-10-30), where it is built and unreleased.
- **`cajeta-cloud-cloudflare`** (`dev.cajeta.cloud.cloudflare`): the four
  Cloudflare adapters, their account and credential configuration, and their
  fakes. It depends on `cajeta-cloud`, `cajeta-cloud-s3`, cajeta-http,
  cajeta-crypto and cajeta-codec, and declares the network capability (parent
  §9.6).
- **`dev.cajeta.cloud`** gains the queue port and the key-value port, each
  with its in-memory driver and its conformance suite (parent §1.4, §10).
- **`cajeta-cloud-aws`** depends on `cajeta-cloud-s3` for S3 instead of
  carrying it, and keeps Cognito and the AWS credential chain.

### 1.3 Scope

Data plane only (parent §1.3): reading and writing objects, verifying Access
tokens, publishing and consuming queue messages, reading and writing KV
entries. Creating buckets, queues, namespaces and Access applications is the
operator's job, done in the dashboard or with Wrangler.

### 1.4 Non-goals

- **1.4.1** Running inside a Worker. Every adapter here is a client of
  Cloudflare's public HTTP APIs from a process cajeta runs. Worker bindings (R2,
  KV, Queues bound to a Worker) are not reachable from outside one.
- **1.4.2** Hosting primavera on Cloudflare Containers. Separate, later (Julian,
  2026-10-30); it stays a parked exploration.
- **1.4.3** D1. It is Cloudflare's SQL family, and the SQL port is not
  specified. Named as the next step if a consumer needs relational storage
  (§9 Q3).
- **1.4.4** Durable Objects, Hyperdrive, Vectorize, Images, Stream (video).
  None is a cajeta-cloud family today.
- **1.4.5** Access as a user pool. Access delegates sign-in to an identity
  provider and issues its own tokens; it has no registration, password or MFA
  API (§4).

### 1.5 Order

1. The shared S3 artifact (§2), because R2 waits on it and it is a move, not
   new code.
2. R2 (§3): the first real deployment target, and conformance carries over
   from the S3 adapter.
3. Access (§4): token verification for a primavera application behind Access.
4. The queue port and the Queues adapter (§5).
5. The key-value port and the KV adapter (§6).

### 1.6 Systems

Cloudflare's S3-compatible R2 API; the Access certificate endpoint
(`https://<team>.cloudflareaccess.com/cdn-cgi/access/certs`); the Cloudflare
REST API v4 (`https://api.cloudflare.com/client/v4/accounts/<account_id>/...`)
for Queues and KV; cajeta-http for transport, cajeta-crypto for HMAC-SHA256 and
RS256 verification, cajeta-codec for JSON and XML.

## 2. Feature: the shared S3 artifact

- **2.1** When an application uses S3, R2, or another S3-compatible store, it
  depends on one `S3ObjectStore`, configured with an endpoint, a signing
  region, path-style or virtual-hosted addressing, and a credential source.
- **2.2** When `cajeta-cloud-aws` is built after the move, its S3 tests and its
  S3 conformance run unchanged against the moved code, and `S3ObjectStore`'s
  public API is unchanged for an AWS caller.
- **2.3** When SigV4 is used, it is proven against AWS's published SigV4 test
  vectors in `cajeta-cloud-s3` itself (the vectors move with the signer), so
  every provider artifact inherits the proof.
- **2.4** When a provider differs from AWS S3 in a way the port permits (a
  multipart rule, a missing header, an error code), the difference is a
  declared capability or a provider profile passed at configuration, never a
  branch on the endpoint's hostname.
- **2.5** When the artifact is released, it is released before
  `cajeta-cloud-aws` 0.1.0 and `cajeta-cloud-cloudflare` 0.1.0, which pin it.

## 3. Feature: the R2 object store

- **3.1** When an application configures R2, it gives the account id, the
  bucket, an R2 access key id and secret (R2's S3 credentials, distinct from a
  Cloudflare API token), and optionally a jurisdiction (`eu`, `fedramp`). The
  endpoint is derived (`https://<account_id>.r2.cloudflarestorage.com`, or the
  jurisdiction's host) and the signing region is `auto`.
- **3.2** When the object-store conformance suite runs against R2, every core
  operation, listing, range read and conditional write passes, including the
  concurrent-writers test (parent §10.3).
- **3.3** When a conditional write is made, `If-None-Match: *` (create) and
  `If-Match` (replace) map as on S3 (parent §9.2). Whether R2 honours both on
  PutObject is verified against R2 before the adapter declares the capability;
  until then it is declared unsupported (§9 Q1).
- **3.4** When a multipart upload is written, the adapter follows R2's part
  rules (verify: every part but the last of equal size, 5 MiB minimum), so a
  streamed write that sizes parts adaptively still succeeds on R2.
- **3.5** When an S3 feature R2 lacks is requested (object lock, ACLs,
  SSE-KMS, the checksum algorithms R2 does not support), the operation fails
  cleanly as unsupported per parent §3.2, and the adapter's capability set
  says so up front.
- **3.6** When R2 returns an error, it maps to the parent's error classes
  (§11.1), with R2's own codes added where they differ from S3's.
- **3.7** When a public bucket or a custom domain serves reads, that is outside
  the adapter: it reads and writes through the S3 API only.

## 4. Feature: Cloudflare Access as an identity verifier

Access sits in front of an application, signs users in through an identity
provider, and forwards each request with a signed JWT. The application's job is
to verify that token. That is the identity port's `TokenVerifier` and nothing
else.

- **4.1** When a request reaches an application behind Access, the token is
  read from the `Cf-Access-Jwt-Assertion` header (or the `CF_Authorization`
  cookie) and verified: RS256 signature against the team's certificate set,
  `iss` equal to the team domain, `aud` containing the application's AUD tag,
  and `exp`/`nbf` within the port's clock-skew allowance.
- **4.2** When the certificate set is fetched, it is cached for the response's
  lifetime and refetched on an unknown `kid`, as the Cognito verifier does
  (`cajeta-cloud-adapters-spec.md` §2.3). The JWKS cache is shared code, not a
  copy.
- **4.3** When a verified token is turned into a principal, the subject, email
  and the identity provider's groups (where Access forwards them) become the
  port's claims, so `primavera-security` authorizes on them unchanged.
- **4.4** When a caller asks the Access adapter to register a user, sign in,
  change a password or enroll MFA, the operation fails as unsupported (parent
  §3.2), and the adapter's capabilities declare only token verification.
- **4.5** When a service token (machine-to-machine Access) calls the
  application, its JWT verifies the same way, and the principal says it is a
  service, not a user.
- **4.6** When the identity conformance suite runs, the Access adapter passes
  the verification cases and is asserted to refuse the rest cleanly (parent
  §10.2).

## 5. Feature: the queue port, and Cloudflare Queues

### 5.1 The port (`dev.cajeta.cloud`)

A queue gives each message to one consumer and redelivers it if that consumer
does not acknowledge it (parent §1.2). The port covers what SQS, Service Bus,
GCP Pub/Sub pull and Cloudflare Queues share.

- **5.1.1** When a message is sent, it carries a body of bytes and optional
  attributes, singly or in a batch, and an optional delivery delay.
- **5.1.2** When a consumer receives, it gets a batch of up to N messages,
  each with its body, attributes, an attempt count, its send time, and a
  **receipt**: an opaque token valid for a visibility window the consumer
  chose.
- **5.1.3** When a message is acknowledged by its receipt, it is not
  delivered again. When the receipt has expired, acknowledging fails with a
  distinct error and the message may already have gone to another consumer.
- **5.1.4** When a message is released (negative acknowledgement), with an
  optional delay, it becomes available again after the delay.
- **5.1.5** When the window passes without an acknowledgement, the message is
  delivered again with its attempt count raised. Delivery is at least once;
  the port promises no ordering.
- **5.1.6** When a provider has a dead-letter queue configured, a message past
  its retry limit goes there; the port reports the attempt count and does not
  manage the dead-letter queue (control plane).
- **5.1.7** When FIFO ordering, exactly-once delivery, or message groups are
  needed, they are declared capabilities (SQS FIFO, Service Bus sessions), not
  assumed.
- **5.1.8** When the in-memory driver is used, it models visibility windows,
  redelivery and receipt expiry faithfully, on an injectable clock, so a test
  can watch a message come back.

### 5.2 The Cloudflare Queues adapter

- **5.2.1** When a message is sent, the adapter calls the Queues REST API
  (send and batch send) with an API token scoped to the queue, and maps the
  body's content type.
- **5.2.2** When a consumer receives, the adapter uses the HTTP pull consumer
  (pull with a batch size and a visibility timeout), and the receipt is the
  message's lease id.
- **5.2.3** When messages are acknowledged or released, the adapter uses the
  ack endpoint (acks and retries with a delay), batching what the caller
  batched.
- **5.2.4** When the queue has no HTTP pull consumer configured, the first
  receive fails naming the setting, not with a bare provider error.
- **5.2.5** When Cloudflare's limits are met (message size, batch size, rate),
  they surface as the parent's error classes with the limit named; a rate
  limit is transient (parent §11.1, §11.2).
- **5.2.6** When the queue conformance suite runs against Queues, it passes,
  and the capabilities Queues lacks (5.1.7) are asserted to refuse cleanly.

## 6. Feature: the key-value port, and Workers KV

### 6.1 The port (`dev.cajeta.cloud`)

The parent describes key-value as "low-latency keyed records with conditional
writes" (§1.2). Workers KV has no conditional write and is eventually
consistent, so the port's core is what every provider shares and the rest is
declared.

- **6.1.1** When a value is put, it is bytes under a string key, with optional
  metadata and an optional expiry (absolute or as a time to live).
- **6.1.2** When a key is read, the value and its metadata come back, or
  not-found.
- **6.1.3** When a key is deleted, later reads report not-found once the
  provider's consistency allows (6.1.6).
- **6.1.4** When keys are listed by prefix, results come in pages behind an
  opaque cursor, as the object-store port lists (parent §5).
- **6.1.5** When a conditional write (create-only, compare-and-set) is needed,
  it is a declared capability. Providers that have it (DynamoDB, Cosmos DB,
  Firestore) declare it; Workers KV does not, and refuses it cleanly.
- **6.1.6** When consistency matters, the adapter declares its model (strong,
  read-your-writes, or eventual with a bound), and the conformance suite tests
  the declared model, not an incidental one (parent §10.5).
- **6.1.7** When the in-memory driver is used, it is strongly consistent and
  supports conditional writes, and it can be configured to model eventual
  consistency with a lag, so code written against KV can be tested for stale
  reads.

### 6.2 The Workers KV adapter

- **6.2.1** When the adapter is configured, it takes the account id, the
  namespace id and an API token scoped to KV.
- **6.2.2** When values are written and read, the adapter uses the KV REST API
  (write with metadata and expiry, read value, read metadata, delete, list
  keys with a cursor), and batches bulk writes and deletes through the bulk
  endpoints.
- **6.2.3** When the adapter declares its consistency, it declares eventual
  consistency with Cloudflare's documented propagation bound, and no
  conditional writes.
- **6.2.4** When KV's limits are met (key and value size, write rate per key),
  they surface as the parent's error classes with the limit named.
- **6.2.5** When the key-value conformance suite runs against KV, it passes,
  with its consistency tests run in their eventual form.

## 7. Feature: account, credentials and configuration

- **7.1** When an adapter needs credentials, they come from an explicit
  configuration component or the environment, never hardcoded and never
  logged (parent §9.5): `CLOUDFLARE_ACCOUNT_ID`, `CLOUDFLARE_API_TOKEN` for the
  REST APIs, and `R2_ACCESS_KEY_ID` with `R2_SECRET_ACCESS_KEY` for R2's S3
  API. A missing value fails at configuration, naming which.
- **7.2** When different services need different tokens (least privilege), each
  adapter can be given its own; one shared token is the default.
- **7.3** When Access is configured, it takes the team domain and the
  application's AUD tag; it needs no secret.
- **7.4** When adapters are selected, the provider is configuration in the
  same form the AWS adapters use (identity spec §9), so switching a primavera
  application between AWS, Cloudflare and the in-memory drivers changes its
  configuration only.

## 8. Feature: testing without an account

- **8.1** When the default CI runs, every adapter is tested against a fake of
  its service (an in-process R2, an Access issuer with a fixture key set, a
  Queues pull API, a KV REST API), as `cajeta-cloud-aws` tests against
  `FakeS3` and `FakeCognito`.
- **8.2** When an account exists, an opt-in workflow (dispatch only, like
  cloud-aws's `aws-live.yml`) runs every conformance suite against the real
  services and fails if a live leg skips.
- **8.3** When the live workflow runs, it creates and removes its own objects,
  messages and keys under a run-unique prefix, so concurrent runs and a shared
  account do not collide.

## 9. Open questions

- **Q1** Does R2 honour `If-Match` and `If-None-Match` on PutObject, so the
  conditional-write capability (and the concurrent-writers conformance) holds?
  Verify against R2 before 3.3 declares it.
- **Q2** Should the queue and key-value ports ship in a `dev.cajeta.cloud`
  minor release (0.5.0) ahead of the adapters, or together with
  `cajeta-cloud-cloudflare` 0.1.0?
  *(Resolved 2026-10-30, Julian: ports first. `dev.cajeta.cloud` 0.5.0 ships
  at the end of Unit 2 and `cajeta-cloud-cloudflare` pins it.)*
- **Q3** D1 next? It needs the SQL port, the parent's largest unspecified
  family. Only if a consumer needs relational storage on Cloudflare.
- **Q4** The Cloudflare account: whose, and which secrets the live workflow
  reads. Until it exists, acceptance is fakes plus conformance against fakes.
- **Q5** Olla trusted-publishing bindings for `cajeta-cloud-s3` and
  `cajeta-cloud-cloudflare`, the same open item as cloud-aws's 2.2.1.

## 10. Acceptance (spec-level)

- **10.1** `cajeta-cloud-s3` released; `cajeta-cloud-aws`'s S3 tests and
  conformance green on it with no API change.
- **10.2** The queue and key-value ports in `dev.cajeta.cloud`, each with an
  in-memory driver that passes its own conformance suite.
- **10.3** `cajeta-cloud-cloudflare` 0.1.0 released: R2, Access, Queues and KV
  green against their fakes in default CI, and against the real services in
  the opt-in workflow once the account exists.
- **10.4** A primavera sample switches between the in-memory drivers and
  Cloudflare by configuration alone.
