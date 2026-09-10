// Release metadata - publisher-trust spec §5.1, §6.2: what the repository says about one
// release, its hash and the organization that owns the name. Both travel inside the
// root-signed envelope, so a mirror can serve the bytes but cannot restate them.

#pragma once

#include "cajeta/buildtool/SignedEnvelope.h"

#include <llvm/Support/Error.h>

#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct ReleaseMetadata {
        std::string name;
        std::string version;
        std::string sha256;         // "sha256:<hex>", normalised
        std::string organization;   // who owns the name (spec 6.2)

        // True when the fields above came out of a VERIFIED envelope. False means they
        // were read from a plain document and must not decide a publisher binding.
        bool signedByRoot = false;
        std::string rootKeyId;      // which root signed it (spec 6.3)

        // Withdrawn by its publisher (spec 7.6.2). Read from the SIGNED payload when
        // there is one: the plain `retracted` beside it is advisory only.
        bool retracted = false;
        std::string retractedReason;
    };

    // Parse release metadata, verifying it when signed. Accepts a bare envelope, an object
    // carrying one under `signed`, or a plain object (signedByRoot = false); a present
    // envelope is authoritative. `verifiers` are the roots or delegated keys allowed to sign.
    llvm::Expected<ReleaseMetadata> loadReleaseMetadata(
        const std::string& json,
        const std::vector<RootKey>& verifiers);

} // namespace cajeta::buildtool
