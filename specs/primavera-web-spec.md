# primavera-web — pooled buffers, message views, and the registration sample

The web layer of primavera, built in four phases and driven by one sample
application that registers a user. Realizes `primavera-spec.md` §6 (web
model), §8 (security) and §10 (streaming) in the primavera repo, and resumes
that repo's Phase 4, deferred on 2026-07-19 because `cajeta-http` had no usable
server. `cajeta-http` 0.3.1 now has a server, a router, WebSocket, SSE and
middleware, so the phase reopens here, under the agents convention.

## 1. Definition

### 1.1 Purpose

A primavera service reads requests into buffers it allocated once, processes
each message through views over those bytes, and writes responses into
buffers it also allocated once. Allocation per request is bounded and does
not grow with traffic. That is the model. Everything else in this spec is the
policy that makes the model usable: typed endpoints, WebSocket on the same
buffers, codecs and compression chosen by content type through one pluggable
seam, and a security layer that puts identity behind the `cajeta-cloud`
identity port (`cajeta-cloud-identity-spec.md`).

The library is grown from a sample. The sample registers a user, and every
feature lands because the sample needs it. Nothing lands in the library that
the sample does not exercise.

### 1.2 Design principles

- **Substrate below, engine beside, policy here.** The buffer pool and byte
  buffer are `cajeta.io.net` (`BufferPool`, `ByteBuffer`). Byte-exact overlays
  are the language's `view` declaration (language spec §12.2). Encoders and
  compressors are `cajeta.wire`. The HTTP engine is `cajeta-http`. primavera
  composes them and adds the request model, the endpoint model and security.
- **Zero-copy is a borrow.** A view over a pooled buffer lives no longer than
  the request. Anything the application keeps is copied out. The compiler's
  borrow checks catch the cases they can, and the sample makes the copy-out
  visible on day one, because the username has to leave the buffer before the
  buffer is reused. The failure this guards against is the one recorded for
  `#x` on a borrow: a receiver that outlives the lender reads reused memory.
- **Bounded, measured.** The buffer pool's acceptance invariant already exists
  (`BufferPoolTests.reuseStaysBounded`). This spec extends it to a whole
  request: after warm-up, N requests on one connection perform zero fresh
  buffer allocations, and the number is asserted, not claimed.
- **Compiled-in, configured, never scanned.** primavera-spec §16 decided that
  which implementations exist is a build-time fact and which one runs is
  configuration over the compiled-in set. Codecs, compressors and identity
  adapters all follow it (§7.5, identity spec §9).

### 1.3 What this spec changes in cajeta-http

The measured starting point: `HttpServer.readExchange` reads the request head
one byte at a time into a one-byte array, then `readBody` pulls the body
through a fresh 2048-byte chunk array per request. Neither touches
`BufferPool`. Only the WebSocket frame decoder uses `ByteBuffer`. The response
side serializes into fresh arrays.

The seam this spec asks of `cajeta-http`, kept small:

- **1.3.1** The server accepts a `BufferPool` and acquires one input and one
  output `ByteBuffer` per connection at accept, releasing both at close.
- **1.3.2** The parser reads the head and body into the connection's input
  buffer, and the request exposes the body as a window into that buffer with
  no copy when the body fits.
- **1.3.3** The response writer serializes head and body into the
  connection's output buffer and issues one write per response when the
  response fits, flushing in bounded pieces when it does not.
- **1.3.4** Content coding is looked up through a registry rather than the
  static four-token list in `ContentCoding`, so a coding can be added by a
  dependency.

Existing entry points keep working with a default pool, so no current
consumer of `cajeta-http` changes.

### 1.4 Non-goals

- **1.4.1** The HTTP engine, HTTP/3, TLS. Those are `cajeta-http` and
  `cajeta.io.net`.
- **1.4.2** The fleet-scale connection registry and state migration in the
  primavera repo's `Connections.md`. §6 takes the minimum: a connection scope
  and an addressable connection on one node.
- **1.4.3** Data access and transactions (`primavera-spec.md` §11).
- **1.4.4** Provider adapters. `cajeta-cloud-aws` and its siblings are their
  own specs. This spec is written against the memory driver and must run
  unchanged against an adapter.
