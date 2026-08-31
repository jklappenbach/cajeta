# Publisher trust — server contract v1

The half of `publisher-trust` that is not built in this repository. This
document is what a server has to do for the cajeta client to verify
anything.

The client covers §3.2–§3.6 today (`src/cajeta/buildtool/`:
`SignedEnvelope`, `OrgKeyDocument`, `OrgKeyCache`, `ReleaseMetadata`,
`ReleaseIntegrity`, `PublisherVerification`, `RepositoryDelegation`). Two
pieces of this contract are specified and NOT yet implemented client-side:
the signed `retracted` flag of §5.2, and the revocation statement of §3.8
— plan Units 9 and 10. A server may implement them ahead of the client;
nothing breaks, they simply go unread until those units land.

Clause numbers in parentheses cite `specs/publisher-trust-spec.md`.

Status: **approved 2026-08-31** (Julian, plan item 7.3.1), all eight
sections reviewed clause by clause.

## 1. Scope

Two surfaces, with different audiences and different risks:

- **Serving** (§3) — read-only, public, hit by every install. Getting it
  wrong breaks installs or, worse, makes verification silently vacuous.
- **Administration** (§5, §6) — write, authenticated, rare. Getting it
  wrong hands an attacker the ability to publish as somebody else.

A server may implement serving alone. The client degrades against one that
serves none of it (§3.7), which is what makes this deployable
incrementally.

**One exception, and it runs the other way.** Revocation (§3.8) fails
CLOSED once advertised: a repository that turns it on and then cannot
serve it stops installs rather than degrading. That is deliberate (§7.3),
and it makes `"revocation": true` the one capability to adopt last and
deliberately, not incrementally like the rest.

## 2. Terms

**Archive** — one `.cja` at a `(name, version)` coordinate, consumed as a
dependency by a build. Olla distributes libraries and only libraries
(spec 1.8.1); applications reach users through their platform's own
channel, so nothing here is written for them.

**Organization** — the publishing entity. It owns one or more dotted
namespace prefixes and holds signing keys. It is an ATTRIBUTE of an
archive, never part of its address: `(name, version)` remains the
coordinate (spec 4.5).

**Root key** — the repository's own ed25519 key. It signs key documents
and release metadata. Clients ship with the public half (spec 3.1).

**Key document** — the signed statement binding an organization to its
keys and namespaces. Schema: `org-key-document.json`.

**Release metadata** — the statement carrying an archive's hash and its
owning organization, signed by a delegated release key. Schema:
`release-metadata.json`.

**Repository delegation** — the root-signed statement naming which keys may
sign release metadata (spec §2.7). Schema: `repository-delegation.json`.
The root signs this and organization key documents, both rare, and can stay
OFFLINE; the delegated key does the per-publish signing.

An archive version is **immutable**. The v2 protocol is content-addressed;
a blob lives at its `sha256` and that URL never changes. Republishing an
existing `(name, version)` is refused, not resolved (spec 1.9).

## 3. Serving

### 3.1 Capability advertisement

```
GET /.well-known/cajeta-capabilities.json
→ { "protocol-versions": ["v1", "v2"], "revocation": true, ... }
```

`revocation` is absent or `false` unless the repository serves §3.8, and
turning it on is a ONE-WAY DOOR in practice: from then on a missing or
expired revocation statement makes clients refuse. Advertise it only once
the statement is being served reliably, and understand that turning it
back off silently disables the fastest protection the repository has.

Everything below is v2. A client that does not see `"v2"` here does not
ask for any of it, and treats the repository as serving no key documents
at all — the legacy path of spec 5.4. A server that implements this
contract but forgets to advertise v2 has therefore disabled publisher
verification without any error appearing anywhere.

### 3.2 The signed envelope

Both signed documents share one wrapper:

```json
{
  "format": 1,
  "root-key-id": "olla-root-1",
  "payload":   "<base64 of the UTF-8 JSON body>",
  "signature": "<base64 ed25519 over the DECODED payload bytes>"
}
```

