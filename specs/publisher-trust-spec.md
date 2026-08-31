# publisher-trust — verifying that an artifact came from its publisher

Spec status: **draft** (2026-08-29)

## 1. Definition

**1.1 Purpose.** Establish that a `.cja` fetched from a repository was
published by the organization that owns its name, and that the bytes are
the ones that organization released.

**1.2 The gap this closes.** Today a signature is verified against
whatever keys happen to sit in the machine's trust store
(`cajeta trust`). Nothing binds a key to a name, so a valid signature
proves only that *someone* signed the bytes. That is the failure mode
that made PyPI's GPG support worthless and led to its removal: signatures
without a name-to-key binding verify nothing anyone cares about.

**1.3 The authority.** `olla.cajeta.dev` decides who owns a namespace at
upload time, so it is already the authority on the mapping from
organization to key. This spec makes that mapping explicit, signed, and
fetchable, rather than implicit in who was allowed to upload.

**1.4 Scope.** One signing key per ORGANIZATION, not per project. A
publisher's release pipeline is the unit that gets compromised; splitting
keys per package multiplies operational burden without changing that
blast radius much.

**1.5 Threat model.** Protects against a hostile network, a hostile
mirror, a hostile CDN, and an attacker who can upload to the repository
under a name they do not own. Does NOT protect against a fully
compromised `olla.cajeta.dev` — olla is the authority, so an attacker
holding olla's root key can assert anything. Surviving that needs
evidence originating outside olla (a transparency log, threshold
signing), which is out of scope here and noted as the upgrade path in
§8. §2.7 nonetheless keeps the root OFFLINE, so a compromise of the
serving infrastructure forges a release, not an organization.

**1.6 Non-goals.** No full TUF deployment (no snapshot or timestamp
roles, so no rollback- or freeze-attack protection). No per-project keys.
No keyless/OIDC publishing. No changes to how a publisher authenticates
to olla when uploading.

**1.8 What olla holds: library archives.** Olla holds ARCHIVES. An archive
is one `.cja` at a `(name, version)` coordinate, consumed as a dependency
by a build. It is written down because §7 originally said "repository" for
this, which named nothing in olla's model and left §7.5–7.6
unspecifiable.

**1.8.1 Olla distributes LIBRARIES, and only libraries** (decided
2026-08-30). Applications reach users through the channel their platform
already has — apt, dnf, Homebrew, winget, the platform stores, Steam —
each of which has a trust chain, a review process and an update mechanism
built for programs that execute. A language registry would be a worse
version of all of them.

The distinction is structural, not a scoping preference. A library only
has meaning INSIDE a compile: it is an input to a build, so the compiler
is necessarily what fetches, verifies and links it, and that is why the
verification in this spec lives in the cajeta binary. An application has
no such relationship — whoever runs it never compiles — so nothing about
this spec's client-side model transfers to it. What cajeta owes the other
channels is a per-platform installer matrix built and published
atomically, which is `release.yml`'s job and not a registry protocol.

**1.9 An archive version is immutable.** The v2 protocol is
content-addressed — a blob lives at its `sha256` and that URL never changes
once published — so republishing under an existing `(name, version)` is
refused rather than resolved. This is not a policy choice layered on top;
it is what content addressing already means. Consequently there is no
UPDATE verb anywhere in §7 for an archive, and withdrawal splits into two
different acts (§7.5, §7.6).

**1.7 Repository boundary.** `olla.cajeta.dev` is a separate service and
is not built here. This spec defines CLIENT behaviour and the PROTOCOL
the service must serve; §6 is the server-side contract, testable here
only against a stub.

## 2. The organization key document

**2.1** When a client needs an organization's signing keys, it fetches a
key document for that organization from the repository.

**2.2** The document names the organization, the namespace prefixes it
owns, and one or more public keys, each with a validity window (not
before, not after).

**2.3** The document is signed by the repository's ROOT key. Serving it
over TLS alone is insufficient: that would make every mirror, proxy and
cache in the path a trust point, which is the property §1.5 exists to
deny.

**2.4** When the document's signature does not verify against a trusted
root key, it is rejected and no artifact from that organization installs.

**2.5** When the document has expired, it is rejected. A client must not
accept metadata whose validity window has closed, however it was
obtained.

**2.6** When an organization has more than one key with an overlapping
validity window, a signature by any one of them is accepted. This is what
makes rotation possible without a flag day.

**2.7 The root does not sign release metadata directly.** It signs a
DELEGATION naming the keys that may, and those keys do the per-publish work.