- **1.4.5** Cryptographic primitives. They are stdlib asks, listed in identity
  spec §5.6 and §8.6 here.
- **1.4.6** Annotation-driven endpoints as the first surface (§5.2). They are
  the target, and they wait on the codegen seam.

### 1.5 Systems

`cajeta.io.net` (`BufferPool`, `ByteBuffer`, `AsyncReader`, `AsyncWriter`,
`ServerModel`), the `view` declaration and `cajeta.wire` (`Encoder`,
`BufferEncoder`, `Compressor`, `Decompressor`), `cajeta.codec.json`
(`JsonIndex`, `JsonCursor`, `Json.parse<T>`), `cajeta-http` 0.3.x (server,
router, `WebSocketHandler`, middleware), `cajeta-codec` (protobuf, avro, ion,
deflate), `cajeta-cloud` identity port, `cajeta-logging`, `dev.cajeta.unit`,
the existing primavera `RequestScope` and `SessionScope`.

### 1.6 Sequencing

Four phases, in order. Each phase ends with the sample running end to end.

1. **HTTP**: §2, §3, §4, §5. The sample registers and looks up a user over
   HTTP with JSON, against the identity port's memory driver in its minimal
   cut.
2. **WebSocket**: §6. The sample adds a live channel on the same buffers.
3. **Codecs and compression**: §7. The sample accepts a binary registration
   message and compressed bodies through the pluggable seam.
4. **Security**: §8. The sample gains confirm, login, OTP, tokens, and an
   authorized endpoint, against the identity port's full cut.

---

## 2. Feature: input buffers

- **2.1** When a connection is accepted, it acquires one input buffer from
  the pool, sized by configuration, and holds it for the connection's life.
- **2.2** When the pool has an idle buffer, acquisition reuses it. After
  warm-up, a steady stream of connections performs no fresh allocations,
  bounded by the pool's `maxIdle` plus peak concurrency (the `BufferPool`
  invariant).
- **2.3** When a request's head and body fit in the buffer, both are parsed
  in place and the request's body is a window into the buffer.
- **2.4** When a body is larger than the buffer, the request is served as a
  stream through `cajeta-http`'s existing body model, and the handler is told
  so. Per-request limits still apply, and a body over the limit is refused
  with 413 before it is buffered.
- **2.5** When bytes of a pipelined next request arrive with the current
  one, they stay in the buffer and are compacted to the front once the
  current request completes.
- **2.6** When the handler returns and the response has been written, the
  buffer is reused for the next request on that connection. Until then no
  byte of the current request is overwritten.
- **2.7** When the server model is shared-pool, each worker uses its own pool
  and no lock is taken on the buffer path. When it is fiber-per-connection,
  one pool per acceptor serves the fibers it spawns.

---

## 3. Feature: message views

- **3.1** When an endpoint declares its body as a view type, the body bytes
  are bound as a language `view` over the input buffer. Construction checks
  the fixed prefix and each variable-length field once, and later field reads
  are constant-offset reads with no copy.
- **3.2** When an endpoint declares its body as JSON, the body is indexed in
  place (`JsonIndex` over the buffer window) and fields are read through the
  index without materializing an object tree.
- **3.3** When an endpoint declares its body as an owned type, the body is
  decoded into a fresh owned value through the codec seam (§7), and the
  buffer is free to be reused after the call.
- **3.4** When a handler stores a view, or any borrow of the buffer, in a
  place that outlives the request, the compiler rejects it where the borrow
  rules can see it (`CAJETA_ERROR_MOVE_OF_BORROW` and its family), and the
  docs name the copy-out as the correct spelling.
- **3.5** When a value read through a view must outlive the request, the
  application copies it. The sample's registration handler copies the
  username and attributes out of the request view into the identity port's
  call, and identity spec §2.3 guarantees the port copies again into its own
  storage.
- **3.6** When a test overwrites the input buffer immediately after a handler
  returns, every value the handler stored is unaffected. This is the
  acceptance for §3.4 and §3.5, and it must fail against a handler that keeps
  a view.

---

## 4. Feature: output buffers

- **4.1** When a response is produced, its head and body are serialized into
  the connection's output buffer and written with one write when they fit.