**Sign the bytes you transmit.** The signature covers the decoded payload
exactly as sent, so there is no canonical-JSON step for signer and
verifier to disagree about. Do NOT parse a document, re-serialize it, and
sign the result — that is a well-known signature-bypass class, and this
format exists to make it unrepresentable.

`root-key-id` is a HINT, telling a client holding several roots which to
try first. The client reports the root that actually verified, not the one
named here (spec 6.3), so a wrong value is a performance bug rather than a
security one — but it must still name a real root.

`format` is checked exactly. A client refuses a version it does not
recognise rather than guessing at it.

### 3.3 Organization key document (6.1)

```
GET /v2/org-keys/<org>
→ 200  the signed envelope
→ 404  this repository serves no document for that organization
```

Payload body: see `org-key-document.json`. Requirements the client
enforces, listed so a server does not produce documents that will be
refused:

- `organization` must equal the `<org>` that was requested. A server that
  answers every request with one organization's document is refused —
  otherwise it could lend that org's namespaces to any name.
- `namespaces` must be non-empty. A document that owns nothing authorises
  nothing.
- Every key needs `id`, `algorithm` (`ed25519`), `public-key` (PEM
  SubjectPublicKeyInfo), `not-before`, `not-after`, with `not-after`
  strictly after `not-before`.
- Timestamps are RFC 3339, UTC, seconds precision, `Z` only. Offsets are
  refused rather than converted.
- The document's own `not-after` must be in the future. An expired
  document is refused even though its signature is good, and refused
  again if it is later found in a cache.

Serve documents with overlapping key windows during rotation. That
overlap is the entire mechanism by which a publisher rotates without a
flag day (spec 2.6).

### 3.4 Repository delegation (spec 2.7)

```
GET /v2/repository-keys
→ 200  the signed envelope
→ 404  this repository delegates nothing; the root signs releases itself
```

Payload body: see `repository-delegation.json`. Signed by the ROOT, never by
a delegated key — a key that could sign its own delegation would be
self-authorising, and the client refuses it.

`repository` in the payload must match the repository the client fetched it
from, or one repository's delegation could be replayed by another.

**Serving a 404 here is a supported configuration**, not a degraded one: it
means the root signs release metadata directly, which is the pre-delegation
shape. What it costs is that the root key must then be online for every
publish, so a compromise of the serving infrastructure forges organization
key documents rather than just releases. Prefer delegating.

Rotate the online key by serving a delegation with two keys whose windows
overlap, exactly as an organization rotates. A delegation whose keys have
ALL lapsed is refused outright — the client does not fall back to the root,
because "no delegation served" and "the delegation expired" are different
conditions and only the first is a supported configuration.

### 3.5 Release metadata (6.2, 5.1)

```
GET /v2/resolve?name=<name>&version=<version>
→ 200  the resolve body
→ 404  no such release
```

The body carries the plain v2 fields a non-verifying client already reads,
plus the envelope under `signed`:

```json
{
  "sha256": "sha256:...",
  "size": 1234567,
  "deps": [ ... ],
  "retracted": false,
  "signed": { "format": 1, "root-key-id": "...", "payload": "...", "signature": "..." }
}
```

**The `signed` half is authoritative and the plain half is never merged
into it.** A verifying client reads `sha256`, `organization` AND
`retracted` out of the payload only. This is deliberate: it lets one
response serve both kinds of client, and it means a mirror rewriting the
plain half accomplishes nothing.

`retracted` appears in both halves and they can disagree. The plain one
exists for non-verifying clients and is advisory; the signed one decides.
A flag carried only in the plain half would be one a mirror clears
invisibly, which would leave the single signal whose job is to reach a
client about to install something bad as the only unprotected field in
the response (§5.2).

Signed payload body: see `release-metadata.json`. It must carry `sha256`
and `organization`; `name` and `version` are strongly recommended so a
signed statement is self-identifying and cannot be replayed under another
coordinate by a future client that checks them. `retracted` absent means
false.

`organization` here is what binds the archive to a publisher. It must be
the organization that actually owns the name, decided by the same
authority that gated the upload (§4.1) — never computed from the name.

### 3.6 Blob