Release metadata is signed on every upload (§5.1), so whatever signs it must
be reachable by request-handling code. If that were the root, a compromise
of the serving infrastructure would forge any organization's key document —
total collapse rather than a bounded loss. §1.5 accepts a fully compromised
repository as outside the threat model, but that is a statement about what
is defended, not a licence to force the most valuable key online.

**2.7.1** The delegation is itself root-signed, carries its own validity
window, and names the repository it speaks for. A delegation fetched from
one repository does not authorise another.

**2.7.2** A delegated key outside its own window authorises nothing, and a
delegation whose keys have all lapsed is a REFUSAL — never a fall back to
verifying against the root. The fallback in 2.7.3 exists for a repository
that serves no delegation at all, not for one whose delegation has expired.

**2.7.3** When a repository serves no delegation, release metadata verifies
against the roots directly. This is the pre-delegation shape and stays
supported, so a repository can adopt delegation without a flag day. A root
signature is strictly stronger evidence than a delegated one, so accepting
both costs a client nothing; keeping the root offline is the repository's
operational discipline, which no client can police.

**2.7.4** A delegation and an organization key document must be
unmistakable for one another. Both are root-signed envelopes of keys with
validity windows, and a client that accepted one as the other would let any
organization's key sign release metadata for every organization. The
delegation carries a required, signed type discriminator; an organization
document is identified by the `organization` and `namespaces` a delegation
never carries.

## 3. The trust anchor

**3.1** The repository's root public key ships with the cajeta toolchain,
the way a CA bundle ships with an OS. A default install can verify
without an operator doing anything first.

**3.2** When a root key is rotated, the new one arrives through a
toolchain release, which is itself already signed and distributed.

**3.3** An operator can add an additional root for a private or mirrored
repository, and can pin a repository to a specific root.

**3.4** `cajeta trust` continues to manage locally trusted keys, and its
entries are additive to the shipped root. §9 covers what happens to
existing entries.

## 4. Verifying an artifact

**4.1** When an artifact is fetched, its signature is verified against a
key from the publishing organization's key document, valid at the time of
verification.

**4.2** When the signature does not verify against any valid key for that
organization, the install fails and nothing is written.

**4.3** When the artifact's name falls outside the namespaces the key
document claims, the install fails. A key valid for one organization must
not sign another's name.

**4.3.1** The match is SEGMENT-AWARE. `dev.cajeta` owns
`dev.cajeta.http` and does not own `dev.cajetaevil`. A plain string
prefix test passes every case written with well-behaved names and fails
against a name chosen adversarially, which is the only case that
matters.

**4.4** The client never DERIVES an organization from a name. Dotted
names have no fixed arity — `uk.co.acme.thing` and `io.foo.bar` place
the boundary differently — so any rule for "how many leading segments
are the org" is wrong for someone, and wrong in the direction an
attacker selects for. Ownership is data the repository holds (§1.3), not
a string operation the client performs. This is a non-goal, stated
because deriving it is the obvious shortcut.

**4.5** The organization is an ATTRIBUTE of an artifact, not part of its
address. `(name, version)` remains the coordinate; manifests, the
archive format, the repository layout and the resolver are unchanged. A
three-part `org:name:version` address would authenticate nothing on its
own — an attacker writes the org slot as freely as a name prefix — so it
buys no security for a great deal of ecosystem churn. If a third
coordinate is ever wanted, it is for readability and enumeration, and it
needs its own justification.

**4.6** When the repository publishes no key document for an
organization, the install fails under the default policy of §5.

## 5. Release integrity and policy

**5.1** The repository serves, per release, the artifact's hash alongside
its download location. The hash is covered by the same signed metadata
path as the rest of the release information, so a mirror cannot alter
it.

**5.2** When fetched bytes do not match the served hash, they are
discarded and the install fails.

**5.3** Publisher verification is the DEFAULT, not opt-in. `install`
verifies the publisher unless explicitly relaxed.

**5.4** When a repository serves no key document at all, verification
cannot be performed; the client refuses by default and the operator may
relax the policy for that repository explicitly. On a conformant
repository this is unreachable by construction (§6.5) — it exists for
private filesystem repositories, v1 servers, and artifacts predating
this spec.

**5.5** The relaxation is per repository, not global. Trusting a local
development repository must not weaken verification of the public one.

## 6. What the repository serves

**6.1** An endpoint returning the signed key document for an
organization.

**6.2** Release metadata carrying the artifact hash, the download
location, AND the owning organization, within the signed path of 5.1.
Carrying ownership here is what lets the client skip deriving it: the
resolve it already performs answers "who owns this name", so
verification costs no additional round trip and no parsing rule.

