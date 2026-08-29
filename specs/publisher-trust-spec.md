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
§8.

**1.6 Non-goals.** No full TUF deployment (no snapshot or timestamp
roles, so no rollback- or freeze-attack protection). No per-project keys.
No keyless/OIDC publishing. No changes to how a publisher authenticates
to olla when uploading.

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

**7.5** The owner can create, read, update and delete repositories.

**7.6** An organization can create, read, update and delete the
repositories it owns, and only those.

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

**7.10** Uploading an artifact remains an organization's own action and
is unchanged by this section (§1.6). Publishing and key management are
separate privileges on purpose: the frequent action does not carry the
dangerous one.

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
