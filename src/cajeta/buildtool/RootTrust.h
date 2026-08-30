// Trust anchors — publisher-trust spec §3.
//
// Every PKI bottoms out in something trusted for a non-cryptographic
// reason. Here that is the root public key embedded in the toolchain: a
// client verifies an organization key document against it with no operator
// action, the way an OS verifies against a shipped CA bundle. Rotation
// rides the toolchain's own release channel, which is already signed.
//
// Operators add roots for private or mirrored repositories, and may PIN a
// repository to one root. Added roots are additive to the shipped one —
// installing a root for a local mirror must not silently change who can
// vouch for the public repository.

#pragma once

#include "cajeta/buildtool/OrgKeyDocument.h"

#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct RootTrustLayout {
        // Directories searched for operator-added roots and pins, highest
        // precedence first. Callers pass the tiers the trust store already
        // defines (env → user → system) rather than this deriving them, so
        // there is one place that decides where trust material lives.
        std::vector<std::string> searchDirs;

        // Substitute for the embedded root. Exists so tests can exercise
        // "a root that needs no operator action" WITHOUT the production
        // private key — which is only absent from this tree because it must
        // be. A test that required it would be evidence of a leak.
        std::optional<RootKey> shippedOverride;
    };

    // The root embedded in this toolchain (§3.1).
    const RootKey& shippedRoot();

    // Every root a key document from `repositoryName` may be verified
    // against, highest precedence first.
    //
    // With no pin: the shipped root plus every operator-added root. With a
    // pin: only the root that pin names, even if others are trusted (§3.3)
    // — a pin is a narrowing statement, and one that silently widened would
    // be worse than none.
    //
    // A pin naming a root that is not installed is an ERROR, not an empty
    // set. Falling back to "verify against everything" when the pin cannot
    // be honoured inverts the operator's intent at exactly the moment it
    // matters.
    llvm::Expected<std::vector<RootKey>> rootsFor(
        const RootTrustLayout& layout,
        const std::string& repositoryName);

    // Install an operator root into the FIRST search directory. Errors on a
    // duplicate id or a PEM that is not an ed25519 public key.
    llvm::Error addRootKey(const RootTrustLayout& layout,
                           const std::string& keyId,
                           const std::string& pemPath);

    // Remove an operator root. Never touches the shipped root: it is part
    // of the binary, so "remove" would be a lie that survives until the
    // next lookup.
    llvm::Error removeRootKey(const RootTrustLayout& layout,
                              const std::string& keyId);

    // Pin `repositoryName` to `keyId`, or clear the pin when `keyId` is
    // empty.
    llvm::Error pinRepository(const RootTrustLayout& layout,
                              const std::string& repositoryName,
                              const std::string& keyId);

    // The pin in force for a repository, if any.
    std::optional<std::string> pinFor(const RootTrustLayout& layout,
                                      const std::string& repositoryName);

} // namespace cajeta::buildtool
