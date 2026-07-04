// SLSA-style build attestation — Phase 13.
//
// The `publish` action generates an in-toto Statement v1 envelope
// whose predicate is a SLSA v1 provenance record. The shape
// matches the spec in docs/BuildTool.md "Build attestation":
//
//   { "_type": "https://in-toto.io/Statement/v1",
//     "subject": [ { "name": "<archive>", "digest": {"sha256": "<hex>"} } ],
//     "predicateType": "https://slsa.dev/provenance/v1",
//     "predicate": {
//       "buildDefinition": {
//         "buildType": "https://cajeta.org/build/v1",
//         "externalParameters": { manifest-checksum, lockfile-checksum },
//         "internalParameters": { compiler-version, flavor, target }
//       },
//       "runDetails": {
//         "builder": { "id": "<builder-id>" },
//         "metadata": { "startedOn": ..., "finishedOn": ... }
//       }
//     }
//   }
//
// The envelope is signed with the same ed25519 key the archive is
// signed with — reuses the Phase 10 trust-store flow. The signed
// envelope ships next to the archive as `<archive>.attestation`.
//
// Consumers (`cajeta install`) verify the signature against the
// trust store, then check the provenance's `subject[].digest`
// matches the archive bytes before extracting anything.

#pragma once

#include <llvm/Support/Error.h>

#include <string>

namespace cajeta::buildtool {

    // Inputs for composing a provenance record. All strings are
    // already-rendered values (the composer doesn't substitute
    // properties — callers do).
    struct ProvenanceInputs {
        // Subject:
        std::string archiveName;       // e.g. "cajeta.io.net.http-1.2.4.cja"
        std::string archiveSha256;      // "sha256:<hex>"
        // Build definition:
        std::string manifestChecksum;
        std::string lockfileChecksum;
        std::string compilerVersion;
        std::string flavor;             // "release" / "debug" / …
        std::string target;             // "x86_64-linux-gnu" / "host"
        // Run details:
        std::string builderId;          // "https://github.com/cajeta-org/builder"
        std::string startedOn;          // ISO 8601
        std::string finishedOn;         // ISO 8601
    };

    // Compose the JSON Statement envelope. The returned value is a
    // canonicalized (sorted-key, 2-space-indent) byte string so its
    // SHA-256 is stable across runs — a precondition for signing.
    std::string composeProvenanceJson(const ProvenanceInputs& in);

    // Verify a provenance JSON document's structural validity:
    //   - Statement type / predicate type strings match the spec
    //   - subject[].digest.sha256 matches `expectedSha256`
    //   - manifest/lockfile checksums + compiler version are
    //     populated (non-empty)
    //
    // Returns the parsed predicate's `buildDefinition` object for
    // callers that want to inspect (e.g. record audit trails).
    // Errors carry one-line citations of the first mismatch.
    struct ProvenanceVerifyResult {
        std::string buildType;
        std::string compilerVersion;
        std::string flavor;
        std::string target;
        std::string manifestChecksum;
        std::string lockfileChecksum;
    };
    llvm::Expected<ProvenanceVerifyResult> verifyProvenanceJson(
        const std::string& jsonDoc,
        const std::string& expectedSha256);

} // namespace cajeta::buildtool