```
GET /v2/blob/<sha256>            → the .cja bytes
```

Unchanged from the v2 protocol. The hash in §3.5's signed payload is what
the fetched bytes are checked against.

### 3.7 Absence and failure are different answers

This is the single most important behaviour in §3, and the easiest to get
subtly wrong.

| Server response | Client reads it as |
|---|---|
| no `v2` in capabilities | serves no documents — legacy path (5.4) |
| `404` on `/v2/org-keys/<org>` | serves no document for that org — legacy path |
| `200` with a valid document | verify against it |
| `200` with a document that does not verify | REFUSE the install |
| `404` on `/v2/repository-keys` | delegates nothing; the root signs releases (§3.4) |
| `404` on `/v2/resolve` for a release it will serve a blob for | **must not happen** — see below |
| `404` on `/v2/revocations`, never advertised | no fast revocation — proceed (§3.8) |
| `404` or expired on `/v2/revocations`, after advertising it | REFUSE — the one absence that is a failure |
| `5xx`, timeout, connection reset | ERROR — refuse, do not degrade |

A server must not answer `404` for a transient internal failure. Doing so
converts an outage into a verification bypass: every client in the fleet
quietly takes the unverified path for as long as the fault lasts. If the
key store is unreachable, return `503`.

Equally, a server must not return `200` with an empty or placeholder
document to avoid a `404`. A document that parses and authorises nothing
is refused, but the failure will be reported as a verification problem
rather than as absence, which sends operators to the wrong place.

**A v2 repository must not 404 `/v2/resolve` for a release whose blob it
will serve.** A 404 there reads as "this repository publishes no signed
metadata for that release", and the client falls back to the unsigned
`.sha256` sidecar — losing the publisher binding entirely while the install
still succeeds. That is the same bypass class as the transient-404 case
above, reached from a different direction.

This one is a SERVER obligation and not client-enforceable: the client
cannot know a blob exists for a coordinate whose metadata it was told does
not exist. An implementer has to hold this invariant themselves — serve
metadata for everything you serve bytes for, or serve neither.

Spec §5.3's default closes it on the client side once it lands, since "no
verification was possible" becomes a refusal rather than a fallback. Until
then the invariant is the only thing standing between a selective 404 and a
silent downgrade.

### 3.8 Revocation statement (spec 2.8)

```
GET /v2/revocations
→ 200  a signed envelope
→ 404  this repository does not do fast revocation
```

Same envelope as §3.2, with one difference that matters: **it is signed by
the DELEGATED key, not the root.** It verifies against the keys in §3.4's
delegation, so a repository serving no delegation cannot serve this
either — fast revocation is something delegation buys (spec 2.8.2).

The signed payload carries a required `type: "key-revocation"`, the
repository it speaks for, a short `not-after`, and the revoked key ids.
See `key-revocation.json`.

It can only SUBTRACT trust: a key id it names becomes unusable, and it can
add no key, widen no namespace, and issue no document. That asymmetry is
why an online key is allowed to sign it. A compromised delegated key can
revoke an organization's key and cause a loud, recoverable outage; it
cannot forge one (spec 2.8.1).

**Serve it fresh and expect it to be refused when stale.** Windows are
minutes to hours. A revocation an attacker can suppress is not a
revocation — yesterday's statement is indistinguishable from "nothing is
revoked" unless the statement bounds its own age (spec 2.8.3).

**This is the one place in §3 where absence and failure do NOT both
degrade safely.** Everywhere else a 404 means "this repository does not
serve that" and the client takes a weaker path. Here, once a repository
has set `"revocation": true` in §3.1, a missing or expired statement is a
FAILURE:
the client refuses rather than proceeding unrevoked, because failing open
would make blocking one fetch equivalent to un-revoking every key (spec
2.8.4). A repository that never advertises it is unaffected.

Revocation is permanent for a key id — there is no un-revoke. Entries may
be pruned once a root-signed document omitting the key is being served,
which is what bounds the list (spec 2.8.5, 2.8.6).

## 4. Upload refusals