- **4.2** When a response does not fit, it is flushed in bounded pieces of
  the buffer's size, chunked or content-length framed as the response
  declares. The buffer never grows to hold a response.
- **4.3** When a body is encoded by a codec that implements
  `BufferEncoder<T>`, it is encoded directly into the output buffer with
  `encodeInto`. A codec that only implements `Encoder<T>` is accepted and
  costs one intermediate array, which the docs say.
- **4.4** When compression is negotiated, the compressor writes into the
  output buffer as well, and a response that grows under compression falls
  back to identity coding rather than to an unbounded buffer.
- **4.5** When the response is written, the output buffer is reset and reused
  for the next response on the connection.

---

## 5. Feature: endpoints and the registration sample

- **5.1** When the sample runs, it serves `POST /users` (register) and
  `GET /users/{id}` (lookup) over HTTP/1.1 with JSON bodies, against the
  identity port's memory driver. A duplicate username answers 409, an
  invalid body answers 400 with a message naming the field, and a lookup of
  an unknown id answers 404.
- **5.2** When an endpoint is declared, the first surface is an explicit,
  typed registration: a route, a body binding mode (view, JSON index, or
  owned) and a handler whose parameters are the bound values. The
  annotation form in `primavera-spec.md` §6 (`@Rest`, `@Post`, `@Body`)
  compiles down to this surface when the codegen seam exists. The sample
  moves to the annotation form then, and the typed surface remains public.
- **5.3** When a request is dispatched, it runs under a fresh `RequestScope`
  so request-scoped components resolve per request, and `SessionScope` is
  available to endpoints that opt in.
- **5.4** When the sample is exercised by its self-test, the test drives N
  requests on one keep-alive connection after a warm-up and asserts the
  pool's allocation count did not move, and drives N connections and asserts
  it stayed within `maxIdle` plus peak concurrency (§2.2).
- **5.5** When the sample is exercised, `Cajeta.liveCount()` is equal before
  and after the run, so no request leaks an object. The test notes that the
  gauge is blind to arrays, and the pool's own counter covers those.
- **5.6** When the sample is built, it lives in the primavera repo under
  `samples/register-user`, has its own `cajeta.json`, depends on the
  published primavera and `cajeta-cloud` archives, and is run by the repo's
  self-test script so it cannot rot.
- **5.7** When a user registers, the application mints its own user id
  before calling the identity port, and stores the provider and the subject
  the port returns against that id, unique per provider. The application's
  id is the primary key of the user record and the one its routes carry.
  The provider's subject is a link, so a provider change re-links subjects
  instead of rekeying the system (identity spec §3.1, §15.17). Decided
  2026-09-25.
- **5.8** When the application needs the user record, it goes through a
  `UserDirectory` seam in primavera: create, resolve by application id, and
  resolve by provider and subject. The memory implementation serves tests
  and the sample's HTTP phase. A durable implementation is application
  configuration over a store, the filesystem driver in `cajeta-cloud-local`
  first, never a store carried by the sample and never cajeta-cloud's.
- **5.9** When a request needs the username behind an application id, for
  the confirm route, the directory answers it and the port is called with
  the username. The sample keeps `POST /users/{id}/confirm`.

---

## 6. Feature: WebSocket on the same buffers

- **6.1** When a client upgrades a connection to WebSocket, the connection
  keeps its input and output buffers, and frames are decoded from and encoded
  into them. The existing `WsFrameDecoder` already reads through a
  `ByteBuffer` and is pointed at the pooled one.
- **6.2** When a message arrives, its payload is available as a view (§3.1)
  or as a JSON index (§3.2) over the input buffer for the duration of the
  handler, under the same copy-out rule (§3.5).
- **6.3** When a message is sent, it is encoded into the output buffer
  through the codec seam (§7) and framed in place.
- **6.4** When a connection is open, it has a connection id, a connection
  scope for components that live as long as the socket, and an entry in a
  registry addressable by connection id and by principal once identity is
  established (§8). This is the single-node minimum of `Connections.md`.
- **6.5** When sends to a connection outpace the socket, the send queue is
  bounded and the overflow policy is configured: drop-oldest, drop-newest,
  or close. Growth without bound is not an option.
