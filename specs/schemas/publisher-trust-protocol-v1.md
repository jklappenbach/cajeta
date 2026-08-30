# Publisher trust — server contract v1

The half of `publisher-trust` that is not built in this repository. The
client is complete and shipped (`src/cajeta/buildtool/`: `SignedEnvelope`,
`OrgKeyDocument`, `OrgKeyCache`, `ReleaseMetadata`, `ReleaseIntegrity`,
`PublisherVerification`); this document is what a server has to do for that
client to verify anything.

Clause numbers in parentheses cite `specs/publisher-trust-spec.md`.

Status: **draft, pending Julian's confirmation** that it matches what olla
will build (plan item 7.3.1).

## 1. Scope

Two surfaces, with different audiences and different risks:

- **Serving** (§3) — read-only, public, hit by every install. Getting it
  wrong breaks installs or, worse, makes verification silently vacuous.
- **Administration** (§5, §6) — write, authenticated, rare. Getting it
  wrong hands an attacker the ability to publish as somebody else.

A server may implement serving alone. The client degrades against one that
serves none of it (§3.6), which is what makes this deployable
incrementally.

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

**Release metadata** — the signed statement carrying an archive's hash and
its owning organization. Schema: `release-metadata.json`.

An archive version is **immutable**. The v2 protocol is content-addressed;
a blob lives at its `sha256` and that URL never changes. Republishing an
existing `(name, version)` is refused, not resolved (spec 1.9).

## 3. Serving

### 3.1 Capability advertisement

```
GET /.well-known/cajeta-capabilities.json
→ { "protocol-versions": ["v1", "v2"], ... }
```

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

### 3.4 Release metadata (6.2, 5.1)

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
into it.** A verifying client reads `sha256` and `organization` out of the
payload only. This is deliberate: it lets one response serve both kinds of
client, and it means a mirror rewriting the plain half accomplishes
nothing.

Signed payload body: see `release-metadata.json`. It must carry `sha256`
and `organization`; `name` and `version` are strongly recommended so a
signed statement is self-identifying and cannot be replayed under another
coordinate by a future client that checks them.

`organization` here is what binds the archive to a publisher. It must be
the organization that actually owns the name, decided by the same
authority that gated the upload (§4.1) — never computed from the name.

### 3.5 Blob

```
GET /v2/blob/<sha256>            → the .cja bytes
```

Unchanged from the v2 protocol. The hash in §3.4's signed payload is what
the fetched bytes are checked against.

### 3.6 Absence and failure are different answers

This is the single most important behaviour in §3, and the easiest to get
subtly wrong.

| Server response | Client reads it as |
|---|---|
| no `v2` in capabilities | serves no documents — legacy path (5.4) |
| `404` on `/v2/org-keys/<org>` | serves no document for that org — legacy path |
| `200` with a valid document | verify against it |
| `200` with a document that does not verify | REFUSE the install |
| `5xx`, timeout, connection reset | ERROR — refuse, do not degrade |

A server must not answer `404` for a transient internal failure. Doing so
converts an outage into a verification bypass: every client in the fleet
quietly takes the unverified path for as long as the fault lasts. If the
key store is unreachable, return `503`.

Equally, a server must not return `200` with an empty or placeholder
document to avoid a `404`. A document that parses and authorises nothing
is refused, but the failure will be reported as a verification problem
rather than as absence, which sends operators to the wrong place.

## 4. Upload refusals

These are the clauses that make §3.6's legacy path a legacy path rather
than a standing hole. An unverifiable artifact must never enter the
repository in the first place.

**4.1 (6.5)** An upload is REFUSED when the publishing organization has no
current key document. Verification is not something a publisher can
decline by omission.

**4.2 (6.6)** Registering a key document therefore precedes an
organization's first upload. It is part of onboarding, not of publishing.
A server that lets an organization exist before it has a key reintroduces
4.1's gap at the moment the org is created — so create the org and its
document in one administrative act, or refuse uploads from a
document-less org.

**4.3 (6.7)** When an organization's only key has expired, its uploads are
refused until a current document is published. An expired key cannot
produce a verifiable artifact, so accepting the upload would store
something no client can install.

**4.4** An upload of a name outside the organization's namespaces is
refused. This is the same check the client performs (spec 4.3), and it
belongs on both sides: the client's copy protects a user from a
compromised server, and the server's copy protects every user from an
upload that would otherwise sit there failing verification.