These are the clauses that make §3.6's legacy path a legacy path rather
than a standing hole. An unverifiable artifact must never enter the
repository in the first place.

**4.1** An upload is REFUSED unless its signature verifies against a key
that is inside the PUBLISHING ORGANIZATION'S OWN current key document and
usable at upload time.

"Own" is the word that gets dropped. A server that verifies against a
global set of registered keys accepts one organization's key signing an
upload under another organization's name; the archive then sits in the
repository looking published and fails verification on every client that
installs it. Being known to the server is not the test — being in that
organization's document is.

**4.2 (6.5)** So an upload from an organization with no current key
document is refused: there is no key to verify against. Verification is
not something a publisher can decline by omission.

**4.3 (6.7)** And when an organization's only key has expired, its uploads
are refused until a current document is published. An expired key cannot
produce a verifiable artifact, so accepting the upload would store
something no client can install.

**4.4 (6.6)** Registering a key document therefore precedes an
organization's first upload. It is part of onboarding, not of publishing —
create the organization and its document in one administrative act.

**4.5** An upload of a name outside the organization's namespaces is
refused. This is the same check the client performs (spec 4.3), and it
belongs on both sides: the client's copy protects a user from a
compromised server, and the server's copy protects every user from an
upload that would otherwise sit there failing verification.

The namespaces are the ones in the organization's current key document —
the same signed list the client reads, which is what makes the two checks
one check rather than two mechanisms sharing a name. How a namespace gets
into that list is §6.5.

The namespace match is SEGMENT-AWARE. `dev.cajeta` owns `dev.cajeta` and
`dev.cajeta.http`; it does not own `dev.cajetaevil`. A plain string prefix
test passes every example written with well-behaved names and fails
against a name chosen adversarially, which is the only case that matters
(spec 4.3.1).

## 5. Archive lifecycle

There are three verbs, not four. `read` is §3 and needs no privilege.

**5.1 Publish** (org). Creates a new `(name, version)`. Gated by §4.1–4.5.
Refused if the coordinate already exists — a version is immutable (spec
1.9), so a change is a new version.

What is immutable is the BYTES. The signed statement about them is not:
§5.2 rewrites it. Do not read this clause as freezing the metadata.

**5.2 Retract** (org). Flips `retracted` in the release metadata. The
bytes STAY reachable: a lockfile already pinning that version keeps
resolving, and new resolves warn. This is the withdrawal a publisher
performs for a bad release, and it is deliberately not deletion — a
registry where a publisher can delete is one where a dependency vanishes
under someone else's build (spec 7.6).

**Retracting RE-SIGNS the release metadata** with the delegated key, since
the flag is inside the signed payload (§3.5). This is the per-publish work
the delegation exists to authorise, and retractions are rarer than
publishes, so it adds no new demand on the root (spec 7.6.2).

It follows that a client caching release metadata can serve a stale
un-retracted view. Release metadata carries no expiry of its own, unlike
an organization document, so a repository that expects retraction to be
timely must keep its resolve responses uncacheable or short-lived. A
retraction nobody re-fetches is a retraction that did not happen.

Un-retraction is permitted — a mistaken withdrawal must be reversible —
and it re-signs the same way. It is also a downgrade path for stolen
publish credentials, so it is recorded like every other mutation (§6.6).

**5.3 Remove** (owner only). The bytes stop being served. This breaks
every downstream build pinning that version, by construction, so it is an
emergency power — a leaked credential inside a release, unlawful content
— and never routine withdrawal (spec 7.5).

Blobs are content-addressed (§3.6), so one blob can back several
coordinates. Remove DELETES THE CONTENT: the blob goes, and any other
coordinate sharing those bytes stops resolving with it. Both motivating
cases are about the bytes themselves, and a remove that leaves them
fetchable under their hash has not done the job. The collateral loss is
the right trade at this severity — but it is a judgement, so the owner is
shown which other coordinates the removal will take down before it runs.

