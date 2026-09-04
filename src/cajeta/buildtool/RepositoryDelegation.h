// The repository delegation — publisher-trust spec §2.7.
//
// Release metadata is signed on EVERY publish, so whatever key signs it has
// to be reachable by request-handling code. The root key must not be that
// key: a root compromise forges any organization's key document, which is
// total collapse rather than a bounded loss.
//
// So the root signs this, and this names the keys that may sign release
// metadata. The root itself then only signs rarely — organization key
// documents and this — and can live offline. A compromise of the online key
// forges release metadata and nothing else, and rotating it out is one
// offline signature instead of a new toolchain release.
//
// WHY THIS IS NOT AN OrgKeyDocument, despite the near-identical shape: if a
// client ever accepted one as the other, any organization's signing key
// would be able to sign release metadata for every other organization. The
// two are made unmistakable in both directions — a delegation carries a
// REQUIRED `type` discriminator inside the signed payload, and an
// organization document is identified by `organization` + `namespaces`,
// which a delegation never carries.

#pragma once

#include "cajeta/buildtool/OrgKeyDocument.h"
#include "cajeta/buildtool/SignedEnvelope.h"

#include <llvm/Support/Error.h>

#include <ctime>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct RepositoryDelegation {
        // The repository this speaks for, as an ORIGIN — checked against the
        // one it was fetched from, so a delegation cannot be replayed at
        // another repository whose online key would then sign for both.
        //
        // An origin, never the manifest's `name`: that is a label the USER
        // chooses, so a document naming it would verify on one machine and
        // be refused on the next.
        std::string repository;
        // Keys permitted to sign release metadata. Reuses OrgSigningKey: the
        // validity-window rules are identical and there is no reason for a
        // second implementation of "is this key usable now".
        std::vector<OrgSigningKey> keys;
        std::time_t notAfter = 0;
        // When this delegation was produced (spec §2.9, which governs every
        // signed document and not only the organization one). REQUIRED, for
        // the same reason it is there: expiry alone does not stop a replay,
        // because a superseded delegation is still validly signed and still
        // inside its own window. Serving last quarter's copy reinstates the
        // release key that was rotated out.
        std::time_t issuedAt = 0;
        std::string rootKeyId;      // which root signed it (spec §6.3)

        // Keys inside their window at `now`. Empty is a legitimate answer for
        // a delegation whose keys have all expired, and the caller must read
        // it as "cannot verify", never as "verified".
        std::vector<const OrgSigningKey*> usableKeys(std::time_t now) const;
    };

    // Parse and verify a delegation envelope against the repository roots.
    //
    // Fails when the envelope is malformed, the signature matches no trusted
    // root, the payload is not of type `repository-delegation`, or the
    // delegation has expired. Expiry is an ERROR rather than a flag, for the
    // same reason it is on an organization document: nothing downstream
    // should be able to hold an expired one and forget to check.
    // `origin` is the repository this was fetched from (Repository::origin()).
    // A delegation claiming a different one is refused.
    // `seenIssuedAt` is the newest issued-at already accepted for this
    // repository, and 0 means none — which is correct on a first fetch, so it
    // is defaulted. A caller holding a durable high-water mark passes it.
    llvm::Expected<RepositoryDelegation> loadRepositoryDelegation(
        const std::string& envelopeJson,
        const std::vector<RootKey>& roots,
        const std::string& origin,
        std::time_t now,
        std::time_t seenIssuedAt = 0);

} // namespace cajeta::buildtool
