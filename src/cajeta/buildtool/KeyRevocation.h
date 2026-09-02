// The revocation statement — publisher-trust spec §2.8.
//
// The root is offline (§2.7), so a re-signed key document omitting a
// compromised key waits on the offline ceremony. This is signed by the
// DELEGATED key and applies in seconds: the brake, with the re-signed
// document as the repair.
//
// An online key may sign it because it can only SUBTRACT trust — it names
// key ids and makes them unusable, and can add no key, widen no namespace,
// and issue no document. A compromised delegated key causes a loud,
// recoverable outage and forges nothing.
//
// Wire format: specs/schemas/key-revocation.json

#pragma once

#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/RepositoryDelegation.h"

#include <llvm/Support/Error.h>

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct RevokedKey {
        std::string id;
        // Empty means EVERY document. Key ids are only required to be
        // unique within a document, so an unscoped id is ambiguous and the
        // broad reading is the safe direction to be wrong in.
        std::string organization;
        std::time_t revokedAt = 0;
        std::string reason;
    };

    struct KeyRevocation {
        std::string repository;
        std::time_t issuedAt = 0;
        std::time_t notAfter = 0;
        std::vector<RevokedKey> revoked;
        // The delegated key that actually verified it, not the one named.
        std::string signedByKeyId;

        // The entry revoking `keyId` for `organization`, or nullptr.
        const RevokedKey* find(const std::string& keyId,
                               const std::string& organization) const;
    };

    // Parse and verify a revocation statement.
    //
    // Verified against the DELEGATION's keys, never against a root: this
    // document's short lifetime is only sustainable because an online key
    // produces it, and accepting a root signature would invite exactly the
    // long-lived statement §2.8.3 forbids.
    //
    // `repositoryName` is the repository it was fetched from, checked
    // against the one it claims so a statement cannot be replayed
    // elsewhere. `seenIssuedAt` is the newest `issued-at` this client has
    // already accepted, or 0 — an older statement is refused, which stops a
    // rollback inside the validity window. Where that value is kept is the
    // caller's problem; expiry already bounds the exposure to one window.
    llvm::Expected<KeyRevocation> loadKeyRevocation(
        const std::string& envelopeJson,
        const RepositoryDelegation& delegation,
        const std::string& repositoryName,
        std::time_t now,
        std::time_t seenIssuedAt);

    // The repository's current revocation statement.
    //
    // Returns nullopt when the repository does not advertise revocation
    // (protocol §3.1) — it does not do fast revocation, and that is a
    // supported choice, not a fault.
    //
    // ERRORS once it DOES advertise and the statement is missing, expired,
    // unverifiable, or rolled back. This is the only document here whose
    // absence is a failure, and the asymmetry is deliberate: failing open
    // would make blocking one fetch equivalent to un-revoking every key in
    // the repository (spec 2.8.4).
    llvm::Expected<std::optional<KeyRevocation>> revocationFor(
        const Repository& repo,
        const RepositoryDelegation* delegation,
        std::time_t now,
        std::time_t seenIssuedAt);

} // namespace cajeta::buildtool