**Retire the blob BEFORE the metadata**, or atomically with it. A remove
that unpublishes the record first and collects the bytes afterwards is
the natural implementation and it is the wrong order: for that window the
repository serves a blob whose metadata 404s, which is exactly the
selective-404 downgrade §3.7 forbids — verifying clients fall back to the
unsigned sidecar and install the release anyway. The other order merely
makes a resolve succeed whose fetch then fails, which is noisy and safe.

**5.4** An organization may publish and retract only within the namespaces
its key document claims. This is routine self-service and NOT a path into
namespaces it does not own, because §6.2 puts that boundary out of its
reach: only the owner edits the key document that defines it (spec 7.6.1).

## 6. Administration

Two roles: OWNER (administrative privilege over the whole repository) and
ORGANIZATION. Every mutation below is one or the other; there is no
anonymous write (spec 7.1).

**6.1 (7.2)** The owner can create, read, update and delete organizations.
The write verbs STAGE — see §6.4.

Deleting an organization removes the key document every archive it
published verifies against: installs that worked yesterday stop working,
no bytes are removed, and nothing in the repository looks wrong. Treat it
like §5.3's remove — WARN AND CONFIRM, showing which archives the deletion
makes unverifiable. Not a refusal: a repository is a delivery hub, not the
system of record for who an organization is, and recovery is re-onboarding
plus a CI republish rather than a restore (spec 7.2.1, 7.2.2).

**6.2 (7.3)** The owner can create, read, update and delete an
organization's public keys. **An organization cannot modify its own
keys.** These write verbs stage too.

**6.3 (7.8)** Revoking a key is available to the owner WITHOUT waiting for
a replacement. Compromise response is "stop trusting this key now", and
requiring a new key first would delay the only urgent step. Do not make
revocation a special case of update.

§6.4 stages rather than signs, so the durable revocation — a re-signed
document omitting the key — waits on the offline ceremony, and this clause
would be unachievable on its own. §3.8's delegated revocation statement is
what makes it real: the brake applies in seconds against the online key,
and the re-signed document follows as the repair. Shortening document
lifetimes is NOT an alternative — it puts the root online and defeats §2.7.

So revocation is two acts with different latencies, and an implementation
that provides only the slow one has not implemented this clause.

**6.4 (7.9)** A key document published through this surface is signed by
the root key. The administrative API is how documents come to exist; it
must not introduce a second, unsigned path to the same data.

**THE ADMINISTRATIVE API STAGES; IT DOES NOT SIGN.** Its write verbs
record an intended next document and take effect only when a root
signature arrives. The signature is an explicit offline act performed
outside this API, and **the administrative surface never holds the root
key** — if it did, compromising an admin credential would forge any
organization's document, which is the collapse §2.7 exists to bound.

So §6.1 and §6.2's verbs are requests, not mutations. An organization
created through them does not exist to a client until its first document
is signed and served, and a key added to a document authorises nothing
until the same. Say so in the API: a staged change that reads back as
applied is how an operator concludes a revocation took effect when it did
not.

The cost is latency. Every change to a key document — onboarding, key
rotation, a namespace addition, a revocation — waits on the offline
ceremony. That is the right trade for the first three and the wrong one
for the fourth, which is §6.3's problem.

**6.5** An organization's `namespaces` enter its key document at
issuance, on evidence of control over the corresponding name — a DNS
record, a file in a repository, whatever the operator is willing to
accept. The owner checks that evidence once and records it (6.6); the root
signature then carries the claim.

The alternative is a namespace table consulted at publish time. It refuses
the same uploads, so it looks equivalent, and it is not: the client cannot
see it, so §4.5 stops being one check performed twice, and a compromised
server rewrites the ownership map with no signature to forge. Evidence of
control belongs at issuance, where a root signature can cover the result.

**6.6 (7.7)** Every mutation is authenticated, attributed, and recorded.
Who changed which key, and when, is the question that matters after a
compromise, and it cannot be reconstructed later if it was not recorded at
the time. Record the actor, the target, the before and after, and the
time — for administrative mutations and for publish, retract and remove
alike.

**6.7 (7.10)** Publishing and key management are separate privileges. An
organization publishes constantly and can never touch a key; the owner
touches keys and has no reason to publish. How an organization
authenticates in order to upload is unchanged by this contract — only what
that authentication is then permitted to reach.