**6.3** The root key's identity is discoverable so a client can tell
which root signed a document it holds.

**6.4** These are additive. A client that verifies must degrade per §5.4
against a repository that serves none of them, rather than failing
opaquely.

**6.5** An upload is REFUSED when the publishing organization has no
current key document. Verification is not something a publisher can
decline by omission: an artifact that cannot be verified must never enter
the repository in the first place. This is what makes §5.4 a legacy path
rather than a standing hole.

**6.6** Registering an organization's key document therefore precedes its
first upload, and is part of onboarding rather than of publishing. A
repository that lets an org exist before it has a key reintroduces 6.5's
gap at the moment the org is created.

**6.7** When an organization's only key has expired, its uploads are
refused until a current document is published. An expired key cannot
produce a verifiable artifact (§4.1), so accepting the upload would
store something no client can install.

## 7. Administration and onboarding

The key documents §2 verifies and §6.5 requires have to come from
somewhere. This is that surface. It is entirely server-side and, like
§6, a contract on a service not built here.

**7.1 Roles.** OWNER (administrative privilege over the whole
repository) and ORGANIZATION. Every mutation below is one or the other;
there is no anonymous write.

**7.2** The owner can create, read, update and delete organizations.

**7.3** The owner can create, read, update and delete an organization's
public keys. **An organization cannot modify its own keys.**

**7.4** 7.3 is the load-bearing choice in this section, and it is a
deliberate trade. Because an organization cannot rotate its own key,
taking over an organization's account does not let an attacker swap the
key and publish as that organization — account compromise and signing
compromise stay separate. The cost is that rotation and compromise
recovery need the owner in the loop, which is a bottleneck and a
response-time risk. §7.8 is what keeps that from being a denial of
service.

**7.5** The owner can REMOVE an archive version outright — the bytes stop
being served. This breaks every downstream build pinning it, by
construction, so it is an emergency power (a leaked credential inside a
release, unlawful content) and never routine withdrawal. It is audited
under §7.7 like any other mutation.

**7.6** An organization can PUBLISH archives into the namespaces its key
document claims, and RETRACT ones it has published. Retraction flips the
release metadata's `retracted` flag: the bytes stay reachable, so a
lockfile already pinning that version keeps resolving, while new resolves
warn. That is the withdrawal a publisher performs for a bad release, and
it is deliberately not deletion — a registry that lets a publisher delete
is a registry where a dependency can vanish under someone else's build.

**7.6.2** The `retracted` flag is INSIDE the signed release metadata, and
retraction re-signs it. A flag carried only in the unsigned half of a
resolve response is one a mirror clears, and clearing it is invisible:
the client reads a release the publisher withdrew and is told nothing.
Retraction is the one lifecycle signal whose entire job is to reach a
client that is about to install something bad, so it needs the same
protection §5.1 gives the hash. Re-signing is what the delegated key
(§2.7) is for; a retraction is rarer than a publish.

**7.6.3** An archive's BYTES are immutable; the signed statement about
them is not. §1.9 fixes the first — a change is a new version. §7.6.2
makes the second mutable on purpose, and the two do not conflict as long
as no one reads §1.9 as freezing the metadata too.

**7.6.1** §7.6 is routine self-service and NOT a path into namespaces an
organization does not own, because §7.3 already put that boundary out of
its reach. What an org may publish is bounded by the namespaces in its key
document, and only the owner can change that document. The two clauses
were written to work together: without §7.3, "an organization manages its
own archives" would be an escalation path, and with it there is nothing to
escalate to.

**7.7** Every mutation is authenticated, attributed, and recorded. Who
changed which key, and when, is the audit question that matters after a
compromise, and it cannot be reconstructed later if it was not recorded
at the time.

**7.8** Revoking a key is available to the owner without waiting for a
replacement. Compromise response is "stop trusting this key now", and
requiring a new key first would delay the only urgent step.

**7.9** A key document published through this surface is signed by the
root key (§2.3). The administrative API is how documents come to exist;
it does not introduce a second, unsigned path to the same data.

**7.10** Publishing and key management are separate privileges on purpose:
the frequent action does not carry the dangerous one. An organization
publishes constantly and can never touch a key; the owner touches keys and
has no reason to publish. How an organization AUTHENTICATES in order to
upload is unchanged by this spec (§1.6) — only what that authentication
is then permitted to reach.

**7.11** Nothing in §7 may derive an organization from an archive's name.
The build tool already has `ManifestDetails::group()`, which splits a
dotted name at the last `.` — exactly the arity assumption §4.4 forbids.
It exists for display and for a template property, and it must stay
there. A server-side implementer reaching for the equivalent rule is
making the mistake §4.4 describes, and it will look reasonable at the
moment they make it.