- **6.6** When a connection closes, its scope is dropped, its registry entry
  is removed by that drop, and both buffers return to the pool.
- **6.7** When the sample runs the WebSocket phase, a client subscribes on
  `/events` and receives a message when a user registers, and the self-test
  asserts the pool's allocation count is flat across N messages.

---

## 7. Feature: pluggable codecs and compression

- **7.1** When a body is decoded or encoded, the codec is chosen by media
  type from a registry, and the media type comes from `Content-Type` on input
  and from `Accept` negotiation on output. JSON is the default and always
  present.
- **7.2** When a codec is registered, it implements `cajeta.wire.Encoder<T>`
  and, where it can, `BufferEncoder<T>` (§4.3). The protobuf, avro and ion
  codecs in `cajeta-codec` are registered by a dependency, not by primavera.
- **7.3** When a content coding is negotiated, the compressor is chosen by
  token from a registry of `cajeta.wire.Compressor` and `Decompressor`.
  gzip and deflate are present by default through `cajeta-http`. A coding
  such as brotli or zstd is added by a dependency and needs no change in
  primavera or `cajeta-http` (§1.3.4).
- **7.4** When a WebSocket connection negotiates permessage-deflate, the same
  compressor registry serves it.
- **7.5** When codecs or codings are added, they arrive as compiled
  dependencies contributing to the registry through the DI graph
  (multibinding over the compiled-in set, `primavera-spec.md` §16 R2). There
  is no runtime registration call.
- **7.6** When a request names a media type or coding the registry does not
  hold, the response is 415 or 406 respectively, naming what is supported.
- **7.7** When the sample runs the codec phase, `POST /users` accepts the
  registration as a protobuf message selected by `Content-Type`, accepts a
  gzip-coded JSON body, and answers in the negotiated type and coding, and
  the self-test covers each pairing.

---

## 8. Feature: security

Realizes `primavera-spec.md` §8 over the identity port. The six commitments
there (type-checked stage ordering, typed route identities, single path
normalization, uniform default-closed, secure production defaults,
locale-independent comparison) are requirements here, not restated.

- **8.1** When the sample runs the security phase, it serves `POST
  /users/{id}/confirm`, `POST /login`, `POST /login/challenge`, `POST
  /token/refresh`, `POST /logout`, and `GET /users/me`, and the last answers
  401 without a token, 403 with a token lacking the required group, and 200
  otherwise.
- **8.2** When a request carries a bearer token, primavera verifies it
  through the port's token verifier, builds a `SecurityContext` (principal,
  claims, roles) on a `FiberLocal` for the request, and binds the principal
  scope to it. The principal is the application's user id, resolved
  from the token's subject through the user directory (§5.8), never the
  subject itself.
- **8.3** When a user's groups are read from claims, they map to primavera
  roles through configuration, and roles feed the default-closed policy
  algebra. The port never sees roles (identity spec §7.4).
- **8.4** When sign-in answers challenge-required, the login endpoint
  returns the challenge kind and session handle, and the challenge endpoint
  completes it. OTP delivery is the provider's. The sample reads the memory
  driver's test hook to obtain the code.
- **8.5** When identity is configured, the adapter is selected per identity
  spec §9, and the required capabilities (TOTP for the sample) are asserted
  at startup.
- **8.6** When session ids, CSRF tokens or challenge handles are generated,
  they come from a CSPRNG. The stdlib has the `getrandom` source in its
  native runtime for seeds and no public CSPRNG API. That API, HMAC-SHA256
  and a constant-time compare are prerequisites of this phase and are stdlib
  work, not primavera's.
- **8.7** When a security check fails, the response body carries no detail
  that distinguishes an unknown user from a wrong password or an expired
  token from a forged one, and the log line does.
- **8.8** When the token is a real provider's, the verifier needs RS256 or
  ES256 (identity spec §5.6). The sample's security phase is complete against
  the memory driver's HS256 tokens, and the asymmetric case is accepted with
  the first provider adapter.

---

## 9. Feature: module layout

`primavera-spec.md` §2 decided a multi-repo split into `primavera-core`,
`-web`, `-security` and others. Today there is one repo and one archive,
`dev.cajeta.primavera`, holding core only.