## 7. Consequences not to "fix"

Every clause below looks like a defect to a reasonable implementer. Each
is deliberate, and quietly relaxing it removes most of what this contract
buys. The list carries no count on purpose: it grew three entries in one
review, and a numbered title goes stale every time the contract does.

**7.1 An organization cannot rotate its own key (§6.2), and that is the
point.** Taking over an organization's account does not let an attacker
swap the key and publish as that organization: account compromise and
signing compromise stay separate.

The cost is larger than "the owner is in the loop", and worth budgeting
honestly. Because §6.4 stages rather than signs, compromise recovery is
TWO PHASES with different latencies — revoke against the online delegated
key in seconds (§3.8), then re-sign a document omitting the key at the
next offline ceremony. An implementer who reads this clause as costing an
API call will discover a ceremony. §3.8 is what keeps the bottleneck from
becoming a denial of service, so build it before anyone proposes
self-service rotation as the workaround (spec 7.4).

**7.2 Never derive an organization from an archive's name.** Dotted names
have no fixed arity — `uk.co.acme.thing` and `io.foo.bar` place the
boundary differently — so any rule for "how many leading segments are the
org" is wrong for someone, and wrong in the direction an attacker selects
for. Ownership is data this server holds, not a string operation anyone
performs on a name (spec 4.4, 7.11).

This is not hypothetical. Deployed olla derives the owner from the first
two segments (`cajeta-olla/src/lib/namespace.ts:78`), which maps BOTH
`uk.co.acme.thing` and `uk.co.evil.thing` to `co.uk` — two unrelated
publishers collapsed onto one ownership key, and that key a public suffix
nobody can hold. It fails toward collision rather than refusal. Found by
reading the code, not by reasoning about the rule (spec 9.5).

The trap is concrete on the client side too: the build tool ships
`ManifestDetails::group()`, which splits a dotted name at the last `.`. It
exists for display and for a template property, it is used nowhere
security-relevant, and it must stay that way. A server-side equivalent
will look reasonable at the moment it is written.

**7.3 Revocation fails CLOSED (§3.8), including at 3am.** Once a
repository advertises `"revocation": true`, a missing or expired statement
makes clients refuse. When that endpoint faults, installs stop across the
fleet, and every instinct will say to soften it — degrade gracefully,
warn instead of refuse, serve the last good statement past its window.

Each of those makes blocking a single fetch equivalent to un-revoking
every key in the repository, which is the exact attack the mechanism
exists to stop. A revocation an attacker can suppress is not a revocation.

This is the most fixable-looking and most dangerous-to-fix behaviour in
the contract. The right response to the outage is to make the endpoint
reliable — it is a short signed blob, cacheable at the edge for its own
lifetime — not to make its absence harmless. A repository unwilling to
carry that obligation should not advertise the capability; declining it is
supported and honest, and softening it while still advertising is neither.

**7.4 Remove deletes shared content (§5.3).** Blobs are content-addressed,
so removing one release can take out an unrelated coordinate that happened
to publish identical bytes. The natural fix is to unlink only the
coordinate — which leaves the leaked credential or the unlawful content
fetchable under its hash, i.e. leaves undone the one thing the verb exists
to do.

**7.5 Retraction re-signs (§5.2).** Someone will ask why the flag cannot
just live in the unsigned half of the resolve body, since retracting is
then a database write instead of a signing operation. It used to live
there. That let any mirror clear a retraction invisibly, leaving the one
signal whose job is to reach a client about to install something bad as
the only unprotected field in the response (spec 7.6.2).

**7.6 A staged change reads back as STAGED (§6.4).** Because the
administrative API stages and the root signs offline, a revocation shows
as pending until the ceremony completes. This will be filed as a bug, and
the fix — report it as applied — is precisely how an operator concludes a
revocation took effect when it did not. Report the truth and make the
pending state legible instead.

## 8. Conformance

### 8.1 What is executable today

