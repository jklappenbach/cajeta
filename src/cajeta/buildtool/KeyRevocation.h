// The revocation statement - publisher-trust spec §2.8. Signed by the DELEGATED
// (online) key and applied in seconds, it is the brake while the offline ceremony
// re-signs the key document; it can only SUBTRACT trust, never add any.

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
        // Empty means EVERY document: key ids are only unique within a document, so
        // an unscoped id is ambiguous and the broad reading is the safe direction.
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

    // Parse and verify a revocation statement against the DELEGATION's keys, never a
    // root. `origin` is the repository it was fetched from, checked against the one it
    // claims; a statement older than `seenIssuedAt` (0 = none) is refused as a rollback.
    llvm::Expected<KeyRevocation> loadKeyRevocation(
        const std::string& envelopeJson,
        const RepositoryDelegation& delegation,
        const std::string& origin,
        std::time_t now,
        std::time_t seenIssuedAt);

    // The repository's current revocation statement. nullopt when the repository does
    // not advertise revocation at all; an ERROR once it does and the statement is
    // missing, expired, unverifiable, or rolled back - failing open would un-revoke.
    llvm::Expected<std::optional<KeyRevocation>> revocationFor(
        const Repository& repo,
        const RepositoryDelegation* delegation,
        std::time_t now,
        std::time_t seenIssuedAt);

} // namespace cajeta::buildtool
