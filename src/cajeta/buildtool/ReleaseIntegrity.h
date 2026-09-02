// Which hash an install is held to — publisher-trust spec §5.1, §5.2.
//
// One place decides where a release's hash comes from, so no caller can
// pick the weaker source by accident. When the repository serves
// root-signed release metadata, the hash inside that signature wins and
// the unsigned sidecar beside it is IGNORED — not compared, not merged. A
// mirror that rewrites the sidecar to match tampered bytes has therefore
// changed nothing the client reads.
//
// The unsigned sidecar remains the answer for repositories that serve no
// signed metadata: a local filesystem tree, a v1 server, an artifact
// published before this existed. It still catches a corrupted download,
// which is what a checksum alone was ever good for. It cannot catch a
// mirror, because whoever can change the bytes can change the sidecar.

#pragma once

#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/RepositoryDelegation.h"
#include "cajeta/buildtool/SignedEnvelope.h"

#include <llvm/Support/Error.h>

#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct ReleaseIntegrity {
        // "sha256:<hex>", or empty when the repository publishes no hash
        // for this release at all.
        std::string sha256;

        // True when `sha256` came out of a root-signed envelope. False
        // means the unsigned sidecar — a self-consistency check on the
        // download, not a statement about who published it.
        bool fromSignedMetadata = false;

        // The owning organization, present only on the signed path (spec
        // 6.2). Empty otherwise, and deliberately so: an unsigned
        // organization is written as freely as a name prefix, so acting
        // on one would reintroduce the unbound trust §1.2 describes.
        std::string organization;
        std::string rootKeyId;

        // True when the signature came from a key the root DELEGATED rather
        // than from the root itself (spec 2.7). Recorded because the two are
        // different evidence, and a later policy may require one.
        bool viaDelegation = false;

        // Withdrawn by its publisher (spec 7.6.2). `fromSignedMetadata`
        // says how much this is worth: signed, a mirror cannot clear it;
        // unsigned, whoever can write it can clear it, so it is advisory.
        bool retracted = false;
        std::string retractedReason;
    };

    // What `name@version` from `repo` must hash to, and who published it.
    //
    // Errors when the repository serves release metadata that is PRESENT
    // and does not verify. That is not the same as serving none: falling
    // back to the unsigned path there would let a mirror strip a signature
    // to reach the weaker route.
    // `delegation` is the repository's verified delegation, or nullptr when
    // it serves none. When present, release metadata must be signed by one of
    // its keys that is valid at `now`; when absent, the roots verify directly,
    // which is the pre-delegation shape and stays supported.
    llvm::Expected<ReleaseIntegrity> releaseIntegrityFor(
        const Repository& repo,
        const std::string& name,
        const std::string& version,
        const std::vector<RootKey>& roots,
        const RepositoryDelegation* delegation,
        std::time_t now);

} // namespace cajeta::buildtool