**7.12** An organization's namespaces enter its key document at issuance,
on evidence of control over the corresponding name — a DNS record, a file
in a repository, whatever the operator is willing to accept. The owner
verifies that evidence once and records it (§7.7); the root signature then
carries the claim.

A namespace table consulted at publish time refuses the same uploads and
is not equivalent. The client cannot read it, so §4.3 has no signed list
to check against and the server's check stops being the client's check; and
a compromised server rewrites the ownership map with no signature to
forge. Evidence of control belongs at issuance, where a root signature can
cover the result.

## 8. Upgrade path

**8.1** The HTTP driver already has a transparency-log endpoint. If the
threat model later extends to a compromised olla, artifact digests
recorded in a log the repository does not solely control is the natural
next step, and this design does not preclude it.

**8.2** Snapshot and timestamp roles remain available as a later
addition if rollback and freeze attacks enter scope.

## 9. Migration

**9.1** Existing `cajeta trust` entries continue to verify signatures as
they do today, so a machine configured before this lands keeps working.

**9.2** When both a key document and a locally trusted key could verify
an artifact, the key document wins: the binding to a name is the property
being added, and a local key has none.

**9.3** The default cannot become "verify publisher" until the repository
serves key documents. A client that requires what no server publishes
rejects every install. Ordering is a hard constraint on any plan built
from this spec.

**9.4 The current key-registration flow contradicts §7.3, and does so in
production.** Found 2026-08-30 by reading the deployed code, not inferred:
olla's `POST /v2/keys` calls `authenticatePublish`
(`cajeta-olla/src/routes/keys.ts`), the same bearer token a publisher uses
to upload. A publisher therefore registers its own signing key today, and
`docs/specification/buildtool/olla-ci-publish.md` documents doing exactly
that as one-time setup.

That is the compound compromise §7.4 exists to prevent: one stolen
`OLLA_TOKEN` currently buys both the ability to register a key and the
ability to publish artifacts signed by it, so account compromise IS
signing compromise. §7.3 requires key management to be owner-only.

**9.4.1** Moving key registration behind owner authority is a MIGRATION,
not just a code change. Every library already publishing has a CI key
registered under the old rule, and those keys must be re-attested by the
owner rather than silently inherited — a key that got there under an
authority the spec no longer accepts has no more standing than one added
tomorrow by the same route.

**9.4.2** This sequences with §6.5. Refusing uploads from an organization
with no current key document is unenforceable while an organization can
mint its own key on demand, since the refusal is then one API call away
from being satisfied by the party it is meant to constrain.

**9.4.3 The gap is wider than key registration.** Measured 2026-08-31 in
the same deployed code. `getTrustKey` (`cajeta-olla/src/lib/catalog.ts:49`)
resolves a key by `key_id` alone against a repository-global `trust_keys`
table. The row carries a `principal` column and the publish path never
compares it to the authenticated principal, so any registered key verifies
an upload under any organization's name — §7.12's cross-organization case,
live. Three details compound it: `addTrustKey` rebinds an existing
`key_id` to new bytes on conflict (`catalog.ts:62`), so a key id is a
mutable pointer rather than a name for a public key; `POST /v2/keys` takes
the principal from the request body (`keys.ts:39`); and `trust_keys` has
no expiry column, which makes §6.7 unrepresentable rather than merely
unenforced.

**9.5 The namespace check derives ownership from the name, which is the
§7.11 trap in production.** `domainForPackage`
(`cajeta-olla/src/lib/namespace.ts:78`) takes the first two segments and
reverses them, then looks that up in a `namespaces` table proven by DNS
TXT or a GitHub file. Fixed arity 2 is wrong for any deeper reverse-DNS
name, and it fails toward collision rather than refusal:

| name | derived owner |
|---|---|
| `dev.cajeta.http` | `cajeta.dev` |
| `uk.co.acme.thing` | `co.uk` |
| `uk.co.evil.thing` | `co.uk` |

Two unrelated publishers collapse onto one key, and that key is a public
suffix nobody can hold. The check is also gated behind a
`REQUIRE_NAMESPACE` environment flag, so it is off unless switched on.

**9.5.1** §7.12 removes the derivation rather than correcting the arity.
Once namespaces are a signed list in the key document, there is no string
operation to get wrong and the client can perform the same check. The DNS
and GitHub proofs are good evidence and should survive the move — as
issuance-time input to the owner, not as a publish-time lookup.