`test/buildtool/OllaContractStub.h` serves §3 against the shipped client
and `OllaContractTests.cpp` runs the client against it — eight tests: the
whole chain end to end, silent-disable when `v2` is unadvertised,
absence-degrades vs failure-refuses, the signed half beating the plain
half, a payload altered after signing, an unsigned resolve binding no
publisher, a client holding a different root, and rotation across
overlapping key windows.

The stub routes `/.well-known/cajeta-capabilities.json`,
`/v2/org-keys/<org>`, `/v2/resolve` and `/v2/blob`. A server can be
checked the same way: point `HttpRepository` at it and run the same
assertions.

### 8.2 What it does not reach yet

**`/v2/repository-keys` (§3.4) and `/v2/revocations` (§3.8) are not
served by the stub**, so neither is contract-tested. Delegation has unit
coverage (`RepositoryDelegationTests`, 7 tests) but has never been
exercised through a served response; revocation has no client at all yet.
The signed `retracted` flag (§5.2) is likewise specified and unread.

These are work items, not permanent gaps — plan Units 9 and 10 carry them.
Until they land, §8.1's suite passing means less than it appears to: it
proves the paths that existed before this contract grew.

### 8.3 Self-checks for what no client can see

Most of §4 through §6 has no client-observable surface, so it cannot be
covered the way §3 is. That is a reason for a server to test itself, not a
reason to leave it untested. Each line below is a check a server
implementation should hold, phrased so it can be written as a test.

**Refusals (§4).** An upload is refused when it is:

- signed by a key valid in ANOTHER organization's document (§4.1) — the
  cross-organization case, and the one most likely to pass by accident;
- from an organization with no current key document (§4.2);
- from one whose only key has expired (§4.3);
- for a name outside the organization's namespaces (§4.5), including the
  negative: `dev.cajeta` does not own `dev.cajetaevil`.

**Lifecycle (§5).**

- Re-publishing an existing `(name, version)` is refused (§5.1).
- Retracting produces release metadata that still verifies, now with
  `retracted: true` INSIDE the signed payload (§5.2). Un-retraction works
  and is recorded.
- Removing retires the blob before the metadata, never the reverse
  (§5.3) — the window in between is §3.7's downgrade.
- Removing a release whose bytes another coordinate shares takes that
  coordinate down too, and the owner saw it listed first (§5.3).

**Administration (§6).**

- A PUBLISH credential cannot reach any key-management endpoint (§6.7).
  This is the §9.4 regression test and it belongs in every olla build.
- A staged change reads back as STAGED until the signature arrives
  (§6.4), never as applied.
- Deleting an organization lists the archives it will make unverifiable
  before proceeding (§6.1).
- Every mutation — administrative, and publish/retract/remove alike —
  records actor, target, before, after, time (§6.6).

**Two absences, which are the two findings this review measured.** Both
are checks that something does NOT exist, so they need a grep or a review
gate rather than a test:

- The administrative surface holds no root key (§6.4). If an admin
  credential can produce a root signature, §2.7 bought nothing.
- No code path computes an organization from an archive name (§7.2). The
  deployed instance had one and it collapsed two publishers onto a public
  suffix (spec 9.5).

**One server obligation from §3** belongs here rather than in §8.1,
because the client cannot check it: never answer `404` on `/v2/resolve`
for a coordinate whose blob is still served (§3.7).

### 8.4 Adoption order

Three sequencing constraints, and only the first is old.

**8.4.1 Serve before requiring (spec 9.3).** Not negotiable: the client
default cannot require what the server does not yet serve. Olla serves key
documents first; the client default flips afterwards.

**8.4.2 Serve ahead of the client freely.** A server may implement signed
retraction (§5.2) and revocation (§3.8) before Units 9 and 10 land.
Nothing breaks — the fields go unread until the client catches up — so
there is no reason to hold them back.

**8.4.3 Advertise `"revocation": true` LAST.** This one runs opposite to
every other capability here. Setting it makes a missing or expired
revocation statement refuse installs fleet-wide (§7.3), so it is adopted
only once that endpoint is reliably served, and it is not something to
turn on early and tune later. Everything else in this contract degrades;
this does not.