The namespace match is SEGMENT-AWARE. `dev.cajeta` owns `dev.cajeta` and
`dev.cajeta.http`; it does not own `dev.cajetaevil`. A plain string prefix
test passes every example written with well-behaved names and fails
against a name chosen adversarially, which is the only case that matters
(spec 4.3.1).

## 5. Archive lifecycle

There are three verbs, not four. `read` is §3 and needs no privilege.

**5.1 Publish** (org). Creates a new `(name, version)`. Gated by §4.1–4.4.
Refused if the coordinate already exists — a version is immutable (spec
1.9), so a change is a new version.

**5.2 Retract** (org). Flips `retracted` in the release metadata. The
bytes STAY reachable: a lockfile already pinning that version keeps
resolving, and new resolves warn. This is the withdrawal a publisher
performs for a bad release, and it is deliberately not deletion — a
registry where a publisher can delete is one where a dependency vanishes
under someone else's build (spec 7.6).

**5.3 Remove** (owner only). The bytes stop being served. This breaks
every downstream build pinning that version, by construction, so it is an
emergency power — a leaked credential inside a release, unlawful content
— and never routine withdrawal (spec 7.5).

**5.4** An organization may publish and retract only within the namespaces
its key document claims. This is routine self-service and NOT a path into
namespaces it does not own, because §6.2 puts that boundary out of its
reach: only the owner edits the key document that defines it (spec 7.6.1).

## 6. Administration

Two roles: OWNER (administrative privilege over the whole repository) and
ORGANIZATION. Every mutation below is one or the other; there is no
anonymous write (spec 7.1).

**6.1 (7.2)** The owner can create, read, update and delete organizations.

**6.2 (7.3)** The owner can create, read, update and delete an
organization's public keys. **An organization cannot modify its own
keys.**

**6.3 (7.8)** Revoking a key is available to the owner WITHOUT waiting for
a replacement. Compromise response is "stop trusting this key now", and
requiring a new key first would delay the only urgent step. Do not make
revocation a special case of update.

**6.4 (7.9)** A key document published through this surface is signed by
the root key. The administrative API is how documents come to exist; it
must not introduce a second, unsigned path to the same data.

**6.5 (7.7)** Every mutation is authenticated, attributed, and recorded.
Who changed which key, and when, is the question that matters after a
compromise, and it cannot be reconstructed later if it was not recorded at
the time. Record the actor, the target, the before and after, and the
time — for administrative mutations and for publish, retract and remove
alike.

**6.6 (7.10)** Publishing and key management are separate privileges. An
organization publishes constantly and can never touch a key; the owner
touches keys and has no reason to publish. How an organization
authenticates in order to upload is unchanged by this contract — only what
that authentication is then permitted to reach.

## 7. Two consequences not to "fix"

Both of these look like defects to a reasonable implementer. They are
deliberate, and quietly relaxing either removes most of what this spec
buys.

**7.1 An organization cannot rotate its own key (6.2), and that is the
point.** Taking over an organization's account does not let an attacker
swap the key and publish as that organization: account compromise and
signing compromise stay separate. The cost is real — rotation and
compromise recovery need the owner in the loop, which is a bottleneck and
a response-time risk. §6.3 is what keeps that bottleneck from becoming a
denial of service, so implement revocation-without-replacement before
anyone asks for self-service rotation as a workaround (spec 7.4).

**7.2 Never derive an organization from an archive's name.** Dotted names
have no fixed arity — `uk.co.acme.thing` and `io.foo.bar` place the
boundary differently — so any rule for "how many leading segments are the
org" is wrong for someone, and wrong in the direction an attacker selects
for. Ownership is data this server holds, not a string operation anyone
performs on a name (spec 4.4, 7.12).

The trap is concrete: the build tool already ships
`ManifestDetails::group()`, which splits a dotted name at the last `.`. It
exists for display and for a template property, it is used nowhere
security-relevant, and it must stay that way. A server-side equivalent
will look reasonable at the moment it is written.

## 8. Conformance

The contract is executable, not prose alone. `test/buildtool/
OllaContractStub.h` implements §3 against the shipped client, and
`OllaContractTests.cpp` runs the client against it — a served document
verifies, a `404` degrades, a `5xx` refuses, an unsigned resolve binds
nothing, and a tampered payload is caught.

A server implementation can be checked the same way: point the client's
`HttpRepository` at it and run the same assertions. What the stub does NOT
cover is §4 through §6 — refusals and administration are server-side
behaviour with no client-observable surface, and they need their own
tests wherever olla is built.

Spec 9.3 fixes the deployment order and it is not negotiable: the client
default cannot require what the server does not yet serve. Olla serves key
documents FIRST; the client default flips afterwards.