- **9.1** When this spec's code lands, it lands in packages
  `dev.cajeta.primavera.web` and `dev.cajeta.primavera.security` inside the
  existing repo and archive, with no dependency from core on either.
- **9.2** When a consumer needs core without web, or web without security,
  the package is split out into its own repo and archive at that point, and
  the package names do not change. The split is a repo move, not an API
  change. §10.1 asks whether to split now instead.

---

## 10. Open questions (resolve at plan time)

- **10.1** *(resolved 2026-09-25, Julian: packages now.)* Split repos now, per the 2026-06 decision, or packages now and
  repos on first demand (§9)? Recommendation: packages now. Three repos with
  three release trains before the first sample runs is process ahead of code,
  and the package names make the later move mechanical.
- **10.2** *(resolved 2026-09-25: 64 KiB default, configurable.)* Input buffer size and the fits-or-streams threshold (§2.4).
  Recommendation: 64 KiB default, configurable, and the sample's bodies are
  under 1 KiB so the streaming path needs its own test, not the sample.
- **10.3** *(resolved 2026-09-25: public and documented.)* Should the typed endpoint surface (§5.2) be the documented public
  API, or an implementation detail the annotation form hides? Recommendation:
  public and documented. It is what tests and generated code both call.
- **10.4** *(resolved 2026-09-25: cajeta-http 0.4.0.)* Does `cajeta-http`'s buffer seam (§1.3) land as a `cajeta-http`
  0.4.0 with a `cajeta` release in between, or ride a primavera-owned fork of
  the read loop? Recommendation: `cajeta-http` 0.4.0, because the WebSocket
  decoder is already there and two read loops would drift.
- **10.5** *(resolved 2026-09-25: close.)* WebSocket overflow default (§6.5): drop-oldest or close?
  Recommendation: close, with the code saying why. A silently thinned event
  stream is harder to debug than a closed one.
- **10.6** *(resolved 2026-09-25: bearer first.)* Bearer tokens or cookie sessions as the default posture
  (`primavera-spec.md` §16 open item)? This spec builds bearer first (§8.2).
  Cookie sessions over `SessionScope` come after, and CSRF with them.
- **10.7** *(resolved 2026-09-25, Julian: the roadmap moves onto the agents convention. The plan for this spec carries the open Phase 3 item and the cleanups as a unit of its own, and the repo's `plan/` directory retires to a pointer.)* Where the primavera repo's remaining roadmap items go. Phase 3's
  open item (wire `@Component` allocation modes to the scoped accessor pair)
  and the cross-cutting cleanups are compiler and core work outside this
  spec. Recommendation: move them into the plan for this spec as a Unit 0
  only if the HTTP phase needs them, otherwise into their own plan, and
  retire the repo's `plan/` directory in favour of the agents convention
  either way.

---

## 11. Acceptance criteria (spec-level)

- **11.1** After warm-up, N requests on one keep-alive connection perform
  zero fresh buffer allocations, and N connections stay within `maxIdle` plus
  peak concurrency, asserted by the sample's self-test (§2.2, §5.4).
- **11.2** Overwriting the input buffer after a handler returns leaves every
  stored value intact, asserted by a test that fails against a handler that
  keeps a view (§3.6).
- **11.3** `Cajeta.liveCount()` is equal before and after the sample's
  self-test run (§5.5).
- **11.4** A response larger than the output buffer is delivered correctly
  in bounded pieces and the buffer's capacity is unchanged afterwards (§4.2).
- **11.5** The sample runs end to end at the close of each phase, and its
  self-test is part of the repo's test script (§1.6, §5.6).
- **11.6** A content coding added by a test-only dependency is negotiated
  with no change to primavera or `cajeta-http` (§7.3).
- **11.7** Every WebSocket message and every HTTP request in the sample is
  served with the pool's allocation count flat (§6.7).
- **11.8** The security phase runs unchanged against the memory driver with
  `identity.provider` unset in the test profile, and refuses to start in the
  production profile until a provider is named (identity spec §9.4).
- **11.9** No password, code or raw token appears in the sample's captured
  output (§8.7, identity spec §13.6).
- **11.10** No existing consumer of `cajeta-http` changes to keep working
  after the buffer seam lands (§1.3).
