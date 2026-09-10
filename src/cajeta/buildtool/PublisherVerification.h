// Binds an artifact to its publisher: the signing key must belong to the org's
// key document, be inside its window, and the artifact's name must fall in a
// namespace that document claims. Ownership is passed in, never derived.

#pragma once

#include "cajeta/buildtool/KeyRevocation.h"
#include "cajeta/buildtool/OrgKeyDocument.h"

#include <ctime>
#include <string>

namespace cajeta::buildtool {

    // Which check decided the outcome; reported instead of a bare failure.
    enum class PublisherCheck {
        Verified,
        Namespace,     // the name is outside what this org owns (4.3)
        NoUsableKey,   // the document has no key valid right now (4.1)
        Signature,     // no valid key of that org signed these bytes (4.2)
        Revoked,       // a key that would have verified is revoked (2.8)
        Unreadable,    // the artifact or a key could not be read at all
    };

    struct PublisherVerdict {
        PublisherCheck check = PublisherCheck::Signature;
        // Names which check failed and what would resolve it.
        std::string message;
        // The key that verified, when one did.
        std::string keyId;
        std::string organization;

        bool ok() const { return check == PublisherCheck::Verified; }
    };

    // Whether `nameSpace` owns `name`, matched SEGMENT-AWARE: `dev.cajeta` owns
    // `dev.cajeta` and `dev.cajeta.http` but not `dev.cajetaevil`, so this is
    // deliberately not a string prefix test.
    bool namespaceOwns(const std::string& nameSpace, const std::string& name);

    // Verify `artifactPath` against the raw detached ed25519 `signature`, using
    // `doc` — which the caller must already have chosen as the owning org's — and
    // `revocation`, or nullptr. A signature only a revoked key verifies is Revoked.
    PublisherVerdict verifyAgainstOrgDocument(const OrgKeyDocument& doc,
                                              const std::string& artifactName,
                                              const std::string& artifactPath,
                                              const std::string& signature,
                                              std::time_t now,
                                              const KeyRevocation* revocation);

} // namespace cajeta::buildtool
