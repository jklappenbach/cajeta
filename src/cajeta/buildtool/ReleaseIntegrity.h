// Which hash an install is held to - publisher-trust spec §5.1, §5.2. One place decides
// where a release's hash comes from: when the repository serves root-signed metadata the
// hash inside the signature wins and the unsigned sidecar beside it is IGNORED.

#pragma once

#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/RepositoryDelegation.h"
#include "cajeta/buildtool/SignedEnvelope.h"

#include <llvm/Support/Error.h>

#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct ReleaseIntegrity {
        // "sha256:<hex>", or empty when the repository publishes no hash for this release.
        std::string sha256;

        // True when `sha256` came out of a root-signed envelope. False means the unsigned
        // sidecar - a self-consistency check on the download, not a claim about publisher.
        bool fromSignedMetadata = false;

        // The owning organization, present only on the signed path; an unsigned one would
        // be as forgeable as a name prefix, so it is deliberately left empty.
        std::string organization;
        std::string rootKeyId;

        // True when a key the root DELEGATED signed it rather than the root itself.
        bool viaDelegation = false;

        // Withdrawn by its publisher; only trustworthy when it arrived signed.
        bool retracted = false;
        std::string retractedReason;
    };

    // What `name@version` from `repo` must hash to, and who published it. Release metadata
    // that is PRESENT and does not verify is an ERROR, never a fall back to the sidecar.
    // `delegation` is the repository's verified delegation, or nullptr when it serves none.
    llvm::Expected<ReleaseIntegrity> releaseIntegrityFor(
        const Repository& repo,
        const std::string& name,
        const std::string& version,
        const std::vector<RootKey>& roots,
        const RepositoryDelegation* delegation,
        std::time_t now);

} // namespace cajeta::buildtool
