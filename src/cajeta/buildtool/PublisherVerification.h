// Binding an artifact to its publisher — publisher-trust spec §4.
//
// A signature that verifies proves somebody signed the bytes. This is the
// step that makes it mean "the organization that owns this name published
// them": the key must come from that organization's key document, be
// inside its own validity window, and the artifact's name must fall within
// the namespaces the document claims. Drop any one of the three and the
// signature stops proving anything anyone cares about — which is the
// failure that got GPG removed from PyPI.
//
// The organization is never DERIVED from the name (spec §4.4). Dotted
// names have no fixed arity, so any rule for "how many leading segments
// are the org" is wrong for someone, and wrong in the direction an
// attacker picks. Ownership arrives as signed data; this header only takes
// it as an argument.

#pragma once

#include "cajeta/buildtool/KeyRevocation.h"
#include "cajeta/buildtool/OrgKeyDocument.h"

#include <ctime>
#include <string>

namespace cajeta::buildtool {

    // Which check decided the outcome. Callers report this rather than a
    // bare "verification failed", which sends a reader nowhere (spec 4.3.1).
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

    // Whether `nameSpace` owns `name`, matched SEGMENT-AWARE (spec 4.3.1).
    //
    // `dev.cajeta` owns `dev.cajeta` and `dev.cajeta.http`, and does NOT
    // own `dev.cajetaevil`. A plain string prefix test passes every case
    // written with well-behaved names and fails against a name chosen
    // adversarially, which is the only case that matters.
    bool namespaceOwns(const std::string& nameSpace, const std::string& name);

    // Verify `artifactPath` against `doc`.
    //
    // `signature` is the raw detached ed25519 signature the repository
    // publishes. `artifactName` is the dotted package name, and `doc` must
    // already be the document of the organization that signed metadata says
    // owns it — this function does not decide ownership, it enforces it.
    // `revocation` is the repository's current revocation statement, or
    // nullptr when it serves none. A revoked key is skipped as if it were
    // outside its window, and a signature that ONLY a revoked key verifies
    // reports `Revoked` rather than `Signature` — an operator sent to
    // "the signature is wrong" when the answer is "that key was
    // compromised" loses the incident (spec 2.8).
    PublisherVerdict verifyAgainstOrgDocument(const OrgKeyDocument& doc,
                                              const std::string& artifactName,
                                              const std::string& artifactPath,
                                              const std::string& signature,
                                              std::time_t now,
                                              const KeyRevocation* revocation);

} // namespace cajeta::buildtool
