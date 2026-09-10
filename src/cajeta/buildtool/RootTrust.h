// Trust anchors - publisher-trust spec §3. The root public key embedded in the toolchain
// verifies an organization key document with no operator action; operators add roots for
// private or mirrored repositories, additively, and may PIN a repository to one root.

#pragma once

#include "cajeta/buildtool/OrgKeyDocument.h"

#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct RootTrustLayout {
        // Directories searched for operator-added roots and pins, highest precedence first.
        std::vector<std::string> searchDirs;

        // Substitute for the embedded root, so tests can exercise "a root that needs no
        // operator action" WITHOUT the production private key.
        std::optional<RootKey> shippedOverride;
    };

    // The root embedded in this toolchain (§3.1).
    const RootKey& shippedRoot();

    // Every root a key document from `repositoryName` may be verified against, highest
    // precedence first: the shipped root plus operator roots, or ONLY the pinned root
    // when a pin exists. A pin naming an uninstalled root is an ERROR, not an empty set.
    llvm::Expected<std::vector<RootKey>> rootsFor(
        const RootTrustLayout& layout,
        const std::string& repositoryName);

    // Install an operator root into the FIRST search directory. Errors on a duplicate id.
    llvm::Error addRootKey(const RootTrustLayout& layout,
                           const std::string& keyId,
                           const std::string& pemPath);

    // Remove an operator root. Never touches the shipped root, which is part of the binary.
    llvm::Error removeRootKey(const RootTrustLayout& layout,
                              const std::string& keyId);

    // Pin `repositoryName` to `keyId`, or clear the pin when `keyId` is empty.
    llvm::Error pinRepository(const RootTrustLayout& layout,
                              const std::string& repositoryName,
                              const std::string& keyId);

    // The pin in force for a repository, if any.
    std::optional<std::string> pinFor(const RootTrustLayout& layout,
                                      const std::string& repositoryName);

} // namespace cajeta::buildtool
