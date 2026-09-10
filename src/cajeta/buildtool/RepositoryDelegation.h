// The repository delegation: the root signs this, and this names the keys allowed to
// sign release metadata, so the root can stay offline and a compromised online key
// forges nothing else. A REQUIRED `type` discriminator keeps it from being read as an
// OrgKeyDocument, which would let any organization's key sign for every other.

#pragma once

#include "cajeta/buildtool/OrgKeyDocument.h"
#include "cajeta/buildtool/SignedEnvelope.h"

#include <llvm/Support/Error.h>

#include <ctime>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct RepositoryDelegation {
        // The repository this speaks for, as an ORIGIN checked against the one it was
        // fetched from, never the manifest `name`, which is a label the user chooses.
        std::string repository;
        // Keys permitted to sign release metadata; OrgSigningKey's validity-window rules
        // are identical, so there is no second "is this key usable now".
        std::vector<OrgSigningKey> keys;
        std::time_t notAfter = 0;
        // When this delegation was produced. REQUIRED: expiry alone does not stop a
        // replay, since a superseded delegation is still validly signed and in window,
        // so serving an old copy would reinstate a rotated-out release key.
        std::time_t issuedAt = 0;
        std::string rootKeyId;      // which root signed it (spec §6.3)

        // Keys inside their window at `now`. Empty is legitimate and means "cannot
        // verify", never "verified".
        std::vector<const OrgSigningKey*> usableKeys(std::time_t now) const;
    };

    // Parse and verify a delegation envelope against the roots, failing on a malformed
    // envelope, an untrusted root, a wrong payload type, an origin other than `origin`,
    // expiry, or an issued-at below `seenIssuedAt` (0 = none, correct on a first fetch).
    llvm::Expected<RepositoryDelegation> loadRepositoryDelegation(
        const std::string& envelopeJson,
        const std::vector<RootKey>& roots,
        const std::string& origin,
        std::time_t now,
        std::time_t seenIssuedAt = 0);

} // namespace cajeta::buildtool
